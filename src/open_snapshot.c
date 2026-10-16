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
    if (d->event_rb) {
        d->event_drain_callbacks_current = 0;
        d->event_drain_callback_limit = PGWT_OPEN_SNAPSHOT_DRAIN_BUDGET;
        int rc = ring_buffer__consume(d->event_rb);
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
