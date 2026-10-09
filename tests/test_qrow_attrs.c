/* test_qrow_attrs.c — #324: the two per-ROW full scans of the Queries tab.
 *
 * THE COST. handle_top_queries answered two per-row questions by walking the
 * ENTIRE loaded event window, once each, FOR EVERY ROW:
 *
 *   query_context_for_events(srv, all_events, ecount, row.query_id, ...)
 *   for (e = 0; !synthetic_quality && e < ecount; e++) ...
 *
 * 2 x num_rows passes. On the 2026-10-08 demo capture (900 s, 5,699,738
 * events, 26 rows, 48 B per record) that is 52 passes over 274 MB = 14.2 GB
 * of memory traffic, measured at 1778 ms of a 4491 ms response — 40 % of the
 * worst tab in the product, to decorate 26 rows.
 *
 * pgwt_compute_qrow_attrs answers every row in ONE pass. The entire safety
 * argument is that the answers are IDENTICAL, so this file is a differential
 * against a VERBATIM COPY of the two deleted loops (§1), over fixtures built
 * to make every verdict reachable, plus literal hand-computed expectations
 * (§2) because an oracle and its subject can share a bug — which is why §5
 * mutates BOTH SIDES and requires each mutation to be caught.
 *
 * §3/§4 ARE ABOUT FALSE NEGATIVES. A per-row attribute table's failure mode
 * is not a wrong number, it is a row that reads as "no context, no synth"
 * when the computation never looked at anything. Every verdict in this
 * function's range is reachable by an empty window (context 0), so "0 == 0"
 * satisfies a careless assertion. §3 pins non-vacuity before anything is
 * compared; §4 is one case per way the pass can be made unable to see:
 *   - an empty window, a NULL event array, zero requested rows
 *   - a resolver that is absent (NULL) or can never answer
 *   - duplicate query_ids (only the first index is in the map)
 *   - query_id 0 and pid 0 as ORDINARY keys, not empty-slot sentinels
 *   - the -1 verdict reached by the FIRST and by the LAST record
 *   - has_synth on a row whose context is already poisoned
 *   - every injected allocation failure: ABSENT (failed=1, rows NULL,
 *     num_rows 0), never short-but-complete-looking
 *   - an UNKNOWN injection point, so an injection that silently did nothing
 *     cannot be what made the allocation cases pass
 *   - more distinct pids than the memo's initial table holds, so the grow
 *     path is exercised rather than assumed
 *
 * Runs anywhere: in-memory arrays, synthetic timestamps, a deterministic
 * PRNG, no clock, no I/O, no threads. Nothing here can be flaky.
 */
#include "compute.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"
#include "summary_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do {                                        \
    checks++;                                                        \
    if (!(cond)) { failures++;                                       \
        printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); }   \
    else printf("  ok: %s\n", msg);                                  \
} while (0)

#define CHECK_INT(got, want, msg) do {                               \
    checks++;                                                        \
    long _g = (long)(got), _w = (long)(want);                        \
    if (_g != _w) { failures++;                                      \
        printf("  FAIL: %s — got %ld, want %ld (%s:%d)\n", msg,      \
               _g, _w, __FILE__, __LINE__); }                        \
    else printf("  ok: %s (%ld)\n", msg, _g);                        \
} while (0)

/* compute.c calls into the summary reader; this test never touches a trace
 * directory, so the symbol is satisfied with a refusal. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    (void)visitor; (void)ctx;
    return -1;
}

/* ── The backend map, as the test models it ──────────────────────────────
 * One entry per pid, mirroring struct bm_entry's three rejection causes:
 * absent, context_ambiguous, or a zero databaseid/userid. */
struct tbm {
    uint32_t pid;
    uint32_t databaseid, userid;
    int      ambiguous;
};

struct tbm_map { const struct tbm *e; int n; };

static const struct tbm *tbm_find(const struct tbm_map *m, uint32_t pid)
{
    for (int i = 0; i < m->n; i++)
        if (m->e[i].pid == pid)
            return &m->e[i];
    return NULL;
}

/* VERBATIM the three rejections from src/server.c's bm_pid_context_cb,
 * which are verbatim the ones from the deleted query_context_for_events. */
static int tbm_resolve(void *ctx, uint32_t pid,
                       uint32_t *databaseid, uint32_t *userid)
{
    const struct tbm_map *m = ctx;
    const struct tbm *bm = tbm_find(m, pid);
    if (!bm || bm->ambiguous || !bm->databaseid || !bm->userid)
        return 0;
    *databaseid = bm->databaseid;
    *userid = bm->userid;
    return 1;
}

/* A counting wrapper. The pid memo's entire purpose is ONE resolver call per
 * DISTINCT pid; without it the old code called it once per matching record
 * per row. Counting the calls is what makes a broken memo visible as
 * something other than "slower" — a silently no-op insert into a full table
 * leaves every answer correct and only the call count wrong, so an
 * answers-only assertion cannot see it. */
static long resolve_calls = 0;
static int tbm_resolve_counting(void *ctx, uint32_t pid,
                                uint32_t *databaseid, uint32_t *userid)
{
    resolve_calls++;
    return tbm_resolve(ctx, pid, databaseid, userid);
}

/* A resolver that can NEVER answer — the "missing dependency" case. */
static int resolve_never(void *ctx, uint32_t pid,
                         uint32_t *databaseid, uint32_t *userid)
{
    (void)ctx; (void)pid; (void)databaseid; (void)userid;
    return 0;
}

/* ── §1 ORACLE: a verbatim copy of the two deleted loops ─────────────────
 *
 * Transcribed from src/server.c at f78ab4c. The ONLY edit is that
 * bm_map_lookup + its three rejections are reached through the same
 * resolver callback, so both sides see one definition of "usable context"
 * and a divergence can only come from the SCAN, which is what changed.
 */
static int oracle_context(const struct pgwt_trace_event *events, int count,
                          uint64_t query_id, pgwt_pid_ctx_fn resolve,
                          void *rctx, uint32_t *databaseid, uint32_t *userid)
{
    *databaseid = 0;
    *userid = 0;
    int found = 0;
    for (int i = 0; events && i < count; i++) {
        if (events[i].query_id != query_id)
            continue;
        uint32_t db = 0, uid = 0;
        if (!resolve || !resolve(rctx, events[i].pid, &db, &uid)) {
            *databaseid = 0;
            *userid = 0;
            return -1;
        }
        if (!found) {
            *databaseid = db;
            *userid = uid;
            found = 1;
        } else if (*databaseid != db || *userid != uid) {
            *databaseid = 0;
            *userid = 0;
            return -1;
        }
    }
    return found ? 1 : 0;
}

static int oracle_synth(const struct pgwt_trace_event *events, int count,
                        uint64_t query_id)
{
    int synthetic_quality = 0;
    for (int e = 0; !synthetic_quality && events && e < count; e++)
        if (events[e].query_id == query_id &&
            (events[e].flags & PGWT_EVENT_FLAG_QUERY_SYNTH))
            synthetic_quality = 1;
    return synthetic_quality;
}

/* Mutation hooks (§5). Zero in every normal run; each flips one side of the
 * differential so the comparator is shown to have teeth. */
static int mut_new_context_sign  = 0;  /* new side: report -1 as 0 */
static int mut_new_synth_off     = 0;  /* new side: drop has_synth */
static int mut_new_dup_skip      = 0;  /* new side: leave duplicates unset */
static int mut_oracle_first_only = 0;  /* ORACLE: stop after first match */
static int mut_oracle_synth_on   = 0;  /* ORACLE: always claim synth */

/* The comparator. Returns the number of DIFFERENCES found; 0 = agreement.
 * It compares all five observable fields, with no epsilon anywhere: every
 * one is an integer verdict or an exact id. */
static int differential(const struct pgwt_trace_event *events, int count,
                        const uint64_t *qids, int nq,
                        pgwt_pid_ctx_fn resolve, void *rctx,
                        const char *label, int verbose)
{
    struct pgwt_qrow_attr_result got = {0};
    int rc = pgwt_compute_qrow_attrs(events, count, qids, nq,
                                     resolve, rctx, &got);
    if (rc != 0 || got.num_rows != nq) {
        if (verbose)
            printf("  FAIL: %s — fast path refused (rc=%d rows=%d want %d)\n",
                   label, rc, got.num_rows, nq);
        pgwt_qrow_attrs_free(&got);
        return 1;
    }

    int diffs = 0;
    for (int i = 0; i < nq; i++) {
        uint32_t odb = 0, ouid = 0;
        int octx = oracle_context(events, count, qids[i], resolve, rctx,
                                  &odb, &ouid);
        int osyn = oracle_synth(events, count, qids[i]);

        if (mut_oracle_first_only) {
            /* ORACLE MUTATION: trust only the first matching record, the
             * classic "forgot that -1 is absorbing" bug. */
            octx = 0; odb = 0; ouid = 0;
            for (int e = 0; e < count; e++) {
                if (events[e].query_id != qids[i]) continue;
                uint32_t db = 0, uid = 0;
                octx = resolve(rctx, events[e].pid, &db, &uid) ? 1 : -1;
                odb = octx == 1 ? db : 0;
                ouid = octx == 1 ? uid : 0;
                break;
            }
        }
        if (mut_oracle_synth_on)
            osyn = 1;                       /* ORACLE MUTATION */

        int gctx = got.rows[i].context;
        int gsyn = got.rows[i].has_synth;
        uint32_t gdb = got.rows[i].databaseid, guid = got.rows[i].userid;
        if (mut_new_context_sign && gctx < 0) gctx = 0;   /* NEW MUTATION */
        if (mut_new_synth_off) gsyn = 0;                  /* NEW MUTATION */
        if (mut_new_dup_skip) {
            for (int j = 0; j < i; j++)
                if (qids[j] == qids[i]) { gctx = 0; gdb = 0; guid = 0; gsyn = 0; }
        }

        if (got.rows[i].query_id != qids[i] || gctx != octx || gdb != odb ||
            guid != ouid || gsyn != osyn) {
            diffs++;
            if (verbose)
                printf("  FAIL: %s row %d qid=%llu — new {ctx=%d db=%u uid=%u "
                       "syn=%d} oracle {ctx=%d db=%u uid=%u syn=%d}\n",
                       label, i, (unsigned long long)qids[i],
                       gctx, gdb, guid, gsyn, octx, odb, ouid, osyn);
        }
    }
    pgwt_qrow_attrs_free(&got);
    return diffs;
}

/* ── Fixtures ────────────────────────────────────────────────────────────
 *
 * STRATIFIED BY QUERY ID, and that is the whole point. The first version of
 * this fixture drew pids uniformly at random with ~1 in 5 unresolvable; over
 * 4000 records and 27 ids, EVERY id met a bad pid, so all 27 rows came back
 * -1 and all 27 came back has_synth=1. §1 was green, §0 recorded
 * "unique=0 none=0 ambiguous=27", and §5's oracle mutation "always claim
 * synth" was NOT DETECTED — the fixture could not tell a correct answer
 * from "report -1 and synth for everything". A uniformly random fixture is
 * a BLIND fixture here; the id classes below are what make each verdict
 * separately falsifiable:
 *
 *   class U (id % 4 == 0): only CLEAN pids sharing one (databaseid,
 *       userid)  -> context 1
 *   class D (id % 4 == 1): clean pids from TWO different contexts
 *                          -> context -1 by disagreement
 *   class B (id % 4 == 2): clean pids plus one UNUSABLE pid, cycling
 *       through all four rejection causes (absent / ambiguous / zero
 *       databaseid / zero userid)  -> context -1 by rejection
 *   class N (id % 4 == 3): no records at all  -> context 0
 *
 * The synth flag is keyed on the RECORD INDEX, not the id class, so
 * has_synth is independent of context and roughly half the claimed ids
 * carry it — neither "always 0" nor "always 1" can satisfy §1.
 *
 * A deterministic PRNG orders the records, so a failure is reproducible and
 * a run can never be flaky.
 */
static uint64_t rng_s = 0x243F6A8885A308D3ULL;
static uint32_t rng(void)
{
    rng_s ^= rng_s >> 12; rng_s ^= rng_s << 25; rng_s ^= rng_s >> 27;
    return (uint32_t)((rng_s * 0x2545F4914F6CDD1DULL) >> 32);
}
static void rng_reset(void) { rng_s = 0x243F6A8885A308D3ULL; }

#define FIX_QIDS  23      /* ids 0..22, classes as above */
#define CLEAN_A_0 1000    /* 1000..1019: (16384, 7) */
#define CLEAN_B_0 1020    /* 1020..1039: (16385, 9) */
#define BAD_0     1040    /* 1040..1043: the four rejection causes */
#define FIX_PIDS  44

static struct tbm fix_bm[FIX_PIDS];
static struct tbm_map fix_map = { fix_bm, FIX_PIDS };

static void build_bm(void)
{
    int k = 0;
    for (int i = 0; i < 20; i++, k++) {
        fix_bm[k].pid = CLEAN_A_0 + (uint32_t)i;
        fix_bm[k].databaseid = 16384; fix_bm[k].userid = 7;
        fix_bm[k].ambiguous = 0;
    }
    for (int i = 0; i < 20; i++, k++) {
        fix_bm[k].pid = CLEAN_B_0 + (uint32_t)i;
        fix_bm[k].databaseid = 16385; fix_bm[k].userid = 9;
        fix_bm[k].ambiguous = 0;
    }
    /* BAD_0+0 is deliberately NOT given an entry (cause: absent); the map
     * below holds the other three causes. */
    fix_bm[k].pid = BAD_0 + 1;
    fix_bm[k].databaseid = 16384; fix_bm[k].userid = 7;
    fix_bm[k].ambiguous = 1;                                   /* ambiguous */
    k++;
    fix_bm[k].pid = BAD_0 + 2;
    fix_bm[k].databaseid = 0; fix_bm[k].userid = 7;
    fix_bm[k].ambiguous = 0;                              /* zero databaseid */
    k++;
    fix_bm[k].pid = BAD_0 + 3;
    fix_bm[k].databaseid = 16384; fix_bm[k].userid = 0;
    fix_bm[k].ambiguous = 0;                                   /* zero userid */
    k++;
    fix_map.n = k;    /* 43: pid BAD_0+0 is absent on purpose */
}

/* The pid a record for `qid` carries, given its ordinal within that qid. */
static uint32_t fixture_pid_for(uint64_t qid, int ordinal)
{
    switch (qid % 4) {
    case 0:   /* U: one shared context */
        return CLEAN_A_0 + (uint32_t)(ordinal % 20);
    case 1:   /* D: two contexts that disagree. The B pid is NOT first, so a
               * "trust the first match" bug still has to notice it. */
        return ordinal % 3 == 2 ? CLEAN_B_0 + (uint32_t)(ordinal % 20)
                                : CLEAN_A_0 + (uint32_t)(ordinal % 20);
    default:  /* B: clean pids plus one unusable one, cause by qid */
        return ordinal % 5 == 3 ? BAD_0 + (uint32_t)((qid / 4) % 4)
                                : CLEAN_A_0 + (uint32_t)(ordinal % 20);
    }
}

static struct pgwt_trace_event *build_events(int n)
{
    struct pgwt_trace_event *ev = calloc((size_t)n ? (size_t)n : 1,
                                         sizeof(*ev));
    if (!ev) { printf("  FAIL: fixture calloc\n"); exit(2); }
    int ordinal[FIX_QIDS];
    memset(ordinal, 0, sizeof(ordinal));
    for (int i = 0; i < n; i++) {
        /* Round-robin over the CLAIMED ids (classes U, D, B — never N), so
         * even a 3-record window touches three different ids rather than
         * piling onto one. */
        uint64_t qid = 0;
        int step = i % FIX_QIDS;
        for (int t = 0; t < FIX_QIDS; t++) {
            uint64_t cand = (uint64_t)((step + t) % FIX_QIDS);
            if (cand % 4 != 3) { qid = cand; break; }
        }
        int o = ordinal[qid]++;
        ev[i].timestamp_ns = 1000000ULL + (uint64_t)i * 1000ULL;
        ev[i].duration_ns = 500;
        ev[i].query_id = qid;
        ev[i].pid = fixture_pid_for(qid, o);
        ev[i].old_event = 100 + (rng() % 5);
        /* Keyed on qid % 3 and the record index — INDEPENDENT of the
         * context class (qid % 4), and sparse enough that some CLAIMED ids
         * carry no synth record at all. A density of "1 record in 7,
         * any id" gave every claimed id a synth record over a 4000-record
         * window, which made "always report synth" indistinguishable
         * (mutation M18). */
        if ((qid % 3) == 0 && (i % 7) == 1)
            ev[i].flags |= PGWT_EVENT_FLAG_QUERY_SYNTH;
    }
    return ev;
}

/* Non-vacuity: every verdict this function can return is ALSO what a
 * function that looked at nothing would plausibly return for some row, so a
 * differential is only meaningful if the fixture reaches all of them. Count
 * them and REFUSE if any is missing — this is the check that caught the
 * blind fixture described above. */
static void section0_non_vacuity(void)
{
    printf("\n[§0] non-vacuity: every verdict in range is REACHED\n");
    rng_reset();
    build_bm();
    int n = 4000;
    struct pgwt_trace_event *ev = build_events(n);
    uint64_t qids[FIX_QIDS];
    for (int i = 0; i < FIX_QIDS; i++) qids[i] = (uint64_t)i;

    struct pgwt_qrow_attr_result r = {0};
    CHECK_INT(pgwt_compute_qrow_attrs(ev, n, qids, FIX_QIDS,
                                      tbm_resolve, &fix_map, &r), 0,
              "fixture computes");
    int n_unique = 0, n_none = 0, n_amb = 0, n_syn = 0, n_nosyn = 0;
    /* Counted among CLAIMED rows only. An unclaimed row has has_synth 0 by
     * construction, so "some row has no synth" is satisfied by the empty
     * class-N ids alone — and then "set synth on EVERY record" would still
     * pass. Mutation M18 was GREEN until this distinction existed. */
    int n_claimed_nosyn = 0, n_claimed_syn = 0;
    for (int i = 0; i < r.num_rows; i++) {
        if (r.rows[i].context == 1) n_unique++;
        else if (r.rows[i].context == 0) n_none++;
        else n_amb++;
        if (r.rows[i].has_synth) n_syn++; else n_nosyn++;
        if (r.rows[i].context != 0) {
            if (r.rows[i].has_synth) n_claimed_syn++;
            else                     n_claimed_nosyn++;
        }
    }
    printf("      unique=%d none=%d ambiguous=%d synth=%d nosynth=%d "
           "(claimed: synth=%d nosynth=%d) of %d rows\n",
           n_unique, n_none, n_amb, n_syn, n_nosyn,
           n_claimed_syn, n_claimed_nosyn, r.num_rows);
    CHECK(n_unique > 0, "the 1 (unique context) verdict is REACHED");
    CHECK(n_none > 0, "the 0 (nothing claimed it) verdict is REACHED");
    CHECK(n_amb > 0, "the -1 (ambiguous) verdict is REACHED");
    CHECK(n_claimed_syn > 0, "has_synth SET on at least one CLAIMED row");
    CHECK(n_claimed_nosyn > 0,
          "has_synth CLEAR on at least one CLAIMED row — so neither "
          "constant answer can satisfy the differential");
    CHECK(n_unique + n_none + n_amb == r.num_rows, "every row has a verdict");

    /* Both -1 causes are reached, not just one: a fixture that only ever
     * disagreed would never exercise the rejection branch, and vice versa. */
    int amb_by_disagreement = 0, amb_by_rejection = 0;
    for (int i = 0; i < r.num_rows; i++) {
        if (r.rows[i].context != -1) continue;
        if (i % 4 == 1) amb_by_disagreement++;
        if (i % 4 == 2) amb_by_rejection++;
    }
    CHECK(amb_by_disagreement > 0, "-1 reached by DISAGREEMENT");
    CHECK(amb_by_rejection > 0, "-1 reached by an unusable pid");
    pgwt_qrow_attrs_free(&r);
    free(ev);
}

/* ── §1 the differential ─────────────────────────────────────────────── */
static void section1_differential(void)
{
    printf("\n[§1] differential vs a verbatim copy of the deleted loops\n");
    build_bm();
    /* Many window sizes AND many row sets: a single size would never
     * exercise the memo's grow path or the duplicate fixup. */
    const int sizes[] = { 0, 1, 2, 3, 7, 31, 100, 977, 4096 };
    int total_diffs = 0, cases = 0;
    for (unsigned s = 0; s < sizeof(sizes)/sizeof(sizes[0]); s++) {
        rng_reset();
        int n = sizes[s];
        struct pgwt_trace_event *ev = n ? build_events(n) : NULL;
        for (int nq = 1; nq <= FIX_QIDS + 4; nq++) {
            uint64_t qids[FIX_QIDS + 4];
            for (int i = 0; i < nq; i++) qids[i] = (uint64_t)i;
            char label[64];
            snprintf(label, sizeof(label), "n=%d nq=%d", n, nq);
            total_diffs += differential(ev, n, qids, nq, tbm_resolve,
                                        &fix_map, label, 1);
            cases++;
        }
        free(ev);
    }
    printf("      %d fixture/rowset cases compared\n", cases);
    CHECK(cases >= 200, "the sweep is wide enough to be worth asserting on");
    CHECK_INT(total_diffs, 0, "ZERO divergence from the deleted loops");
}

/* ── §2 literal expectations ─────────────────────────────────────────────
 * Hand-computed, so a bug SHARED by the new code and the oracle is caught.
 * The §1 differential cannot do this: both sides could be wrong together. */
static void section2_literals(void)
{
    printf("\n[§2] hand-computed expectations (oracle-independent)\n");
    static const struct tbm bm[] = {
        { 10, 16384, 7, 0 },     /* usable */
        { 11, 16384, 7, 0 },     /* usable, SAME context as pid 10 */
        { 12, 16385, 7, 0 },     /* usable, DIFFERENT databaseid */
        { 13, 16384, 7, 1 },     /* ambiguous -> unusable */
        { 14, 0,     7, 0 },     /* zero databaseid -> unusable */
        { 15, 16384, 0, 0 },     /* zero userid -> unusable */
    };
    struct tbm_map m = { bm, 6 };

    /* qid 100: pids 10 and 11 — one agreed context.
     * qid 200: pids 10 and 12 — disagree on databaseid.
     * qid 300: pid 13 — ambiguous.
     * qid 400: pid 14 — zero databaseid.
     * qid 500: pid 15 — zero userid.
     * qid 600: pid 99 — absent from the map entirely.
     * qid 700: claimed by nobody.
     * qid 800: pid 10, with the SYNTH flag.
     * qid 900: pid 13 (ambiguous) AND the SYNTH flag — a poisoned context
     *          must not suppress the synth report. */
    struct pgwt_trace_event ev[] = {
        { .timestamp_ns = 1, .pid = 10, .query_id = 100 },
        { .timestamp_ns = 2, .pid = 11, .query_id = 100 },
        { .timestamp_ns = 3, .pid = 10, .query_id = 200 },
        { .timestamp_ns = 4, .pid = 12, .query_id = 200 },
        { .timestamp_ns = 5, .pid = 13, .query_id = 300 },
        { .timestamp_ns = 6, .pid = 14, .query_id = 400 },
        { .timestamp_ns = 7, .pid = 15, .query_id = 500 },
        { .timestamp_ns = 8, .pid = 99, .query_id = 600 },
        { .timestamp_ns = 9, .pid = 10, .query_id = 800,
          .flags = PGWT_EVENT_FLAG_QUERY_SYNTH },
        { .timestamp_ns = 10, .pid = 13, .query_id = 900,
          .flags = PGWT_EVENT_FLAG_QUERY_SYNTH },
    };
    uint64_t qids[] = { 100, 200, 300, 400, 500, 600, 700, 800, 900 };
    struct pgwt_qrow_attr_result r = {0};
    CHECK_INT(pgwt_compute_qrow_attrs(ev, 10, qids, 9, tbm_resolve, &m, &r),
              0, "literal fixture computes");
    CHECK_INT(r.num_rows, 9, "one row per requested query id");

    CHECK_INT(r.rows[0].context, 1, "qid 100: two pids, one agreed context");
    CHECK_INT(r.rows[0].databaseid, 16384, "qid 100 databaseid");
    CHECK_INT(r.rows[0].userid, 7, "qid 100 userid");

    CHECK_INT(r.rows[1].context, -1, "qid 200: disagreeing databaseid");
    CHECK_INT(r.rows[1].databaseid, 0, "qid 200 databaseid forced to 0");
    CHECK_INT(r.rows[1].userid, 0, "qid 200 userid forced to 0");

    CHECK_INT(r.rows[2].context, -1, "qid 300: ambiguous pid");
    CHECK_INT(r.rows[3].context, -1, "qid 400: zero databaseid");
    CHECK_INT(r.rows[4].context, -1, "qid 500: zero userid");
    CHECK_INT(r.rows[5].context, -1, "qid 600: pid absent from the map");
    CHECK_INT(r.rows[6].context, 0, "qid 700: claimed by no record");
    CHECK_INT(r.rows[6].databaseid, 0, "qid 700 databaseid 0");

    CHECK_INT(r.rows[7].context, 1, "qid 800: usable context");
    CHECK_INT(r.rows[7].has_synth, 1, "qid 800: synth flag reported");
    CHECK_INT(r.rows[8].context, -1, "qid 900: poisoned context");
    CHECK_INT(r.rows[8].has_synth, 1,
              "qid 900: has_synth survives a poisoned context");
    CHECK_INT(r.rows[0].has_synth, 0, "qid 100: no synth record, no flag");
    pgwt_qrow_attrs_free(&r);
}

/* ── §3 ordering: -1 reached first vs last ───────────────────────────── */
static void section3_absorbing(void)
{
    printf("\n[§3] -1 is absorbing from either direction\n");
    static const struct tbm bm[] = {
        { 10, 16384, 7, 0 },
        { 13, 16384, 7, 1 },     /* unusable */
    };
    struct tbm_map m = { bm, 2 };
    uint64_t q[1] = { 55 };

    /* Bad record LAST: a row already settled at 1 must fall to -1. */
    struct pgwt_trace_event a[] = {
        { .timestamp_ns = 1, .pid = 10, .query_id = 55 },
        { .timestamp_ns = 2, .pid = 10, .query_id = 55 },
        { .timestamp_ns = 3, .pid = 13, .query_id = 55 },
    };
    struct pgwt_qrow_attr_result r = {0};
    pgwt_compute_qrow_attrs(a, 3, q, 1, tbm_resolve, &m, &r);
    CHECK_INT(r.rows[0].context, -1, "bad record LAST still poisons the row");
    CHECK_INT(r.rows[0].databaseid, 0, "and clears databaseid");
    pgwt_qrow_attrs_free(&r);

    /* Bad record FIRST: a good record after it must NOT un-poison the row.
     * This is the one a `continue` in the wrong place gets wrong, and it is
     * exactly §5's mut_oracle_first_only mutation. */
    struct pgwt_trace_event b[] = {
        { .timestamp_ns = 1, .pid = 13, .query_id = 55 },
        { .timestamp_ns = 2, .pid = 10, .query_id = 55 },
        { .timestamp_ns = 3, .pid = 10, .query_id = 55 },
    };
    struct pgwt_qrow_attr_result r2 = {0};
    pgwt_compute_qrow_attrs(b, 3, q, 1, tbm_resolve, &m, &r2);
    CHECK_INT(r2.rows[0].context, -1,
              "bad record FIRST is not undone by later good records");
    CHECK_INT(r2.rows[0].databaseid, 0, "and databaseid stays 0");
    pgwt_qrow_attrs_free(&r2);

    /* A synth record AFTER the poisoning one is still ORed in. */
    struct pgwt_trace_event c[] = {
        { .timestamp_ns = 1, .pid = 13, .query_id = 55 },
        { .timestamp_ns = 2, .pid = 10, .query_id = 55,
          .flags = PGWT_EVENT_FLAG_QUERY_SYNTH },
    };
    struct pgwt_qrow_attr_result r3 = {0};
    pgwt_compute_qrow_attrs(c, 2, q, 1, tbm_resolve, &m, &r3);
    CHECK_INT(r3.rows[0].context, -1, "still poisoned");
    CHECK_INT(r3.rows[0].has_synth, 1,
              "a synth record after the poison is still counted");
    pgwt_qrow_attrs_free(&r3);
}

/* ── §4 the bypass suite ─────────────────────────────────────────────── */

static void s4_1_empty_and_absent(void)
{
    printf("\n[§4.1] empty window / NULL array / zero rows\n");
    build_bm();
    uint64_t q[3] = { 1, 2, 3 };

    struct pgwt_qrow_attr_result r = {0};
    CHECK_INT(pgwt_compute_qrow_attrs(NULL, 0, q, 3, tbm_resolve, &fix_map, &r),
              0, "NULL events, count 0: success, not an error");
    CHECK_INT(r.num_rows, 3, "still one row per requested id");
    CHECK_INT(r.failed, 0, "not flagged failed");
    for (int i = 0; i < 3; i++) {
        CHECK_INT(r.rows[i].context, 0, "empty window: context 0");
        CHECK_INT(r.rows[i].has_synth, 0, "empty window: no synth");
    }
    CHECK_INT((long)r.rows[0].query_id, 1, "row 0 carries its query id");
    pgwt_qrow_attrs_free(&r);

    /* A NON-NULL array with count 0, and a NULL array with a positive
     * count: both must be treated as "no records", never dereferenced. */
    struct pgwt_trace_event one = { .pid = 1000, .query_id = 1 };
    struct pgwt_qrow_attr_result r2 = {0};
    CHECK_INT(pgwt_compute_qrow_attrs(&one, 0, q, 3, tbm_resolve, &fix_map, &r2),
              0, "non-NULL array with count 0 succeeds");
    CHECK_INT(r2.rows[0].context, 0, "and sees no records");
    pgwt_qrow_attrs_free(&r2);

    struct pgwt_qrow_attr_result r3 = {0};
    CHECK_INT(pgwt_compute_qrow_attrs(NULL, 5000, q, 3, tbm_resolve,
                                      &fix_map, &r3), 0,
              "NULL array with a positive count does not dereference");
    CHECK_INT(r3.rows[0].context, 0, "and sees no records");
    pgwt_qrow_attrs_free(&r3);

    /* Zero requested rows: nothing to answer, and no allocation. */
    struct pgwt_qrow_attr_result r4 = {0};
    CHECK_INT(pgwt_compute_qrow_attrs(&one, 1, q, 0, tbm_resolve,
                                      &fix_map, &r4), 0,
              "zero requested rows succeeds");
    CHECK_INT(r4.num_rows, 0, "with zero rows");
    CHECK_INT(r4.failed, 0, "and no failure flag");
    CHECK(r4.rows == NULL, "and no allocation");
    pgwt_qrow_attrs_free(&r4);

    struct pgwt_qrow_attr_result r5 = {0};
    CHECK_INT(pgwt_compute_qrow_attrs(&one, 1, NULL, 3, tbm_resolve,
                                      &fix_map, &r5), 0,
              "NULL query_ids with a positive count does not dereference");
    CHECK_INT(r5.num_rows, 0, "and answers nothing");
    pgwt_qrow_attrs_free(&r5);
}

static void s4_2_resolver_cannot_answer(void)
{
    printf("\n[§4.2] a resolver that is absent or can never answer\n");
    uint64_t q[2] = { 77, 88 };
    struct pgwt_trace_event ev[] = {
        { .timestamp_ns = 1, .pid = 10, .query_id = 77,
          .flags = PGWT_EVENT_FLAG_QUERY_SYNTH },
        /* qid 88 is claimed by nobody. */
    };

    /* resolve == NULL: the dependency is MISSING. Every claimed row must be
     * -1 (cannot see), never 1 (approved) and never 0 (reads as "no record
     * claimed it", which is a different, wrong statement). */
    struct pgwt_qrow_attr_result r = {0};
    CHECK_INT(pgwt_compute_qrow_attrs(ev, 1, q, 2, NULL, NULL, &r), 0,
              "NULL resolver still returns a result");
    CHECK_INT(r.rows[0].context, -1,
              "NULL resolver: a CLAIMED row refuses (-1), never approves");
    CHECK_INT(r.rows[0].has_synth, 1,
              "and the synth flag, which needs no resolver, still works");
    CHECK_INT(r.rows[1].context, 0,
              "an UNCLAIMED row is still 0 — absence is not ambiguity");
    pgwt_qrow_attrs_free(&r);

    struct pgwt_qrow_attr_result r2 = {0};
    pgwt_compute_qrow_attrs(ev, 1, q, 2, resolve_never, NULL, &r2);
    CHECK_INT(r2.rows[0].context, -1, "always-failing resolver refuses too");
    pgwt_qrow_attrs_free(&r2);

    /* And the differential agrees with the oracle under both. */
    CHECK_INT(differential(ev, 1, q, 2, NULL, NULL, "null resolver", 1), 0,
              "differential holds with a NULL resolver");
    CHECK_INT(differential(ev, 1, q, 2, resolve_never, NULL, "never", 1), 0,
              "differential holds with an always-failing resolver");
}

static void s4_3_duplicate_query_ids(void)
{
    printf("\n[§4.3] duplicate query_ids: the hash holds only the first\n");
    static const struct tbm bm[] = { { 10, 16384, 7, 0 } };
    struct tbm_map m = { bm, 1 };
    struct pgwt_trace_event ev[] = {
        { .timestamp_ns = 1, .pid = 10, .query_id = 42,
          .flags = PGWT_EVENT_FLAG_QUERY_SYNTH },
    };
    /* 42 appears three times, with another id between, so a fixup that only
     * handles ADJACENT duplicates would still be red. */
    uint64_t q[5] = { 42, 7, 42, 7, 42 };
    struct pgwt_qrow_attr_result r = {0};
    CHECK_INT(pgwt_compute_qrow_attrs(ev, 1, q, 5, tbm_resolve, &m, &r), 0,
              "duplicate ids compute");
    for (int i = 0; i < 5; i += 2) {
        CHECK_INT(r.rows[i].context, 1, "every copy of qid 42 has context 1");
        CHECK_INT(r.rows[i].databaseid, 16384, "and the same databaseid");
        CHECK_INT(r.rows[i].has_synth, 1, "and the same synth flag");
        CHECK_INT((long)r.rows[i].query_id, 42, "and its own query id");
    }
    CHECK_INT(r.rows[1].context, 0, "the interleaved unclaimed id is 0");
    CHECK_INT(r.rows[3].context, 0, "both copies of it");
    pgwt_qrow_attrs_free(&r);
    CHECK_INT(differential(ev, 1, q, 5, tbm_resolve, &m, "dups", 1), 0,
              "differential holds with duplicate query ids");
}

static void s4_4_zero_keys(void)
{
    printf("\n[§4.4] query_id 0 and pid 0 are ORDINARY keys\n");
    /* An index that used 0 as its empty-slot sentinel would silently drop
     * both. query_id 0 is a real Queries-tab row (time no query claimed);
     * pid 0 is the pid markers carry. */
    static const struct tbm bm[] = {
        { 0,  16384, 7, 0 },         /* pid 0, resolvable */
        { 10, 16384, 7, 0 },
    };
    struct tbm_map m = { bm, 2 };
    struct pgwt_trace_event ev[] = {
        { .timestamp_ns = 1, .pid = 0,  .query_id = 0 },
        { .timestamp_ns = 2, .pid = 10, .query_id = 5 },
    };
    uint64_t q[2] = { 0, 5 };
    struct pgwt_qrow_attr_result r = {0};
    pgwt_compute_qrow_attrs(ev, 2, q, 2, tbm_resolve, &m, &r);
    CHECK_INT(r.rows[0].context, 1, "query_id 0 is found, not skipped");
    CHECK_INT(r.rows[0].databaseid, 16384, "and resolves through pid 0");
    CHECK_INT(r.rows[1].context, 1, "the non-zero id still works");
    pgwt_qrow_attrs_free(&r);
    CHECK_INT(differential(ev, 2, q, 2, tbm_resolve, &m, "zero keys", 1), 0,
              "differential holds for query_id 0 / pid 0");
}

static void s4_5_memo_grows(void)
{
    printf("\n[§4.6] more distinct pids than the memo's first table\n");
    /* The pid memo starts at 128 slots and must GROW. If the grow path were
     * broken, qa_ix_put_first would become a silent no-op on a full table
     * and every later lookup would re-resolve and append — right answers,
     * unbounded memory, no signal. 5000 distinct pids forces ~6 growths. */
    enum { NP = 5000 };
    struct tbm *bm = calloc(NP, sizeof(*bm));
    struct pgwt_trace_event *ev = calloc(NP, sizeof(*ev));
    if (!bm || !ev) { printf("  FAIL: fixture calloc\n"); exit(2); }
    for (int i = 0; i < NP; i++) {
        bm[i].pid = 2000 + (uint32_t)i;
        bm[i].databaseid = 16384;
        bm[i].userid = 7;
        ev[i].timestamp_ns = 1 + (uint64_t)i;
        ev[i].pid = 2000 + (uint32_t)i;
        ev[i].query_id = 31;        /* all one query: one agreed context */
    }
    struct tbm_map m = { bm, NP };
    uint64_t q[1] = { 31 };
    struct pgwt_qrow_attr_result r = {0};
    resolve_calls = 0;
    CHECK_INT(pgwt_compute_qrow_attrs(ev, NP, q, 1, tbm_resolve_counting,
                                      &m, &r), 0,
              "5000 distinct pids compute");
    CHECK_INT(r.rows[0].context, 1, "all agree: context 1");
    CHECK_INT(r.rows[0].databaseid, 16384, "with the shared databaseid");
    /* EXACTLY one resolve per distinct pid. If the memo's grow path were a
     * silent no-op on a full table, every answer here would still be right
     * and only this number would move — which is why it is asserted. */
    CHECK_INT(resolve_calls, NP, "one resolver call per DISTINCT pid, no more");
    pgwt_qrow_attrs_free(&r);

    /* And the same pid repeated is resolved ONCE, not once per record: the
     * per-row scans this replaced did the latter. */
    for (int i = 0; i < NP; i++)
        ev[i].pid = 2000;                  /* one pid, NP records */
    struct pgwt_qrow_attr_result r1 = {0};
    resolve_calls = 0;
    pgwt_compute_qrow_attrs(ev, NP, q, 1, tbm_resolve_counting, &m, &r1);
    CHECK_INT(r1.rows[0].context, 1, "one repeated pid: context 1");
    CHECK_INT(resolve_calls, 1, "5000 records for one pid = ONE resolve");
    pgwt_qrow_attrs_free(&r1);
    free(ev);

    /* MANY distinct pids, each REPEATED. This combination is what makes a
     * broken grow visible: with 5000 distinct pids appearing once each, a
     * memo stuck at its initial 512 slots still resolves exactly 5000 times
     * (every pid is new), so the count above cannot see it. Repeat each pid
     * and the stuck memo must re-resolve every repeat past the 512th pid —
     * ~9976 calls against the correct 5000. Found by mutation M10, which was
     * GREEN until this case existed. */
    enum { REPS = 2 };
    struct pgwt_trace_event *ev2 = calloc(NP * REPS, sizeof(*ev2));
    if (!ev2) { printf("  FAIL: fixture calloc\n"); exit(2); }
    for (int rep = 0; rep < REPS; rep++)
        for (int i = 0; i < NP; i++) {
            int k = rep * NP + i;
            ev2[k].timestamp_ns = 1 + (uint64_t)k;
            ev2[k].pid = 2000 + (uint32_t)i;
            ev2[k].query_id = 31;
        }
    struct pgwt_qrow_attr_result r3 = {0};
    resolve_calls = 0;
    CHECK_INT(pgwt_compute_qrow_attrs(ev2, NP * REPS, q, 1,
                                      tbm_resolve_counting, &m, &r3), 0,
              "5000 pids x 2 records compute");
    CHECK_INT(resolve_calls, NP,
              "10000 records over 5000 pids = 5000 resolves (memo GREW)");
    pgwt_qrow_attrs_free(&r3);
    CHECK_INT(differential(ev2, NP * REPS, q, 1, tbm_resolve, &m,
                           "5000x2", 1), 0,
              "differential holds across the memo grow path");
    free(ev2);

    ev = calloc(NP, sizeof(*ev));
    if (!ev) { printf("  FAIL: fixture calloc\n"); exit(2); }
    for (int i = 0; i < NP; i++) {
        ev[i].timestamp_ns = 1 + (uint64_t)i;
        ev[i].pid = 2000 + (uint32_t)i;
        ev[i].query_id = 31;
    }
    CHECK_INT(differential(ev, NP, q, 1, tbm_resolve, &m, "5000 pids", 1), 0,
              "differential holds across the memo grow path");

    /* And one disagreeing pid at the very end must still poison it. */
    bm[NP - 1].databaseid = 16385;
    struct pgwt_qrow_attr_result r2 = {0};
    pgwt_compute_qrow_attrs(ev, NP, q, 1, tbm_resolve, &m, &r2);
    CHECK_INT(r2.rows[0].context, -1,
              "the 5000th pid disagreeing is still seen");
    pgwt_qrow_attrs_free(&r2);
    free(bm); free(ev);
}

static void s4_6_alloc_failures(void)
{
    printf("\n[§4.7] injected allocation failures must be ABSENT, not short\n");
    build_bm();
    rng_reset();
    int n = 500;
    struct pgwt_trace_event *ev = build_events(n);
    uint64_t q[8];
    for (int i = 0; i < 8; i++) q[i] = (uint64_t)i;

    /* Points that must make the whole computation refuse. */
    static const char *fatal[] = { "qrow_attrs", "qrow_qid_index" };
    for (unsigned k = 0; k < sizeof(fatal)/sizeof(fatal[0]); k++) {
        setenv("PGWT_TEST_ALLOC_FAIL", fatal[k], 1);
        struct pgwt_qrow_attr_result r = {0};
        int rc = pgwt_compute_qrow_attrs(ev, n, q, 8, tbm_resolve, &fix_map, &r);
        unsetenv("PGWT_TEST_ALLOC_FAIL");
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: returns -1", fatal[k]);
        CHECK_INT(rc, -1, msg);
        snprintf(msg, sizeof(msg), "%s: failed flag set", fatal[k]);
        CHECK_INT(r.failed, 1, msg);
        snprintf(msg, sizeof(msg), "%s: num_rows 0 (not short-but-plausible)",
                 fatal[k]);
        CHECK_INT(r.num_rows, 0, msg);
        snprintf(msg, sizeof(msg), "%s: rows NULL", fatal[k]);
        CHECK(r.rows == NULL, msg);
        pgwt_qrow_attrs_free(&r);
    }

    /* The pid-index/memo points are a PERFORMANCE fallback, not a
     * correctness one: the resolver is consulted directly instead. The
     * answers must be unchanged — which is the dangerous case to get wrong,
     * so it is asserted against the oracle rather than assumed. */
    static const char *degrade[] = { "qrow_pid_index", "qrow_pid_memo" };
    for (unsigned k = 0; k < sizeof(degrade)/sizeof(degrade[0]); k++) {
        setenv("PGWT_TEST_ALLOC_FAIL", degrade[k], 1);
        char label[64];
        snprintf(label, sizeof(label), "degraded:%s", degrade[k]);
        int d = differential(ev, n, q, 8, tbm_resolve, &fix_map, label, 1);
        unsetenv("PGWT_TEST_ALLOC_FAIL");
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "%s: answers still bit-exact (degrades, never lies)",
                 degrade[k]);
        CHECK_INT(d, 0, msg);
    }

    /* An UNKNOWN injection point. If the harness were ignoring the variable
     * entirely, every case above would "pass" for the wrong reason; this is
     * the control that says injection is wired at all. */
    setenv("PGWT_TEST_ALLOC_FAIL", "not_a_real_point_xyz", 1);
    struct pgwt_qrow_attr_result r = {0};
    int rc = pgwt_compute_qrow_attrs(ev, n, q, 8, tbm_resolve, &fix_map, &r);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
    CHECK_INT(rc, 0, "an unknown injection point changes nothing");
    CHECK_INT(r.num_rows, 8, "and the answer is complete");
    CHECK_INT(r.failed, 0, "and not flagged failed");
    pgwt_qrow_attrs_free(&r);
    free(ev);
}

/* ── §5 mutation probes: the differential must be able to go RED ───────
 *
 * A green differential proves nothing unless it CAN fail. Each probe breaks
 * one side — including the ORACLE, because a comparator that only reacts to
 * the new code cannot catch a bug the two share — and requires a diff. */
static void section5_mutations(void)
{
    printf("\n[§5] mutation probes — each must be DETECTED\n");
    build_bm();
    rng_reset();
    int n = 2000;
    struct pgwt_trace_event *ev = build_events(n);
    uint64_t q[FIX_QIDS];
    for (int i = 0; i < FIX_QIDS; i++) q[i] = (uint64_t)i;
    /* A duplicated id whose answer is NOT the default: id 0 is class U, so
     * its rows carry context 1 and a real databaseid, and blanking the
     * second copy is visible. An earlier version duplicated a class-N id
     * (context 0, no synth) — zeroing it changed nothing and M3 went
     * UNDETECTED, the same blindness §0 guards against. */
    uint64_t qd[4] = { 0, 5, 0, 9 };

    CHECK_INT(differential(ev, n, q, FIX_QIDS, tbm_resolve, &fix_map,
                           "control", 1), 0, "control: agreement");

    mut_new_context_sign = 1;
    CHECK(differential(ev, n, q, FIX_QIDS, tbm_resolve, &fix_map,
                       "M1", 0) > 0,
          "M1 new side reports -1 as 0 — DETECTED");
    mut_new_context_sign = 0;

    mut_new_synth_off = 1;
    CHECK(differential(ev, n, q, FIX_QIDS, tbm_resolve, &fix_map,
                       "M2", 0) > 0,
          "M2 new side drops has_synth — DETECTED");
    mut_new_synth_off = 0;

    mut_new_dup_skip = 1;
    CHECK(differential(ev, n, qd, 4, tbm_resolve, &fix_map, "M3", 0) > 0,
          "M3 new side leaves a duplicate row unset — DETECTED");
    mut_new_dup_skip = 0;

    mut_oracle_first_only = 1;
    CHECK(differential(ev, n, q, FIX_QIDS, tbm_resolve, &fix_map,
                       "M4", 0) > 0,
          "M4 ORACLE trusts only the first match — DETECTED");
    mut_oracle_first_only = 0;

    mut_oracle_synth_on = 1;
    CHECK(differential(ev, n, q, FIX_QIDS, tbm_resolve, &fix_map,
                       "M5", 0) > 0,
          "M5 ORACLE always claims synth — DETECTED");
    mut_oracle_synth_on = 0;

    CHECK_INT(differential(ev, n, q, FIX_QIDS, tbm_resolve, &fix_map,
                           "control-after", 1), 0,
              "control after the mutations: agreement restored");
    free(ev);
}

int main(void)
{
    printf("=== test_qrow_attrs (#324) ===\n");
    printf("sizeof(struct pgwt_trace_event) = %zu B\n",
           sizeof(struct pgwt_trace_event));

    section0_non_vacuity();
    section1_differential();
    section2_literals();
    section3_absorbing();
    s4_1_empty_and_absent();
    s4_2_resolver_cannot_answer();
    s4_3_duplicate_query_ids();
    s4_4_zero_keys();
    s4_5_memo_grows();
    s4_6_alloc_failures();
    section5_mutations();

    printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
