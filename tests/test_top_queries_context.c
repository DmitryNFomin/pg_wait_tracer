/* test_top_queries_context.c — the Queries tab's per-row CONTEXT resolution
 * and QUERY_SYNTH flag, moved out of the row loop into one pass
 * (src/query_ctx_index.h).
 *
 * WHAT CHANGED. src/server.c handle_top_queries resolved two facts per ROW:
 * query_context_for_events() scanned every event in the window looking for
 * that row's query_id, and a second inline loop scanned them again looking
 * for PGWT_EVENT_FLAG_QUERY_SYNTH. At the UI's default 900 s window on the
 * dense trace that is 26 rows x 2 full passes x 5.7M events (~274 MB per
 * pass, ~14 GB of memory traffic), measured at 1778 ms — about 40% of the
 * whole response. Both facts are per-query_id properties of the same array,
 * so they are now computed once and each row is a lookup.
 *
 * THE WHOLE SAFETY ARGUMENT IS "BIT-IDENTICAL". So §1 is a differential
 * against a VERBATIM copy of the deleted function and the deleted synth
 * loop, over adversarial fixtures, comparing state / databaseid / userid /
 * has_synth field by field — no epsilon, nothing to be "close".
 *
 * ORDER. The deleted function returned -1 at the FIRST unusable backend and
 * kept the FIRST contributor's ids; the one-pass form accumulates. The claim
 * that those are the same is a claim that the rule is order-independent, so
 * §2 asserts it twice over: the oracle's own answer must not change under
 * reversal or rotation (the property), and index-vs-oracle must agree in
 * every one of those orders (the differential).
 *
 * §3 IS ABOUT FALSE NEGATIVES, not true positives — the detection path has
 * never been the broken one here. Seven deliberately broken one-pass
 * implementations (first-wins, latest-wins, resume-after-refusal,
 * absent-reads-as-resolved, missing-metadata-ignored, zero-userid-accepted,
 * synth-from-the-first-event-only) must EACH be caught by exactly the
 * comparator §1 uses. And the converse, which is the part that actually
 * rots: a fixture where every pid resolves and agrees, or one with no synth
 * flags at all, is GREEN for most of those mutants — §3b names those
 * fixtures and asserts they are blind, so the suite's power is known to come
 * from the adversarial fixtures rather than from luck.
 *
 * §4 IS THE BYPASS SUITE. An index whose failure mode is "no context" and a
 * legitimately context-less row are indistinguishable in the response, so
 * every way this table can fail to SEE must refuse rather than approve: a
 * NULL resolver, a NULL/empty/negative event range, an allocation failure at
 * each of the two growth points, an injection point that is never reached, a
 * partial event range, and a comparator that cannot see a single field.
 *
 * §5 IS THE NON-VACUITY LEDGER. "The two sides agree" is satisfied by
 * nothing-versus-nothing, so each resolution shape (resolved, resolved from
 * several agreeing pids, refused for missing metadata, for the ambiguous
 * flag, for a zero databaseid, for a zero userid, for disagreement, absent,
 * synth present, synth absent, query_id 0 with contributors, and both growth
 * points crossed) carries a counter that FAILS the test when zero.
 *
 * Runs anywhere: in-memory arrays, a fixed-seed PRNG, no clock, no I/O, no
 * threads. Nothing here can be flaky.
 */
#include "query_ctx_index.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do {                                           \
    if (!(cond)) {                                                      \
        failures++;                                                     \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                   \
        printf(__VA_ARGS__);                                            \
        printf("\n");                                                   \
    }                                                                   \
} while (0)

/* ─── The world: a stand-in for server.c's bm_map ─────────────────────
 *
 * Shaped like struct bm_entry's context-relevant fields so the oracle below
 * can keep the deleted predicate verbatim. `present == 0` is the
 * bm_map_lookup() miss.
 */
struct test_bm {
    uint32_t pid;
    uint32_t databaseid;
    uint32_t userid;
    uint8_t  context_ambiguous;
    int      present;
};

struct world {
    const struct test_bm *bms;
    int n_bms;
};

static const struct test_bm *world_lookup(const struct world *w, uint32_t pid)
{
    for (int i = 0; i < w->n_bms; i++)
        if (w->bms[i].pid == pid)
            return w->bms[i].present ? &w->bms[i] : NULL;
    return NULL;
}

/* The resolver src/server.c's srv_bm_context_resolver mirrors, with the same
 * predicate. This is the ONLY place the test applies it on the new side. */
static int world_resolver(void *ctx, uint32_t pid,
                          uint32_t *databaseid, uint32_t *userid)
{
    const struct world *w = (const struct world *)ctx;
    const struct test_bm *bm = world_lookup(w, pid);
    if (!bm || bm->context_ambiguous || !bm->databaseid || !bm->userid)
        return 0;
    *databaseid = bm->databaseid;
    *userid = bm->userid;
    return 1;
}

/* ─── The oracle ──────────────────────────────────────────────────────
 *
 * A VERBATIM copy of query_context_for_events() as deleted from
 * src/server.c, with `srv`/bm_map_lookup swapped for the world above and
 * nothing else touched. Do not "clean this up": its only job is to be the
 * code that used to run.
 */
static int oracle_query_context(const struct world *w,
                                const struct pgwt_trace_event *events,
                                int count, uint64_t query_id,
                                uint32_t *databaseid, uint32_t *userid)
{
    *databaseid = 0;
    *userid = 0;
    bool found = false;
    for (int i = 0; events && i < count; i++) {
        if (events[i].query_id != query_id)
            continue;
        const struct test_bm *bm = world_lookup(w, events[i].pid);
        if (!bm || bm->context_ambiguous || !bm->databaseid || !bm->userid) {
            *databaseid = 0;
            *userid = 0;
            return -1;
        }
        if (!found) {
            *databaseid = bm->databaseid;
            *userid = bm->userid;
            found = true;
        } else if (*databaseid != bm->databaseid || *userid != bm->userid) {
            *databaseid = 0;
            *userid = 0;
            return -1;
        }
    }
    return found ? 1 : 0;
}

/* A VERBATIM copy of the deleted synth scan. The real site OR-ed this with
 * `qt_entry->source_priority == 2`, which is a property of the query-text
 * map and not of the events; that disjunct is unchanged in src/server.c and
 * is deliberately out of scope here. */
static bool oracle_synth(const struct pgwt_trace_event *all_events,
                         int ecount, uint64_t query_id)
{
    bool synthetic_quality = false;
    for (int e = 0; !synthetic_quality && all_events && e < ecount; e++)
        if (all_events[e].query_id == query_id &&
            (all_events[e].flags & PGWT_EVENT_FLAG_QUERY_SYNTH))
            synthetic_quality = true;
    return synthetic_quality;
}

/* ─── What a row resolves to, on either side ──────────────────────── */

struct resolution {
    int      state;    /* -1 ambiguous, 0 absent, 1 resolved */
    uint32_t db;
    uint32_t uid;
    int      synth;
};

/* The ONE comparator. Both the real index and every mutant go through it,
 * so a comparator that cannot see a field is a single point of failure —
 * §4f is its self-test. Returns a bitmask of differing fields, 0 on
 * identical. */
#define DIFF_STATE 1
#define DIFF_DB    2
#define DIFF_UID   4
#define DIFF_SYNTH 8

static int diff_resolution(struct resolution a, struct resolution b)
{
    int d = 0;
    if (a.state != b.state) d |= DIFF_STATE;
    if (a.db    != b.db)    d |= DIFF_DB;
    if (a.uid   != b.uid)   d |= DIFF_UID;
    if (a.synth != b.synth) d |= DIFF_SYNTH;
    return d;
}

static struct resolution oracle_resolution(const struct world *w,
                                           const struct pgwt_trace_event *ev,
                                           int count, uint64_t qid)
{
    struct resolution r = {0, 0, 0, 0};
    r.state = oracle_query_context(w, ev, count, qid, &r.db, &r.uid);
    r.synth = oracle_synth(ev, count, qid) ? 1 : 0;
    return r;
}

/* The production path: build the real one-pass index, then look the row up
 * exactly as handle_top_queries does. */
static int index_resolutions(const struct world *w,
                             const struct pgwt_trace_event *ev, int count,
                             const uint64_t *qids, int n_qids,
                             struct resolution *out)
{
    struct pgwt_query_ctx_index qc;
    if (pgwt_query_ctx_index_build(&qc, ev, count, world_resolver,
                                   (void *)w) != 0 || qc.failed) {
        pgwt_query_ctx_index_free(&qc);
        return -1;
    }
    for (int q = 0; q < n_qids; q++) {
        const struct pgwt_query_ctx *row = pgwt_query_ctx_lookup(&qc, qids[q]);
        out[q].state = row ? row->state : 0;
        out[q].db    = (row && row->state > 0) ? row->databaseid : 0;
        out[q].uid   = (row && row->state > 0) ? row->userid : 0;
        out[q].synth = (row && row->has_synth) ? 1 : 0;
    }
    pgwt_query_ctx_index_free(&qc);
    return 0;
}

/* ─── Mutants: seven broken one-pass builds ───────────────────────────
 *
 * Each is the real accumulation with ONE rule bent, and each models a
 * silent-wrong answer the shipping code would have given: wrong query text
 * attached to a mixed row, or a quality badge that appears/vanishes.
 */
enum mutant {
    M_FIRST_WINS = 1,     /* disagreement keeps the first, never refuses */
    M_LATEST_WINS,        /* disagreement overwrites, stays resolved */
    M_RESUME,             /* a later good event lifts a refusal */
    M_ABSENT_RESOLVED,    /* a qid with no contributor reads as resolved 0/0 */
    M_MISSING_IGNORED,    /* unusable metadata is skipped, not refused */
    M_ZERO_UID_OK,        /* predicate drops the !userid arm */
    M_SYNTH_FIRST_ONLY    /* synth read from a qid's first event only */
};

struct mut_slot {
    uint64_t qid;
    uint32_t db, uid;
    int      state;
    int      synth;
    int      seen;        /* events contributed so far (for SYNTH_FIRST_ONLY) */
};

static void mutant_resolutions(enum mutant m, const struct world *w,
                               const struct pgwt_trace_event *ev, int count,
                               const uint64_t *qids, int n_qids,
                               struct resolution *out)
{
    /* Linear table: the mutants are only ever run on the fixtures here, and
     * a linear scan cannot itself be the bug under test. */
    struct mut_slot slots[512];
    int n = 0;
    for (int i = 0; i < count; i++) {
        int s = -1;
        for (int k = 0; k < n; k++)
            if (slots[k].qid == ev[i].query_id) { s = k; break; }
        if (s < 0) {
            if (n >= (int)(sizeof(slots) / sizeof(slots[0]))) {
                printf("  FAIL mutant table overflow — fixture too wide\n");
                failures++;
                return;
            }
            s = n++;
            memset(&slots[s], 0, sizeof(slots[s]));
            slots[s].qid = ev[i].query_id;
        }
        struct mut_slot *e = &slots[s];

        if (ev[i].flags & PGWT_EVENT_FLAG_QUERY_SYNTH) {
            if (m != M_SYNTH_FIRST_ONLY || e->seen == 0)
                e->synth = 1;
        }
        e->seen++;

        if (e->state < 0 && m != M_RESUME)
            continue;

        const struct test_bm *bm = world_lookup(w, ev[i].pid);
        int ok;
        if (m == M_ZERO_UID_OK)
            ok = !(!bm || bm->context_ambiguous || !bm->databaseid);
        else
            ok = !(!bm || bm->context_ambiguous || !bm->databaseid ||
                   !bm->userid);
        uint32_t db = ok ? bm->databaseid : 0;
        uint32_t uid = ok ? bm->userid : 0;

        if (!ok) {
            if (m == M_MISSING_IGNORED)
                continue;
            e->state = -1; e->db = 0; e->uid = 0;
        } else if (e->state <= 0) {
            e->state = 1; e->db = db; e->uid = uid;
        } else if (e->db != db || e->uid != uid) {
            if (m == M_FIRST_WINS) {
                /* keep what we have */
            } else if (m == M_LATEST_WINS) {
                e->db = db; e->uid = uid;
            } else {
                e->state = -1; e->db = 0; e->uid = 0;
            }
        }
    }
    for (int q = 0; q < n_qids; q++) {
        int s = -1;
        for (int k = 0; k < n; k++)
            if (slots[k].qid == qids[q]) { s = k; break; }
        if (s < 0) {
            if (m == M_ABSENT_RESOLVED) {
                out[q].state = 1; out[q].db = 0; out[q].uid = 0;
                out[q].synth = 0;
            } else {
                out[q].state = 0; out[q].db = 0; out[q].uid = 0;
                out[q].synth = 0;
            }
            continue;
        }
        out[q].state = slots[s].state;
        out[q].db    = slots[s].state > 0 ? slots[s].db : 0;
        out[q].uid   = slots[s].state > 0 ? slots[s].uid : 0;
        out[q].synth = slots[s].synth;
    }
}

/* ─── Fixtures ────────────────────────────────────────────────────────
 *
 * Each names the resolution shapes it is here to exercise. `qids` includes
 * ids that appear in no event — the `return found ? 1 : 0` case, which is
 * what every summary-path row and every late-reported id looks like.
 */
#define MAX_EV 4096
#define MAX_Q  600

struct fixture {
    const char *name;
    struct world w;
    struct pgwt_trace_event ev[MAX_EV];
    int count;
    uint64_t qids[MAX_Q];
    int n_qids;
};

static struct pgwt_trace_event mk(uint32_t pid, uint64_t qid, uint32_t flags)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.pid = pid;
    e.query_id = qid;
    e.flags = flags;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

/* The shared backend table most fixtures use.
 *   100: good (db 5, user 7)
 *   101: good, same context as 100
 *   102: good, DIFFERENT db        -> disagreement
 *   103: good, DIFFERENT user      -> disagreement
 *   104: present but context_ambiguous
 *   105: present, databaseid 0
 *   106: present, userid 0
 *   107: absent from the map entirely (present = 0)
 *   0:   pid 0, good -- key 0 is a legal key, the input is a trace file
 */
static const struct test_bm std_bms[] = {
    { 100, 5, 7, 0, 1 },
    { 101, 5, 7, 0, 1 },
    { 102, 6, 7, 0, 1 },
    { 103, 5, 8, 0, 1 },
    { 104, 5, 7, 1, 1 },
    { 105, 0, 7, 0, 1 },
    { 106, 5, 0, 0, 1 },
    { 107, 5, 7, 0, 0 },
    {   0, 9, 3, 0, 1 },
};

static uint64_t prng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t prng(void)
{
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 7;
    prng_state ^= prng_state << 17;
    return prng_state;
}

static void push(struct fixture *f, struct pgwt_trace_event e)
{
    if (f->count >= MAX_EV) { printf("  FAIL fixture overflow\n"); failures++; return; }
    f->ev[f->count++] = e;
}

static void want(struct fixture *f, uint64_t qid)
{
    if (f->n_qids >= MAX_Q) { printf("  FAIL qid overflow\n"); failures++; return; }
    f->qids[f->n_qids++] = qid;
}

/* fixture 0: every resolution shape, one qid each. */
static void fx_shapes(struct fixture *f)
{
    f->name = "shapes";
    f->w.bms = std_bms;
    f->w.n_bms = (int)(sizeof(std_bms) / sizeof(std_bms[0]));

    push(f, mk(100, 1001, 0));                               /* resolved, one pid */
    push(f, mk(100, 1002, 0));                               /* resolved, two agreeing pids */
    push(f, mk(101, 1002, 0));
    push(f, mk(100, 1003, 0));                               /* disagreement on db */
    push(f, mk(102, 1003, 0));
    push(f, mk(100, 1004, 0));                               /* disagreement on user */
    push(f, mk(103, 1004, 0));
    push(f, mk(104, 1005, 0));                               /* ambiguous flag */
    push(f, mk(105, 1006, 0));                               /* databaseid 0 */
    push(f, mk(106, 1007, 0));                               /* userid 0 */
    push(f, mk(107, 1008, 0));                               /* pid not in the map */
    push(f, mk(100, 1009, PGWT_EVENT_FLAG_QUERY_SYNTH));     /* resolved + synth */
    push(f, mk(104, 1010, PGWT_EVENT_FLAG_QUERY_SYNTH));     /* refused + synth */
    push(f, mk(100, 1011, 0));                               /* synth on a LATER event */
    push(f, mk(101, 1011, PGWT_EVENT_FLAG_QUERY_SYNTH));
    push(f, mk(  0,    0, 0));                               /* pid 0, query_id 0 */
    push(f, mk(100, 1012, 0));                               /* refusal AFTER a good event */
    push(f, mk(107, 1012, 0));
    push(f, mk(107, 1013, 0));                               /* refusal BEFORE a good event */
    push(f, mk(100, 1013, 0));
    push(f, mk(100, 1014, PGWT_EVENT_FLAG_SAMPLE));          /* an unrelated flag bit */

    for (uint64_t q = 1001; q <= 1014; q++) want(f, q);
    want(f, 0);
    want(f, 9999);                                           /* absent from the events */
    want(f, 1015);                                           /* absent, adjacent id */
}

/* fixture 1: the GREEN-WHILE-BROKEN fixture (§3b). Every pid resolves and
 * every qid's contributors agree, so first-wins / latest-wins /
 * resume-after-refusal / missing-ignored / zero-uid-ok are all invisible. */
static void fx_all_clean(struct fixture *f)
{
    f->name = "all-clean (blind by construction)";
    f->w.bms = std_bms;
    f->w.n_bms = (int)(sizeof(std_bms) / sizeof(std_bms[0]));
    for (uint64_t q = 2001; q <= 2020; q++) {
        push(f, mk(100, q, PGWT_EVENT_FLAG_QUERY_SYNTH));
        push(f, mk(101, q, PGWT_EVENT_FLAG_QUERY_SYNTH));
        want(f, q);
    }
}

/* fixture 2: no synth flag anywhere — blind to M_SYNTH_FIRST_ONLY (§3b). */
static void fx_no_synth(struct fixture *f)
{
    f->name = "no-synth (blind by construction)";
    f->w.bms = std_bms;
    f->w.n_bms = (int)(sizeof(std_bms) / sizeof(std_bms[0]));
    for (uint64_t q = 3001; q <= 3010; q++) {
        push(f, mk(100, q, 0));
        push(f, mk(102, q, 0));
        want(f, q);
    }
}

/* fixture 3: 300 distinct query ids — crosses BOTH growth points (the slot
 * array starts at 64, the qid index at 128 with a 70% load factor), with
 * interleaved pids so the per-pid memo is exercised rather than hit every
 * time. */
static void fx_wide(struct fixture *f)
{
    f->name = "wide (300 qids, both growth points)";
    f->w.bms = std_bms;
    f->w.n_bms = (int)(sizeof(std_bms) / sizeof(std_bms[0]));
    static const uint32_t pids[] = { 100, 101, 102, 103, 104, 105, 106, 107, 0 };
    for (uint64_t q = 0; q < 300; q++) {
        int reps = 1 + (int)(prng() % 4);
        for (int r = 0; r < reps; r++) {
            uint32_t pid = pids[prng() % (sizeof(pids) / sizeof(pids[0]))];
            uint32_t fl = (prng() & 7) == 0 ? PGWT_EVENT_FLAG_QUERY_SYNTH : 0;
            push(f, mk(pid, q * 7 + 1, fl));
        }
        want(f, q * 7 + 1);
        want(f, q * 7 + 2);          /* absent neighbour */
    }
}

/* fixture 4: consecutive query ids and a long run of one pid — the probe
 * chain and both memos under their best and worst cases. */
static void fx_dense(struct fixture *f)
{
    f->name = "dense (consecutive ids, long pid runs)";
    f->w.bms = std_bms;
    f->w.n_bms = (int)(sizeof(std_bms) / sizeof(std_bms[0]));
    for (uint64_t q = 0; q < 200; q++) {
        push(f, mk(100, q, 0));
        push(f, mk(100, q, 0));
        want(f, q);
    }
    for (uint64_t q = 0; q < 50; q++)
        push(f, mk(107, q, PGWT_EVENT_FLAG_QUERY_SYNTH));  /* poisons 0..49 */
}

/* fixture 5: random, the adversarial sweep. */
static void fx_random(struct fixture *f)
{
    f->name = "random";
    f->w.bms = std_bms;
    f->w.n_bms = (int)(sizeof(std_bms) / sizeof(std_bms[0]));
    static const uint32_t pids[] = { 100, 101, 102, 103, 104, 105, 106, 107, 0, 999 };
    for (int i = 0; i < 1500; i++) {
        uint32_t pid = pids[prng() % (sizeof(pids) / sizeof(pids[0]))];
        uint64_t qid = prng() % 40;
        uint32_t fl = (prng() & 3) == 0 ? PGWT_EVENT_FLAG_QUERY_SYNTH : 0;
        push(f, mk(pid, qid, fl));
    }
    for (uint64_t q = 0; q < 45; q++) want(f, q);
}

/* ─── The ledger (§5) ─────────────────────────────────────────────── */

struct ledger {
    long compared;
    long resolved_one_pid;
    long resolved_multi_agree;
    long refused_missing_bm;
    long refused_ambiguous_flag;
    long refused_zero_db;
    long refused_zero_uid;
    long refused_disagree;
    long absent;
    long synth_present;
    long synth_absent;
    long qid_zero_with_events;
    long growth_slots;
    long growth_qid_index;
};
static struct ledger led;

/* Classify a (fixture, qid) INDEPENDENTLY of both implementations: walk the
 * fixture's own events and backend table. This only feeds the ledger; it is
 * never an assertion oracle. */
static void ledger_classify(const struct fixture *f, uint64_t qid)
{
    int n = 0, missing = 0, ambiguous = 0, zero_db = 0, zero_uid = 0;
    int distinct_ctx = 0;
    uint32_t first_db = 0, first_uid = 0;
    int synth = 0;
    for (int i = 0; i < f->count; i++) {
        if (f->ev[i].query_id != qid) continue;
        n++;
        if (f->ev[i].flags & PGWT_EVENT_FLAG_QUERY_SYNTH) synth = 1;
        const struct test_bm *bm = world_lookup(&f->w, f->ev[i].pid);
        if (!bm) { missing++; continue; }
        if (bm->context_ambiguous) { ambiguous++; continue; }
        if (!bm->databaseid) { zero_db++; continue; }
        if (!bm->userid) { zero_uid++; continue; }
        if (distinct_ctx == 0) {
            first_db = bm->databaseid; first_uid = bm->userid; distinct_ctx = 1;
        } else if (first_db != bm->databaseid || first_uid != bm->userid) {
            distinct_ctx = 2;
        }
    }
    if (n == 0) { led.absent++; return; }
    if (synth) led.synth_present++; else led.synth_absent++;
    if (qid == 0) led.qid_zero_with_events++;
    if (missing) led.refused_missing_bm++;
    if (ambiguous) led.refused_ambiguous_flag++;
    if (zero_db) led.refused_zero_db++;
    if (zero_uid) led.refused_zero_uid++;
    if (!missing && !ambiguous && !zero_db && !zero_uid) {
        if (distinct_ctx == 2) led.refused_disagree++;
        else if (n == 1) led.resolved_one_pid++;
        else led.resolved_multi_agree++;
    }
}

/* ─── Order variants (§2) ─────────────────────────────────────────── */

static int reorder(const struct fixture *f, int variant,
                   struct pgwt_trace_event *out)
{
    int n = f->count;
    if (variant == 0) {
        memcpy(out, f->ev, (size_t)n * sizeof(*out));
    } else if (variant == 1) {                 /* reversed */
        for (int i = 0; i < n; i++) out[i] = f->ev[n - 1 - i];
    } else if (variant == 2) {                 /* rotated by a third */
        int k = n / 3;
        for (int i = 0; i < n; i++) out[i] = f->ev[(i + k) % n];
    } else {                                   /* shuffled, fixed seed */
        memcpy(out, f->ev, (size_t)n * sizeof(*out));
        for (int i = n - 1; i > 0; i--) {
            int j = (int)(prng() % (uint64_t)(i + 1));
            struct pgwt_trace_event t = out[i]; out[i] = out[j]; out[j] = t;
        }
    }
    return n;
}

/* ─── §1 + §2: the differential ───────────────────────────────────── */

static struct fixture fx;
static struct pgwt_trace_event evbuf[MAX_EV];
static struct resolution r_oracle[MAX_Q], r_index[MAX_Q], r_mut[MAX_Q];
static struct resolution r_oracle_v0[MAX_Q];

static long mutant_caught[M_SYNTH_FIRST_ONLY + 1];

typedef void (*fx_builder)(struct fixture *);

static void run_fixture(fx_builder build)
{
    memset(&fx, 0, sizeof(fx));
    build(&fx);
    printf("  fixture: %s (%d events, %d qids)\n", fx.name, fx.count, fx.n_qids);
    if (fx.count == 0 || fx.n_qids == 0) {
        CHECK(0, "fixture %s built nothing — it cannot see anything", fx.name);
        return;
    }

    for (int q = 0; q < fx.n_qids; q++)
        ledger_classify(&fx, fx.qids[q]);

    for (int v = 0; v < 4; v++) {
        int n = reorder(&fx, v, evbuf);
        struct world w = fx.w;

        for (int q = 0; q < fx.n_qids; q++)
            r_oracle[q] = oracle_resolution(&w, evbuf, n, fx.qids[q]);

        /* §2a: the ORDER-INDEPENDENCE property, asserted on the oracle
         * itself. If this ever fails, the one-pass form is not equivalent
         * and no amount of agreement below would mean anything. */
        if (v == 0) {
            memcpy(r_oracle_v0, r_oracle,
                   (size_t)fx.n_qids * sizeof(r_oracle[0]));
        } else {
            for (int q = 0; q < fx.n_qids; q++)
                CHECK(diff_resolution(r_oracle_v0[q], r_oracle[q]) == 0,
                      "%s v%d qid %llu: the DELETED rule is order-dependent "
                      "(state %d/%d db %u/%u uid %u/%u synth %d/%d)",
                      fx.name, v, (unsigned long long)fx.qids[q],
                      r_oracle_v0[q].state, r_oracle[q].state,
                      r_oracle_v0[q].db, r_oracle[q].db,
                      r_oracle_v0[q].uid, r_oracle[q].uid,
                      r_oracle_v0[q].synth, r_oracle[q].synth);
        }

        /* §1: index vs oracle, same order, field by field. */
        if (index_resolutions(&w, evbuf, n, fx.qids, fx.n_qids, r_index) != 0) {
            CHECK(0, "%s v%d: the index REFUSED a build with no injected "
                     "failure", fx.name, v);
            continue;
        }
        for (int q = 0; q < fx.n_qids; q++) {
            int d = diff_resolution(r_oracle[q], r_index[q]);
            led.compared++;
            CHECK(d == 0,
                  "%s v%d qid %llu: diff mask %d — oracle "
                  "(state %d db %u uid %u synth %d) vs index "
                  "(state %d db %u uid %u synth %d)",
                  fx.name, v, (unsigned long long)fx.qids[q], d,
                  r_oracle[q].state, r_oracle[q].db, r_oracle[q].uid,
                  r_oracle[q].synth,
                  r_index[q].state, r_index[q].db, r_index[q].uid,
                  r_index[q].synth);
        }

        /* §3: every mutant through the SAME comparator. */
        for (int m = M_FIRST_WINS; m <= M_SYNTH_FIRST_ONLY; m++) {
            mutant_resolutions((enum mutant)m, &w, evbuf, n,
                               fx.qids, fx.n_qids, r_mut);
            int caught = 0;
            for (int q = 0; q < fx.n_qids; q++)
                if (diff_resolution(r_oracle[q], r_mut[q]) != 0) caught = 1;
            if (caught) mutant_caught[m]++;
        }
    }
}

/* ─── §4: the bypass suite ────────────────────────────────────────── */

static void bypass_suite(void)
{
    struct world w = { std_bms, (int)(sizeof(std_bms) / sizeof(std_bms[0])) };
    struct pgwt_trace_event one[2];
    one[0] = mk(100, 1001, 0);
    one[1] = mk(101, 1001, 0);

    /* §4a: a NULL resolver must REFUSE. Returning "everything ambiguous"
     * would be a plausible-looking response with every row's text dropped
     * and nothing in the payload saying why. */
    {
        struct pgwt_query_ctx_index qc;
        int rc = pgwt_query_ctx_index_build(&qc, one, 2, NULL, NULL);
        CHECK(rc != 0, "§4a NULL resolver: build returned success");
        CHECK(qc.failed, "§4a NULL resolver: failed flag not set");
        CHECK(pgwt_query_ctx_lookup(&qc, 1001) == NULL,
              "§4a NULL resolver: a row resolved anyway");
        pgwt_query_ctx_index_free(&qc);
    }

    /* §4b: NULL / empty / negative ranges. These are LEGITIMATE emptiness
     * (the window held no events), so they must succeed with no rows and
     * `failed` clear — and every lookup must come back ABSENT, which is
     * also what the deleted function returned. */
    {
        const int counts[] = { 0, -1, -1000 };
        for (int i = 0; i < 3; i++) {
            struct pgwt_query_ctx_index qc;
            int rc = pgwt_query_ctx_index_build(&qc, one, counts[i],
                                                world_resolver, &w);
            CHECK(rc == 0 && !qc.failed,
                  "§4b count=%d: refused a legitimately empty window",
                  counts[i]);
            CHECK(pgwt_query_ctx_lookup(&qc, 1001) == NULL,
                  "§4b count=%d: resolved a row from no events", counts[i]);
            uint32_t db = 0, uid = 0;
            CHECK(oracle_query_context(&w, one, counts[i], 1001, &db, &uid) == 0,
                  "§4b count=%d: oracle disagrees about emptiness", counts[i]);
            pgwt_query_ctx_index_free(&qc);
        }
        struct pgwt_query_ctx_index qc;
        int rc = pgwt_query_ctx_index_build(&qc, NULL, 10, world_resolver, &w);
        CHECK(rc == 0 && !qc.failed, "§4b NULL events: unexpected refusal");
        CHECK(pgwt_query_ctx_lookup(&qc, 1001) == NULL,
              "§4b NULL events: resolved a row from nothing");
        pgwt_query_ctx_index_free(&qc);
    }

    /* §4c: allocation failure at EACH growth point must refuse, not return
     * a short table. 300 distinct ids crosses both. */
    {
        static struct pgwt_trace_event wide[300];
        for (int i = 0; i < 300; i++) wide[i] = mk(100, (uint64_t)i * 7 + 1, 0);
        const char *points[] = { "query_ctx_slots_grow", "qid_index_grow" };
        for (int p = 0; p < 2; p++) {
            setenv("PGWT_TEST_ALLOC_FAIL", points[p], 1);
            struct pgwt_query_ctx_index qc;
            int rc = pgwt_query_ctx_index_build(&qc, wide, 300,
                                                world_resolver, &w);
            CHECK(rc != 0, "§4c %s: build reported success", points[p]);
            CHECK(qc.failed, "§4c %s: failed flag not set", points[p]);
            pgwt_query_ctx_index_free(&qc);
            unsetenv("PGWT_TEST_ALLOC_FAIL");

            /* §4d: the injection point must be REACHED. Without the env var
             * the same fixture must succeed and resolve every id — an
             * injection that never fires proves nothing about the refusal,
             * and a point name that no longer matches any call site is the
             * silent way this whole section stops testing anything. */
            rc = pgwt_query_ctx_index_build(&qc, wide, 300,
                                            world_resolver, &w);
            CHECK(rc == 0 && !qc.failed,
                  "§4d %s: control build failed", points[p]);
            int resolved = 0;
            for (int i = 0; i < 300; i++) {
                const struct pgwt_query_ctx *row =
                    pgwt_query_ctx_lookup(&qc, (uint64_t)i * 7 + 1);
                if (row && row->state == 1 && row->databaseid == 5 &&
                    row->userid == 7) resolved++;
            }
            CHECK(resolved == 300,
                  "§4d %s: control build resolved %d/300", points[p],
                  resolved);
            pgwt_query_ctx_index_free(&qc);

            /* And a bogus point name must NOT fail the build, or §4c's
             * evidence could come from an unconditional refusal. */
            setenv("PGWT_TEST_ALLOC_FAIL", "no_such_injection_point", 1);
            rc = pgwt_query_ctx_index_build(&qc, wide, 300,
                                            world_resolver, &w);
            CHECK(rc == 0 && !qc.failed,
                  "§4d %s: an unrelated injection name failed the build",
                  points[p]);
            pgwt_query_ctx_index_free(&qc);
            unsetenv("PGWT_TEST_ALLOC_FAIL");
        }
    }

    /* §4e: a PARTIAL range must give a DIFFERENT answer. A build that only
     * ever read its first event would satisfy most of §1; this pins that the
     * whole array is traversed, by requiring the second half to change the
     * verdict. */
    {
        struct pgwt_trace_event part[2];
        part[0] = mk(100, 1001, 0);          /* resolves to db 5 / user 7 */
        part[1] = mk(102, 1001, 0);          /* disagrees -> refusal */
        struct pgwt_query_ctx_index qc;
        CHECK(pgwt_query_ctx_index_build(&qc, part, 1, world_resolver, &w) == 0,
              "§4e prefix build failed");
        const struct pgwt_query_ctx *row = pgwt_query_ctx_lookup(&qc, 1001);
        CHECK(row && row->state == 1 && row->databaseid == 5,
              "§4e prefix: expected resolved db 5");
        pgwt_query_ctx_index_free(&qc);
        CHECK(pgwt_query_ctx_index_build(&qc, part, 2, world_resolver, &w) == 0,
              "§4e full build failed");
        row = pgwt_query_ctx_lookup(&qc, 1001);
        CHECK(row && row->state == -1 && row->databaseid == 0 &&
              row->userid == 0,
              "§4e full: the second event did not reach the verdict");
        pgwt_query_ctx_index_free(&qc);
    }

    /* §4f: the comparator's own self-test. One comparator gates §1 and §3,
     * so a comparator blind to a field makes both vacuous for that field. */
    {
        struct resolution base = { 1, 5, 7, 1 };
        struct resolution s = base; s.state = -1;
        struct resolution d = base; d.db = 6;
        struct resolution u = base; u.uid = 8;
        struct resolution y = base; y.synth = 0;
        CHECK(diff_resolution(base, base) == 0, "§4f identical compared unequal");
        CHECK(diff_resolution(base, s) == DIFF_STATE, "§4f blind to state");
        CHECK(diff_resolution(base, d) == DIFF_DB, "§4f blind to databaseid");
        CHECK(diff_resolution(base, u) == DIFF_UID, "§4f blind to userid");
        CHECK(diff_resolution(base, y) == DIFF_SYNTH, "§4f blind to has_synth");
    }

    /* §4g: the ABSENT case must not be confused with the REFUSED one. Both
     * leave the response without databaseid/userid, so only `state` tells
     * them apart, and handle_top_queries falls back to the (0,0) query-text
     * lookup on 0 but NOT on -1. */
    {
        struct pgwt_query_ctx_index qc;
        struct pgwt_trace_event bad[1];
        bad[0] = mk(107, 1001, 0);          /* pid absent from the map */
        CHECK(pgwt_query_ctx_index_build(&qc, bad, 1, world_resolver, &w) == 0,
              "§4g build failed");
        const struct pgwt_query_ctx *refused = pgwt_query_ctx_lookup(&qc, 1001);
        CHECK(refused && refused->state == -1,
              "§4g missing metadata did not refuse");
        CHECK(pgwt_query_ctx_lookup(&qc, 1002) == NULL,
              "§4g an id with no events was not absent");
        pgwt_query_ctx_index_free(&qc);
    }

    /* §4h: free() then reuse. handle_top_queries builds one per request;
     * a free that left the qid map pointing at released slots would be a
     * use-after-free on the next request, not a wrong number. */
    {
        struct pgwt_query_ctx_index qc;
        CHECK(pgwt_query_ctx_index_build(&qc, one, 2, world_resolver, &w) == 0,
              "§4h first build failed");
        pgwt_query_ctx_index_free(&qc);
        CHECK(pgwt_query_ctx_lookup(&qc, 1001) == NULL,
              "§4h a freed index still resolves rows");
        CHECK(pgwt_query_ctx_index_build(&qc, one, 2, world_resolver, &w) == 0,
              "§4h rebuild after free failed");
        const struct pgwt_query_ctx *row = pgwt_query_ctx_lookup(&qc, 1001);
        CHECK(row && row->state == 1 && row->databaseid == 5 && row->userid == 7,
              "§4h rebuild resolved wrongly");
        pgwt_query_ctx_index_free(&qc);
    }
}

/* ─── §6: literal expectations ────────────────────────────────────────
 *
 * Constants, not a re-traversal of either implementation's loop. If both
 * sides ever drifted together, §1 would stay green and only this would go
 * red.
 */
static void literal_expectations(void)
{
    struct world w = { std_bms, (int)(sizeof(std_bms) / sizeof(std_bms[0])) };
    struct pgwt_trace_event ev[8];
    struct pgwt_query_ctx_index qc;
    const struct pgwt_query_ctx *row;

    /* one good pid */
    ev[0] = mk(100, 42, 0);
    CHECK(pgwt_query_ctx_index_build(&qc, ev, 1, world_resolver, &w) == 0, "§6.1 build");
    row = pgwt_query_ctx_lookup(&qc, 42);
    CHECK(row && row->state == 1 && row->databaseid == 5 && row->userid == 7 &&
          row->has_synth == 0, "§6.1 one good pid");
    pgwt_query_ctx_index_free(&qc);

    /* two pids, same context */
    ev[0] = mk(100, 42, 0); ev[1] = mk(101, 42, 0);
    CHECK(pgwt_query_ctx_index_build(&qc, ev, 2, world_resolver, &w) == 0, "§6.2 build");
    row = pgwt_query_ctx_lookup(&qc, 42);
    CHECK(row && row->state == 1 && row->databaseid == 5 && row->userid == 7,
          "§6.2 two agreeing pids");
    pgwt_query_ctx_index_free(&qc);

    /* disagreement poisons, and zeroes the ids */
    ev[0] = mk(100, 42, 0); ev[1] = mk(102, 42, 0);
    CHECK(pgwt_query_ctx_index_build(&qc, ev, 2, world_resolver, &w) == 0, "§6.3 build");
    row = pgwt_query_ctx_lookup(&qc, 42);
    CHECK(row && row->state == -1 && row->databaseid == 0 && row->userid == 0,
          "§6.3 disagreement");
    pgwt_query_ctx_index_free(&qc);

    /* ambiguous flag / zero db / zero uid / absent pid each refuse */
    const uint32_t bad_pids[] = { 104, 105, 106, 107, 999 };
    for (int i = 0; i < 5; i++) {
        ev[0] = mk(100, 42, 0); ev[1] = mk(bad_pids[i], 42, 0);
        CHECK(pgwt_query_ctx_index_build(&qc, ev, 2, world_resolver, &w) == 0, "§6.4 build");
        row = pgwt_query_ctx_lookup(&qc, 42);
        CHECK(row && row->state == -1 && row->databaseid == 0,
              "§6.4 pid %u did not refuse", bad_pids[i]);
        pgwt_query_ctx_index_free(&qc);
    }

    /* synth survives a refusal, and is found on a later event */
    ev[0] = mk(104, 42, 0);
    ev[1] = mk(100, 42, PGWT_EVENT_FLAG_QUERY_SYNTH);
    CHECK(pgwt_query_ctx_index_build(&qc, ev, 2, world_resolver, &w) == 0, "§6.5 build");
    row = pgwt_query_ctx_lookup(&qc, 42);
    CHECK(row && row->state == -1 && row->has_synth == 1,
          "§6.5 synth lost behind a refusal");
    pgwt_query_ctx_index_free(&qc);

    /* query_id 0 is a legal key with its own contributors */
    ev[0] = mk(0, 0, 0); ev[1] = mk(100, 1, 0);
    CHECK(pgwt_query_ctx_index_build(&qc, ev, 2, world_resolver, &w) == 0, "§6.6 build");
    row = pgwt_query_ctx_lookup(&qc, 0);
    CHECK(row && row->state == 1 && row->databaseid == 9 && row->userid == 3,
          "§6.6 query_id 0 resolved wrongly");
    row = pgwt_query_ctx_lookup(&qc, 1);
    CHECK(row && row->state == 1 && row->databaseid == 5,
          "§6.6 qid 1 inherited qid 0's slot");
    pgwt_query_ctx_index_free(&qc);

    /* an unrelated flag bit must not read as synth */
    ev[0] = mk(100, 42, PGWT_EVENT_FLAG_SAMPLE);
    CHECK(pgwt_query_ctx_index_build(&qc, ev, 1, world_resolver, &w) == 0, "§6.7 build");
    row = pgwt_query_ctx_lookup(&qc, 42);
    CHECK(row && row->has_synth == 0, "§6.7 SAMPLE read as QUERY_SYNTH");
    pgwt_query_ctx_index_free(&qc);
}

int main(void)
{
    printf("=== top_queries context: one pass vs the deleted per-row scans ===\n");

    printf("\n§1/§2 differential (4 orders per fixture)\n");
    run_fixture(fx_shapes);
    run_fixture(fx_all_clean);
    run_fixture(fx_no_synth);
    run_fixture(fx_wide);
    run_fixture(fx_dense);
    run_fixture(fx_random);

    printf("\n§3 mutation probes (each must be caught by the comparator)\n");
    static const char *mnames[] = {
        "", "first-wins", "latest-wins", "resume-after-refusal",
        "absent-reads-resolved", "missing-metadata-ignored",
        "zero-userid-accepted", "synth-from-first-event-only"
    };
    for (int m = M_FIRST_WINS; m <= M_SYNTH_FIRST_ONLY; m++) {
        printf("  %-30s caught in %ld fixture/order runs\n",
               mnames[m], mutant_caught[m]);
        CHECK(mutant_caught[m] > 0,
              "§3 mutant %s was NEVER caught — the differential cannot see "
              "this class of wrong answer", mnames[m]);
    }

    /* §3b: the converse. The blind fixtures must be blind — if they alone
     * caught everything, the adversarial fixtures would be decoration and
     * nobody would notice when they rotted. */
    printf("\n§3b blindness of the deliberately-blind fixtures\n");
    {
        struct world w = { std_bms, (int)(sizeof(std_bms) / sizeof(std_bms[0])) };
        memset(&fx, 0, sizeof(fx));
        fx_all_clean(&fx);
        int blind[] = { M_FIRST_WINS, M_LATEST_WINS, M_RESUME,
                        M_MISSING_IGNORED, M_ZERO_UID_OK };
        for (int b = 0; b < 5; b++) {
            for (int q = 0; q < fx.n_qids; q++)
                r_oracle[q] = oracle_resolution(&w, fx.ev, fx.count, fx.qids[q]);
            mutant_resolutions((enum mutant)blind[b], &w, fx.ev, fx.count,
                               fx.qids, fx.n_qids, r_mut);
            int caught = 0;
            for (int q = 0; q < fx.n_qids; q++)
                if (diff_resolution(r_oracle[q], r_mut[q]) != 0) caught = 1;
            printf("  all-clean vs %-26s caught=%d (expected 0)\n",
                   mnames[blind[b]], caught);
            CHECK(caught == 0,
                  "§3b the all-clean fixture caught %s — it was supposed to "
                  "be blind, so the fixture set no longer means what the "
                  "header says it means", mnames[blind[b]]);
        }
        memset(&fx, 0, sizeof(fx));
        fx_no_synth(&fx);
        for (int q = 0; q < fx.n_qids; q++)
            r_oracle[q] = oracle_resolution(&w, fx.ev, fx.count, fx.qids[q]);
        mutant_resolutions(M_SYNTH_FIRST_ONLY, &w, fx.ev, fx.count,
                           fx.qids, fx.n_qids, r_mut);
        int caught = 0;
        for (int q = 0; q < fx.n_qids; q++)
            if (diff_resolution(r_oracle[q], r_mut[q]) != 0) caught = 1;
        printf("  no-synth vs %-27s caught=%d (expected 0)\n",
               mnames[M_SYNTH_FIRST_ONLY], caught);
        CHECK(caught == 0, "§3b the no-synth fixture caught %s",
              mnames[M_SYNTH_FIRST_ONLY]);
    }

    printf("\n§4 bypass suite\n");
    bypass_suite();

    printf("\n§6 literal expectations\n");
    literal_expectations();

    /* The growth counters cannot be read off a fixture, so they are taken
     * from the real index: build fx_wide and read cap/n back. */
    {
        memset(&fx, 0, sizeof(fx));
        fx_wide(&fx);
        struct pgwt_query_ctx_index qc;
        struct world w = fx.w;
        if (pgwt_query_ctx_index_build(&qc, fx.ev, fx.count, world_resolver,
                                       &w) == 0) {
            if (qc.cap > 64) led.growth_slots = qc.cap;
            if (qc.by_qid.cap > 128) led.growth_qid_index = qc.by_qid.cap;
        }
        pgwt_query_ctx_index_free(&qc);
    }

    printf("\n§5 non-vacuity ledger\n");
    struct { const char *name; long v; } l[] = {
        { "comparisons",                led.compared },
        { "resolved, one pid",          led.resolved_one_pid },
        { "resolved, several agreeing", led.resolved_multi_agree },
        { "refused: pid not in map",    led.refused_missing_bm },
        { "refused: ambiguous flag",    led.refused_ambiguous_flag },
        { "refused: databaseid 0",      led.refused_zero_db },
        { "refused: userid 0",          led.refused_zero_uid },
        { "refused: disagreement",      led.refused_disagree },
        { "absent (no contributor)",    led.absent },
        { "synth present",              led.synth_present },
        { "synth absent",               led.synth_absent },
        { "query_id 0 with events",     led.qid_zero_with_events },
        { "slot array grew to",         led.growth_slots },
        { "qid index grew to",          led.growth_qid_index },
    };
    for (size_t i = 0; i < sizeof(l) / sizeof(l[0]); i++) {
        printf("  %-28s %ld\n", l[i].name, l[i].v);
        CHECK(l[i].v > 0,
              "§5 ledger '%s' is 0 — the suite never exercised this shape, "
              "so agreement about it means nothing", l[i].name);
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
