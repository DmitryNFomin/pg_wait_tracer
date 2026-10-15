/* test_open_interval.c — the #424 daemon half: one shared open-stretch
 * arithmetic, the frozen snapshot wire shape, and the control socket's
 * queued-write transport.
 *
 * Three things are pinned here, each for a different failure the live
 * open-interval seam can produce:
 *
 * 1. EQUIVALENCE. flush_open_intervals' per-pid arithmetic moved into
 *    src/open_interval.h so the de-escalation flush, the shutdown flush and
 *    the new snapshot cannot drift. The safety argument is "identical
 *    output", so the test is a DIFFERENTIAL against a verbatim copy of the
 *    pre-#424 loop body (ref_build_open_stretch below, transcribed from
 *    304a772 src/escalation.c:266-335) over a state matrix — plus a coverage
 *    assertion, because a differential whose inputs all take the same branch
 *    proves nothing, and two mutation probes proving it can go red.
 *
 * 2. WIRE SHAPE. The live server half codes against these field names and
 *    cannot renegotiate them. The JSON builder is pure, so the exact bytes
 *    are testable here rather than only on a box with PostgreSQL.
 *
 * 3. TRANSPORT. A reply of hundreds of KB does not fit one socket buffer, so
 *    EAGAIN is normal. The old sender dropped the client there, producing
 *    truncated JSON — a partial answer presented as data. legacy_send()
 *    below reproduces that truncation, and the queue does not.
 *
 * Pure: no daemon, no BPF, no PostgreSQL, no socket. Runs anywhere. */
#include "open_interval.h"
#include "open_snapshot.h"
#include "ctrl_outq.h"
#include "pg_wait_tracer.h"
#include "cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, fmt, ...) do {                                        \
    tests_run++;                                                          \
    if (!(cond)) {                                                        \
        tests_failed++;                                                   \
        printf("  FAIL(%d): " fmt "\n", __LINE__, ##__VA_ARGS__);         \
    }                                                                     \
} while (0)

#define S(x) ((uint64_t)(x) * 1000000000ULL)
#define MS(x) ((uint64_t)(x) * 1000000ULL)

/* ── 1. The reference: a VERBATIM copy of the pre-#424 loop body ─────────
 *
 * Transcribed from 304a772 src/escalation.c flush_open_intervals(), with the
 * registry/BPF lookups (which the builder never had) left to the caller. Do
 * not "clean this up": its value is that it is the deleted code. */
static int ref_build_open_stretch(uint32_t pid, const struct pgwt_pid_state *st,
                                  uint64_t now, bool cpu_accounting,
                                  const struct pgwt_open_stretch_attr *attr,
                                  struct pgwt_trace_event *out)
{
    if (!st->wp_live)
        return 0;               /* a seed entry — no interval to close */
    if (now <= st->last_ts)
        return 0;               /* zero/negative duration — nothing open */
    uint32_t we = st->last_event;
    if (PGWT_IS_MARKER(we))
        return 0;               /* never a real wait */
    uint64_t cpu_ns = PGWT_CPU_NS_UNKNOWN;
    if (cpu_accounting) {
        uint64_t exact = st->cpu_ns_total;
        if (st->on_cpu_ts && now >= st->on_cpu_ts)
            exact += now - st->on_cpu_ts;
        cpu_ns = (exact >= st->last_cpu_ns) ? exact - st->last_cpu_ns : 0;
    }
    uint16_t cmd_open = attr->cmd_open;
    uint16_t quality = attr->quality;
    struct pgwt_trace_event ev = {
        .timestamp_ns = now,
        .pid          = pid,
        .old_event    = we,
        .new_event    = we,
        .flags        = (cmd_open ? PGWT_EVENT_FLAG_CMD_OPEN : 0) |
            (quality == PGWT_QUERY_QUALITY_PG13_SYNTH
                ? PGWT_EVENT_FLAG_QUERY_SYNTH : 0),
        .duration_ns  = now - st->last_ts,
        .query_id     = attr->query_id,
        .cpu_ns       = cpu_ns,
    };
    *out = ev;
    return 1;
}

/* Mutation A: the measured-CPU underflow guard removed — the single line that
 * decides whether a backend whose seeded base exceeds its accumulator reports
 * 0 or 1.8e19 ns of CPU. */
static int mut_underflow(uint32_t pid, const struct pgwt_pid_state *st,
                         uint64_t now, bool cpu_accounting,
                         const struct pgwt_open_stretch_attr *attr,
                         struct pgwt_trace_event *out)
{
    int rc = ref_build_open_stretch(pid, st, now, cpu_accounting, attr, out);
    if (rc && cpu_accounting) {
        uint64_t exact = st->cpu_ns_total;
        if (st->on_cpu_ts && now >= st->on_cpu_ts)
            exact += now - st->on_cpu_ts;
        out->cpu_ns = exact - st->last_cpu_ns;   /* no >= guard */
    }
    return rc;
}

/* Mutation B: the PG13 synthetic-query flag dropped. */
static int mut_no_synth_flag(uint32_t pid, const struct pgwt_pid_state *st,
                             uint64_t now, bool cpu_accounting,
                             const struct pgwt_open_stretch_attr *attr,
                             struct pgwt_trace_event *out)
{
    int rc = ref_build_open_stretch(pid, st, now, cpu_accounting, attr, out);
    if (rc)
        out->flags &= ~(uint32_t)PGWT_EVENT_FLAG_QUERY_SYNTH;
    return rc;
}

typedef int (*ref_fn)(uint32_t, const struct pgwt_pid_state *, uint64_t, bool,
                      const struct pgwt_open_stretch_attr *,
                      struct pgwt_trace_event *);

struct matrix_cov {
    int built, skip_seed, skip_not_open, skip_marker;
    int cpu_unknown, cpu_zero, cpu_positive;
    int flag_cmd_open, flag_synth;
    int cases;
};

/* Run the whole state matrix through `fn` and the production builder.
 * Returns the number of DIFFERENCES found. */
static int run_matrix(ref_fn fn, struct matrix_cov *cov, bool verbose)
{
    const uint64_t now = S(1000);
    const uint32_t events[] = { 0, 0x06000004 /* a real wait */,
                                PGWT_MARKER_EXEC_START, PGWT_MARKER_CMD_END };
    const uint64_t last_ts[] = { now - S(2), now - MS(1), now, now + MS(3) };
    const uint64_t cpu_total[] = { 0, MS(40) };
    const uint64_t on_cpu[] = { 0, now - MS(10), now + MS(50) /* future */ };
    const uint64_t base[] = { 0, MS(10), MS(500) /* > exact: underflow bait */ };
    const uint16_t cmd[] = { 0, 1 };
    const uint16_t qual[] = { PGWT_QUERY_QUALITY_NONE,
                              PGWT_QUERY_QUALITY_REAL,
                              PGWT_QUERY_QUALITY_PG13_SYNTH };
    const uint16_t live[] = { 0, 1 };
    const bool acct[] = { false, true };
    int diffs = 0;
    memset(cov, 0, sizeof(*cov));

    for (size_t a = 0; a < sizeof(events) / sizeof(*events); a++)
    for (size_t b = 0; b < sizeof(last_ts) / sizeof(*last_ts); b++)
    for (size_t c = 0; c < sizeof(cpu_total) / sizeof(*cpu_total); c++)
    for (size_t e = 0; e < sizeof(on_cpu) / sizeof(*on_cpu); e++)
    for (size_t f = 0; f < sizeof(base) / sizeof(*base); f++)
    for (size_t g = 0; g < sizeof(cmd) / sizeof(*cmd); g++)
    for (size_t h = 0; h < sizeof(qual) / sizeof(*qual); h++)
    for (size_t i = 0; i < sizeof(live) / sizeof(*live); i++)
    for (size_t j = 0; j < sizeof(acct) / sizeof(*acct); j++) {
        struct pgwt_pid_state st = {
            .last_event   = events[a],
            .wp_live      = live[i],
            .cmd_open     = cmd[g],
            .last_ts      = last_ts[b],
            .last_query_id = 0x5150,
            .cpu_ns_total = cpu_total[c],
            .on_cpu_ts    = on_cpu[e],
            .last_cpu_ns  = base[f],
        };
        struct pgwt_open_stretch_attr attr = {
            .query_id = 0x5150, .cmd_open = cmd[g], .quality = qual[h],
        };
        struct pgwt_trace_event got, want;
        memset(&got, 0xA5, sizeof(got));
        memset(&want, 0xA5, sizeof(want));
        enum pgwt_open_stretch_rc rc =
            pgwt_build_open_stretch(4242, &st, now, acct[j], &attr, &got);
        int ref_built = fn(4242, &st, now, acct[j], &attr, &want);

        cov->cases++;
        switch (rc) {
        case PGWT_OPEN_STRETCH_BUILT:         cov->built++; break;
        case PGWT_OPEN_STRETCH_SKIP_SEED:     cov->skip_seed++; break;
        case PGWT_OPEN_STRETCH_SKIP_NOT_OPEN: cov->skip_not_open++; break;
        case PGWT_OPEN_STRETCH_SKIP_MARKER:   cov->skip_marker++; break;
        }
        if ((rc == PGWT_OPEN_STRETCH_BUILT) != (ref_built != 0)) {
            diffs++;
            if (verbose)
                printf("  DIFF build decision: rc=%d ref=%d we=%u live=%u\n",
                       (int)rc, ref_built, events[a], live[i]);
            continue;
        }
        if (rc != PGWT_OPEN_STRETCH_BUILT)
            continue;
        if (got.cpu_ns == PGWT_CPU_NS_UNKNOWN) cov->cpu_unknown++;
        else if (got.cpu_ns == 0)              cov->cpu_zero++;
        else                                   cov->cpu_positive++;
        if (got.flags & PGWT_EVENT_FLAG_CMD_OPEN)    cov->flag_cmd_open++;
        if (got.flags & PGWT_EVENT_FLAG_QUERY_SYNTH) cov->flag_synth++;
        if (memcmp(&got, &want, sizeof(got)) != 0) {
            diffs++;
            if (verbose)
                printf("  DIFF fields: we=%u acct=%d cpu got=%llu want=%llu "
                       "flags got=%u want=%u dur got=%llu want=%llu\n",
                       events[a], (int)acct[j],
                       (unsigned long long)got.cpu_ns,
                       (unsigned long long)want.cpu_ns,
                       got.flags, want.flags,
                       (unsigned long long)got.duration_ns,
                       (unsigned long long)want.duration_ns);
        }
    }
    return diffs;
}

static void test_builder_equivalence(void)
{
    printf("test_builder_equivalence\n");
    struct matrix_cov cov;
    int diffs = run_matrix(ref_build_open_stretch, &cov, true);
    CHECK(diffs == 0, "shared builder differs from the pre-#424 loop body in "
                      "%d of %d cases", diffs, cov.cases);

    /* A differential that never reaches the interesting branches is a gate
     * that cannot see. Every outcome the arithmetic can produce must occur in
     * the matrix, or the equivalence claim is vacuous. */
    CHECK(cov.cases > 500, "matrix too small: %d cases", cov.cases);
    CHECK(cov.built > 0, "no case BUILT a record");
    CHECK(cov.skip_seed > 0, "no case hit the seed skip");
    CHECK(cov.skip_not_open > 0, "no case hit the not-open skip");
    CHECK(cov.skip_marker > 0, "no case hit the marker skip");
    CHECK(cov.cpu_unknown > 0, "no case produced an UNMEASURED cpu_ns");
    CHECK(cov.cpu_zero > 0, "no case produced a measured-zero cpu_ns "
                            "(the underflow guard was never exercised)");
    CHECK(cov.cpu_positive > 0, "no case produced a positive cpu_ns");
    CHECK(cov.flag_cmd_open > 0, "no case set CMD_OPEN");
    CHECK(cov.flag_synth > 0, "no case set QUERY_SYNTH");
    printf("  coverage: %d cases, built=%d seed=%d not_open=%d marker=%d, "
           "cpu unknown/zero/pos=%d/%d/%d, flags cmd/synth=%d/%d\n",
           cov.cases, cov.built, cov.skip_seed, cov.skip_not_open,
           cov.skip_marker, cov.cpu_unknown, cov.cpu_zero, cov.cpu_positive,
           cov.flag_cmd_open, cov.flag_synth);

    /* Mutation probes: the differential must be able to go RED. If a mutated
     * arithmetic still compares equal, the comparison is not comparing. */
    struct matrix_cov mc;
    int m1 = run_matrix(mut_underflow, &mc, false);
    CHECK(m1 > 0, "mutation A (CPU underflow guard removed) was NOT detected "
                  "— the differential cannot see cpu_ns");
    int m2 = run_matrix(mut_no_synth_flag, &mc, false);
    CHECK(m2 > 0, "mutation B (QUERY_SYNTH flag dropped) was NOT detected "
                  "— the differential cannot see flags");
    printf("  mutation probes: underflow=%d diffs, synth_flag=%d diffs\n",
           m1, m2);
}

/* ── 2. The frozen wire shape ───────────────────────────────────────────── */

static struct pgwt_open_snapshot *new_snapshot(void)
{
    struct pgwt_open_snapshot *s = calloc(1, sizeof(*s));
    if (!s) {
        printf("  FATAL: out of memory\n");
        exit(2);
    }
    s->status = PGWT_OPEN_SNAP_INCLUDED;
    s->complete = true;
    s->snapshot_ns = S(1000);
    s->now_mono_ns = S(1000) + MS(2);
    s->now_wall_ns = 1760000000000000000ULL;
    s->snapshot_wall_ns = pgwt_open_snapshot_wall_of(s, s->snapshot_ns);
    s->committed_through_ns = S(1000) + MS(3);
    s->daemon_generation = 7;
    s->daemon_pid = 4242;
    s->daemon_start_wall_ns = s->now_wall_ns - S(60);
    s->cpu_accounting = true;
    s->cmd_gate_active = true;
    snprintf(s->tier, sizeof(s->tier), "escalated");
    return s;
}

static void add_row(struct pgwt_open_snapshot *s, uint32_t pid, uint32_t we,
                    uint64_t cpu_ns)
{
    struct pgwt_open_row *r = &s->rows[s->n_rows++];
    r->pid = pid;
    r->incarnation = S(900) + pid;
    r->generation = 7;
    r->event_id = we;
    r->cat_flag = we == 0 ? 0 : PGWT_EVENT_FLAG_IO_WORKER;
    r->eff_event = we;
    r->flags = PGWT_EVENT_FLAG_CMD_OPEN;
    r->open_start_ns = S(998);
    r->duration_ns = s->snapshot_ns - r->open_start_ns;
    r->cpu_ns = cpu_ns;
    r->cpu_ns_total = MS(900);
    r->on_cpu_ts = S(999);
    r->last_cpu_ns = MS(100);
    r->query_id = 0xDEADBEEFULL;
    r->cmd_open_ns = MS(1200);
    r->cmd_open = true;
    r->cmd_in_window = true;
    r->cmd_by_markers = true;
    r->query_synth = false;
    s->open_count = s->n_rows;
}

static cJSON *json_of(const struct pgwt_open_snapshot *s, size_t cap,
                      char **out_str)
{
    cJSON *root = pgwt_open_snapshot_to_json(s, cap);
    if (!root) {
        if (out_str) *out_str = NULL;
        return NULL;
    }
    char *str = cJSON_PrintUnformatted(root);
    if (out_str)
        *out_str = str;
    else
        free(str);
    return root;
}

/* Exact u64 straight out of the TEXT. cJSON parses every number into a
 * double, which silently rounds a wall-clock nanosecond value (1.76e18 is far
 * past 2^53) — so a test that read wall fields through cJSON would be unable
 * to tell a correct anchor from one off by hundreds of nanoseconds. The
 * daemon avoids the same trap by emitting u64s as raw JSON; this reads them
 * back the same way. Top-level keys only (they do not recur in rows). */
static uint64_t raw_u64(const char *json, const char *key, bool *found)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    *found = p != NULL;
    if (!p)
        return 0;
    return strtoull(p + strlen(pat), NULL, 10);
}

static uint64_t u64_of(cJSON *o, const char *k, bool *found)
{
    cJSON *it = cJSON_GetObjectItem(o, k);
    *found = it != NULL && cJSON_IsNumber(it);
    if (!*found)
        return 0;
    return (uint64_t)it->valuedouble;
}

static void test_json_shape(void)
{
    printf("test_json_shape\n");
    struct pgwt_open_snapshot *s = new_snapshot();
    add_row(s, 101, 0, MS(37));                      /* on-CPU, measured */
    add_row(s, 102, 0x06000004, PGWT_CPU_NS_UNKNOWN);/* wait, unmeasured */
    add_row(s, 103, 0x01000003, MS(0));              /* wait, measured zero */

    char *str = NULL;
    cJSON *root = json_of(s, PGWT_OPEN_INTERVALS_REPLY_MAX, &str);
    CHECK(root != NULL && str != NULL, "snapshot did not serialise");
    if (!root || !str) { free(s); return; }

    cJSON *parsed = cJSON_Parse(str);
    CHECK(parsed != NULL, "the reply does not parse as JSON");
    if (!parsed) { free(str); cJSON_Delete(root); free(s); return; }

    bool f;
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(parsed, "ok")), "ok must be true");
    CHECK(strcmp(cJSON_GetObjectItem(parsed, "status")->valuestring,
                 "included") == 0, "status must be included");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(parsed, "complete")),
          "complete must be true");
    /* Every top-level field the consumer cannot repair if it is missing. */
    const char *req[] = { "snapshot_ns", "snapshot_wall_ns", "now_mono_ns",
                          "now_wall_ns", "committed_through_ns",
                          "committed_through_wall_ns", "daemon_generation",
                          "daemon_start_wall_ns", "daemon_pid", "open_count" };
    for (size_t i = 0; i < sizeof(req) / sizeof(*req); i++) {
        u64_of(parsed, req[i], &f);
        CHECK(f, "top-level field %s missing", req[i]);
    }
    CHECK(cJSON_GetObjectItem(parsed, "tier") != NULL, "tier missing");
    CHECK(cJSON_GetObjectItem(parsed, "reason") != NULL, "reason missing");
    CHECK(u64_of(parsed, "snapshot_ns", &f) == s->snapshot_ns,
          "snapshot_ns wrong");
    CHECK(u64_of(parsed, "committed_through_ns", &f) >=
          u64_of(parsed, "snapshot_ns", &f),
          "committed_through_ns must not precede snapshot_ns: the state map "
          "is read BEFORE publication");
    /* The anchor must convert, and must not be the server's per-file
     * offset: wall(snapshot) == now_wall - (now_mono - snapshot). Read from
     * the TEXT: a double round trip would hide a sub-microsecond error. */
    CHECK(raw_u64(str, "snapshot_wall_ns", &f) ==
              s->now_wall_ns - (s->now_mono_ns - s->snapshot_ns),
          "snapshot_wall_ns is not the anchor conversion");
    CHECK(raw_u64(str, "now_wall_ns", &f) == s->now_wall_ns,
          "now_wall_ns lost precision on the wire (u64 must be raw JSON, "
          "never a double)");
    CHECK(raw_u64(str, "committed_through_wall_ns", &f) ==
              s->now_wall_ns - (s->now_mono_ns - s->committed_through_ns),
          "committed_through_wall_ns is not the anchor conversion");
    CHECK((uint64_t)(double)s->now_wall_ns != s->now_wall_ns ||
          raw_u64(str, "now_wall_ns", &f) == s->now_wall_ns,
          "precision check is vacuous on this platform");

    cJSON *pids = cJSON_GetObjectItem(parsed, "pids");
    CHECK(cJSON_IsArray(pids), "pids must be an array when included");
    CHECK(cJSON_GetArraySize(pids) == 3, "expected 3 rows, got %d",
          cJSON_GetArraySize(pids));
    cJSON *r0 = cJSON_GetArrayItem(pids, 0);
    const char *rowreq[] = { "pid", "incarnation", "generation", "event_id",
                             "cat_flag", "eff_event", "flags",
                             "open_start_ns", "duration_ns", "cpu_ns_total",
                             "on_cpu_ts", "last_cpu_ns", "query_id",
                             "cmd_open_ns" };
    for (size_t i = 0; i < sizeof(rowreq) / sizeof(*rowreq); i++) {
        u64_of(r0, rowreq[i], &f);
        CHECK(f, "row field %s missing", rowreq[i]);
    }
    CHECK(cJSON_GetObjectItem(r0, "cmd_open") != NULL, "cmd_open missing");
    CHECK(cJSON_GetObjectItem(r0, "cmd_in_window") != NULL,
          "cmd_in_window missing");
    CHECK(cJSON_GetObjectItem(r0, "cmd_by_markers") != NULL,
          "cmd_by_markers missing");
    CHECK(u64_of(r0, "duration_ns", &f) == S(2), "duration_ns wrong");
    CHECK(u64_of(r0, "cpu_ns", &f) == MS(37), "measured cpu_ns wrong");

    /* UNKNOWN is ABSENT, not zero — a consumer must be able to tell "no CPU
     * measurement" from "measured no CPU". */
    cJSON *r1 = cJSON_GetArrayItem(pids, 1);
    CHECK(cJSON_GetObjectItem(r1, "cpu_ns") == NULL,
          "an unmeasured cpu_ns must be ABSENT, never serialised");
    cJSON *r2 = cJSON_GetArrayItem(pids, 2);
    u64_of(r2, "cpu_ns", &f);
    CHECK(f && u64_of(r2, "cpu_ns", &f) == 0,
          "a measured-zero cpu_ns must be PRESENT and 0");

    cJSON_Delete(parsed);
    cJSON_Delete(root);
    free(str);
    free(s);
}

/* An INCLUDED snapshot with nothing open is a measurement: pids present and
 * empty, complete true. This is the "the thing is absent, not wrong" case —
 * if the collector silently returned this on a tier that cannot observe
 * intervals, the UI would read "no in-flight waits" where the truth is "no
 * answer". The refusal test below is what keeps those apart. */
static void test_empty_but_included(void)
{
    printf("test_empty_but_included\n");
    struct pgwt_open_snapshot *s = new_snapshot();
    char *str = NULL;
    cJSON *root = json_of(s, PGWT_OPEN_INTERVALS_REPLY_MAX, &str);
    cJSON *p = str ? cJSON_Parse(str) : NULL;
    CHECK(p != NULL, "empty snapshot did not serialise");
    if (p) {
        cJSON *pids = cJSON_GetObjectItem(p, "pids");
        CHECK(cJSON_IsArray(pids) && cJSON_GetArraySize(pids) == 0,
              "an included-but-empty snapshot must carry an EMPTY pids array");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(p, "complete")),
              "nothing open is a complete answer");
        bool f;
        CHECK(u64_of(p, "open_count", &f) == 0, "open_count must be 0");
        cJSON_Delete(p);
    }
    free(str);
    cJSON_Delete(root);
    free(s);
}

static void check_degraded(const char *what,
                           enum pgwt_open_snapshot_status status,
                           const char *reason, int rows)
{
    struct pgwt_open_snapshot *s = new_snapshot();
    for (int i = 0; i < rows; i++)
        add_row(s, 200 + (uint32_t)i, 0, MS(5));
    s->status = status;
    s->complete = false;
    pgwt_open_snapshot_set_reason(s, reason);

    char *str = NULL;
    cJSON *root = json_of(s, PGWT_OPEN_INTERVALS_REPLY_MAX, &str);
    cJSON *p = str ? cJSON_Parse(str) : NULL;
    CHECK(p != NULL, "%s: did not serialise", what);
    if (p) {
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(p, "ok")),
              "%s: ok must be false", what);
        CHECK(strcmp(cJSON_GetObjectItem(p, "status")->valuestring,
                     pgwt_open_snapshot_status_str(status)) == 0,
              "%s: wrong status string", what);
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(p, "complete")),
              "%s: complete must be false", what);
        /* The load-bearing one: rows must NOT be published under a degraded
         * status, even when the collector happened to fill some in. A short
         * array read as data is exactly the silent under-count this seam
         * exists to prevent. */
        CHECK(cJSON_GetObjectItem(p, "pids") == NULL,
              "%s: pids must be ABSENT (not empty, not partial) when the "
              "answer is not trustworthy", what);
        cJSON *rs = cJSON_GetObjectItem(p, "reason");
        CHECK(rs && rs->valuestring && strlen(rs->valuestring) > 10,
              "%s: reason must be specific enough to render as a label",
              what);
        cJSON_Delete(p);
    }
    free(str);
    cJSON_Delete(root);
    free(s);
}

static void test_degraded_statuses(void)
{
    printf("test_degraded_statuses\n");
    check_degraded("refused/lightweight", PGWT_OPEN_SNAP_REFUSED,
                   "lightweight tier: BPF accumulator only, no per-backend "
                   "open intervals exist", 0);
    check_degraded("refused/sampled", PGWT_OPEN_SNAP_REFUSED,
                   "sampled tier: no watchpoints armed; escalate for "
                   "in-flight waits", 0);
    /* With rows present: a degraded status must still suppress them. */
    check_degraded("unavailable/with rows", PGWT_OPEN_SNAP_UNAVAILABLE,
                   "event ring still backlogged after the bounded drain; "
                   "committed_through_ns is a floor", 3);
}

/* 1024 backends: the size the transport must actually carry, measured rather
 * than assumed — 64 KB would have been a silent truncation at ~170 rows. */
static void test_full_house(void)
{
    printf("test_full_house\n");
    struct pgwt_open_snapshot *s = new_snapshot();
    for (int i = 0; i < MAX_BACKENDS; i++)
        add_row(s, 10000 + (uint32_t)i, (uint32_t)(i % 2 ? 0x06000004 : 0),
                i % 3 == 0 ? PGWT_CPU_NS_UNKNOWN : MS(i));
    CHECK(s->n_rows == MAX_BACKENDS, "expected %d rows", MAX_BACKENDS);

    char *str = NULL;
    cJSON *root = json_of(s, PGWT_OPEN_INTERVALS_REPLY_MAX, &str);
    CHECK(root && str, "1024-row snapshot did not serialise");
    size_t bytes = str ? strlen(str) + 1 : 0;
    printf("  1024 backends serialise to %zu bytes (cap %u, 64 KB would be "
           "%s)\n", bytes, PGWT_OPEN_INTERVALS_REPLY_MAX,
           bytes > 65536 ? "TRUNCATED" : "enough");
    CHECK(bytes > 65536, "the 64 KB buffer the first contract specified must "
                         "be demonstrably insufficient: %zu bytes", bytes);
    CHECK(bytes + 1 <= PGWT_OPEN_INTERVALS_REPLY_MAX,
          "a full house (%zu bytes) must fit the production cap %u", bytes,
          PGWT_OPEN_INTERVALS_REPLY_MAX);
    if (str) {
        cJSON *p = cJSON_Parse(str);
        CHECK(p != NULL, "the 1024-row reply must parse");
        if (p) {
            CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(p, "pids")) ==
                  MAX_BACKENDS, "all 1024 rows must survive the round trip");
            cJSON_Delete(p);
        }
    }

    /* Over-cap: the reply is replaced WHOLE, never truncated. Reached here
     * with the 64 KB number the superseded contract specified. */
    char *str2 = NULL;
    cJSON *root2 = json_of(s, 65536, &str2);
    CHECK(root2 && str2, "the over-cap reply must still be a valid reply");
    if (str2) {
        cJSON *p = cJSON_Parse(str2);
        CHECK(p != NULL, "the over-cap reply must parse (not be truncated)");
        if (p) {
            CHECK(strcmp(cJSON_GetObjectItem(p, "status")->valuestring,
                         "unavailable") == 0,
                  "an over-cap reply must report unavailable");
            CHECK(cJSON_GetObjectItem(p, "pids") == NULL,
                  "an over-cap reply must carry NO pids");
            CHECK(cJSON_IsFalse(cJSON_GetObjectItem(p, "complete")),
                  "an over-cap reply is not complete");
            bool f;
            CHECK(u64_of(p, "open_count", &f) == MAX_BACKENDS,
                  "an over-cap reply still reports HOW MANY it could not send");
            CHECK(strlen(str2) + 1 <= 65536,
                  "the replacement must itself fit the cap");
            cJSON_Delete(p);
        }
        free(str2);
    }
    cJSON_Delete(root2);

    /* A cap too small for even the replacement: refuse (NULL), never emit
     * something that parses as a successful empty answer. */
    cJSON *root3 = json_of(s, 16, NULL);
    CHECK(root3 == NULL, "an impossibly small cap must refuse, not approve");
    cJSON_Delete(root3);

    free(str);
    cJSON_Delete(root);
    free(s);
}

/* A snapshot whose anchor was never filled must not fabricate a wall clock. */
static void test_anchor_absent(void)
{
    printf("test_anchor_absent\n");
    struct pgwt_open_snapshot *s = calloc(1, sizeof(*s));
    s->status = PGWT_OPEN_SNAP_INCLUDED;
    s->snapshot_ns = S(5);
    CHECK(pgwt_open_snapshot_wall_of(s, s->snapshot_ns) == 0,
          "with no anchor the wall conversion must be 0, never a guess");
    /* And a mono value far below the anchor's own base must not wrap. */
    s->now_mono_ns = S(1000);
    s->now_wall_ns = S(10);
    CHECK(pgwt_open_snapshot_wall_of(s, 1) == 0,
          "a negative wall conversion must clamp to 0, not wrap");
    free(s);
}

/* ── 3. Transport: queued writes with backpressure ─────────────────────── */

struct fake_sock {
    char    got[1200000];
    size_t  n;
    size_t  chunk;        /* max bytes accepted per call (0 = unlimited) */
    int     eagain_every; /* return EAGAIN on every Nth call (0 = never) */
    int     calls;
    int     fail_at;      /* hard error on this call number (0 = never) */
    int     zero_at;      /* return 0 on this call number (0 = never) */
};

static ssize_t fake_write(void *ctx, const char *p, size_t n)
{
    struct fake_sock *f = (struct fake_sock *)ctx;
    f->calls++;
    if (f->fail_at && f->calls == f->fail_at) {
        errno = EPIPE;
        return -1;
    }
    if (f->zero_at && f->calls == f->zero_at)
        return 0;
    if (f->eagain_every && f->calls % f->eagain_every == 0) {
        errno = EAGAIN;
        return -1;
    }
    size_t take = f->chunk && n > f->chunk ? f->chunk : n;
    if (f->n + take > sizeof(f->got))
        take = sizeof(f->got) - f->n;
    memcpy(f->got + f->n, p, take);
    f->n += take;
    return (ssize_t)take;
}

/* The pre-#424 sender, verbatim in behaviour: loop on send, give up (and the
 * caller drops the client) on EAGAIN. Used to DEMONSTRATE the truncation the
 * queue removes — this is what red looks like. */
static size_t legacy_send(struct fake_sock *f, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = fake_write(f, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;      /* EAGAIN included: drop the client mid-reply */
        }
        off += (size_t)w;
    }
    return off;
}

static void test_transport_queue(void)
{
    printf("test_transport_queue\n");
    /* A realistic payload: the full-house reply. */
    struct pgwt_open_snapshot *s = new_snapshot();
    for (int i = 0; i < MAX_BACKENDS; i++)
        add_row(s, 10000 + (uint32_t)i, 0, MS(i));
    char *str = NULL;
    cJSON *root = json_of(s, PGWT_OPEN_INTERVALS_REPLY_MAX, &str);
    size_t len = strlen(str);
    str[len] = '\n';
    size_t total = len + 1;
    free(s);

    /* (a) The old sender on a socket that takes 16 KB then says EAGAIN:
     *     TRUNCATED. This is the defect, reproduced. */
    struct fake_sock legacy = { .chunk = 16384, .eagain_every = 2 };
    size_t sent = legacy_send(&legacy, str, total);
    CHECK(sent < total, "the legacy sender must truncate here, else this test "
                        "proves nothing (sent %zu of %zu)", sent, total);
    printf("  legacy sender: %zu of %zu bytes before EAGAIN (truncated)\n",
           sent, total);

    /* (b) The queue, same socket: nothing is lost, and it reports that it is
     *     not finished so the caller arms EPOLLOUT. */
    struct pgwt_ctrl_outq q;
    pgwt_outq_init(&q);
    struct fake_sock sock = { .chunk = 16384, .eagain_every = 2 };
    CHECK(pgwt_outq_append(&q, str, total) == 0, "append failed");
    int rc = pgwt_outq_flush(&q, fake_write, &sock);
    CHECK(rc == 0, "a partially-written reply must report 'more to do' (0), "
                   "got %d", rc);
    CHECK(pgwt_outq_pending(&q) > 0, "bytes must remain queued");
    int rounds = 0;
    while (rc == 0 && rounds++ < 10000)
        rc = pgwt_outq_flush(&q, fake_write, &sock);
    CHECK(rc == 1, "the queue must eventually drain, got %d after %d rounds",
          rc, rounds);
    CHECK(sock.n == total, "every byte must arrive: %zu of %zu", sock.n,
          total);
    CHECK(memcmp(sock.got, str, total) == 0,
          "the delivered bytes must be identical to the reply");
    printf("  queued sender: %zu bytes delivered in %d flushes\n", sock.n,
           rounds);

    /* (c) One byte at a time — the most fragmented reply there is. */
    pgwt_outq_reset(&q);
    pgwt_outq_init(&q);
    struct fake_sock drip = { .chunk = 1 };
    CHECK(pgwt_outq_append(&q, str, total) == 0, "append failed");
    rc = 0; rounds = 0;
    while (rc == 0 && rounds++ < 2000000)
        rc = pgwt_outq_flush(&q, fake_write, &drip);
    CHECK(rc == 1 && drip.n == total && memcmp(drip.got, str, total) == 0,
          "a 1-byte-per-write socket must still deliver the whole reply");

    /* (d) A zero-byte write is "would block", not an infinite loop. */
    pgwt_outq_reset(&q);
    pgwt_outq_init(&q);
    struct fake_sock stall = { .chunk = 4096, .zero_at = 1 };
    CHECK(pgwt_outq_append(&q, str, total) == 0, "append failed");
    rc = pgwt_outq_flush(&q, fake_write, &stall);
    CHECK(rc == 0 && stall.calls == 1,
          "a zero-byte write must return control immediately (rc=%d calls=%d)",
          rc, stall.calls);

    /* (e) A hard error is fatal — the client is dropped, not left half-fed. */
    pgwt_outq_reset(&q);
    pgwt_outq_init(&q);
    struct fake_sock broken = { .chunk = 4096, .fail_at = 3 };
    CHECK(pgwt_outq_append(&q, str, total) == 0, "append failed");
    rc = pgwt_outq_flush(&q, fake_write, &broken);
    CHECK(rc == -1, "a hard write error must be fatal, got %d", rc);

    /* (f) A client that never reads: the backlog is CAPPED and the append
     *     refuses, so the daemon neither grows without bound nor sends a
     *     shortened reply. */
    pgwt_outq_reset(&q);
    pgwt_outq_init(&q);
    struct fake_sock deaf = { .eagain_every = 1 };
    int appended = 0, refused = 0;
    for (int i = 0; i < 64; i++) {
        if (pgwt_outq_append(&q, str, total) == 0) {
            appended++;
            pgwt_outq_flush(&q, fake_write, &deaf);
        } else {
            refused++;
            break;
        }
    }
    CHECK(refused == 1, "the backlog cap must eventually refuse");
    CHECK(q.overflow, "an over-cap queue must be marked unrecoverable");
    CHECK(pgwt_outq_pending(&q) <= PGWT_CTRL_OUTQ_MAX,
          "the backlog must stay under the cap");
    CHECK(deaf.n == 0, "nothing can be delivered to a client that never reads");
    CHECK(pgwt_outq_append(&q, "x", 1) == -1,
          "an overflowed queue must keep refusing: a later short reply would "
          "be a partial answer");
    printf("  backlog cap: %d replies (%zu bytes each) queued, then refused\n",
           appended, total);

    pgwt_outq_reset(&q);
    free(str);
    cJSON_Delete(root);
}

int main(void)
{
    printf("=== test_open_interval ===\n");
    test_builder_equivalence();
    test_json_shape();
    test_empty_but_included();
    test_degraded_statuses();
    test_full_house();
    test_anchor_absent();
    test_transport_queue();
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
