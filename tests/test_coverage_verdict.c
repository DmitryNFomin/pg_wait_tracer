/* Coverage verdict differential test (U2a; Refs #947, #871).
 *
 * Every window of tests/fixtures/coverage/coverage-table-inputs.md is
 * checked, field by field, against coverage-answers-final.tsv, for both the
 * DB and the Idle verdict, plus the A1 item 4 ordering case. The table is
 * generated from those files by gen_coverage_cases.py at build time.
 *
 * Separately from that agreed table: one hand-written case per rule the
 * table cannot reach (each expectation derived from the spec text cited
 * beside it), and the A2 item 2 cost bound. */
#define _POSIX_C_SOURCE 200809L
#include "coverage.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SPANS 8

struct cov_expect {
    int verdict, error;
    size_t n_reasons;
    int reasons[PGWT_COV_N_REASONS];
    size_t n_unc;
    struct pgwt_cov_span unc[MAX_SPANS];
    int64_t covered_to;
    int restart, xf;
};

struct cov_case {
    const char *id, *window;
    const struct pgwt_cov_model *model;
    int64_t from, to, now;
    struct cov_expect db, idle;
};

#include "coverage_cases.h"

static int failures;

static void fail(const char *id, const char *kind, const char *what)
{
    printf("FAIL %s %s %s\n", id, kind, what);
    failures++;
}

static const char *reason_list(char *buf, size_t size, const int *r, size_t n)
{
    size_t used = 0;
    buf[0] = 0;
    for (size_t i = 0; i < n && used < size; i++)
        used += (size_t)snprintf(buf + used, size - used, "%s%s", i ? "," : "",
                                 pgwt_cov_reason_name(r[i]));
    return buf;
}

static const char *span_list(char *buf, size_t size, const struct pgwt_cov_span *s, size_t n)
{
    size_t used = 0;
    buf[0] = 0;
    for (size_t i = 0; i < n && used < size; i++)
        used += (size_t)snprintf(buf + used, size - used, "%s[%" PRId64 ",%" PRId64 ")%s",
                                 i ? " " : "", s[i].from, s[i].to,
                                 pgwt_cov_reason_name(s[i].reason));
    return buf;
}

/* Returns 1 when every compared field matches. */
static int compare(const struct cov_case *c, const char *kind, const struct cov_expect *e,
                   const struct pgwt_cov_result *got, int full)
{
    char a[512], b[512], what[1100];
    int before = failures;
    if (got->verdict != e->verdict) {
        snprintf(what, sizeof(what), "verdict: got %s want %s",
                 pgwt_cov_verdict_name(got->verdict), pgwt_cov_verdict_name(e->verdict));
        fail(c->id, kind, what);
    }
    if (got->error != e->error) {
        snprintf(what, sizeof(what), "error: got %s want %s",
                 pgwt_cov_error_name(got->error), pgwt_cov_error_name(e->error));
        fail(c->id, kind, what);
    }
    if (got->n_reasons != e->n_reasons ||
        memcmp(got->reasons, e->reasons, e->n_reasons * sizeof(int)) != 0) {
        snprintf(what, sizeof(what), "reasons: got [%s] want [%s]",
                 reason_list(a, sizeof(a), got->reasons, got->n_reasons),
                 reason_list(b, sizeof(b), e->reasons, e->n_reasons));
        fail(c->id, kind, what);
    }
    int spans_ok = got->n_uncovered == e->n_unc;
    for (size_t i = 0; spans_ok && i < e->n_unc; i++)
        spans_ok = got->uncovered[i].from == e->unc[i].from &&
                   got->uncovered[i].to == e->unc[i].to &&
                   got->uncovered[i].reason == e->unc[i].reason;
    if (!spans_ok) {
        snprintf(what, sizeof(what), "uncovered (ns): got {%s} want {%s}",
                 span_list(a, sizeof(a), got->uncovered, got->n_uncovered),
                 span_list(b, sizeof(b), e->unc, e->n_unc));
        fail(c->id, kind, what);
    }
    if (full && got->covered_to != e->covered_to) {
        snprintf(what, sizeof(what), "covered_to: got %" PRId64 " want %" PRId64,
                 got->covered_to, e->covered_to);
        fail(c->id, kind, what);
    }
    if (full && got->restart_cause != e->restart) {
        snprintf(what, sizeof(what), "restart_cause: got %s want %s",
                 pgwt_cov_restart_name(got->restart_cause), pgwt_cov_restart_name(e->restart));
        fail(c->id, kind, what);
    }
    if (full && got->excludes_in_flight != e->xf) {
        snprintf(what, sizeof(what), "excludes_in_flight: got %d want %d",
                 got->excludes_in_flight, e->xf);
        fail(c->id, kind, what);
    }
    return failures == before;
}

/* Scratch of exactly the size the function asks for. */
static void *scratch_for(const struct pgwt_cov_model *m, size_t *size)
{
    void *p;
    *size = pgwt_cov_scratch_bytes(m);
    if (*size == SIZE_MAX || !(p = malloc(*size ? *size : 1))) {
        printf("FAIL scratch: no %zu-byte scratch\n", *size);
        exit(1);
    }
    return p;
}

static void run_case(const struct cov_case *c)
{
    struct pgwt_cov_span db_spans[MAX_SPANS], idle_spans[MAX_SPANS];
    struct pgwt_cov_result db = {.uncovered = db_spans, .uncovered_cap = MAX_SPANS};
    struct pgwt_cov_result idle = {.uncovered = idle_spans, .uncovered_cap = MAX_SPANS};
    db.scratch = idle.scratch = scratch_for(c->model, &db.scratch_size);
    idle.scratch_size = db.scratch_size;
    pgwt_coverage_verdict(c->model, c->from, c->to, c->now, PGWT_COV_FLOOR_DB, &db);
    pgwt_coverage_verdict(c->model, c->from, c->to, c->now, PGWT_COV_FLOOR_IDLE, &idle);
    free(db.scratch);
    int ok = compare(c, "db", &c->db, &db, 1);
    ok &= compare(c, "idle", &c->idle, &idle, 0);
    if (ok) {
        char r[256];
        printf("PASS %-3s %-13s %s [%s]%s%s idle=%s\n", c->id, c->window,
               pgwt_cov_verdict_name(db.verdict),
               db.verdict == PGWT_COV_ERROR ? pgwt_cov_error_name(db.error)
                                            : reason_list(r, sizeof(r), db.reasons, db.n_reasons),
               db.excludes_in_flight ? " xf" : "",
               db.verdict == PGWT_COV_LIVE_EDGE ? " covered_to" : "",
               pgwt_cov_verdict_name(idle.verdict));
    }
}

/* API edges the table does not reach: a full caller buffer, a model whose
 * first record is not a FILE fact and a scratch area one byte short are
 * errors, never a verdict. */
static void api_edges(void)
{
    const struct cov_case *c = NULL;
    for (size_t i = 0; i < sizeof(cov_cases) / sizeof(cov_cases[0]); i++)
        if (strcmp(cov_cases[i].id, "16") == 0)
            c = &cov_cases[i];
    if (!c) {
        fail("api", "db", "row 16 is missing");
        return;
    }
    struct pgwt_cov_span one[1], many[MAX_SPANS];
    struct pgwt_cov_result r = {.uncovered = one, .uncovered_cap = 1};
    r.scratch = scratch_for(c->model, &r.scratch_size);
    if (pgwt_coverage_verdict(c->model, c->from, c->to, c->now, PGWT_COV_FLOOR_DB, &r) !=
            PGWT_COV_ERROR || r.error != PGWT_COV_E_BUFFER_FULL || r.n_uncovered)
        fail("api", "db", "two ranges in a one-range buffer must be buffer_full");
    /* Row 16 is incomplete with enough scratch; one byte short, or none,
     * must refuse rather than give any verdict. */
    struct pgwt_cov_result s = {.uncovered = many, .uncovered_cap = MAX_SPANS,
                                .scratch = r.scratch, .scratch_size = r.scratch_size - 1};
    if (pgwt_coverage_verdict(c->model, c->from, c->to, c->now, PGWT_COV_FLOOR_DB, &s) !=
            PGWT_COV_ERROR || s.error != PGWT_COV_E_SCRATCH_SMALL)
        fail("api", "db", "scratch one byte short must be scratch_small");
    s.scratch = NULL;
    s.scratch_size = 0;
    if (pgwt_coverage_verdict(c->model, c->from, c->to, c->now, PGWT_COV_FLOOR_DB, &s) !=
            PGWT_COV_ERROR || s.error != PGWT_COV_E_SCRATCH_SMALL)
        fail("api", "db", "no scratch must be scratch_small");
    free(r.scratch);
    const struct pgwt_cov_rec recs[] = {{PGWT_COV_REC_WATERMARK, 1, 1, 1}};
    const struct pgwt_cov_boot boot = {.recs = recs, .n_recs = 1};
    const struct pgwt_cov_model m = {&boot, 1, NULL, 0};
    r.scratch = NULL;
    r.scratch_size = 0;
    if (pgwt_coverage_verdict(&m, 0, 10, 10, PGWT_COV_FLOOR_DB, &r) != PGWT_COV_ERROR ||
        r.error != PGWT_COV_E_BAD_MODEL)
        fail("api", "db", "records not opened by a FILE fact must be bad_model");
}

/* ---- Rule cases outside the agreed table ------------------------------
 * Spec = m871/spec-coverage-label-r2.md (r2 §1-§2, Amendments A1, A2).
 * Times in seconds (T), wall = mono unless an offset is given. Each case's
 * expectation is derived from the cited spec text, not from this code; each
 * has a mutant in coverage_verdict_mutants.py that removes the rule. */
#define T(x) ((int64_t)((x) * 1e9))
#define F(obs, no, ok) {PGWT_COV_REC_FILE, T(obs), (no), (ok)}
#define WM(w, fd) {PGWT_COV_REC_WATERMARK, T(w), T(fd), T(fd)}
#define GO(s, id) {PGWT_COV_REC_GAP_OPEN, T(s), 0, (id)}
#define GC(e, id) {PGWT_COV_REC_GAP_CLOSE, T(e), 0, (id)}
#define SEAL(w) {PGWT_COV_REC_SEAL, T(w), 0, 0}
#define NRECS(a) (sizeof(a) / sizeof((a)[0]))
/* start, wall offset, postmaster start, closure clean, same kernel boot,
 * control status, last observation, records */
#define BOOT(start, off, pm, clean, kern, status, obs, recs)                             \
    {T(start), T(off), (pm), (clean), (kern), PGWT_COV_STATUS_##status, T(obs), (recs),  \
     NRECS(recs)}
#define MODEL(boots) {(boots), NRECS(boots), NULL, 0}
#define R(x) PGWT_COV_R_##x
#define M_COMMA ,
#define INCOMPLETE(nr, rs, nu, us, rc)                                                \
    {PGWT_COV_INCOMPLETE, PGWT_COV_E_NONE, nr, rs, nu, us, PGWT_COV_NONE, rc, 0}
#define COMPLETE(xf) {PGWT_COV_COMPLETE, PGWT_COV_E_NONE, 0, {0}, 0, {{0, 0, 0}}, \
                      PGWT_COV_NONE, PGWT_COV_RESTART_NONE, xf}
#define SPAN(a, b, r) {T(a), T(b), R(r)}
#define RULE(id, w, m, f, t, now, e) {id, w, &(m), T(f), T(t), T(now), e, e}

/* gap_id pairing. §1 kinds: GAP_OPEN and GAP_CLOSE carry gap_id in c; §2
 * Model "per gap: [start, end or ∞)"; §2 Taint: taint_from = floor of the
 * last WATERMARK with W <= gap start. Gap 1 [55,70), gap 2 [75,160); the
 * close of gap 1 is appended after gap 2 opens. [85,95) lies in gap 2. */
static const struct pgwt_cov_rec rg_id_r[] = {F(0, 0, 1), WM(58, 55), GO(60, 1), WM(78, 75),
                                              GO(80, 2), GC(70, 1), GC(160, 2), WM(200, 200),
                                              SEAL(200)};
static const struct pgwt_cov_boot rg_id_b[] = {BOOT(0, 0, 1, 1, 1, NONE, 200, rg_id_r)};
static const struct pgwt_cov_model rg_id = MODEL(rg_id_b);

/* gap_id reused, record order (review nit 4). §1 "when": GAP_CLOSE is
 * written at recovery, after its GAP_OPEN, so an open pairs with the first
 * close of its id that follows it. G(49,50) id 7, then GO(80) id 7 closed
 * at 120: taint [79,120). Second model: the only close of id 7 precedes
 * the open, so the gap never closes: taint [79, ∞), fail closed. */
static const struct pgwt_cov_rec rg_reuse_r[] = {F(0, 0, 1), WM(49, 49), GO(49, 7), GC(50, 7),
                                                 WM(79, 79), GO(80, 7), GC(120, 7),
                                                 WM(200, 200), SEAL(200)};
static const struct pgwt_cov_boot rg_reuse_b[] = {BOOT(0, 0, 1, 1, 1, NONE, 200, rg_reuse_r)};
static const struct pgwt_cov_model rg_reuse = MODEL(rg_reuse_b);
static const struct pgwt_cov_rec rg_early_r[] = {F(0, 0, 1), WM(79, 79), GC(120, 7), GO(80, 7),
                                                 WM(200, 200), SEAL(200)};
static const struct pgwt_cov_boot rg_early_b[] = {BOOT(0, 0, 1, 1, 1, NONE, 200, rg_early_r)};
static const struct pgwt_cov_model rg_early = MODEL(rg_early_b);

/* Liveness. §2 Live = unsealed; same kernel boot_id as the server; last
 * observation within L of now; with a control socket, the status reports
 * this nonce. Not live => §2 Covered piece E_b = floor(W) = 40, and (3)
 * unclean_end runs from E_b to the window end (A1 item 6). Row 7's boot
 * (wm 100/40, obs 100.5, now 101) with one live condition broken. */
static const struct pgwt_cov_rec rl_r[] = {F(0, 0, 1), WM(100, 40)};
static const struct pgwt_cov_boot rl_kernel_b[] = {
    BOOT(0, 0, 1, 0, 0, NONE, 100.5, rl_r)};
static const struct pgwt_cov_model rl_kernel = MODEL(rl_kernel_b);
static const struct pgwt_cov_boot rl_mismatch_b[] = {
    BOOT(0, 0, 1, 0, 1, MISMATCH, 100.5, rl_r)};
static const struct pgwt_cov_model rl_mismatch = MODEL(rl_mismatch_b);
static const struct pgwt_cov_boot rl_match_b[] = {
    BOOT(0, 0, 1, 0, 1, MATCH, 100.5, rl_r)};
static const struct pgwt_cov_model rl_match = MODEL(rl_match_b);

/* Unreadable files. §2 Holes: "an unreadable file ... is a hole
 * [floor(last valid W before it), BOOT observation of the next present
 * file of the boot, else ∞)". f1 is unreadable, so the hole is [90,200):
 * f1 opens it (rule a) and cannot close it, since its own observation was
 * not read; f2 closes it (rule b). */
static const struct pgwt_cov_rec ru_r[] = {F(0, 0, 1), WM(100, 90), F(150, 1, 0), F(200, 2, 1),
                                           WM(300, 300), SEAL(300)};
static const struct pgwt_cov_boot ru_b[] = {BOOT(0, 0, 1, 1, 1, NONE, 300, ru_r)};
static const struct pgwt_cov_model ru = MODEL(ru_b);

/* Non-monotone W. §2 Holes: "a bad CRC / unknown kind / non-monotone W at
 * record r is a hole [floor(last valid W before it), BOOT observation of
 * the next present file ...)". wm 50 follows wm 100/90 in f0, so the hole
 * is [90,150), f1's observation. */
static const struct pgwt_cov_rec rw_r[] = {F(0, 0, 1),  WM(100, 90),  WM(50, 50), WM(120, 110),
                                           F(150, 1, 1), WM(300, 300), SEAL(300)};
static const struct pgwt_cov_boot rw_b[] = {BOOT(0, 0, 1, 1, 1, NONE, 300, rw_r)};
static const struct pgwt_cov_model rw = MODEL(rw_b);

/* restart_cause unknown. A1 item 2: `unknown` when either postmaster start
 * is missing. Row 4's boots, B1's postmaster start 0 (= unknown, §1). */
static const struct pgwt_cov_rec rr1_r[] = {F(0, 0, 1), WM(100, 100), SEAL(100)};
static const struct pgwt_cov_rec rr2_r[] = {F(130, 0, 1), WM(200, 200), SEAL(200)};
static const struct pgwt_cov_boot rr_b[] = {
    BOOT(0, 0, 0, 1, 1, NONE, 100, rr1_r),
    BOOT(130, 0, 1, 1, 1, NONE, 200, rr2_r)};
static const struct pgwt_cov_model rr = MODEL(rr_b);

/* clock_step. A2 item 1: if the wall ranges [S_b, E_b] of two boots
 * overlap, every point inside the overlap is uncovered with clock_step,
 * after restart in the reason list. B1 generation 0 (wall = mono + 1000)
 * covers wall [1000,1100]; B2 generation 1 (wall = mono + 950) covers wall
 * [1070,1150] after a backward step: overlap [1070,1100). A window inside
 * it meets both boots, so the window-level rule of §2 States adds restart
 * (cause daemon: equal postmaster starts, A1 item 2). Outside it each boot
 * alone covers its window. */
static const struct pgwt_cov_rec rc1_r[] = {F(0, 0, 1), WM(100, 100), SEAL(100)};
static const struct pgwt_cov_rec rc2_r[] = {F(120, 0, 1), WM(200, 200), SEAL(200)};
static const struct pgwt_cov_boot rc_b[] = {
    BOOT(0, 1000, 1, 1, 1, NONE, 100, rc1_r),
    BOOT(120, 950, 1, 1, 1, NONE, 200, rc2_r)};
static const struct pgwt_cov_model rc = MODEL(rc_b);

static const struct cov_case rule_cases[] = {
    RULE("G1-gap-id", "[85,95)", rg_id, 85, 95, 10000,
         INCOMPLETE(1, {R(CAPTURE_LOSS)}, 1, {SPAN(85, 95, CAPTURE_LOSS)}, PGWT_COV_RESTART_NONE)),
    RULE("G2-id-reused", "[85,95)", rg_reuse, 85, 95, 10000,
         INCOMPLETE(1, {R(CAPTURE_LOSS)}, 1, {SPAN(85, 95, CAPTURE_LOSS)}, PGWT_COV_RESTART_NONE)),
    RULE("G3-close-first", "[150,160)", rg_early, 150, 160, 10000,
         INCOMPLETE(1, {R(CAPTURE_LOSS)}, 1, {SPAN(150, 160, CAPTURE_LOSS)},
                    PGWT_COV_RESTART_NONE)),
    RULE("L1-other-kernel", "[50,100)", rl_kernel, 50, 100, 101,
         INCOMPLETE(1, {R(UNCLEAN_END)}, 1, {SPAN(50, 100, UNCLEAN_END)}, PGWT_COV_RESTART_NONE)),
    RULE("L2-status-mismatch", "[50,100)", rl_mismatch, 50, 100, 101,
         INCOMPLETE(1, {R(UNCLEAN_END)}, 1, {SPAN(50, 100, UNCLEAN_END)}, PGWT_COV_RESTART_NONE)),
    /* Control: the same boot with its own nonce reported is live (row 7a). */
    RULE("L3-status-match", "[50,100)", rl_match, 50, 100, 101, COMPLETE(1)),
    RULE("H1-unreadable", "[50,150)", ru, 50, 150, 10000,
         INCOMPLETE(1, {R(MISSING_FILE)}, 1, {SPAN(90, 150, MISSING_FILE)}, PGWT_COV_RESTART_NONE)),
    RULE("H2-unreadable-no-close", "[160,190)", ru, 160, 190, 10000,
         INCOMPLETE(1, {R(MISSING_FILE)}, 1, {SPAN(160, 190, MISSING_FILE)},
                    PGWT_COV_RESTART_NONE)),
    RULE("H3-nonmonotone-w", "[95,140)", rw, 95, 140, 10000,
         INCOMPLETE(1, {R(MISSING_FILE)}, 1, {SPAN(95, 140, MISSING_FILE)}, PGWT_COV_RESTART_NONE)),
    RULE("C1-cause-unknown", "[90,140)", rr, 90, 140, 10000,
         INCOMPLETE(1, {R(RESTART)}, 1, {SPAN(100, 130, RESTART)}, PGWT_COV_RESTART_UNKNOWN)),
    RULE("S1-clock-step-inside", "[1075,1095)", rc, 1075, 1095, 10000,
         INCOMPLETE(2, {R(RESTART) M_COMMA R(CLOCK_STEP)}, 1, {SPAN(1075, 1095, CLOCK_STEP)},
                    PGWT_COV_RESTART_DAEMON)),
    RULE("S2-clock-step-edge", "[1060,1080)", rc, 1060, 1080, 10000,
         INCOMPLETE(2, {R(RESTART) M_COMMA R(CLOCK_STEP)}, 1, {SPAN(1070, 1080, CLOCK_STEP)},
                    PGWT_COV_RESTART_DAEMON)),
    RULE("S3-before-step", "[1010,1060)", rc, 1010, 1060, 10000, COMPLETE(0)),
    RULE("S4-after-step", "[1110,1140)", rc, 1110, 1140, 10000, COMPLETE(0)),
};

/* A2 item 2: a whole-day window over one sealed boot with 86,400
 * WATERMARKs and 1,000 gaps must take under 50 ms. Gap k opens at
 * t = 86k + 43 right after wm t/(t-1) and closes at t + 1 (the master
 * path's GAP_OPEN+GAP_CLOSE pair), so §2 Taint gives [t-1, t+1): 1,000
 * disjoint capture_loss ranges. */
#define BENCH_TICKS 86400
#define BENCH_GAPS 1000
#define BENCH_BOUND_MS 50.0
static struct pgwt_cov_rec bench_recs[BENCH_TICKS + 2 * BENCH_GAPS + 2];
static struct pgwt_cov_span bench_spans[BENCH_GAPS + 1];

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void bench(void)
{
    size_t n = 0;
    int gaps = 0;
    bench_recs[n++] = (struct pgwt_cov_rec){PGWT_COV_REC_FILE, 0, 0, 1};
    for (int t = 1; t <= BENCH_TICKS; t++) {
        bench_recs[n++] = (struct pgwt_cov_rec){PGWT_COV_REC_WATERMARK, T(t), T(t - 1), T(t - 1)};
        if (t % 86 == 43 && gaps < BENCH_GAPS) {
            gaps++;
            bench_recs[n++] = (struct pgwt_cov_rec){PGWT_COV_REC_GAP_OPEN, T(t), 0, gaps};
            bench_recs[n++] = (struct pgwt_cov_rec){PGWT_COV_REC_GAP_CLOSE, T(t + 1), 0, gaps};
        }
    }
    bench_recs[n++] = (struct pgwt_cov_rec){PGWT_COV_REC_SEAL, T(BENCH_TICKS), 0, 0};
    const struct pgwt_cov_boot b = {.boot_start = 0, .postmaster_start = 1, .closure_clean = 1,
                                    .same_kernel_boot = 1, .last_obs = T(BENCH_TICKS),
                                    .recs = bench_recs, .n_recs = n};
    const struct pgwt_cov_model m = {&b, 1, NULL, 0};
    struct pgwt_cov_result r = {.uncovered = bench_spans, .uncovered_cap = BENCH_GAPS + 1};
    r.scratch = scratch_for(&m, &r.scratch_size);
    double t0 = now_ms();
    pgwt_coverage_verdict(&m, 0, T(BENCH_TICKS), T(BENCH_TICKS + 100), PGWT_COV_FLOOR_DB, &r);
    double ms = now_ms() - t0;
    free(r.scratch);
    int ok = gaps == BENCH_GAPS && r.verdict == PGWT_COV_INCOMPLETE && r.n_reasons == 1 &&
             r.reasons[0] == PGWT_COV_R_CAPTURE_LOSS && r.n_uncovered == BENCH_GAPS;
    for (size_t k = 0; ok && k < r.n_uncovered; k++) {
        int64_t t = 86 * (int64_t)k + 43;
        ok = r.uncovered[k].from == T(t - 1) && r.uncovered[k].to == T(t + 1) &&
             r.uncovered[k].reason == PGWT_COV_R_CAPTURE_LOSS;
    }
    if (!ok)
        fail("BENCH", "db", "1,000 gaps must give 1,000 capture_loss ranges [t-1,t+1)");
    if (ms >= BENCH_BOUND_MS) {
        char what[128];
        snprintf(what, sizeof(what), "%.3f ms, bound %.0f ms (A2 item 2)", ms, BENCH_BOUND_MS);
        fail("BENCH", "db", what);
    }
    printf("%s BENCH %zu records (%d watermarks, %d gaps), whole-day window: %.3f ms "
           "(bound %.0f ms)\n",
           ok && ms < BENCH_BOUND_MS ? "PASS" : "FAIL", n, BENCH_TICKS, gaps, ms,
           BENCH_BOUND_MS);
}

int main(void)
{
    size_t n = sizeof(cov_cases) / sizeof(cov_cases[0]);
    for (size_t i = 0; i < n; i++)
        run_case(&cov_cases[i]);
    size_t nr = sizeof(rule_cases) / sizeof(rule_cases[0]);
    for (size_t i = 0; i < nr; i++)
        run_case(&rule_cases[i]);
    api_edges();
    bench();
    printf("coverage verdict: %zu windows (%d table + %zu extra) + %zu rule cases + bench, "
           "db and idle: %s (%d failed checks)\n",
           n, COV_TABLE_WINDOWS, n - (size_t)COV_TABLE_WINDOWS, nr, failures ? "FAIL" : "PASS",
           failures);
    return failures ? 1 : 0;
}
