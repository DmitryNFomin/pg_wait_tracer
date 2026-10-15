/* open_interval.h — the ONE arithmetic for a backend's still-open stretch.
 *
 * An open interval (the wait or on-CPU run a backend is inside RIGHT NOW)
 * carries no closing record: the watchpoint only fires at the wait's END. Three
 * producers have to turn that BPF state into a record anyway:
 *
 *   1. the de-escalation flush  (src/escalation.c flush_open_intervals)
 *   2. the shutdown flush       (pgwt_flush_open_intervals, same loop)
 *   3. the live snapshot        ({"cmd":"open_intervals"}, #424)
 *
 * Before #424 the arithmetic existed once, inside (1)/(2). A second copy in (3)
 * is how the two producers drift apart — the live screen would then disagree
 * with the trace it is overlaid on, in a way no single-reader test can see. So
 * the skip rules AND the measured-CPU arithmetic live here, in a pure
 * header-only builder with no daemon, no BPF and no libbpf dependency, and all
 * three callers go through it.
 *
 * Pure by construction: the attribution the builder cannot compute itself
 * (query_id / cmd_open / quality, which need pgwt_exact_resolve_attr and the
 * registry) is an INPUT, resolved by the caller exactly as before. */
#ifndef PGWT_OPEN_INTERVAL_H
#define PGWT_OPEN_INTERVAL_H

#include "pg_wait_tracer.h"

#include <stdbool.h>
#include <stdint.h>

/* Why a pid contributed no record. Every non-BUILT value is a SKIP: the three
 * conditions the flush loop has always applied, named so a caller (and a test)
 * can tell "nothing was open" apart from "something was open and we dropped
 * it". */
enum pgwt_open_stretch_rc {
    PGWT_OPEN_STRETCH_BUILT = 0,
    PGWT_OPEN_STRETCH_SKIP_SEED,      /* wp_live == 0: a query-id/cmd-gate seed,
                                       * NOT interval state (T2 defect 2) */
    PGWT_OPEN_STRETCH_SKIP_NOT_OPEN,  /* now <= last_ts: zero/negative duration.
                                       * Reachable, not theoretical: BPF keeps
                                       * stamping last_ts while userspace
                                       * iterates, so last_ts can be AHEAD of a
                                       * `now` read before the scan (map_reader.h
                                       * pgwt_live_open_ns: measured 3.1 ms of
                                       * skew under load). The bare subtraction
                                       * would underflow to ~584 years. */
    PGWT_OPEN_STRETCH_SKIP_MARKER,    /* last_event is a marker, never a wait */
};

/* Attribution resolved by the caller (pgwt_exact_resolve_attr), passed in so
 * the builder stays pure. */
struct pgwt_open_stretch_attr {
    uint64_t query_id;
    uint16_t cmd_open;      /* instantaneous command gate */
    uint16_t quality;       /* enum pgwt_query_attribution_quality */
};

/* Is there an open stretch to close at all? The builder applies this itself;
 * it is exposed so a caller can skip resolving attribution (two BPF map reads
 * per pid) for a backend that will be skipped anyway — NOT so a caller can
 * reimplement the rule. */
static inline enum pgwt_open_stretch_rc
pgwt_open_stretch_gate(const struct pgwt_pid_state *st, uint64_t now_ns)
{
    if (!st->wp_live)
        return PGWT_OPEN_STRETCH_SKIP_SEED;
    if (now_ns <= st->last_ts)
        return PGWT_OPEN_STRETCH_SKIP_NOT_OPEN;
    if (PGWT_IS_MARKER(st->last_event))
        return PGWT_OPEN_STRETCH_SKIP_MARKER;
    return PGWT_OPEN_STRETCH_BUILT;
}

/* Build the closing record for pid's open stretch at `now_ns`
 * (CLOCK_MONOTONIC == bpf_ktime), or report why there is none.
 *
 * *out is written only on PGWT_OPEN_STRETCH_BUILT. */
static inline enum pgwt_open_stretch_rc
pgwt_build_open_stretch(uint32_t pid, const struct pgwt_pid_state *st,
                        uint64_t now_ns, bool cpu_accounting,
                        const struct pgwt_open_stretch_attr *attr,
                        struct pgwt_trace_event *out)
{
    enum pgwt_open_stretch_rc rc = pgwt_open_stretch_gate(st, now_ns);
    if (rc != PGWT_OPEN_STRETCH_BUILT)
        return rc;
    uint32_t we = st->last_event;

    /* S3: close the open stretch with its exact MEASURED CPU — the straddling
     * command's on-CPU ns the watchpoint never emitted (no wait boundary
     * fired). exact-on-CPU-now = cpu_ns_total + the current open stretch
     * (on_cpu_ts != 0 iff running); subtract the seeded base. */
    uint64_t cpu_ns = PGWT_CPU_NS_UNKNOWN;
    if (cpu_accounting) {
        uint64_t exact = st->cpu_ns_total;
        if (st->on_cpu_ts && now_ns >= st->on_cpu_ts)
            exact += now_ns - st->on_cpu_ts;
        cpu_ns = (exact >= st->last_cpu_ns) ? exact - st->last_cpu_ns : 0;
    }

    struct pgwt_trace_event ev = {
        .timestamp_ns = now_ns,
        .pid          = pid,
        .old_event    = we,     /* the interval that was open ends here */
        .new_event    = we,
        .flags        = (attr->cmd_open ? PGWT_EVENT_FLAG_CMD_OPEN : 0) |
            (attr->quality == PGWT_QUERY_QUALITY_PG13_SYNTH
                ? PGWT_EVENT_FLAG_QUERY_SYNTH : 0),
        .duration_ns  = now_ns - st->last_ts,
        .query_id     = attr->query_id,
        .cpu_ns       = cpu_ns,
    };
    *out = ev;
    return PGWT_OPEN_STRETCH_BUILT;
}

#endif /* PGWT_OPEN_INTERVAL_H */
