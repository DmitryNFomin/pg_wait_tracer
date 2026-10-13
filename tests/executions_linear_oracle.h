/* Frozen verbatim execution scan from 6bfe7d3; only names changed. */
struct oracle_exec_pid_state {
    uint32_t pid;
    /* Stack (LIFO) of row indices this pid currently has open, deepest
     * (innermost) execution last -- see linear_compute_executions's
     * EXEC_START/EXEC_END/CMD_END handling. A single `active_row` int
     * (the pre-#222-review shape) loses the OUTER row the moment a second
     * EXEC_START fires -- true nesting (SQL-level EXECUTE of a prepared
     * statement from inside plpgsql, PortalRun genuinely nested) and,
     * just as commonly, a backend that errors on one statement and the
     * client immediately sends another on the same connection with no
     * EXEC_END in between for the first. Pushing (never overwriting)
     * keeps every still-open row reachable so CMD_END -- the one marker
     * that fires only once this pid has actually gone idle, so it can
     * never cut a genuinely still-running statement -- can close every
     * one of them, however deep. */
    int *open_rows;
    int n_open, cap_open;
    /* #222 review: a wait event must attribute to the top-of-stack row
     * only while that row is genuinely the one that JUST started -- not
     * merely whatever an EXEC_END's pop happened to re-expose underneath
     * it. Set on every push (a fresh EXEC_START), cleared on every pop
     * (EXEC_END or CMD_END): once cleared, oracle_exec_pid_attributable_row()
     * returns -1 until the next EXEC_START, even though the stack itself
     * may still be non-empty. Without this, a pid that errors on A (A
     * stays stacked, unclosed), then runs and finishes B, has every wait
     * event AFTER B's EXEC_END mis-credited to A -- a long-dead orphan
     * that silently keeps accruing n_events (and matches_event_filter)
     * for as long as CMD_END never arrives to close it (see the CMD_END
     * comment below). Master attributed those same events to no row at
     * all; that is the safer default here too, since the marker stream
     * alone cannot distinguish "B is legitimately nested inside A" from
     * "B is an unrelated statement after A orphaned" -- CMD_END is the
     * only marker that can. */
    int top_attributable;
    uint64_t plan_start_ns;
    uint64_t plan_query_id;
    int plan_open;
    uint64_t ready_plan_start_ns;
    uint64_t ready_plan_end_ns;
    uint64_t ready_plan_query_id;
    int plan_ready;
};

static int oracle_exec_pid_state_get(struct oracle_exec_pid_state **states, int *n_states,
                              int *cap_states, uint32_t pid)
{
    for (int i = 0; i < *n_states; i++)
        if ((*states)[i].pid == pid)
            return i;

    if (*n_states >= *cap_states) {
        int new_cap = *cap_states ? *cap_states * 2 : 64;
        struct oracle_exec_pid_state *tmp = realloc(*states,
                                             new_cap * sizeof(**states));
        if (!tmp)
            return -1;
        *states = tmp;
        *cap_states = new_cap;
    }

    int idx = (*n_states)++;
    memset(&(*states)[idx], 0, sizeof((*states)[idx]));
    (*states)[idx].pid = pid;
    return idx;
}

/* Push a newly-opened row's index onto its pid's open stack. Marks it
 * freshly attributable -- see top_attributable's own comment. */
static int oracle_exec_pid_push_row(struct oracle_exec_pid_state *st, int row_idx)
{
    if (st->n_open >= st->cap_open) {
        int new_cap = st->cap_open ? st->cap_open * 2 : 4;
        int *tmp = realloc(st->open_rows, new_cap * sizeof(*tmp));
        if (!tmp)
            return -1;
        st->open_rows = tmp;
        st->cap_open = new_cap;
    }
    st->open_rows[st->n_open++] = row_idx;
    st->top_attributable = 1;
    return 0;
}

/* Pop the innermost (most recently opened) still-open row, or -1 if none.
 * Whatever the pop exposes underneath (if anything) is no longer
 * attributable until its own EXEC_START pushes it again -- see
 * top_attributable's own comment for why. */
static int oracle_exec_pid_pop_row(struct oracle_exec_pid_state *st)
{
    st->top_attributable = 0;
    if (st->n_open <= 0)
        return -1;
    return st->open_rows[--st->n_open];
}

/* The innermost still-open row without removing it -- the row a wait event
 * arriving right now belongs to. */
static int oracle_exec_pid_peek_row(const struct oracle_exec_pid_state *st)
{
    return st->n_open > 0 ? st->open_rows[st->n_open - 1] : -1;
}

/* Like oracle_exec_pid_peek_row, but -1 whenever the top was exposed by a pop
 * rather than freshly pushed -- see top_attributable's own comment. This
 * is the one callers use for wait-event attribution (n_events,
 * matches_event_filter, n_workers); oracle_exec_pid_peek_row alone stays
 * available for anything that legitimately wants "whatever's on top"
 * regardless (there is no such caller today, kept for symmetry with push/
 * pop). */
static int oracle_exec_pid_attributable_row(const struct oracle_exec_pid_state *st)
{
    return st->top_attributable ? oracle_exec_pid_peek_row(st) : -1;
}

static int oracle_execution_append(struct pgwt_execution **rows, int *n_rows,
                            int *cap_rows, const struct pgwt_execution *row)
{
    if (*n_rows >= *cap_rows) {
        int new_cap = *cap_rows ? *cap_rows * 2 : 128;
        struct pgwt_execution *tmp = realloc(*rows,
                                             new_cap * sizeof(**rows));
        if (!tmp)
            return -1;
        *rows = tmp;
        *cap_rows = new_cap;
    }
    (*rows)[*n_rows] = *row;
    return (*n_rows)++;
}

static int oracle_worker_link_index(const struct pgwt_backend_link *links, int n_links,
                             uint32_t pid)
{
    int lo = 0, hi = n_links;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (links[mid].pid < pid) lo = mid + 1;
        else                      hi = mid;
    }
    return lo < n_links && links[lo].pid == pid && links[lo].leader_pid != 0
         ? lo : -1;
}

static int oracle_interval_overlaps(uint64_t event_end, uint64_t duration,
                             uint64_t from_ns, uint64_t to_ns);

void linear_compute_executions(const struct pgwt_trace_event *events, int count,
                             uint64_t from_ns, uint64_t to_ns,
                             const struct pgwt_filter *f,
                             const struct pgwt_backend_link *links, int n_links,
                             struct pgwt_executions_result *out)
{
    memset(out, 0, sizeof(*out));

    struct pgwt_execution *rows = NULL;
    int n_rows = 0, cap_rows = 0;
    struct oracle_exec_pid_state *states = NULL;
    int n_states = 0, cap_states = 0;
    int *worker_last_row = NULL;
    if (n_links > 0) {
        worker_last_row = malloc(n_links * sizeof(*worker_last_row));
        if (!worker_last_row) {
            out->failed = 1;
            return;
        }
        for (int i = 0; i < n_links; i++) worker_last_row[i] = -1;
    }

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        int si = oracle_exec_pid_state_get(&states, &n_states, &cap_states, ev->pid);
        if (si < 0) {
            out->failed = 1;
            break;
        }
        struct oracle_exec_pid_state *st = &states[si];
        uint32_t marker = ev->old_event;

        if (marker == PGWT_MARKER_PLAN_START) {
            st->plan_start_ns = ev->timestamp_ns;
            st->plan_query_id = ev->query_id;
            st->plan_open = 1;
            continue;
        }
        if (marker == PGWT_MARKER_PLAN_END) {
            if (st->plan_open && ev->timestamp_ns >= st->plan_start_ns) {
                st->ready_plan_start_ns = st->plan_start_ns;
                st->ready_plan_end_ns = ev->timestamp_ns;
                st->ready_plan_query_id = st->plan_query_id
                                        ? st->plan_query_id : ev->query_id;
                st->plan_ready = 1;
            }
            st->plan_open = 0;
            continue;
        }
        if (marker == PGWT_MARKER_EXEC_START) {
            /* PUSH, never overwrite: PortalRun genuinely nests for
             * SQL-level EXECUTE of a prepared statement from inside
             * plpgsql, and a backend that errors on one statement (no
             * EXEC_END) commonly has the client send another right away
             * on the same connection -- both leave an earlier row still
             * open when this EXEC_START fires. Closing it here (or
             * discarding it by overwriting a single active_row slot)
             * would be wrong: the outer/earlier row might still be
             * genuinely running. Only CMD_END (below) is a safe place to
             * give up on a row this pid left open. */
            if (ev->timestamp_ns <= to_ns) {
                struct pgwt_execution row;
                memset(&row, 0, sizeof(row));
                row.pid = ev->pid;
                row.query_id = ev->query_id;
                row.start_ns = ev->timestamp_ns;
                row.in_progress = 1;
                row.started_before_window = ev->timestamp_ns < from_ns;
                if (st->plan_ready &&
                    (st->ready_plan_query_id == 0 || row.query_id == 0 ||
                     st->ready_plan_query_id == row.query_id)) {
                    row.plan_start_ns = st->ready_plan_start_ns;
                    row.plan_end_ns = st->ready_plan_end_ns;
                    row.has_plan = 1;
                }
                int row_idx = oracle_execution_append(&rows, &n_rows, &cap_rows,
                                               &row);
                if (row_idx < 0 || oracle_exec_pid_push_row(st, row_idx) < 0) {
                    out->failed = 1;
                    break;
                }
            } else {
                /* #222 review round 3, item 2: this pid HAS started a new
                 * statement; we simply are not keeping a row for it (its
                 * start is past the caller's window). The previous row
                 * must stop being attributable all the same -- otherwise
                 * top_attributable keeps whatever the last push left it
                 * at, and a later wait event is credited to an earlier,
                 * still-open row that the new statement has in fact
                 * superseded. Master cleared active_row unconditionally
                 * here for the same reason. Unreachable through the
                 * server today (server_load_events_fi stops at
                 * `ts > sample_to_m`, so no event after this one is even
                 * loaded, and oracle_interval_overlaps would gate it anyway) --
                 * this makes it unreachable BY CONSTRUCTION rather than
                 * by caller discipline, and linear_compute_executions is a
                 * public entry point that does not get to assume its
                 * caller pre-filtered. */
                st->top_attributable = 0;
            }
            /* A completed plan belongs to this next execution, including a
             * start outside the caller's output window. Never reuse it. */
            st->plan_ready = 0;
            continue;
        }
        if (marker == PGWT_MARKER_EXEC_END) {
            /* Pops the INNERMOST open row -- correct for nesting (the
             * statement that started last finishes first) and for the
             * plain non-nested case (stack depth 1). A real, measured
             * end: end_inferred stays 0. */
            int row_idx = oracle_exec_pid_pop_row(st);
            if (row_idx >= 0 && row_idx < n_rows &&
                ev->timestamp_ns >= rows[row_idx].start_ns) {
                struct pgwt_execution *row = &rows[row_idx];
                row->end_ns = ev->timestamp_ns;
                row->in_progress = 0;
                if (row->query_id == 0)
                    row->query_id = ev->query_id;
            }
            continue;
        }
        if (marker == PGWT_MARKER_CMD_END) {
            /* A command can close without ever reaching EXEC_END: an ERROR
             * longjmp, a cancel, statement_timeout, a client disconnect
             * mid-query, or an EXEC_END lost to a ringbuf drop all skip
             * src/daemon.c's query__execute__done probe. Left alone, that
             * row stays in_progress for the rest of the retained capture
             * with a duration fabricated from the caller's window bound
             * (execution_sort_duration_ns, src/server.c) -- sorted above
             * every genuinely slow COMPLETED execution once "longest
             * running first" ranks by duration (#222 review: the
             * demo-blocking failure #222 was filed to prevent,
             * reintroduced from the other direction -- concretely, a
             * single presenter Ctrl-C on an ad-hoc query pins one
             * "In progress" row at the top of the tab for the rest of the
             * session; n=1 is enough).
             *
             * CMD_END is the correct closing point, and closing every row
             * still open for this pid (not just the innermost) is safe,
             * BECAUSE of what CMD_END means: src/bpf/pg_wait_tracer.bpf.c
             * emits it only on the pgstat_report_activity gate flip to
             * IDLE, which happens after every nested portal for this
             * command has itself finished or errored -- the backend
             * cannot be RUNNING anything when this fires. So CMD_END can
             * never truncate a still-running statement, nested or not;
             * closing on a second EXEC_START instead would be wrong for
             * exactly that reason (see the EXEC_START comment above). Each
             * closed-here row's end_ns is a real wall-clock timestamp --
             * not fabricated from the window bound -- but DEDUCED from the
             * idle transition rather than measured at the query's own
             * completion, so end_inferred=1 (struct pgwt_execution,
             * compute.h): a cancelled statement is consequently
             * indistinguishable from a normally-completed one of the same
             * measured span in this field alone -- acceptable, but not
             * invisible: end_inferred is exactly the bit that says so.
             * Mirrors pgwt_tag_events's own CMD_END handling (this file,
             * ~line 267: "an EXEC_END lost to a ringbuf drop must not leak
             * the window across statements") for a different computation
             * over the same markers.
             *
             * Not a total fix: CMD_END itself is not guaranteed. It is
             * emitted only while a live watchpoint is open on this pid
             * (src/bpf/pg_wait_tracer.bpf.c: st->wp_live &&
             * exact_admission_open()) -- a pid that never escalated, or
             * whose CMD_END marker is itself lost to a ringbuf drop, still
             * leaves a genuine zombie. Nothing below claims otherwise. */
            int row_idx;
            while ((row_idx = oracle_exec_pid_pop_row(st)) >= 0) {
                if (row_idx < n_rows && rows[row_idx].in_progress &&
                    ev->timestamp_ns >= rows[row_idx].start_ns) {
                    struct pgwt_execution *row = &rows[row_idx];
                    row->end_ns = ev->timestamp_ns;
                    row->in_progress = 0;
                    row->end_inferred = 1;
                }
            }
            st->plan_open = 0;
            st->plan_ready = 0;
            continue;
        }
        if (PGWT_IS_MARKER(marker) ||
            (ev->flags & PGWT_EVENT_FLAG_SAMPLE))
            continue;

        /* The transition stream is ordered by interval end. Lifecycle END is
         * emitted after the final interval closes, so active-row accounting
         * is both linear and faithful to interval overlap at execution edges. */
        struct pgwt_filter event_filter = f ? *f : (struct pgwt_filter){0};
        /* A pid filter identifies the leader execution. Parallel-worker
         * membership supplies pid scope; the shared matcher applies the
         * event-level class/event_id/query_id dimensions. */
        event_filter.pid = 0;
        int event_matches =
            oracle_interval_overlaps(ev->timestamp_ns, ev->duration_ns,
                              from_ns, to_ns) &&
            pgwt_filter_matches(&event_filter, ev);
        int active_row = oracle_exec_pid_attributable_row(st);
        if (event_matches && active_row >= 0 && active_row < n_rows) {
            rows[active_row].n_events++;
            rows[active_row].matches_event_filter = 1;
        }

        int wi = worker_last_row
               ? oracle_worker_link_index(links, n_links, ev->pid) : -1;
        if (wi >= 0) {
            int li = oracle_exec_pid_state_get(&states, &n_states, &cap_states,
                                        links[wi].leader_pid);
            if (li < 0) {
                out->failed = 1;
                break;
            } else {
                int row_idx = oracle_exec_pid_attributable_row(&states[li]);
                if (event_matches && row_idx >= 0 && row_idx < n_rows &&
                    worker_last_row[wi] != row_idx) {
                    rows[row_idx].n_workers++;
                    rows[row_idx].matches_event_filter = 1;
                    worker_last_row[wi] = row_idx;
                }
            }
        }
    }

    free(worker_last_row);
    for (int i = 0; i < n_states; i++)
        free(states[i].open_rows);
    free(states);
    if (out->failed) {
        free(rows);
    } else {
        int kept = 0;
        for (int i = 0; i < n_rows; i++) {
            if (rows[i].start_ns > to_ns)
                continue;
            if (!rows[i].in_progress && rows[i].end_ns < from_ns)
                continue;
            rows[kept++] = rows[i];
        }
        out->rows = rows;
        out->num_rows = kept;
    }
}

static int oracle_interval_overlaps(uint64_t event_end, uint64_t duration,
                             uint64_t from_ns, uint64_t to_ns)
{
    uint64_t event_start = event_end >= duration ? event_end - duration : 0;
    return event_start < to_ns && event_end > from_ns;
}
