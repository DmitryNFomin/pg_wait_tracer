/* test_cmd_gate.c — the bypass suite for src/cmd_gate.h (issue #115).
 *
 * The shared command-gate rule splits a backend's on-CPU interval against
 * PostgreSQL's statement boundaries. Each case below is a way to make that
 * split silently credit NOTHING, credit TWICE, or skip a boundary; each one
 * fails against the pre-#115 arithmetic, quoted in the case's comment
 * (case 2 and case 5 pin behaviour the fix deliberately PRESERVES).
 *
 * Pure: no BPF, no daemon, no PostgreSQL. Runs anywhere.
 */
#include "cmd_gate.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static int tests_run, tests_passed;
#define CHECK(cond, ...) do {                                   \
    tests_run++;                                                \
    if (cond) { tests_passed++; printf("  ok   "); }            \
    else      { printf("  FAIL "); }                            \
    printf(__VA_ARGS__); printf("\n");                          \
} while (0)

#define MS(x) ((uint64_t)(x) * 1000000ULL)
#define US(x) ((uint64_t)(x) * 1000ULL)

/* ── 1. A straddling interval is credited proportionally, not all-or-nothing */
static void test_straddle(void)
{
    printf("--- straddle: proportional, and every ns conserved ---\n");
    /* Statement [1.0, 3.0) ms; the on-CPU gap that closes it runs
     * [0.5, 4.0) — 2 ms inside, 1.5 ms outside. The majority rule gave the
     * whole 3.5 ms to CPU*. */
    struct pgwt_cmd_gate g = { 0 };
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_START, MS(1));
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_END, MS(3));
    struct pgwt_cmd_split s = pgwt_cmd_gate_split(&g, true, US(500), MS(4), false);
    CHECK(s.gated, "decided by markers");
    CHECK(s.in_ns == MS(2), "in-command = 2.0 ms (got %.3f)", s.in_ns / 1e6);
    CHECK(s.out_ns == US(1500), "out-of-command = 1.5 ms (got %.3f)", s.out_ns / 1e6);
    CHECK(s.in_ns + s.out_ns == US(3500), "the two halves conserve the wall");
    CHECK(pgwt_cmd_split_majority(&s),
          "the old majority verdict (2.0 of 3.5 ms) would have taken all 3.5");

    /* Mirror image: mostly OUTSIDE. The majority rule threw the whole
     * interval away — 0.9 ms of real statement CPU vanished. */
    struct pgwt_cmd_gate h = { 0 };
    pgwt_cmd_gate_marker(&h, PGWT_MARKER_CMD_START, MS(30) + US(100));
    pgwt_cmd_gate_marker(&h, PGWT_MARKER_CMD_END, MS(31));
    struct pgwt_cmd_split t = pgwt_cmd_gate_split(&h, true, MS(30), MS(34), false);
    CHECK(t.in_ns == US(900) && t.out_ns == US(3100),
          "0.9 ms kept, 3.1 ms dropped (majority kept 0)");
    CHECK(!pgwt_cmd_split_majority(&t), "…and the old verdict was non-command");
}

/* ── 2. Degenerate (empty) interval: credits nothing, either way ────── */
static void test_zero_length(void)
{
    printf("--- degenerate interval: credits nothing; the shared swallow ---\n");
    struct pgwt_cmd_gate g = { 0 };
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_START, MS(10));
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_END, MS(12));   /* banks 2 ms */
    CHECK(g.banked_ns == MS(2), "2 ms banked before the degenerate record");

    struct pgwt_cmd_split z = pgwt_cmd_gate_split(&g, true, MS(12), MS(12), false);
    CHECK(z.in_ns == 0 && z.out_ns == 0, "a [t, t) interval splits into nothing");
    CHECK(!z.in_cmd, "…and takes the instantaneous gate (closed here)");
    /* KNOWN SHARED LIMITATION (cmd_gate.h pgwt_cmd_gate_take): the record
     * has no wall to hold the banked 2 ms, and consumes it anyway. Pinned
     * here rather than left silent; it can only change in lock-step with
     * compute.c pgwt_tag_events, whose sweep this one mirrors byte for
     * byte (tests/test_live_accum.c diffs them). */
    CHECK(g.banked_ns == 0,
          "the banked 2 ms is consumed and credited nowhere — the one "
          "limitation shared with compute.c, deliberately not fixed here");
    /* A t1 BEFORE t0 (a duration longer than the record's own timestamp)
     * must behave the same and never produce a negative out_ns. */
    struct pgwt_cmd_gate n = { 0 };
    pgwt_cmd_gate_marker(&n, PGWT_MARKER_CMD_START, MS(1));
    struct pgwt_cmd_split b = pgwt_cmd_gate_split(&n, true, MS(9), MS(4), false);
    CHECK(b.in_ns == 0 && b.out_ns == 0,
          "an inverted [t1 < t0) interval splits into nothing, not into a "
          "16-exabyte out_ns");
}

/* ── 3. A command open at the window start, and still open at its end ── */
static void test_open_at_edges(void)
{
    printf("--- command open at window start / still open at window end ---\n");
    /* Open before the interval begins: the run is clipped to t0, not
     * counted from the anchor (which would credit more wall than the
     * interval lasted). */
    struct pgwt_cmd_gate g = { 0 };
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_START, MS(1));
    struct pgwt_cmd_split a = pgwt_cmd_gate_split(&g, true, MS(5), MS(8), false);
    CHECK(a.in_ns == MS(3) && a.out_ns == 0,
          "open at the start: the whole 3 ms is in-command, no more");

    /* Still open at the end: the sweep re-anchors at t1, so the NEXT
     * interval is not credited the run it already consumed. */
    CHECK(g.open && g.anchor_ns == MS(8), "take() re-anchored to t1");
    struct pgwt_cmd_split b = pgwt_cmd_gate_split(&g, true, MS(8), MS(9), false);
    CHECK(b.in_ns == MS(1), "the next interval gets exactly its own 1 ms");

    /* Peek (open stretch at tick time) must not advance the sweep. */
    struct pgwt_cmd_gate before = g;
    struct pgwt_cmd_split c = pgwt_cmd_gate_split(&g, false, MS(9), MS(11), false);
    CHECK(c.in_ns == MS(2), "peek reports the open stretch's 2 ms");
    CHECK(memcmp(&g, &before, sizeof(g)) == 0, "…without advancing the sweep");

    /* Banked time that PREDATES the interval (state_map's last_ts running
     * ahead of the last drained record) is clipped to the interval's wall
     * — otherwise a 3 ms interval is credited 9 ms of CPU. */
    struct pgwt_cmd_gate lag = { .seen = 1, .open = 0, .banked_ns = MS(9) };
    struct pgwt_cmd_split d = pgwt_cmd_gate_split(&lag, true, MS(100), MS(103), false);
    CHECK(d.in_ns == MS(3) && d.out_ns == 0,
          "stale banked time cannot credit more than the interval lasted");
}

/* ── 4. Markers out of order ─────────────────────────────────────────── */
static void test_markers_out_of_order(void)
{
    printf("--- markers out of order: no underflow, no phantom command ---\n");
    /* A CMD_END stamped BEFORE the open run's anchor. The pre-#115 copy in
     * tests/cross_validate.c did `banked += ts - anchor` unguarded, which
     * underflows to ~1.8e19 ns and makes every later interval look
     * entirely in-command. */
    struct pgwt_cmd_gate g = { 0 };
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_START, MS(50));
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_END, MS(40));
    CHECK(g.banked_ns == 0, "out-of-order CMD_END banks nothing (got %" PRIu64 ")",
          g.banked_ns);
    CHECK(!g.open, "…but still closes the window");
    struct pgwt_cmd_split s = pgwt_cmd_gate_split(&g, true, MS(50), MS(60), false);
    CHECK(s.in_ns == 0 && s.out_ns == MS(10),
          "the interval after it is not credited a phantom command");

    /* A duplicate CMD_START (lost CMD_END) keeps the first anchor; a
     * CMD_END with no open window banks nothing. */
    struct pgwt_cmd_gate h = { 0 };
    pgwt_cmd_gate_marker(&h, PGWT_MARKER_CMD_START, MS(10));
    pgwt_cmd_gate_marker(&h, PGWT_MARKER_CMD_START, MS(12));
    CHECK(h.open && h.anchor_ns == MS(10), "duplicate CMD_START keeps the anchor");
    pgwt_cmd_gate_marker(&h, PGWT_MARKER_CMD_END, MS(13));
    pgwt_cmd_gate_marker(&h, PGWT_MARKER_CMD_END, MS(14));
    CHECK(!h.open && h.banked_ns == MS(3), "duplicate CMD_END banks once (3 ms)");

    /* Non-command markers never touch the gate. */
    struct pgwt_cmd_gate q = { 0 };
    pgwt_cmd_gate_marker(&q, PGWT_MARKER_EXEC_START, MS(1));
    pgwt_cmd_gate_marker(&q, PGWT_MARKER_PLAN_START, MS(1));
    pgwt_cmd_gate_marker(&q, PGWT_MARKER_ESCALATE_START, MS(1));
    CHECK(!q.seen && !q.open && q.banked_ns == 0,
          "plan/exec/escalation markers are not command boundaries");
}

/* ── 5. A pid with no markers at all ─────────────────────────────────── */
static void test_unmarked_pid(void)
{
    printf("--- a pid with no markers: legacy CPU, and it says so ---\n");
    struct pgwt_cmd_gate g = { 0 };
    struct pgwt_cmd_split s = pgwt_cmd_gate_split(&g, true, MS(0), MS(5), false);
    CHECK(!s.gated, "not decided by markers — the caller must count it");
    CHECK(s.in_ns == MS(5) && s.out_ns == 0 && s.in_cmd,
          "the server's seen_cmd rule: we==0 stays CPU*");
    CHECK(g.anchor_ns == 0 && g.banked_ns == 0 && !g.seen,
          "…and the sweep stays untouched until the first marker");

    /* No per-pid state at all (accumulator full): the emission-time gate. */
    struct pgwt_cmd_split o = pgwt_cmd_gate_split(NULL, true, MS(0), MS(1), true);
    struct pgwt_cmd_split c = pgwt_cmd_gate_split(NULL, true, MS(0), MS(1), false);
    CHECK(!o.gated && o.in_ns == MS(1) && !c.gated && c.out_ns == MS(1),
          "no state: falls back to the emission flag, both ways");
}

/* ── 6. One interval spanning several commands ───────────────────────── */
static void test_many_commands(void)
{
    printf("--- one interval spanning several commands ---\n");
    /* Three statements inside a single on-CPU gap [0, 10) ms:
     * [1,2), [3,4.5) and [6,9.5) → 1 + 1.5 + 3.5 = 6 ms in-command.
     * A rule that only remembered the LAST boundary would credit 3.5. */
    struct pgwt_cmd_gate g = { 0 };
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_START, MS(1));
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_END, MS(2));
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_START, MS(3));
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_END, US(4500));
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_START, MS(6));
    pgwt_cmd_gate_marker(&g, PGWT_MARKER_CMD_END, US(9500));
    struct pgwt_cmd_split s = pgwt_cmd_gate_split(&g, true, MS(0), MS(10), false);
    CHECK(s.in_ns == MS(6) && s.out_ns == MS(4),
          "all three windows counted: 6 ms in, 4 ms out (got %.2f / %.2f)",
          s.in_ns / 1e6, s.out_ns / 1e6);
    CHECK(g.banked_ns == 0, "consumed exactly once — the next interval gets 0");
    struct pgwt_cmd_split n = pgwt_cmd_gate_split(&g, true, MS(10), MS(11), false);
    CHECK(n.in_ns == 0 && n.out_ns == MS(1), "…so nothing is credited twice");

    /* The same span with the last command still OPEN at t1. */
    struct pgwt_cmd_gate h = { 0 };
    pgwt_cmd_gate_marker(&h, PGWT_MARKER_CMD_START, MS(1));
    pgwt_cmd_gate_marker(&h, PGWT_MARKER_CMD_END, MS(2));
    pgwt_cmd_gate_marker(&h, PGWT_MARKER_CMD_START, MS(7));
    struct pgwt_cmd_split t = pgwt_cmd_gate_split(&h, true, MS(0), MS(10), false);
    CHECK(t.in_ns == MS(4) && t.out_ns == MS(6),
          "banked 1 ms + the open run's 3 ms (got %.2f ms)", t.in_ns / 1e6);
}

/* ── 7. Apportioning a measured quantity across the split ────────────── */
static void test_scale(void)
{
    printf("--- pgwt_cmd_scale: no overflow, no lost nanoseconds ---\n");
    CHECK(pgwt_cmd_scale(MS(5), MS(2), MS(4)) == US(2500), "5 * 2/4 = 2.5 ms");
    CHECK(pgwt_cmd_scale(100, 0, 10) == 0, "zero numerator");
    CHECK(pgwt_cmd_scale(100, 10, 0) == 100, "zero denominator: unscaled");
    CHECK(pgwt_cmd_scale(100, 20, 10) == 100, "num >= den: unscaled, never more");
    uint64_t big = UINT64_MAX / 2;
    uint64_t half = pgwt_cmd_scale(big, 1, 2);
    CHECK(half > big / 2 - 4 && half <= big / 2 + 4,
          "a huge value halves without overflowing (%" PRIu64 ")", half);
}

int main(void)
{
    test_straddle();
    test_zero_length();
    test_open_at_edges();
    test_markers_out_of_order();
    test_unmarked_pid();
    test_many_commands();
    test_scale();
    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
