/* replay_accum.c — ordered offline accumulation, separate from the file reader.
 * Daemon-only in production; tests link it alongside the production server
 * reader so loader/replay comparisons exercise that exact reader object. */
#include "event_reader.h"
#include "event_sort.h"
#include "map_reader.h"
#include "wait_event.h"
#include "window_clip.h"

/* The resolver is the live resolver; only the emitted statistics differ:
 * replay clips time and selects occurrence statistics independently. */
static void replay_qattr_emit(void *ctx, uint64_t qid, uint32_t we,
                               const struct pgwt_qattr_pending *p,
                               bool backfilled)
{
    struct pgwt_accumulator *acc = ctx;
    struct pgwt_query_event_stats *qe =
        pgwt_get_or_create_query_event(acc, qid, we);
    if (qe) {
        qe->count += p->count;
        qe->total_ns += p->total_ns;
        if (p->min_ns < qe->min_ns) qe->min_ns = p->min_ns;
        if (p->max_ns > qe->max_ns) qe->max_ns = p->max_ns;
    }
    if (backfilled) acc->qattr_backfilled_ns += p->total_ns;
    else acc->qattr_unattributed_ns += p->total_ns;
}

/* End of input: resolve each pid's remaining waits by rule 2/3. */
void pgwt_replay_finish(struct pgwt_accumulator *acc)
{
    for (int i = 0; i < acc->num_pids; i++)
        pgwt_live_qattr_pid_exit(acc, acc->pids[i].pid);
}

int pgwt_replay_check_finished(const struct pgwt_accumulator *acc)
{
    for (int i = 0; i < acc->num_pids; i++) {
        if (acc->pids[i].qattr.npending) {
            fprintf(stderr, "ERROR: replay has pending query attribution; call pgwt_replay_finish before reading query totals\n");
            return -1;
        }
    }
    return 0;
}

void pgwt_replay_events(struct pgwt_accumulator *acc,
                         const struct pgwt_trace_event *events, int count,
                         uint64_t from_mono_ns, uint64_t to_mono_ns,
                         const struct pgwt_block_info *bi,
                         struct pgwt_replay_stats *st)
{
    int is_samples = bi && bi->block_type == PGWT_BLOCK_SAMPLES;
    uint64_t period = is_samples ? bi->sample_period_ns : 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *evt = &events[i];
        int sampled = is_samples || (evt->flags & PGWT_EVENT_FLAG_SAMPLE);
        /* The file sort carries each sample block's weight in duration_ns
         * on its private decoded copy. Direct block callers use bi. */
        uint64_t sample_ns = bi ? period : evt->duration_ns;
        if (st && sampled && sample_ns) st->sample_period_ns = sample_ns;

        uint32_t we;
        uint64_t add_ns;   /* time credited to THIS window (clipped) */
        uint64_t occ_ns;   /* this occurrence's whole duration */
        int exact;
        int closed_in_window;

        if (sampled) {
            /* A sample is a POINT observation of the CURRENT wait
             * (new_event): worth one sample period of estimated time, no real
             * duration, and no interval to clip. So it keeps its own
             * timestamp-selected rule — the overlap rule below is for
             * intervals only. */
            if (from_mono_ns > 0 && evt->timestamp_ns < from_mono_ns)
                continue;
            if (to_mono_ns > 0 && evt->timestamp_ns > to_mono_ns) {
                if (is_samples)
                    break;  /* a SAMPLES block is sorted and holds only points */
                continue;
            }
            we = evt->new_event;
            occ_ns = add_ns = sample_ns;
            exact = 0;
            closed_in_window = 1;
        } else {
            /* #586: replay pre-window markers in stream order to seed
             * cmd_qid at the left edge, exactly as the server's seeded tag
             * pass. They admit no time or occurrences. Markers past `to`
             * are NOT evidence for this window. */
            if (PGWT_IS_MARKER(evt->old_event)) {
                if (to_mono_ns == 0 || evt->timestamp_ns <= to_mono_ns) {
                    if (evt->old_event != PGWT_MARKER_ESCALATE_START &&
                        evt->old_event != PGWT_MARKER_ESCALATE_END &&
                        !(evt->flags & PGWT_EVENT_FLAG_IO_WORKER))
                        pgwt_live_qattr_marker(acc,
                            pgwt_get_or_create_pid(acc, evt->pid),
                            evt->old_event, evt->query_id);
                    if (st && (from_mono_ns == 0 || evt->timestamp_ns >= from_mono_ns))
                        st->markers_skipped++;
                }
                continue;
            }
            /* INTERVAL record (#424). Admission is by OVERLAP
             * (close >= from && start <= to) and the contribution is CLIPPED
             * to the window. The old rule selected on the close timestamp
             * alone and added duration_ns whole, which was wrong twice:
             *   - a LEFT straddler (started before `from`) over-reported by
             *     everything that ran before the window opened;
             *   - a RIGHT straddler (still running at `to`) was dropped
             *     entirely, contributing 0 of its in-window part.
             *
             * There is no `break` on ts > to any more: the records past the
             * right edge are exactly where the right straddlers are. The
             * block loop (pgwt_replay_file) has to scan past `to` for the
             * same reason — fixing only this loop leaves them unreachable.
             *
             * What this canNOT fix, and no admission rule can: a wait that is
             * still open has no closing record in the trace at all, so a
             * window at the very end of a still-growing capture still
             * under-reports it until the close is written. That needs
             * open-interval markers in the trace format. */
            add_ns = event_window_ns(evt, from_mono_ns, to_mono_ns);
            closed_in_window =
                (from_mono_ns == 0 || evt->timestamp_ns >= from_mono_ns) &&
                (to_mono_ns   == 0 || evt->timestamp_ns <= to_mono_ns);
            if (add_ns == 0 && !closed_in_window)
                continue;   /* no overlap with the window at all */

            we = evt->old_event;
            occ_ns = evt->duration_ns;

            exact = 1;
        }

        /* INSTANT vs INTERVAL, the product-wide rule (src/summary_writer.h's
         * v8 note, src/compute.c's raw half): TIME is overlap-selected and
         * clipped; per-OCCURRENCE statistics (count, histogram, min/max) are
         * selected by the record's own close timestamp and use the whole
         * duration. Widening admission without this split would turn a known
         * undercount of time into a new OVERCOUNT of counts and latencies —
         * two adjacent windows would each count the same close. */
        if (st) {
            if (!exact)                 st->samples++;
            else if (closed_in_window)  st->transitions++;
            else                        st->straddlers_admitted++;
        }

        uint64_t cpu_add = add_ns, offcpu_add = 0;
        /* Per-event-table row value (#531). `row_add` is what the TIME
         * column of every per-event table gets (window-clipped), `row_occ`
         * what the per-OCCURRENCE columns get (min/max/histogram, the whole
         * occurrence). Both are wall for every wait and idle label; for the
         * CPU* row they are the MEASURED on-CPU ns, exactly as
         * map_reader.c's add_event_stats does on the live path. Before this,
         * one replay invocation printed the same "CPU*" label twice with two
         * different numbers -- wall in system_event/session_event/
         * query_event, measured in time_model -- and src/output.c's
         * print_offcpu_row_wide (#498) then added the off-CPU remainder on
         * top of a row that already held the whole wall, so the view's %DB
         * column summed past 100%. */
        uint64_t row_add = add_ns, row_occ = occ_ns;
        if (we == 0 && !sampled && evt->cpu_ns != PGWT_CPU_NS_UNKNOWN) {
            /* #495: match map_reader.c's measured CPU split and compute.c's
             * window scaling. Clamp skew before scaling; measured zero is
             * real zero, while UNKNOWN keeps legacy whole-gap CPU. Samples
             * keep their whole sample-period weight as CPU, with no
             * off-CPU. */
            uint64_t m = evt->cpu_ns < occ_ns ? evt->cpu_ns : occ_ns;
            /* #534: the clamp removes a measurable amount -- report it
             * instead of discarding it. Non-zero means measured CPU
             * exceeded the wall interval it belongs to (clock skew, a
             * schedstat read straddling a boundary, an accounting error),
             * exactly what the live path's cpu_clamped_ns_total says, in
             * the same unit. Silence here is indistinguishable from zero,
             * so a trace with real skew replayed as though it were clean.
             * Deliberately NOT scaled by the window clip: it describes the
             * record's measurement, not time being apportioned. */
            if (st && evt->cpu_ns > occ_ns)
                st->cpu_clamped_ns += evt->cpu_ns - occ_ns;
            cpu_add = occ_ns
                ? (uint64_t)(((unsigned __int128)m * add_ns) / occ_ns) : 0;
            if (cpu_add > add_ns) cpu_add = add_ns;
            offcpu_add = add_ns - cpu_add;
            row_add = cpu_add;
            row_occ = m;
        }

        /* Per-PID accumulation */
        struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(acc, evt->pid);
        if (pa) {
            struct pgwt_event_stats *es = pgwt_get_or_create_event(pa, we);
            if (es) {
                if (closed_in_window) es->count++;
                es->total_ns += row_add;
                if (exact && closed_in_window) {
                    if (row_occ < es->min_ns) es->min_ns = row_occ;
                    if (row_occ > es->max_ns) es->max_ns = row_occ;
                    uint32_t bucket = pgwt_duration_to_bucket(row_occ);
                    if (bucket < HISTOGRAM_BUCKETS)
                        es->histogram[bucket]++;
                }
            }
            if (we == 0)
                pa->cpu_time_ns += cpu_add;
            else if (!pgwt_is_idle_event(we))
                pa->wait_time_ns += add_ns;
            if (!pgwt_is_idle_event(we))
                pa->db_time_ns += add_ns;
        }

        /* System-wide accumulation */
        struct pgwt_event_stats *se = pgwt_get_or_create_system_event(acc, we);
        if (se) {
            if (closed_in_window) se->count++;
            se->total_ns += row_add;
            if (exact && closed_in_window) {
                if (row_occ < se->min_ns) se->min_ns = row_occ;
                if (row_occ > se->max_ns) se->max_ns = row_occ;
                uint32_t bucket = pgwt_duration_to_bucket(row_occ);
                if (bucket < HISTOGRAM_BUCKETS)
                    se->histogram[bucket]++;
            }
        }

        /* #495: split inline as in map_reader.c; pgwt_update_time_model
         * accepts only wall time and cannot express measured off-CPU.
         * Wait-labelled spin stays observability only (compute.c): every
         * other label keeps its full wall contribution. */
        if (we == 0) {
            acc->tm.cpu_time_ns += cpu_add;
            acc->tm.offcpu_time_ns += offcpu_add;
            acc->tm.db_time_ns += add_ns;
        } else {
            pgwt_update_time_model(&acc->tm, we, add_ns);
        }

        /* Same bounded per-pid resolver as live, with replay's clipped
         * time and independently selected occurrence statistics. */
        uint32_t cat = evt->flags & (PGWT_EVENT_FLAG_IO_WORKER |
            PGWT_EVENT_FLAG_MAINT | PGWT_EVENT_FLAG_BACKGROUND);
        if (cat & PGWT_EVENT_FLAG_IO_WORKER)
            continue; /* structurally query-less, as on the live path */
        bool idle = pgwt_is_session_idle_event(we);
        if (evt->query_id != 0) {
            if (pa)
                pgwt_qattr_observe(&pa->qattr, evt->query_id, replay_qattr_emit, acc);
            struct pgwt_query_event_stats *qe =
                pgwt_get_or_create_query_event(acc, evt->query_id, we);
            if (qe) {
                if (closed_in_window) qe->count++;
                qe->total_ns += row_add;
                if (exact && closed_in_window) {
                    if (row_occ < qe->min_ns) qe->min_ns = row_occ;
                    if (row_occ > qe->max_ns) qe->max_ns = row_occ;
                }
            }
        } else if (cat == 0 && pa) {
            if (idle) {
                if (!sampled || !(evt->flags & PGWT_EVENT_FLAG_CMD_OPEN))
                    pgwt_qattr_boundary(&pa->qattr, false, replay_qattr_emit, acc);
            } else {
                struct pgwt_qattr_pending p = {
                    .count = closed_in_window ? 1 : 0, .total_ns = row_add,
                    .min_ns = exact && closed_in_window ? row_occ : UINT64_MAX,
                    .max_ns = exact && closed_in_window ? row_occ : 0,
                };
                if (pgwt_qattr_defer_stats(&pa->qattr, we, &p, replay_qattr_emit, acc))
                    acc->qattr_pending_overflow++;
            }
        }
        /* No pid slot: match live's drop of zero-id query time. */
        if (sampled && cat == 0 && idle && !(evt->flags & PGWT_EVENT_FLAG_CMD_OPEN))
            pgwt_live_qattr_between_commands(acc, pa);
        if (evt->new_event == PGWT_EVENT_EXIT)
            pgwt_live_qattr_pid_exit(acc, evt->pid);
    }
}

int pgwt_replay_file(struct pgwt_event_reader *r,
                     struct pgwt_accumulator *acc,
                     uint64_t from_mono_ns, uint64_t to_mono_ns,
                     struct pgwt_trace_event *buf, int max_events,
                     int *lookahead_budget,
                     struct pgwt_replay_stats *st,
                     uint64_t *out_decoded)
{
    if (out_decoded) *out_decoded = 0;
    if (!r || !acc || !buf || max_events <= 0)
        return -1;

    uint64_t decoded = 0;
    struct pgwt_trace_event *events = NULL;
    int count = 0, capacity = 0;

    /* EVERY block is examined, by its HEADER (#424 both edges, #429).
     * #586 extends the left side: older transition blocks are decoded for
     * seed markers even when no wait in the block overlaps the window.
     *
     * The pattern this replaces — binary-search block_index[] for a start,
     * iterate forward, stop at the first block past `to` — is wrong three
     * times over, and each way is silent:
     *   - block_index[] is in COMMIT order, so the binary search has no
     *     precondition and returns an arbitrary index on a lagging
     *     `--mode full` drain (#429);
     *   - one start index assumes only the immediately preceding block can
     *     reach into the window, but blocks OVERLAP in time, so a block two
     *     or more positions back can still be inside it (#429; this cost
     *     400 ms of 1200 ms of sampled time in one window of the test);
     *   - stopping at `to` discards the straddlers still running there
     *     (#424).
     * A header read costs no decompression. Measured warm over a
     * 1024-block trace (tests/results/issue424-replay/bench-lookahead.log):
     * 0.12 us per block to ask, against 58 us to decode one. So the sound
     * scan is simply to ask every block whether it can matter, and the
     * binary search was never buying much to begin with.
     * There is NO `break` in this loop on purpose:
     * under commit order, a block that cannot matter says nothing about the
     * blocks after it. */
    for (int bi = 0; bi < r->num_blocks; bi++) {
        struct pgwt_block_info hdr;
        int right_open = 0;
        int cand = pgwt_reader_block_candidate(r, bi, 0,
                                               to_mono_ns, &hdr, &right_open);
        if (cand == 0)
            continue;
        /* #586: TRANSITIONS before `from` may contain the command-id
         * markers needed by a left-edge wait. Decode them for seeds only;
         * pgwt_replay_events still applies the original time admission.
         * Samples contain no seed markers and retain the header fast path. */
        if (cand > 0 && hdr.block_type == PGWT_BLOCK_SAMPLES &&
            from_mono_ns && hdr.last_timestamp_ns < from_mono_ns)
            continue;
        /* cand < 0: the header could not be read. NOT treated as "nothing
         * here" — fall through to the decode and let the record rule
         * decide, so a bad header can only cost work, never silently remove
         * time. If the decode fails too it is counted below. */

        if (cand > 0 && right_open) {
            /* Still-running straddlers only (#424). Nothing in the trace
             * bounds how far right the close can be — an io_worker can hold
             * one wait for a whole capture, and the index carries timestamps
             * but no maximum duration — so the scan past `to` is budgeted.
             * When the budget is gone the answer is MARKED
             * (right_edge_truncated) and the scan CONTINUES: skipping this
             * block must not skip the in-window blocks that follow it in
             * commit order. */
            if (lookahead_budget) {
                if (*lookahead_budget <= 0) {
                    if (st) st->right_edge_truncated = 1;
                    continue;
                }
                (*lookahead_budget)--;
            }
        }

        struct pgwt_block_info binfo;
        int n = pgwt_reader_decode_block_info(r, bi, buf, max_events, &binfo);
        if (n <= 0) {
            /* A block that cannot be decoded is time this window will never
             * see. Skipping it quietly (what the old loop did) makes a
             * truncated or corrupt file look like a complete, smaller
             * answer; count it so the caller can say so. */
            if (n < 0 && st) st->blocks_unreadable++;
            continue;
        }

        decoded += (uint64_t)n;
        if (n > INT_MAX - count) goto failed;
        int needed = count + n;
        if (needed > capacity) {
            int next = capacity ? capacity : max_events;
            while (next < needed) {
                if (next > INT_MAX / 2) { next = needed; break; }
                next *= 2;
            }
            if ((size_t)next > SIZE_MAX / sizeof(*events)) goto failed;
            void *grown = realloc(events, (size_t)next * sizeof(*events));
            if (!grown) goto failed;
            events = grown;
            capacity = next;
        }
        /* Preserve sample weights across the merge, including files whose
         * sampling period changes between blocks. No extra record type. */
        if (binfo.block_type == PGWT_BLOCK_SAMPLES)
            for (int i = 0; i < n; i++) buf[i].duration_ns = binfo.sample_period_ns;
        memcpy(events + count, buf, (size_t)n * sizeof(*buf));
        count = needed;
    }

    /* Blocks overlap at their seams and arrive in commit order. Restore
     * stable timestamp order ONCE per file before touching resolver state.
     * Files themselves are visited in wall order by pgwt_run_replay. */
    if (pgwt_stable_sort_events(events, count, NULL) != 0) goto failed;
    pgwt_replay_events(acc, events, count, from_mono_ns, to_mono_ns, NULL, st);
    free(events);
    if (out_decoded) *out_decoded = decoded;
    return 0;
failed:
    fprintf(stderr, "ERROR: cannot buffer/sort replay file %s; refusing replay output\n", r->path);
    free(events);
    if (out_decoded) *out_decoded = decoded;
    return -1;
}
