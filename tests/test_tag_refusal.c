/* test_tag_refusal.c — #363: pgwt_tag_events must REFUSE, never half-tag.
 *
 * THE DEFECT. pgwt_tag_events (src/compute.c) was declared `void` and had
 * two ways to stop partway:
 *
 *   1. the working allocations — the per-pid state table, and the forward
 *      query-id array, which is one uint64 per record (~45 MB on a 5.7M-event
 *      window). `if (!ht) return;` / `fwd = ... ; if (fwd) ...` both carried
 *      on as if nothing had happened;
 *   2. the per-pid table filling up — `tag_pid_get` returned NULL for the
 *      (TAG_HT_SIZE+1)-th distinct pid and the caller did `continue`, so
 *      every record of every surplus pid stayed exactly as it came off disk.
 *
 * WHY THAT IS WORSE THAN AN OOM. This array IS the oracle: #357 established
 * that every aggregate path reconciles to it, never the reverse. The pass
 * rewrites old_event 0 -> PGWT_WEI_NONCMD_CPU (post/between-command time the
 * contract excludes from DB Time, docs/AAS_SEMANTICS_DECISION.md) and
 * back-fills query_id. A record the pass never reached therefore reads as
 * CPU inside a command, attributed to whatever statement happened to be
 * resolved at emission time. So a partial pass does not return a smaller
 * answer; it returns a confidently WRONG one, biased towards looking busier,
 * with nothing in the response saying so. On the demo trace (#362) the
 * relabel moves 44,119 records and the CPU->ClientRead link by 16%.
 *
 * THE FIX UNDER TEST. `int` return: 1 = every record tagged, 0 = refused.
 * The single call site (server_load_events_fi_mode) routes 0 into the
 * loader's EXISTING info->allocation_failed channel and discards the array.
 *
 * WHAT THIS FILE CAN AND CANNOT REACH. pgwt_tag_events is C, so sections 1-6
 * drive the real function directly. The CALL SITE is in server.c, which no C
 * unit can link (same limitation as tests/test_server_alloc_tune.py); that
 * half — including the dump_summary route, which reads allocation_failed
 * directly rather than through reject_overload — is
 * tests/test_data_tag_refusal.py, driving the real pgwt-server binary.
 *
 * SECTION 6 IS ABOUT FALSE NEGATIVES, because every assertion about a
 * refusal is satisfied by a function that refuses ALWAYS, and every
 * assertion about a success is satisfied by one that never refuses. So:
 * section 1 pins the success path by LITERAL tag counts (a refuse-always
 * build fails it), and section 6 pins the refusals against the ways they
 * could be reached without checking anything — an injection name that does
 * not exist, an empty env var, an injection point that cannot be reached
 * (count 0), a fixture with nothing to tag (absent, not wrong), and
 * leakage from one case to the next.
 *
 * TIMING AND ORDERING. Nothing here reads a clock, allocates concurrently,
 * or depends on scheduling: in-memory arrays, synthetic timestamps. The one
 * order-dependent property is WHERE the failure lands — section 2/3 refuse
 * BEFORE any record is touched (asserted by memcmp against the input), while
 * section 4 refuses MID-ARRAY, with partial rewrites already applied
 * (asserted by memcmp showing a difference, and by a relabel count strictly
 * between zero and complete). That partial state is the whole reason the
 * caller must invalidate the count instead of using what it got.
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

/* compute.c's only foreign symbol (summary streaming) — unused here. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    (void)visitor; (void)ctx;
    return -1;
}

/* ── Non-vacuity ledger ───────────────────────────────────────────────────
 * Every counter here must end non-zero. Each one is a thing this file
 * claims to have exercised; at zero the corresponding assertions were
 * vacuous and the test fails. */
static struct {
    int relabels_seen;        /* records the success path moved to NONCMD_CPU */
    int backfills_seen;       /* records the success path re-attributed */
    int cat_flags_seen;       /* records stamped from the pid->category table */
    int refusals_alloc;       /* refusals from an injected allocation failure */
    int refusals_exhaustion;  /* refusals from the REAL pid table filling up */
    int partial_relabels;     /* relabels present in a REFUSED array */
    int unreached_injections; /* injection points set but not reachable */
} led;

#define ENV_FAIL "PGWT_TEST_LOAD_ALLOC_FAIL"

/* WEI() comes from pg_wait_tracer.h. */
#define EV_LOCK WEI(PG_WAIT_LOCK, 1)
#define EV_IO   WEI(PG_WAIT_IO, 7)
#define MS      1000000ULL
#define BASE_TS (5000ULL * 1000000000ULL)
#define BASE_PID 40000u
#define QID     0xABCDEF01ULL

/* ── Fixture ──────────────────────────────────────────────────────────────
 * Per pid, six records that make the tagging pass OBSERVABLE — if the pass
 * does not run, four of the six differ:
 *
 *   t+0   CMD_START                      (marker)
 *   t+1   Lock, query_id 0               -> back-filled to QID (rule 1)
 *   t+2   we==0, 1 ms, query_id 0        -> stays CPU, FLAG_CMD_OPEN,
 *                                           back-filled to QID
 *   t+3   IO, query_id QID               -> the id the back-fill comes from
 *   t+4   CMD_END                        (marker)
 *   t+10  we==0, 4 ms                    -> PGWT_WEI_NONCMD_CPU
 *
 * The last record's 4 ms duration is deliberate: CMD_END banks the 1 ms
 * between the previous record and the close, and the majority rule would
 * call a 1 ms interval in-command on the strength of that banked time. Four
 * milliseconds puts it majority-outside, which is what makes the relabel
 * happen at all. */
#define RECS_PER_PID 6

static struct pgwt_trace_event mk(uint64_t ts, uint32_t pid, uint32_t we,
                                  uint64_t dur_ns, uint64_t qid)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid = pid;
    e.old_event = we;
    e.duration_ns = dur_ns;
    e.query_id = qid;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

/* Fill `ev` with npids pids' worth of the sequence above, interleaved by
 * time so the pids are discovered in round-robin order (a cap that drops a
 * tail would otherwise be indistinguishable from one that drops a scattered
 * subset). Returns the record count. */
static int build_fixture(struct pgwt_trace_event *ev, int npids)
{
    int n = 0;
    for (int p = 0; p < npids; p++) {
        uint32_t pid = BASE_PID + (uint32_t)p;
        uint64_t t = BASE_TS + (uint64_t)p * 20 * MS;
        ev[n++] = mk(t,            pid, PGWT_MARKER_CMD_START, 0, 0);
        ev[n++] = mk(t +  1 * MS,  pid, EV_LOCK,            1 * MS, 0);
        ev[n++] = mk(t +  2 * MS,  pid, 0,                  1 * MS, 0);
        ev[n++] = mk(t +  3 * MS,  pid, EV_IO,              1 * MS, QID);
        ev[n++] = mk(t +  4 * MS,  pid, PGWT_MARKER_CMD_END,     0, 0);
        ev[n++] = mk(t + 10 * MS,  pid, 0,                  4 * MS, 0);
    }
    return n;
}

static struct pgwt_trace_event *alloc_fixture(int npids, int *out_n)
{
    struct pgwt_trace_event *ev =
        calloc((size_t)npids * RECS_PER_PID, sizeof(*ev));
    if (!ev) {
        printf("  REFUSE: cannot allocate a %d-pid fixture\n", npids);
        exit(2);
    }
    *out_n = build_fixture(ev, npids);
    return ev;
}

static int count_relabels(const struct pgwt_trace_event *ev, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        if (ev[i].old_event == PGWT_WEI_NONCMD_CPU) c++;
    return c;
}

static int count_backfills(const struct pgwt_trace_event *ev, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        if ((ev[i].flags & PGWT_EVENT_FLAG_QUERY_BACKFILL) &&
            ev[i].query_id == QID) c++;
    return c;
}

static int count_flag(const struct pgwt_trace_event *ev, int n, uint32_t flag)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        if (ev[i].flags & flag) c++;
    return c;
}

/* ── 1. The SUCCESS path, by literal counts ───────────────────────────────
 * Without this section every refusal assertion in the file is satisfied by
 * a pgwt_tag_events that returns 0 unconditionally. The counts are literal
 * (3 pids -> 3 relabels, 6 back-fills), not re-derived from the function's
 * own output. */
#define NPIDS_OK 3

static void s1_success_is_complete_and_does_real_work(void)
{
    printf("--- 1. success: returns 1 and the tagging actually happened ---\n");
    int n;
    struct pgwt_trace_event *ev = alloc_fixture(NPIDS_OK, &n);

    CHECK(pgwt_tag_events(ev, n, NULL, 0) == 1,
          "a taggable window returns 1 (complete)");

    int relabels  = count_relabels(ev, n);
    int backfills = count_backfills(ev, n);
    int cmd_open  = count_flag(ev, n, PGWT_EVENT_FLAG_CMD_OPEN);
    led.relabels_seen  += relabels;
    led.backfills_seen += backfills;

    CHECK_INT(relabels, NPIDS_OK,
              "one we==0 record per pid became NONCMD_CPU (the relabel the "
              "untagged array gets wrong, in the direction of more CPU)");
    CHECK_INT(backfills, NPIDS_OK * 2,
              "two records per pid were re-attributed to the command's id");
    CHECK(cmd_open >= NPIDS_OK,
          "in-command records carry FLAG_CMD_OPEN");

    /* The relabelled record must be the LAST of each pid, not an arbitrary
     * one: a pass that relabelled the in-command we==0 record instead would
     * produce the same count. */
    int in_cmd_relabelled = 0;
    for (int i = 0; i < n; i++)
        if (ev[i].old_event == PGWT_WEI_NONCMD_CPU &&
            (ev[i].flags & PGWT_EVENT_FLAG_CMD_OPEN))
            in_cmd_relabelled++;
    CHECK_INT(in_cmd_relabelled, 0,
              "no in-command record was relabelled (the count above is the "
              "right records, not just the right number of them)");

    free(ev);
}

/* The pid->category table is the third thing the pass writes; a refusal
 * must not be confused with "no metadata". Separate pid so the MAINT flag
 * does not suppress the relabel asserted above. */
static void s1b_category_flags_are_stamped(void)
{
    printf("--- 1b. success: category flags come from the pid table ---\n");
    int n;
    struct pgwt_trace_event *ev = alloc_fixture(2, &n);
    struct pgwt_pid_cat cats[2] = {
        { BASE_PID,     PGWT_EVENT_FLAG_MAINT },
        { BASE_PID + 1, 0 },
    };
    CHECK(pgwt_tag_events(ev, n, cats, 2) == 1,
          "tagging with a pid->category table returns 1");
    int maint = count_flag(ev, n, PGWT_EVENT_FLAG_MAINT);
    led.cat_flags_seen += maint;
    CHECK(maint > 0, "the autovacuum pid's records carry FLAG_MAINT");
    CHECK_INT(count_relabels(ev, n), 1,
              "only the FOREGROUND pid's we==0 record was relabelled");
    free(ev);
}

/* ── 2/3. The two injected allocation failures, as SEPARATE cases ─────────
 * Both allocations happen before the first record is read, so the refusal
 * must leave the array byte-identical: there is no half-tagged state to
 * argue about here, and a caller that ignored the return value would get
 * the raw array rather than a plausible-looking one. */
static void refused_before_any_mutation(const char *point, const char *what)
{
    int n;
    struct pgwt_trace_event *ev = alloc_fixture(NPIDS_OK, &n);
    struct pgwt_trace_event *before = calloc((size_t)n, sizeof(*before));
    memcpy(before, ev, (size_t)n * sizeof(*ev));

    setenv(ENV_FAIL, point, 1);
    int rc = pgwt_tag_events(ev, n, NULL, 0);
    unsetenv(ENV_FAIL);

    char msg[256];
    snprintf(msg, sizeof(msg), "%s: returns 0 (refused)", what);
    CHECK(rc == 0, msg);
    if (rc == 0) led.refusals_alloc++;
    snprintf(msg, sizeof(msg),
             "%s: the array is byte-identical to the input", what);
    CHECK(memcmp(before, ev, (size_t)n * sizeof(*ev)) == 0, msg);
    snprintf(msg, sizeof(msg), "%s: nothing was relabelled", what);
    CHECK_INT(count_relabels(ev, n), 0, msg);

    free(before);
    free(ev);
}

static void s2_forward_array_allocation_failure(void)
{
    printf("--- 2. the FORWARD query-id array fails to allocate ---\n");
    refused_before_any_mutation("tag_fwd", "tag_fwd");
}

static void s3_pid_state_table_allocation_failure(void)
{
    printf("--- 3. the PER-PID STATE TABLE fails to allocate ---\n");
    refused_before_any_mutation("tag_ht", "tag_ht");
}

/* ── 4. PID-table EXHAUSTION — the real table, no test hook ───────────────
 * This is the path the issue is really about: no allocation failed, the
 * machine is healthy, the window simply holds more distinct pids than the
 * table has slots (connection CHURN, not concurrency, drives that count).
 *
 * The table size is private to compute.c, so this searches for the bound
 * instead of hard-coding it: a future change that grows the table makes the
 * search find a bigger threshold, and one that removes the bound entirely
 * makes it REFUSE at the ceiling rather than quietly becoming a
 * success-path test. Both failure modes are loud. */
#define PID_CEILING (1 << 17)

static void s4_pid_table_exhaustion(void)
{
    printf("--- 4. the per-pid table fills up (no injection) ---\n");
    int refuse_at = 0, last_ok = 0;
    for (int npids = 256; npids <= PID_CEILING; npids *= 2) {
        int n;
        struct pgwt_trace_event *ev = alloc_fixture(npids, &n);
        int rc = pgwt_tag_events(ev, n, NULL, 0);
        free(ev);
        if (rc) { last_ok = npids; continue; }
        refuse_at = npids;
        break;
    }

    if (!refuse_at) {
        checks++; failures++;
        printf("  FAIL: REFUSE — no distinct-pid count up to %d made the "
               "tagging pass refuse, so this case cannot see the defect "
               "it exists for. Either the per-pid table is now unbounded "
               "(delete this section and say so) or the refusal was lost.\n",
               PID_CEILING);
        return;
    }
    printf("  (bound located: %d pids tag completely, %d refuse)\n",
           last_ok, refuse_at);
    CHECK(last_ok > 0,
          "a pid count just under the bound still tags COMPLETELY, so the "
          "refusal is a bound and not an unconditional 0");

    /* At the refusing count: refuse, and prove the partial state is real. */
    int n;
    struct pgwt_trace_event *ev = alloc_fixture(refuse_at, &n);
    struct pgwt_trace_event *before = calloc((size_t)n, sizeof(*before));
    memcpy(before, ev, (size_t)n * sizeof(*ev));
    int rc = pgwt_tag_events(ev, n, NULL, 0);
    CHECK(rc == 0, "more distinct pids than the table holds returns 0");
    if (rc == 0) led.refusals_exhaustion++;

    int partial = count_relabels(ev, n);
    led.partial_relabels += partial;
    CHECK(memcmp(before, ev, (size_t)n * sizeof(*ev)) != 0,
          "the refused array IS partly rewritten — which is why the caller "
          "must discard it rather than use what it got");
    CHECK(partial > 0 && partial < refuse_at,
          "the partial rewrite covers SOME pids and not all: this is the "
          "silently-wrong array the pre-fix code handed back");
    CHECK_INT(count_flag(ev, n, PGWT_EVENT_FLAG_QUERY_BACKFILL), 0,
              "the back-fill pass did not run at all, so the surviving "
              "query ids are emission-time ids (the wrong statement)");

    free(before);
    free(ev);
}

/* ── 5. Absent input is a COMPLETE result, not a refusal ──────────────────
 * The opposite failure mode of the one above: a refusal on every empty
 * window would turn each quiet period into "memory allocation failed". */
static void s5_empty_is_success(void)
{
    printf("--- 5. empty / absent input ---\n");
    CHECK(pgwt_tag_events(NULL, 0, NULL, 0) == 1,
          "an empty window (NULL, 0) is a complete success");

    int n;
    struct pgwt_trace_event *ev = alloc_fixture(1, &n);
    CHECK(pgwt_tag_events(ev, 0, NULL, 0) == 1,
          "a non-NULL array with count 0 is a complete success");
    CHECK(pgwt_tag_events(ev, -1, NULL, 0) == 1,
          "a negative count does not crash and is not reported as a "
          "tagging failure");
    CHECK_INT(count_relabels(ev, n), 0,
              "neither call touched the array");
    free(ev);
}

/* ── 6. BYPASS SUITE: every way the refusal could be reached, or avoided,
 *      without checking anything ─────────────────────────────────────────── */

/* 6a. An injection point that does not exist must fail NOTHING. Without
 * this, sections 2 and 3 could be passing because the env var was merely
 * SET rather than because the named allocation failed. */
static void s6a_unknown_injection_point_fails_nothing(void)
{
    printf("--- 6a. an unknown injection point must not refuse ---\n");
    int n;
    struct pgwt_trace_event *ev = alloc_fixture(NPIDS_OK, &n);
    setenv(ENV_FAIL, "tag_fwd_but_misspelled", 1);
    int rc = pgwt_tag_events(ev, n, NULL, 0);
    unsetenv(ENV_FAIL);
    CHECK(rc == 1, "an unknown injection point returns 1 (name-matched, so "
                   "sections 2/3 refused because of the NAMED allocation)");
    CHECK_INT(count_relabels(ev, n), NPIDS_OK,
              "and the pass did its full work under it");
    free(ev);
}

/* 6b. Set-but-empty: the same shape as "the variable exists in the
 * environment of every test on this box". */
static void s6b_empty_injection_value_fails_nothing(void)
{
    printf("--- 6b. an empty injection value must not refuse ---\n");
    int n;
    struct pgwt_trace_event *ev = alloc_fixture(NPIDS_OK, &n);
    setenv(ENV_FAIL, "", 1);
    int rc = pgwt_tag_events(ev, n, NULL, 0);
    unsetenv(ENV_FAIL);
    CHECK(rc == 1, "an empty injection value returns 1");
    free(ev);
}

/* 6c. An injection point that is set but NOT REACHABLE must not be read as
 * a refusal: with count 0 the function returns before either allocation.
 * A gate that cannot reach its own failure point must say so by passing
 * cleanly, never by refusing for the wrong reason. */
static void s6c_injection_set_but_unreachable(void)
{
    printf("--- 6c. an injection point set but never reached ---\n");
    setenv(ENV_FAIL, "tag_fwd", 1);
    int rc_fwd = pgwt_tag_events(NULL, 0, NULL, 0);
    setenv(ENV_FAIL, "tag_ht", 1);
    int rc_ht = pgwt_tag_events(NULL, 0, NULL, 0);
    unsetenv(ENV_FAIL);
    CHECK(rc_fwd == 1 && rc_ht == 1,
          "an empty window is complete even with both injections armed "
          "(the allocations never happen, so there is nothing to fail)");
    if (rc_fwd == 1 && rc_ht == 1) led.unreached_injections += 2;
}

/* 6d. ABSENT vs WRONG. A fixture with no markers and no query ids gives the
 * pass nothing to do. Its array comes back with zero relabels — exactly
 * like a REFUSED array — so the return value is the only thing that
 * distinguishes "nothing to tag" from "could not tag". Assert both, or a
 * caller could reasonably infer completeness from the array's contents. */
static void s6d_nothing_to_tag_is_not_a_refusal(void)
{
    printf("--- 6d. nothing to tag (absent) vs could not tag (refused) ---\n");
    struct pgwt_trace_event ev[3];
    int n = 0;
    ev[n++] = mk(BASE_TS + 1 * MS, BASE_PID, EV_IO, 1 * MS, QID);
    ev[n++] = mk(BASE_TS + 2 * MS, BASE_PID, EV_IO, 1 * MS, QID);
    ev[n++] = mk(BASE_TS + 3 * MS, BASE_PID, 0,     1 * MS, QID);

    struct pgwt_trace_event ok[3], refused[3];
    memcpy(ok, ev, sizeof(ev));
    memcpy(refused, ev, sizeof(ev));

    int rc_ok = pgwt_tag_events(ok, n, NULL, 0);
    setenv(ENV_FAIL, "tag_fwd", 1);
    int rc_ref = pgwt_tag_events(refused, n, NULL, 0);
    unsetenv(ENV_FAIL);

    CHECK(rc_ok == 1, "a fixture with no command markers tags COMPLETELY");
    CHECK_INT(count_relabels(ok, n), 0,
              "and relabels nothing — there is no command window to be "
              "outside of (absent, not wrong)");
    CHECK(rc_ref == 0,
          "the same fixture under an injected failure REFUSES, even though "
          "it had no tagging work to do: the refusal is not contingent on "
          "the fixture mattering");
    if (rc_ref == 0) led.refusals_alloc++;
    CHECK_INT(count_relabels(refused, n), 0,
              "both arrays look identical afterwards — the return value is "
              "the ONLY signal, which is why `void` was the bug");
}

/* 6e. No leakage between cases: a refusal must not be sticky, or every
 * later success assertion in this file would be measuring state left by an
 * earlier setenv rather than the function. */
static void s6e_no_leakage_between_cases(void)
{
    printf("--- 6e. a refusal does not persist into the next call ---\n");
    int n;
    struct pgwt_trace_event *ev = alloc_fixture(NPIDS_OK, &n);
    setenv(ENV_FAIL, "tag_fwd", 1);
    (void)!pgwt_tag_events(ev, n, NULL, 0);
    unsetenv(ENV_FAIL);
    free(ev);

    ev = alloc_fixture(NPIDS_OK, &n);
    CHECK(pgwt_tag_events(ev, n, NULL, 0) == 1,
          "the call after a refused one succeeds");
    CHECK_INT(count_relabels(ev, n), NPIDS_OK,
          "with the full tag count, so no case here is reading another's "
          "leftovers");
    free(ev);
}

static void ledger(void)
{
    printf("--- non-vacuity ledger ---\n");
    CHECK(led.relabels_seen > 0,
          "the success path relabelled records somewhere in this file");
    CHECK(led.backfills_seen > 0,
          "the success path back-filled query ids somewhere in this file");
    CHECK(led.cat_flags_seen > 0,
          "the success path stamped category flags somewhere in this file");
    CHECK(led.refusals_alloc >= 3,
          "both injected allocation failures, and the no-work fixture, "
          "actually produced refusals");
    CHECK(led.refusals_exhaustion > 0,
          "the REAL pid-table bound produced a refusal with no test hook");
    CHECK(led.partial_relabels > 0,
          "a refused array was observed partly rewritten (the reason the "
          "caller must invalidate the count, not just flag it)");
    CHECK(led.unreached_injections > 0,
          "an armed-but-unreachable injection point was exercised");
}

int main(void)
{
    printf("=== test_tag_refusal (#363) ===\n");
    /* The suite sets this itself, per case. Inheriting it would make every
     * success assertion below refuse for a reason this file did not choose. */
    if (getenv(ENV_FAIL)) {
        printf("REFUSE: %s is set in the environment (%s); this suite "
               "controls that variable per case and cannot run under an "
               "inherited value.\n", ENV_FAIL, getenv(ENV_FAIL));
        return 1;
    }

    s1_success_is_complete_and_does_real_work();
    s1b_category_flags_are_stamped();
    s2_forward_array_allocation_failure();
    s3_pid_state_table_allocation_failure();
    s4_pid_table_exhaustion();
    s5_empty_is_success();
    s6a_unknown_injection_point_fails_nothing();
    s6b_empty_injection_value_fails_nothing();
    s6c_injection_set_but_unreachable();
    s6d_nothing_to_tag_is_not_a_refusal();
    s6e_no_leakage_between_cases();
    ledger();

    printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
