/* open_snapshot.c — the live open-interval snapshot behind
 * {"cmd":"open_intervals"} (#424).
 *
 * WHY ITS OWN TRANSLATION UNIT. This is the daemon-side twin of
 * pgwt_read_state_map and lived next to it at first, which cost
 * map_reader.o a link-time dependency on pgwt_exact_resolve_attr — and
 * through it on exact_probe.o, query_text.o and cmdline.o. Two BPF-free unit
 * tests (test_event_reader, test_concurrent_rw) link map_reader.o for the
 * accumulator core alone and failed to link on Linux because of it, which is
 * a whole box-check lost to a slice of the daemon that those tests have no
 * business pulling in.
 *
 * Splitting it out is the fix that does not grow: the ATTRIBUTION lives on
 * this side of the line, the accumulator core stays linkable on its own, and
 * tests/Makefile needs no change at all (four branches are editing that file
 * right now). tests/results/issue424-daemon/link-closure-check.sh pins it:
 * map_reader.o must reference no symbol it did not reference at the base
 * commit.
 */
#include "map_reader.h"
#include "wait_event.h"
#include "open_interval.h"
#include "open_snapshot.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#ifndef PGWT_SERVER
#include "daemon.h"
#include "exact_probe.h"
#include "event_writer.h"
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

/* Must match bpf_ktime_get_ns() clock source (CLOCK_MONOTONIC). */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ── The snapshot ───────────────────────────────────────────────────────
 *
 * The producer behind {"cmd":"open_intervals"}. It READS: it writes nothing to
 * the trace and synthesizes no close. (The de-escalation/shutdown flush does
 * persist a synthetic close WITHOUT advancing BPF state, so the eventual real
 * close would overlap time already credited — which is why this is a separate
 * loop over the SAME shared builder rather than a call into that flush.)
 *
 * Ordering, deliberately: the state map is read FIRST, then the ring is
 * drained and the writer published. So committed_through_ns >= snapshot_ns,
 * and a wait that closes in between is BOTH published and still shown open
 * here — which the consumer's uniform precedence rule resolves (an admitted
 * record with ts > open_start drops the span). The other order would leave a
 * hole instead: the close unpublished AND the interval reported from its new,
 * later start, with the stretch between belonging to neither. An
 * over-reported-and-dropped span is recoverable; a hole is not.
 */
#define PGWT_OPEN_SNAPSHOT_DRAIN_BUDGET 65536U

static uint64_t wall_ns_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void pgwt_collect_open_intervals(struct pgwt_daemon *d,
                                 struct pgwt_open_snapshot *s)
{
    memset(s, 0, sizeof(*s));
    s->status = PGWT_OPEN_SNAP_UNAVAILABLE;
    s->complete = false;
    s->daemon_pid = (uint32_t)getpid();
    s->cpu_accounting = d->cpu_accounting;
    s->cmd_gate_active = d->cmd_gate_active;
    s->daemon_generation = d->escalation.generation;

    /* The (now_mono, now_wall) anchor, read back to back. Every mono→wall
     * conversion downstream uses THIS pair — never a per-file offset. */
    s->now_mono_ns = now_ns();
    s->now_wall_ns = wall_ns_now();
    if (s->now_mono_ns >= d->start_ts)
        s->daemon_start_wall_ns = pgwt_open_snapshot_wall_of(s, d->start_ts);

    /* Tier: refuse where no watchpoint interval can exist, with a reason
     * specific enough to render as a UI label. An empty success here would be
     * a lie — "nothing is waiting" rather than "this tier cannot tell you". */
    if (d->lightweight_mode) {
        snprintf(s->tier, sizeof(s->tier), "lightweight");
        s->status = PGWT_OPEN_SNAP_REFUSED;
        pgwt_open_snapshot_set_reason(s,
            "lightweight tier: BPF accumulator only, no per-backend open "
            "intervals exist");
        return;
    }
    snprintf(s->tier, sizeof(s->tier), "%s",
             pgwt_mode_uses_watchpoints(d) ? "escalated" : "sampled");
    if (!pgwt_mode_uses_watchpoints(d)) {
        s->status = PGWT_OPEN_SNAP_REFUSED;
        pgwt_open_snapshot_set_reason(s,
            d->mode == PGWT_MODE_TIERED
            ? "sampled tier: no watchpoints armed; escalate for in-flight waits"
            : "sampled tier: this capture mode has no watchpoint intervals");
        return;
    }
    if (!d->skel) {
        pgwt_open_snapshot_set_reason(s, "BPF skeleton unavailable");
        return;
    }
    int state_fd = bpf_map__fd(d->skel->maps.state_map);
    if (state_fd < 0) {
        pgwt_open_snapshot_set_reason(s, "state_map fd unavailable");
        return;
    }

    uint64_t snap = now_ns();
    s->snapshot_ns = snap;
    s->snapshot_wall_ns = pgwt_open_snapshot_wall_of(s, snap);

    /* Registry order, exactly like the de-escalation flush: the registry is
     * what carries the incarnation (attach_ts) and the category. */
    for (int i = 0; i < d->backends.count && s->n_rows < MAX_BACKENDS; i++) {
        struct pgwt_backend *be = &d->backends.entries[i];
        if (!be->is_alive || be->pid <= 0 || be->wp_fd < 0)
            continue;
        uint32_t key = (uint32_t)be->pid;
        struct pgwt_pid_state st;
        if (bpf_map_lookup_elem(state_fd, &key, &st) != 0)
            continue;
        if (pgwt_open_stretch_gate(&st, snap) != PGWT_OPEN_STRETCH_BUILT)
            continue;
        uint64_t query_id = 0;
        uint16_t cmd_open = 0, quality = PGWT_QUERY_QUALITY_NONE;
        pgwt_exact_resolve_attr(d, be->pid, &query_id, &cmd_open,
                                &quality, NULL, NULL, NULL);
        struct pgwt_open_stretch_attr attr = {
            .query_id = query_id, .cmd_open = cmd_open, .quality = quality,
        };
        struct pgwt_trace_event ev;
        if (pgwt_build_open_stretch(key, &st, snap, d->cpu_accounting,
                                    &attr, &ev) != PGWT_OPEN_STRETCH_BUILT)
            continue;

        /* Command classification inputs. PEEK, never consume: the closing
         * record consumes the same banked time later, and a snapshot that
         * advanced the sweep would silently change what the live view counts.
         * pa is LOOKED UP, not created — a read must not grow the
         * accumulator. */
        struct pgwt_pid_accum *pa = pgwt_find_pid_accum(&d->accum, key);
        bool in_cmd = false;
        bool by_markers = pgwt_live_cmd_gate_classify(
            pa ? &pa->cmd_gate : NULL, false, st.last_ts, snap,
            st.cmd_open != 0, &in_cmd);
        uint64_t cmd_ns = pa
            ? pgwt_live_cmd_gate_peek(&pa->cmd_gate, st.last_ts, snap) : 0;
        /* The RAW sweep state as well as the verdict: the server half reduces
         * it with its own arithmetic, and handing it only a verdict would
         * force it to keep a second copy of the peek. When this pid has no
         * per-pid state the three fields are NOT measurements, which is what
         * cmd_gate_present says. */
        const struct pgwt_live_cmd_gate *g = pa ? &pa->cmd_gate : NULL;
        /* cat_flag with pa = NULL: resolved from the registry, cached
         * nowhere — the same call the flush makes, and it keeps this read
         * free of side effects on the live accumulator. */
        uint32_t cat = pgwt_live_pid_cat_flag(d, NULL, key);

        struct pgwt_open_row *r = &s->rows[s->n_rows++];
        r->pid            = key;
        r->incarnation    = be->attach_ts;
        r->generation     = d->escalation.generation;
        r->event_id       = ev.old_event;
        r->cat_flag       = cat;
        r->eff_event      = pgwt_live_effective_event(ev.old_event, cat,
                                                      d->cmd_gate_active,
                                                      in_cmd);
        r->flags          = ev.flags;
        r->open_start_ns  = st.last_ts;
        r->duration_ns    = ev.duration_ns;
        r->cpu_ns         = ev.cpu_ns;
        r->cpu_ns_total   = st.cpu_ns_total;
        r->on_cpu_ts      = st.on_cpu_ts;
        r->last_cpu_ns    = st.last_cpu_ns;
        r->query_id       = ev.query_id;
        r->query_quality  = quality;
        /* The wait CLASS as its own field, resolved with the same function the
         * server emits "class" from — derivable is not the same as present
         * for a consumer that refuses on absence. */
        snprintf(r->category, sizeof(r->category), "%s",
                 pgwt_class_name(ev.old_event));
        r->category_id    = ev.old_event ? WE_CLASS(ev.old_event) : 0;
        r->cmd_open_ns    = cmd_ns;
        r->cmd_banked_ns  = g ? g->banked_ns : 0;
        r->cmd_anchor_ns  = g ? g->anchor_ns : 0;
        r->cmd_open       = st.cmd_open != 0;
        r->cmd_sweep_open = g && g->open != 0;
        r->cmd_seen       = g && g->seen != 0;
        r->cmd_gate_present = g != NULL;
        r->cmd_in_window  = in_cmd;
        r->cmd_by_markers = by_markers;
        r->query_synth    = (ev.flags & PGWT_EVENT_FLAG_QUERY_SYNTH) != 0;
    }
    s->open_count = s->n_rows;

    /* Publication coordination. An open-only answer is insufficient: the
     * consumer has to know how far the CLOSED prefix reaches, or it cannot
     * tell "still open" from "closed but not yet visible". Bounded: the drain
     * stops after a fixed number of callbacks and SAYS SO rather than
     * spinning while a producer keeps refilling the ring. */
    uint64_t publish_from = now_ns();
    const char *degraded = NULL;
    if (d->event_rb || d->exact_perf) {
        d->event_drain_callbacks_current = 0;
        d->event_drain_callback_limit = PGWT_OPEN_SNAPSHOT_DRAIN_BUDGET;
        int rc = pgwt_consume_event_ring(d, true);
        bool yielded = d->event_drain_callbacks_current
                       >= PGWT_OPEN_SNAPSHOT_DRAIN_BUDGET && rc < 0;
        d->event_drain_callback_limit = 0;
        if (yielded)
            degraded = "event ring still backlogged after the bounded drain; "
                       "committed_through_ns is a floor";
    }
    if (!d->event_writer) {
        /* No trace, no publication point: the consumer has nothing to
         * reconcile these spans against, so this is unavailable rather than a
         * success with an unverifiable committed_through_ns. */
        s->status = PGWT_OPEN_SNAP_UNAVAILABLE;
        pgwt_open_snapshot_set_reason(s,
            "trace recording disabled: no publication point to reconcile "
            "against");
        s->n_rows = 0;
        return;
    }
    if (pgwt_writer_publish(d->event_writer) != 0)
        degraded = "trace publish failed; closed records may be unflushed";

    /* The honest watermark: when the bounded drain completed AND the writer
     * flushed, every record produced before `publish_from` is in the file, so
     * that instant is the commit point. When either step degraded, fall back
     * to the largest timestamp actually written — a floor, never a claim. */
    s->committed_through_ns = degraded
        ? pgwt_writer_committed_through_ns(d->event_writer)
        : publish_from;
    if (degraded) {
        /* The rows themselves are fine, but the CLOSED prefix they must be
         * reconciled against is not known to the needed precision — and a
         * consumer handed spans plus complete:false has to decide what that
         * means. It does not get to decide: the invariant is
         * `pids present == status included == complete`, so a degraded answer
         * drops the rows and the pane is labelled. Under-reporting with a
         * visible label beats a number nobody can audit. */
        s->status = PGWT_OPEN_SNAP_UNAVAILABLE;
        s->complete = false;
        s->n_rows = 0;
        pgwt_open_snapshot_set_reason(s, degraded);
        return;
    }
    s->status = PGWT_OPEN_SNAP_INCLUDED;
    s->complete = true;
}

#endif /* !PGWT_SERVER */

/* ── #947 coverage watermark: the pure half ─────────────────────────────
 *
 * Built on every host (no BPF, no daemon), so the rules that decide what a
 * WATERMARK says are unit-tested against the real writer on a Mac:
 * tests/test_coverage_writer.c. The daemon half below only gathers inputs. */
#include "event_writer.h"
#include "coverage_record.h"
#include "idle_rule.h"

void pgwt_cov_floors_begin(struct pgwt_cov_floors *f, uint64_t snap_ns)
{
    f->snap_ns = snap_ns;
    f->floor_db_ns = snap_ns;
    f->floor_idle_ns = snap_ns;
}

void pgwt_cov_floors_add(struct pgwt_cov_floors *f, bool readable,
                         uint64_t last_ts_ns, uint32_t last_event,
                         bool cmd_open, uint64_t unreadable_bound_ns)
{
    if (!readable) {
        /* Fail closed: nothing is known about this backend's open wait
         * except that it cannot have started before the bound. */
        if (unreadable_bound_ns < f->floor_db_ns)
            f->floor_db_ns = unreadable_bound_ns;
        if (unreadable_bound_ns < f->floor_idle_ns)
            f->floor_idle_ns = unreadable_bound_ns;
        return;
    }
    /* A transition at or after the snapshot opened nothing before it. */
    uint64_t start = last_ts_ns < f->snap_ns ? last_ts_ns : f->snap_ns;
    if (start < f->floor_idle_ns)
        f->floor_idle_ns = start;
    bool idle = pgwt_is_idle_event(last_event) && !cmd_open;
    if (!idle && start < f->floor_db_ns)
        f->floor_db_ns = start;
}

enum pgwt_cov_cv_state
pgwt_cov_cv_compare(const struct pgwt_counter_vector *base,
                    const struct pgwt_counter_vector *cur)
{
    if (!cur || !cur->valid)
        return PGWT_COV_CV_UNKNOWN;
    for (int i = 0; i < PGWT_COUNTER_COMPONENTS; i++)
        if (!cur->components[i].valid)
            return PGWT_COV_CV_UNKNOWN;
    if (!base)
        return PGWT_COV_CV_CHANGED;
    for (int i = 0; i < PGWT_COUNTER_COMPONENTS; i++)
        if (cur->components[i].value != base->components[i].value)
            return PGWT_COV_CV_CHANGED;
    return PGWT_COV_CV_UNCHANGED;
}

void pgwt_cov_collect_tick(struct pgwt_cov_tick_in *in,
                           struct pgwt_counter_vector *cv, void *ctx,
                           const struct pgwt_cov_tick_ops *ops)
{
    *cv = ops->counters(ctx);
    in->cv = cv;
    in->publish_from_ns = ops->clock(ctx);
    ops->drain_publish(ctx, in);
    in->observation_ns = ops->clock(ctx);
}

void pgwt_cov_tracker_init(struct pgwt_cov_tracker *t, uint64_t boot_start_ns,
                           bool watermarks,
                           const struct pgwt_counter_vector *base)
{
    memset(t, 0, sizeof(*t));
    t->watermarks = watermarks;
    t->boot_start_ns = boot_start_ns;
    t->w_ns = t->fd_ns = t->fi_ns = boot_start_ns;
    if (base && pgwt_cov_cv_compare(NULL, base) != PGWT_COV_CV_UNKNOWN) {
        t->base = *base;
        t->base_known = true;
    }
}

static int cov_gap(struct pgwt_cov_tracker *t, struct pgwt_event_writer *w,
                   uint64_t obs, bool open, uint64_t at, uint8_t reason)
{
    if (open) {
        t->gap_id++;
        t->gaps++;
    }
    int rc = pgwt_writer_coverage_gap(w, obs, open, t->gap_id, at, reason);
    if (rc != 0)
        t->write_failures++;
    return rc;
}

/* The counter-vector half of a tick. Returns true when this tick may be a
 * good one. A loss is a gap from the LAST WRITTEN W: nothing at or after it
 * has been vouched for yet, and the reader derives the taint from that
 * watermark's floor (C6). */
static bool cov_counters(struct pgwt_cov_tracker *t,
                         struct pgwt_event_writer *w,
                         const struct pgwt_cov_tick_in *in, int *rc)
{
    enum pgwt_cov_cv_state cs =
        pgwt_cov_cv_compare(t->base_known ? &t->base : NULL, in->cv);
    if (cs == PGWT_COV_CV_UNKNOWN) {
        /* Unknown for as long as it lasts: ONE open gap, closed when the
         * counters can be read again. */
        t->base_known = false;
        if (!t->gap_open) {
            if (cov_gap(t, w, in->observation_ns, true, t->w_ns,
                        PGWT_COVR_COUNTERS_UNKNOWN) != 0)
                *rc = -1;
            else
                t->gap_open = true;
        }
        return false;
    }
    if (!t->base_known) {
        /* Known again (or for the first time): whatever happened while it
         * was not is a loss we cannot rule out. */
        if (!t->gap_open &&
            cov_gap(t, w, in->observation_ns, true, t->w_ns,
                    PGWT_COVR_COUNTERS_UNKNOWN) != 0) {
            *rc = -1;
            return false;
        }
        if (cov_gap(t, w, in->observation_ns, false, in->publish_from_ns,
                    0) != 0)
            *rc = -1;
        t->gap_open = false;
        t->base = *in->cv;
        t->base_known = true;
        return false;
    }
    if (cs == PGWT_COV_CV_CHANGED) {
        if (cov_gap(t, w, in->observation_ns, true, t->w_ns,
                    PGWT_COVR_DROP) != 0 ||
            cov_gap(t, w, in->observation_ns, false, in->publish_from_ns,
                    0) != 0)
            *rc = -1;
        t->base = *in->cv;
        return false;
    }
    return true;
}

static int cov_write_wm(struct pgwt_cov_tracker *t,
                        struct pgwt_event_writer *w, uint64_t obs)
{
    int rc = pgwt_writer_coverage_watermark(w, obs, t->w_ns, t->fd_ns,
                                            t->fi_ns);
    if (rc != 0)
        t->write_failures++;
    return rc;
}

/* Promote the pending triple to "written". */
static void cov_take_pending(struct pgwt_cov_tracker *t)
{
    /* W never goes backwards; floors stay at or below it. */
    if (t->p_ns > t->w_ns)
        t->w_ns = t->p_ns;
    t->fd_ns = t->pfd_ns > t->w_ns ? t->w_ns : t->pfd_ns;
    t->fi_ns = t->pfi_ns > t->fd_ns ? t->fd_ns : t->pfi_ns;
    t->has_pending = false;
}

static void cov_set_pending(struct pgwt_cov_tracker *t,
                            const struct pgwt_cov_tick_in *in)
{
    uint64_t p = in->publish_from_ns;
    uint64_t fd = in->floors_known ? in->floor_db_ns : p;
    uint64_t fi = in->floors_known ? in->floor_idle_ns : p;
    /* Clamp into [boot_start, P]: a floor before the boot began means the
     * same to the verdict as the boot's start, and keeps the measured lag
     * readable. */
    if (fd > p) fd = p;
    if (fi > fd) fi = fd;
    if (fd < t->boot_start_ns) fd = t->boot_start_ns;
    if (fi < t->boot_start_ns) fi = t->boot_start_ns;
    t->p_ns = p;
    t->pfd_ns = fd;
    t->pfi_ns = fi;
    t->has_pending = true;
}

int pgwt_cov_tracker_tick(struct pgwt_cov_tracker *t,
                          struct pgwt_event_writer *w,
                          const struct pgwt_cov_tick_in *in)
{
    if (!t->watermarks || !w)
        return 0;
    int rc = 0;
    t->ticks++;
    bool good = cov_counters(t, w, in, &rc) &&
                in->drain_complete && in->publish_ok;
    if (good) {
        t->good_ticks++;
        if (t->has_pending)
            cov_take_pending(t);
        else
            t->heartbeats++;
        cov_set_pending(t, in);
    } else {
        t->heartbeats++;
    }
    if (cov_write_wm(t, w, in->observation_ns) != 0)
        rc = -1;
    return rc;
}

int pgwt_cov_tracker_seal(struct pgwt_cov_tracker *t,
                          struct pgwt_event_writer *w,
                          const struct pgwt_cov_tick_in *in)
{
    if (!w)
        return -1;
    int rc = 0;
    /* A session that never wrote a block has no boot on disk to seal, and
     * one without coverage enabled has no boot at all. */
    if (!w->cov_enabled ||
        (!w->fp && w->cov_next_file_no == 0 && !t->watermarks))
        return 0;
    if (t->watermarks) {
        t->ticks++;
        bool good = cov_counters(t, w, in, &rc) &&
                    in->drain_complete && in->publish_ok;
        if (good && t->has_pending) {
            t->good_ticks++;
            cov_take_pending(t);
        } else {
            t->heartbeats++;
        }
        if (cov_write_wm(t, w, in->observation_ns) != 0)
            rc = -1;
    }
    if (pgwt_writer_coverage_seal(w, in->observation_ns, t->w_ns) != 0) {
        t->write_failures++;
        rc = -1;
    }
    return rc;
}

#ifndef PGWT_SERVER
#include "capture_counters.h"
/* ── #947 coverage watermark: the daemon half ─────────────────────────── */

/* PostgreSQL's postmaster start, wall ns: /proc/<pid>/stat starttime (clock
 * ticks since kernel boot) + /proc/stat btime. Stored only so a reader can
 * tell a PostgreSQL restart from a daemon restart; 0 = unknown. */
static uint64_t cov_postmaster_start_wall_ns(pid_t pid)
{
    if (pid <= 0)
        return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *p = strrchr(buf, ')');       /* comm may contain spaces */
    if (!p)
        return 0;
    p++;
    unsigned long long start_ticks = 0;
    /* After ")": field 3 (state) ... field 22 (starttime). */
    int field = 2;
    char *tok = strtok(p, " ");
    while (tok) {
        field++;
        if (field == 22) {
            start_ticks = strtoull(tok, NULL, 10);
            break;
        }
        tok = strtok(NULL, " ");
    }
    if (field != 22)
        return 0;
    unsigned long long btime = 0;
    f = fopen("/proc/stat", "r");
    if (!f)
        return 0;
    char line[256];
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "btime %llu", &btime) == 1)
            break;
    fclose(f);
    long hz = sysconf(_SC_CLK_TCK);
    if (btime == 0 || hz <= 0)
        return 0;
    return (uint64_t)btime * 1000000000ULL
         + (uint64_t)start_ticks * (1000000000ULL / (uint64_t)hz);
}

void pgwt_coverage_start(struct pgwt_daemon *d,
                          const struct pgwt_counter_vector *base)
{
    if (!d->event_writer)
        return;
    struct pgwt_event_writer *w = d->event_writer;
    uint64_t mode = (uint64_t)d->mode & 0xFFu;
    if (d->lightweight_mode)
        mode |= PGWT_COVERAGE_MODE_LIGHTWEIGHT;
    if (d->exact_perf)
        mode |= PGWT_COVERAGE_MODE_EXACT_PERF;
    /* Watermarks on the master path only. Lightweight has no records to
     * vouch for (not_captured, fail closed); the exact per-CPU transport
     * gets its own W from the merge frontier in the transport PR. */
    bool watermarks = !d->lightweight_mode && !d->exact_perf;
    if (w->fp) {
        /* A file opened before capture start already carries a BOOT whose
         * boot_start predates it. Vouch for nothing in this boot rather than
         * for a stretch the capture was not yet watching. */
        fprintf(stderr, "WARN: coverage: the trace file opened before "
                "capture start; this run writes no coverage watermarks\n");
        watermarks = false;
    } else {
        pgwt_writer_coverage_configure(w, d->start_ts,
            cov_postmaster_start_wall_ns(d->postmaster_pid), mode);
    }
    pgwt_cov_tracker_init(&d->coverage, d->start_ts, watermarks, base);
    d->coverage_started = true;
}

static uint64_t cov_thread_cpu_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* TEST ONLY: PGWT_TEST_COVERAGE_YIELD=1 makes every coverage drain report a
 * yield, so the box test can watch the heartbeat path on a real daemon. */
static bool cov_test_force_yield(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PGWT_TEST_COVERAGE_YIELD");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v == 1;
}

static struct pgwt_counter_vector cov_read_counters(void *ctx)
{
    return pgwt_daemon_counter_vector(ctx);
}

static uint64_t cov_clock(void *ctx)
{
    (void)ctx;
    return now_ns();
}

static void cov_drain_publish(void *ctx, struct pgwt_cov_tick_in *in)
{
    struct pgwt_daemon *d = ctx;
    in->drain_complete = true;
    uint64_t drain_cpu0 = cov_thread_cpu_ns();
    if (d->event_rb) {
        d->event_drain_callbacks_current = 0;
        d->event_drain_callback_limit = PGWT_OPEN_SNAPSHOT_DRAIN_BUDGET;
        int rc = pgwt_consume_event_ring(d, true);
        bool yielded = d->event_drain_callbacks_current
                       >= PGWT_OPEN_SNAPSHOT_DRAIN_BUDGET && rc < 0;
        d->event_drain_callback_limit = 0;
        in->drain_complete = !yielded;
    }
    d->coverage.drain_cpu_ns += cov_thread_cpu_ns() - drain_cpu0;
    if (cov_test_force_yield())
        in->drain_complete = false;
    /* A publish with nothing appended since the last one writes nothing:
     * the writer's buffers are empty and both flushes return at once. */
    in->publish_ok = pgwt_writer_publish(d->event_writer) == 0;
}

static void cov_gather(struct pgwt_daemon *d, struct pgwt_cov_tick_in *in,
                       struct pgwt_counter_vector *cv)
{
    memset(in, 0, sizeof(*in));
    in->floors_known = false;
    bool wp = pgwt_mode_uses_watchpoints(d);
    int state_fd = (wp && d->skel)
                 ? bpf_map__fd(d->skel->maps.state_map) : -1;
    if (wp) {
        /* Floors FIRST, then the drain (open_snapshot order): a wait open at
         * the snapshot started at or after its floor. */
        struct pgwt_cov_floors f;
        pgwt_cov_floors_begin(&f, now_ns());
        for (int i = 0; i < d->backends.count; i++) {
            struct pgwt_backend *be = &d->backends.entries[i];
            if (!be->is_alive || be->pid <= 0 || be->wp_fd < 0)
                continue;
            uint32_t key = (uint32_t)be->pid;
            struct pgwt_pid_state st;
            bool readable = state_fd >= 0 &&
                bpf_map_lookup_elem(state_fd, &key, &st) == 0 &&
                st.wp_live;
            pgwt_cov_floors_add(&f, readable, readable ? st.last_ts : 0,
                                readable ? st.last_event : 0,
                                readable && st.cmd_open != 0,
                                be->attach_ts);
        }
        in->floors_known = true;
        in->floor_db_ns = f.floor_db_ns;
        in->floor_idle_ns = f.floor_idle_ns;
    }
    const struct pgwt_cov_tick_ops ops = {
        .counters = cov_read_counters,
        .clock = cov_clock,
        .drain_publish = cov_drain_publish,
    };
    pgwt_cov_collect_tick(in, cv, d, &ops);
}

void pgwt_coverage_tick(struct pgwt_daemon *d)
{
    uint64_t expirations;
    if (d->coverage_timer_armed &&
        read(d->coverage_timer_fd, &expirations, sizeof(expirations)) < 0 &&
        errno != EAGAIN)
        return;
    if (!d->coverage_started || !d->event_writer || !d->coverage.watermarks)
        return;
    uint64_t cpu0 = cov_thread_cpu_ns();
    struct pgwt_cov_tick_in in;
    struct pgwt_counter_vector cv;
    cov_gather(d, &in, &cv);
    pgwt_cov_tracker_tick(&d->coverage, d->event_writer, &in);
    d->coverage.tick_cpu_ns += cov_thread_cpu_ns() - cpu0;
}

void pgwt_coverage_shutdown(struct pgwt_daemon *d)
{
    if (!d->coverage_started || !d->event_writer)
        return;
    d->coverage_started = false;
    struct pgwt_cov_tick_in in = {0};
    /* The run loop's final drain was unbounded and is complete; the
     * open-interval flush has already pushed its closing records. */
    /* Frozen BEFORE final_drain_from, just as in a periodic tick. Taking a
     * fresh baseline here would absorb losses during the final drain/flush
     * into a gap ending before those losses. The conservative pending W
     * leaves that tail uncovered. */
    in.cv = &d->final_drain_counters;
    in.publish_from_ns = d->final_drain_from_ns;
    in.drain_complete = true;
    in.publish_ok = pgwt_writer_publish(d->event_writer) == 0;
    in.observation_ns = now_ns();
    if (pgwt_cov_tracker_seal(&d->coverage, d->event_writer, &in) != 0)
        fprintf(stderr, "WARN: coverage: the SEAL record was not written "
                "durably — this run will read as stopped uncleanly\n");
    const struct pgwt_cov_tracker *t = &d->coverage;
    /* One line per run: what the box test reads for the cost budget. */
    fprintf(stderr, "INFO: coverage: ticks=%llu good=%llu heartbeats=%llu "
            "gaps=%llu write_failures=%llu records=%llu bytes=%llu "
            "tick_cpu_ns=%llu drain_cpu_ns=%llu W_final=%llu\n",
            (unsigned long long)t->ticks, (unsigned long long)t->good_ticks,
            (unsigned long long)t->heartbeats, (unsigned long long)t->gaps,
            (unsigned long long)t->write_failures,
            (unsigned long long)d->event_writer->cov_records_written,
            (unsigned long long)d->event_writer->cov_bytes_written,
            (unsigned long long)t->tick_cpu_ns,
            (unsigned long long)t->drain_cpu_ns,
            (unsigned long long)t->w_ns);
}
#endif /* !PGWT_SERVER */
