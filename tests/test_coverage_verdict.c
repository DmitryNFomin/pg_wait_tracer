/* Coverage verdict differential test (U2a; Refs #947, #871).
 *
 * Every window of tests/fixtures/coverage/coverage-table-inputs.md is
 * checked, field by field, against coverage-answers-final.tsv, for both the
 * DB and the Idle verdict, plus the A1 item 4 ordering case. The table is
 * generated from those files by gen_coverage_cases.py at build time. */
#include "coverage.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

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

static void run_case(const struct cov_case *c)
{
    struct pgwt_cov_span db_spans[MAX_SPANS], idle_spans[MAX_SPANS];
    struct pgwt_cov_result db = {.uncovered = db_spans, .uncovered_cap = MAX_SPANS};
    struct pgwt_cov_result idle = {.uncovered = idle_spans, .uncovered_cap = MAX_SPANS};
    pgwt_coverage_verdict(c->model, c->from, c->to, c->now, PGWT_COV_FLOOR_DB, &db);
    pgwt_coverage_verdict(c->model, c->from, c->to, c->now, PGWT_COV_FLOOR_IDLE, &idle);
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

/* API edges the table does not reach: a full caller buffer and a model
 * whose first record is not a FILE fact are errors, never a verdict. */
static void api_edges(void)
{
    const struct cov_case *c = NULL;
    for (size_t i = 0; i < sizeof(cov_cases) / sizeof(cov_cases[0]); i++)
        if (strcmp(cov_cases[i].id, "16") == 0)
            c = &cov_cases[i];
    struct pgwt_cov_span one[1];
    struct pgwt_cov_result r = {.uncovered = one, .uncovered_cap = 1};
    if (!c || pgwt_coverage_verdict(c->model, c->from, c->to, c->now, PGWT_COV_FLOOR_DB, &r) !=
                  PGWT_COV_ERROR || r.error != PGWT_COV_E_BUFFER_FULL || r.n_uncovered)
        fail("api", "db", "two ranges in a one-range buffer must be buffer_full");
    const struct pgwt_cov_rec recs[] = {{PGWT_COV_REC_WATERMARK, 1, 1, 1}};
    const struct pgwt_cov_boot boot = {.recs = recs, .n_recs = 1};
    const struct pgwt_cov_model m = {&boot, 1, NULL, 0};
    if (pgwt_coverage_verdict(&m, 0, 10, 10, PGWT_COV_FLOOR_DB, &r) != PGWT_COV_ERROR ||
        r.error != PGWT_COV_E_BAD_MODEL)
        fail("api", "db", "records not opened by a FILE fact must be bad_model");
}

int main(void)
{
    size_t n = sizeof(cov_cases) / sizeof(cov_cases[0]);
    for (size_t i = 0; i < n; i++)
        run_case(&cov_cases[i]);
    api_edges();
    printf("coverage verdict: %zu windows (%d table + %zu extra), db and idle: %s "
           "(%d failed checks)\n",
           n, COV_TABLE_WINDOWS, n - (size_t)COV_TABLE_WINDOWS, failures ? "FAIL" : "PASS",
           failures);
    return failures ? 1 : 0;
}
