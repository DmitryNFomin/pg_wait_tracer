/* cmd_gate.h — the command-window gate: ONE rule for splitting a backend's
 * on-CPU (we==0) interval against PostgreSQL's own statement boundaries.
 * Header-only, pure: no daemon, no BPF, no allocation (src/query_attr.h is
 * the precedent for a rule shared between the live and offline paths).
 *
 * THE DEFECT (issue #115). PostgreSQL's statement boundaries arrive as
 * CMD_START/CMD_END markers from the on_report_activity uprobe — the same
 * pgstat_report_activity() call that drives pg_stat_activity.state, so the
 * marker window is PostgreSQL's own "active" window (measured: 273.630 s of
 * markers against 273.603 s of kernel-observed in-statement time, 0.05 us
 * per command). An exact-tier we==0 interval, though, is the gap between
 * two wait events, and one such gap routinely STRADDLES a boundary: a
 * statement's trailing on-CPU run continues past CMD_END into the protocol
 * work before the next ClientRead.
 *
 * The old rule credited a straddling interval all-or-nothing by majority —
 * more than half inside a command and ALL of it became CPU*, otherwise all
 * of it became PGWT_WEI_NONCMD_CPU. The error is per boundary, so its size
 * tracks commands per backend-second: measured on TPC-B, +3.73 pp of the
 * CPU share at 2 clients and -0.40 pp at 8 (low concurrency is the worse
 * case — each backend runs more statements per second of its own wall).
 *
 * THE RULE. An interval is credited PROPORTIONALLY: the wall it actually
 * spends inside a command window is CPU*, the remainder is non-command
 * time. Everything else is unchanged:
 *
 *   - a pid with no CMD marker yet (`seen` == 0) keeps we==0 as CPU*
 *     (the server's seen_cmd rule — legacy traces, background processes);
 *   - no per-pid state at all (accumulator full) falls back to the
 *     emission-time gate the record carries;
 *   - a degenerate (empty) interval takes the instantaneous gate and is
 *     credited nothing; see pgwt_cmd_gate_take for the one limitation this
 *     rule still shares with compute.c.
 *
 * WHY A SINGLE-PASS SWEEP IS ENOUGH. The banked/anchor sweep below computes
 * the same overlap as intersecting the interval against a per-pid,
 * timestamp-sorted list of command intervals, because a pid's exact records
 * tile its timeline contiguously and arrive in close-time order. Verified
 * on live traces: the two agree to 0.01 s over a 60 s, 3-million-record
 * window (issue #115, "the sweep is not the cause"). Banked time that
 * predates the interval (state_map's last_ts running ahead of the last
 * drained record) is clipped to the interval's own wall, so no interval can
 * ever be credited more than it lasted.
 *
 * CONSUMERS. The live paths (src/event_stream.c closed records,
 * src/map_reader.c's open state_map stretch) split the interval into two
 * rows; tests/cross_validate.c credits the in-command fraction.
 * src/compute.c pgwt_tag_events keeps its own copy of this sweep and still
 * applies the MAJORITY verdict (pgwt_cmd_split_majority) — it annotates a
 * fixed event array in place, so one record cannot become two there; going
 * proportional on the server's raw path is tracked separately. Until then
 * the two agree on the overlap and differ only in the verdict, which
 * tests/test_live_accum.c's cross-check pins.
 */
#ifndef PGWT_CMD_GATE_H
#define PGWT_CMD_GATE_H

#include "pg_wait_tracer.h"

#include <stdint.h>
#include <stdbool.h>

/* Per-pid sweep state. Zero-initialised = fresh (unmarked pid). */
struct pgwt_cmd_gate {
    uint8_t  seen;        /* a CMD marker was seen for this pid */
    uint8_t  open;        /* inside a command window at the sweep point */
    uint64_t anchor_ns;   /* start of the current open run, or the last record end */
    uint64_t banked_ns;   /* closed command-open ns since the last consumed record */
};

/* How one interval's wall time divides at the command boundary. */
struct pgwt_cmd_split {
    uint64_t in_ns;     /* wall inside a command window */
    uint64_t out_ns;    /* wall outside it; in_ns + out_ns == the interval's wall */
    bool     gated;     /* the pid's markers decided it (else: a fallback) */
    bool     in_cmd;    /* single-row verdict for callers that cannot split */
    bool     open_now;  /* gate state at the sweep point (zero-length tie-break) */
};

/* Feed one CMD_START/CMD_END marker (timestamp = the boundary instant).
 * Other markers are ignored. An out-of-order CMD_END (ts before the open
 * run's anchor) banks nothing rather than underflowing the counter into a
 * multi-century "command" — the pre-#115 copy in tests/cross_validate.c did
 * exactly that. */
static inline void pgwt_cmd_gate_marker(struct pgwt_cmd_gate *g,
                                        uint32_t marker, uint64_t ts_ns)
{
    switch (marker) {
    case PGWT_MARKER_CMD_START:
        g->seen = 1;
        if (!g->open) {
            g->open = 1;
            g->anchor_ns = ts_ns;
        }
        break;
    case PGWT_MARKER_CMD_END:
        g->seen = 1;
        if (g->open) {
            if (ts_ns > g->anchor_ns)
                g->banked_ns += ts_ns - g->anchor_ns;
            g->open = 0;
        }
        break;
    default:
        break;   /* plan/exec/escalation markers: not a command boundary */
    }
}

/* Raw command-open ns of [t0, t1): banked closed runs plus the currently
 * open run clipped to the interval. May exceed the interval's wall when
 * banked time predates t0; the callers below clip. */
static inline uint64_t pgwt_cmd_gate_raw_ns(const struct pgwt_cmd_gate *g,
                                            uint64_t t0_ns, uint64_t t1_ns)
{
    uint64_t open_ns = g->banked_ns;
    if (g->open) {
        uint64_t a = g->anchor_ns > t0_ns ? g->anchor_ns : t0_ns;
        if (t1_ns > a)
            open_ns += t1_ns - a;
    }
    return open_ns;
}

/* Command-open ns of the closed record [t0, t1), clipped to its wall, and
 * ADVANCE the sweep to t1 (a pid's exact records tile contiguously, so
 * banked time since the previous record belongs to this one).
 *
 * KNOWN SHARED LIMITATION, pinned by tests/test_cmd_gate.c: a DEGENERATE
 * record — zero-length, or one whose duration runs past its own timestamp
 * so [t0, t1) is empty — still consumes the pid's banked command time and
 * credits it nowhere, because it has no wall to put it in. Not consuming
 * would be better, but this state machine is byte-for-byte the one in
 * compute.c pgwt_tag_events and tests/test_live_accum.c diffs the two on
 * one stream; changing it here alone would split them. It moves with
 * compute.c when the server path goes proportional. */
static inline uint64_t pgwt_cmd_gate_take(struct pgwt_cmd_gate *g,
                                          uint64_t t0_ns, uint64_t t1_ns)
{
    if (!g->seen)
        return 0;
    uint64_t wall = t1_ns > t0_ns ? t1_ns - t0_ns : 0;
    uint64_t open_ns = pgwt_cmd_gate_raw_ns(g, t0_ns, t1_ns);
    g->banked_ns = 0;
    if (g->open)
        g->anchor_ns = t1_ns;
    /* The clip cannot change the majority verdict (open_ns > wall already
     * implies open_ns * 2 >= wall); it keeps out_ns from underflowing. */
    return open_ns < wall ? open_ns : wall;
}

/* Same quantity for the still-OPEN stretch [t0, now) at tick time, WITHOUT
 * advancing (its closing record consumes it later). */
static inline uint64_t pgwt_cmd_gate_peek(const struct pgwt_cmd_gate *g,
                                          uint64_t t0_ns, uint64_t now_ns)
{
    if (!g->seen)
        return 0;
    uint64_t wall = now_ns > t0_ns ? now_ns - t0_ns : 0;
    uint64_t open_ns = pgwt_cmd_gate_raw_ns(g, t0_ns, now_ns);
    return open_ns < wall ? open_ns : wall;
}

/* Split [t0, t1) at the command boundary. `g` NULL = no per-pid state
 * (accumulator full): fall back to the emission-time gate `fallback_open`.
 * `consume` = closed record (advance the sweep); false = open stretch. */
static inline struct pgwt_cmd_split
pgwt_cmd_gate_split(struct pgwt_cmd_gate *g, bool consume,
                    uint64_t t0_ns, uint64_t t1_ns, bool fallback_open)
{
    uint64_t wall = t1_ns > t0_ns ? t1_ns - t0_ns : 0;
    struct pgwt_cmd_split s = { 0, 0, false, false, false };

    if (!g) {
        s.in_cmd = s.open_now = fallback_open;
        if (fallback_open) s.in_ns = wall; else s.out_ns = wall;
        return s;
    }
    s.open_now = g->open != 0;
    if (!g->seen) {
        /* Unmarked pid: the server's seen_cmd rule — we==0 stays CPU*. */
        s.in_cmd = true;
        s.in_ns = wall;
        return s;
    }
    uint64_t open_ns = consume ? pgwt_cmd_gate_take(g, t0_ns, t1_ns)
                               : pgwt_cmd_gate_peek(g, t0_ns, t1_ns);
    s.gated  = true;
    s.in_ns  = open_ns;
    s.out_ns = wall - open_ns;
    s.in_cmd = wall == 0 ? s.open_now : open_ns > 0;
    return s;
}

/* The server's all-or-nothing verdict (src/compute.c pgwt_tag_events), kept
 * so the live and server paths can still be diffed on one stream. Not used
 * for accounting on the live path any more — see the header comment. */
static inline bool pgwt_cmd_split_majority(const struct pgwt_cmd_split *s)
{
    uint64_t wall = s->in_ns + s->out_ns;
    if (wall == 0)
        return s->open_now;
    return s->in_ns * 2 >= wall;
}

/* v * num / den without overflowing, for apportioning a measured quantity
 * (cpu_ns) across the two halves of a split interval. */
static inline uint64_t pgwt_cmd_scale(uint64_t v, uint64_t num, uint64_t den)
{
    if (den == 0 || num >= den)
        return v;
    if (v == 0 || num == 0)
        return 0;
    if (v <= UINT64_MAX / num)
        return v * num / den;
    return (uint64_t)((double)v * ((double)num / (double)den));
}

#endif /* PGWT_CMD_GATE_H */
