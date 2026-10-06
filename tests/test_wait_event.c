/* test_wait_event.c — Unit tests for wait event decode tables (PG18) */
#include "wait_event.h"
#include "idle_rule.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>   /* mkdtemp on BSD/macOS; harmless on glibc */

static int tests_run = 0;
static int tests_passed = 0;

/* WEI is provided by pg_wait_tracer.h */

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

#define CHECK_NAME(wei, expected) do { \
    char buf[128]; \
    pgwt_event_full_name(wei, buf, sizeof(buf)); \
    CHECK(strcmp(buf, expected) == 0, \
          "event 0x%08x: expected \"%s\", got \"%s\"", wei, expected, buf); \
} while(0)

static void test_cpu(void)
{
    printf("--- CPU ---\n");
    CHECK_NAME(0, "CPU*");
    CHECK(strcmp(pgwt_class_name(0), "CPU") == 0,
          "class_name(0) expected CPU");
    CHECK(strcmp(pgwt_event_name(0), "CPU") == 0,
          "event_name(0) expected CPU");
    CHECK(pgwt_is_idle_event(0) == 0, "CPU should not be idle");
    CHECK(pgwt_is_hidden_event(0) == 0, "CPU should not be hidden");
}

static void test_io_events(void)
{
    printf("--- IO Events ---\n");
    CHECK_NAME(WEI(PG_WAIT_IO, 0),  "IO:AioIoCompletion");
    CHECK_NAME(WEI(PG_WAIT_IO, 3),  "IO:BasebackupRead");
    CHECK_NAME(WEI(PG_WAIT_IO, 17), "IO:DataFileExtend");
    CHECK_NAME(WEI(PG_WAIT_IO, 18), "IO:DataFileFlush");
    CHECK_NAME(WEI(PG_WAIT_IO, 21), "IO:DataFileRead");
    CHECK_NAME(WEI(PG_WAIT_IO, 24), "IO:DataFileWrite");
    CHECK_NAME(WEI(PG_WAIT_IO, 50), "IO:SlruFlushSync");
    CHECK_NAME(WEI(PG_WAIT_IO, 75), "IO:WalRead");
    CHECK_NAME(WEI(PG_WAIT_IO, 78), "IO:WalSync");
    CHECK_NAME(WEI(PG_WAIT_IO, 80), "IO:WalWrite");
    /* Class name */
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_IO, 0)), "IO") == 0,
          "class_name for IO");
    /* Not idle, not hidden */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_IO, 21)) == 0,
          "IO events should not be idle");
    CHECK(pgwt_is_hidden_event(WEI(PG_WAIT_IO, 21)) == 0,
          "IO events should not be hidden");
}

static void test_lock_events(void)
{
    printf("--- Lock Events (0-indexed, matches LockTagType) ---\n");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 0),  "Lock:relation");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 1),  "Lock:extend");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 3),  "Lock:page");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 4),  "Lock:tuple");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 5),  "Lock:transactionid");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 6),  "Lock:virtualxid");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 10), "Lock:advisory");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 11), "Lock:applytransaction");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_LOCK, 0)), "Lock") == 0,
          "class_name for Lock");
}

static void test_lwlock_events(void)
{
    printf("--- LWLock Events ---\n");
    /* Predefined LWLocks */
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 1),  "LWLock:ShmemIndex");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 3),  "LWLock:XidGen");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 4),  "LWLock:ProcArray");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 8),  "LWLock:WALWrite");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 9),  "LWLock:ControlFile");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 22), "LWLock:Autovacuum");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 32), "LWLock:SyncRep");
    /* Builtin tranches */
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 54), "LWLock:XactBuffer");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 61), "LWLock:WALInsert");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 62), "LWLock:BufferContent");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 66), "LWLock:BufferMapping");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 67), "LWLock:LockManager");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 92), "LWLock:XactSLRU");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 94), "LWLock:AioUringCompletion");
    /* Unknown tranche → numeric fallback */
    char buf[128];
    pgwt_event_full_name(WEI(PG_WAIT_LWLOCK, 200), buf, sizeof(buf));
    CHECK(strstr(buf, "id=200") != NULL,
          "LWLock unknown tranche should show id=200, got \"%s\"", buf);
    /* Removed slots (e.g. 10, 11) → numeric fallback */
    pgwt_event_full_name(WEI(PG_WAIT_LWLOCK, 10), buf, sizeof(buf));
    CHECK(strstr(buf, "id=10") != NULL,
          "LWLock removed slot 10 should show id=10, got \"%s\"", buf);

    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_LWLOCK, 1)), "LWLock") == 0,
          "class_name for LWLock");
}

static void test_timeout_events(void)
{
    printf("--- Timeout Events ---\n");
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 0), "Timeout:BaseBackupThrottle");
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 1), "Timeout:CheckpointWriteDelay");
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 2), "Timeout:PgSleep");
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 6), "Timeout:SpinDelay");
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 9), "Timeout:WalSummarizerError");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_TIMEOUT, 0)), "Timeout") == 0,
          "class_name for Timeout");
}

static void test_client_events(void)
{
    printf("--- Client Events ---\n");
    CHECK_NAME(WEI(PG_WAIT_CLIENT, 0), "Client:ClientRead");
    CHECK_NAME(WEI(PG_WAIT_CLIENT, 1), "Client:ClientWrite");
    CHECK_NAME(WEI(PG_WAIT_CLIENT, 5), "Client:SslOpenServer");
    CHECK_NAME(WEI(PG_WAIT_CLIENT, 8), "Client:WalSenderWriteData");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_CLIENT, 0)), "Client") == 0,
          "class_name for Client");
    /* Client:ClientRead is IDLE for LOAD accounting — excluded from DB
     * Time / AAS, like Oracle's "SQL*Net message from client" — but it is
     * NOT hidden: it must remain visible in event lists/graphs. See the
     * load-vs-visibility split in src/wait_event.c. */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_CLIENT, 0)) == 1,
          "Client:ClientRead should be idle (excluded from DB Time)");
    CHECK(pgwt_is_hidden_event(WEI(PG_WAIT_CLIENT, 0)) == 0,
          "Client:ClientRead should NOT be hidden (stays visible)");
    /* Other Client events are NOT idle and NOT hidden */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_CLIENT, 1)) == 0,
          "Client:ClientWrite should NOT be idle");
    CHECK(pgwt_is_hidden_event(WEI(PG_WAIT_CLIENT, 1)) == 0,
          "Client:ClientWrite should NOT be hidden");
}

static void test_activity_events(void)
{
    printf("--- Activity Events ---\n");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 0),  "Activity:ArchiverMain");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 4),  "Activity:CheckpointerMain");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 6),  "Activity:IoWorkerMain");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 17), "Activity:WalWriterMain");
    /* Activity events ARE idle AND hidden */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_ACTIVITY, 0)) != 0,
          "Activity events should be idle");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_ACTIVITY, 4)) != 0,
          "Activity:CheckpointerMain should be idle");
    CHECK(pgwt_is_hidden_event(WEI(PG_WAIT_ACTIVITY, 0)) != 0,
          "Activity events should be hidden");
    CHECK(pgwt_is_hidden_event(WEI(PG_WAIT_ACTIVITY, 4)) != 0,
          "Activity:CheckpointerMain should be hidden");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_ACTIVITY, 0)), "Activity") == 0,
          "class_name for Activity");
}

static void test_ipc_events(void)
{
    printf("--- IPC Events ---\n");
    CHECK_NAME(WEI(PG_WAIT_IPC, 0),  "IPC:AppendReady");
    CHECK_NAME(WEI(PG_WAIT_IPC, 8),  "IPC:BufferIO");
    CHECK_NAME(WEI(PG_WAIT_IPC, 12), "IPC:CheckpointStart");
    CHECK_NAME(WEI(PG_WAIT_IPC, 52), "IPC:SyncRep");
    CHECK_NAME(WEI(PG_WAIT_IPC, 56), "IPC:XactGroupUpdate");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_IPC, 0)), "IPC") == 0,
          "class_name for IPC");
}

static void test_bufferpin(void)
{
    printf("--- BufferPin ---\n");
    CHECK_NAME(WEI(PG_WAIT_BUFFERPIN, 0), "BufferPin:BufferPin");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_BUFFERPIN, 0)), "BufferPin") == 0,
          "class_name for BufferPin");
}

static void test_extension(void)
{
    printf("--- Extension ---\n");
    CHECK_NAME(WEI(PG_WAIT_EXTENSION, 0), "Extension:Extension");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_EXTENSION, 0)), "Extension") == 0,
          "class_name for Extension");
}

static void test_unknown_fallbacks(void)
{
    printf("--- Unknown / Out-of-Range ---\n");
    char buf[128];
    /* IO out of range → numeric fallback "IO:id=999" */
    pgwt_event_full_name(WEI(PG_WAIT_IO, 999), buf, sizeof(buf));
    CHECK(strstr(buf, "id=999") != NULL,
          "IO:999 should contain 'id=999', got \"%s\"", buf);
    /* Lock id=99 (out of range) → numeric fallback "Lock:id=99" */
    pgwt_event_full_name(WEI(PG_WAIT_LOCK, 99), buf, sizeof(buf));
    CHECK(strstr(buf, "id=99") != NULL,
          "Lock:99 should contain 'id=99', got \"%s\"", buf);
    /* Unknown class */
    pgwt_event_full_name(WEI(0xFF, 0), buf, sizeof(buf));
    CHECK(strstr(buf, "Unknown") != NULL || strstr(buf, "id=") != NULL,
          "class 0xFF should be unknown or numeric, got \"%s\"", buf);
}

/* Regression: dynamic names loaded from pg_wait_events arrive ordered by
 * NAME (alphabetical), which is NOT enum order for the Lock class
 * (LockTagType: relation=0 … advisory=10). The loader must map each name
 * to its correct enum id, not its row position. Before the fix,
 * Lock:relation (id 0) was mislabelled "advisory". */
static void test_dynamic_name_mapping(void)
{
    printf("--- Dynamic Name Mapping (pg_wait_events order) ---\n");

    /* Lock rows exactly as `SELECT type,name ... ORDER BY type,name`
     * returns them: alphabetical by name. Includes a fabricated future
     * event ("zzznewlock") to exercise the unknown-name fallback. */
    const char *buf =
        "Lock|advisory\n"
        "Lock|applytransaction\n"
        "Lock|extend\n"
        "Lock|frozenid\n"
        "Lock|object\n"
        "Lock|page\n"
        "Lock|relation\n"
        "Lock|spectoken\n"
        "Lock|transactionid\n"
        "Lock|tuple\n"
        "Lock|userlock\n"
        "Lock|virtualxid\n"
        "Lock|zzznewlock\n";

    CHECK(pgwt_load_event_names_from_buffer(buf) == 0,
          "load dynamic names from buffer");

    /* Correct enum ids despite alphabetical input order */
    CHECK_NAME(WEI(PG_WAIT_LOCK, 0),  "Lock:relation");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 1),  "Lock:extend");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 5),  "Lock:transactionid");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 6),  "Lock:virtualxid");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 10), "Lock:advisory");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 11), "Lock:applytransaction");
    /* Unknown future name appended after the class max (11) */
    CHECK_NAME(WEI(PG_WAIT_LOCK, 12), "Lock:zzznewlock");
}

/* PG13 has different wait-event enum orderings than 17/18; pgwt_init_event_names(13)
 * swaps in the generated PG13 tables (src/wait_event_pg13.inc). Spot-check the
 * classes that differ most (IO, LWLock, IPC, Activity, Client, Timeout) plus
 * the Lock class which is shared. Offsets verified against PG13.23 headers. */
static void test_pg13_names(void)
{
    printf("--- PG13 tables ---\n");
    pgwt_init_event_names(13);

    /* IO: PG13 enum starts at BufFileRead (no Aio/Basebackup events of 17/18). */
    CHECK_NAME(WEI(PG_WAIT_IO, 0),  "IO:BufFileRead");
    CHECK_NAME(WEI(PG_WAIT_IO, 13), "IO:DataFileRead");
    CHECK_NAME(WEI(PG_WAIT_IO, 32), "IO:RelationMapSync");  /* PG17 renamed -> Replace */
    CHECK_NAME(WEI(PG_WAIT_IO, 67), "IO:WalWrite");

    /* Timeout: PG13 ordering (PgSleep at id 1, not 2 as in PG17/18). */
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 1), "Timeout:PgSleep");

    /* Activity: PG13 includes PgStatMain (removed in PG15). */
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 0), "Activity:ArchiverMain");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 7), "Activity:PgStatMain");

    /* Client: PG13 spells WalSenderWaitWal (PG17 -> WaitForWal). */
    CHECK_NAME(WEI(PG_WAIT_CLIENT, 0), "Client:ClientRead");
    CHECK_NAME(WEI(PG_WAIT_CLIENT, 7), "Client:WalSenderWaitWal");

    /* IPC: PG13 ordering, BgWorkerShutdown casing. */
    CHECK_NAME(WEI(PG_WAIT_IPC, 1), "IPC:BgWorkerShutdown");

    /* LWLock: PG13 individual locks + tranches (NUM_INDIVIDUAL_LWLOCKS=48). */
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 4),  "LWLock:ProcArray");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 11), "LWLock:XactSLRU");   /* SLRU was individual in PG13 */
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 55), "LWLock:WALInsert");  /* first tranche after 48 base + ... */
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 62), "LWLock:LockManager");

    /* Lock class is identical to 17/18 (LockTagType 0..10). */
    CHECK_NAME(WEI(PG_WAIT_LOCK, 0),  "Lock:relation");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 10), "Lock:advisory");

    /* Restore PG18 tables for subsequent tests. */
    pgwt_init_event_names(18);
}

/* Regression (#8 mislabeling class; caught live by the CI capture-smoke
 * PG13 cell): a trace recorded on PG13 must ship a name sidecar carrying
 * the mapping it was WRITTEN with. pgwt_write_names_json() must dump the
 * active version-selected hardcoded tables even when no dynamic names
 * were loaded (PG13 has no pg_wait_events view) — before the fix, PG13
 * traces had no sidecar at all and pgwt-server silently decoded PG13 ids
 * with PG18 tables (PgSleep rendered as CheckpointWriteDelay).
 *
 * MUST RUN FIRST (fresh-process dyn state): dyn_max[] is 0-initialized at
 * process start and only becomes -1 after a dyn_clear(). The daemon
 * writes the sidecar in exactly that fresh state, and the first version
 * of the fix passed this test only because an earlier test had already
 * clear()ed — while a fresh daemon still wrote a bogus one-empty-entry
 * sidecar. Running first reproduces the daemon's real initial state. */
static void test_pg13_sidecar_roundtrip(void)
{
    printf("--- PG13 name sidecar round-trip ---\n");

    char dir[] = "/tmp/pgwt_names_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp failed");

    /* Daemon side: PG13 tables active, no dynamic names available. */
    pgwt_init_event_names(13);
    CHECK(pgwt_write_names_json(dir) == 0,
          "write sidecar without dynamic names");

    /* Reader side: fresh pgwt-server defaults to PG18 tables, then loads
     * the sidecar — PG13 ids must decode with PG13 names afterwards. */
    pgwt_init_event_names(18);
    CHECK(pgwt_load_names_json(dir) == 0, "load sidecar");

    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 1), "Timeout:PgSleep");      /* PG18 id1 = CheckpointWriteDelay */
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 4), "Timeout:VacuumDelay");  /* PG18 id4 = RecoveryRetrieve... */
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 11), "LWLock:XactSLRU");      /* individual lock in PG13 */
    CHECK_NAME(WEI(PG_WAIT_LOCK, 0),   "Lock:relation");

    char path[600];
    snprintf(path, sizeof(path), "%s/wait_event_names.json", dir);
    remove(path);
    remove(dir);

    /* Reset dynamic-name state for the remaining tests: an unparseable
     * buffer makes the loader clear the dyn tables and return -1. */
    CHECK(pgwt_load_event_names_from_buffer("no separators here") == -1,
          "dyn reset via invalid buffer");
}


/* ══════════════════════════════════════════════════════════════════════════
 * TIMER-SLEEP / IDLE ACCOUNTING (2026-10-06)
 *
 * Owner's rule: "there is DB time when database doing smth - not just waiting
 * for timer". Six Timeout events are pure pacing sleeps and leave DB Time;
 * four stay in it. src/idle_rule.c holds the one list.
 *
 * WHAT MAKES THIS HARD, AND WHY THESE TESTS LOOK THE WAY THEY DO. The Timeout
 * event ids are VERSION-DEPENDENT:
 *
 *      id   PG18                             PG13
 *      0    BaseBackupThrottle   (pacing)    BaseBackupThrottle  (pacing)
 *      1    CheckpointWriteDelay (pacing)    PgSleep             (DB Time)
 *      2    PgSleep              (DB Time)   RecoveryApplyDelay  (pacing)
 *      3    RecoveryApplyDelay   (pacing)    RecoveryRetrieve... (pacing)
 *      4    RecoveryRetrieve...  (pacing)    VacuumDelay         (pacing)
 *      5    RegisterSyncRequest  (DB Time)   RegisterSyncRequest (DB Time)
 *      6    SpinDelay            (DB Time)   -- none --
 *      7    VacuumDelay          (pacing)    -- none --
 *      8    VacuumTruncate       (DB Time)   -- none --
 *      9    WalSummarizerError   (pacing)    -- none --
 *
 * Ids 1 and 2 INVERT between the two versions, so a hardcoded id list is not
 * merely imprecise on PG13 — it gets PgSleep exactly backwards, which is the
 * one event the live accuracy tests' premise depends on. Every assertion below
 * is therefore written BY NAME: it looks the name up through the same
 * pgwt_event_name() the UI prints, then asserts the classification of THAT.
 * ══════════════════════════════════════════════════════════════════════════ */

static int popcount32(uint32_t v)
{
    int n = 0;
    while (v) { n += (int)(v & 1u); v >>= 1; }
    return n;
}

/* Assert, for Timeout event `id` on the currently-active tables, that its name
 * is `want_name` and that pgwt_is_idle_event agrees with `want_idle`. Also
 * asserts it is never HIDDEN: the whole point of the change is that pacing time
 * leaves DB Time and stays VISIBLE (the Client:ClientRead precedent). */
static void check_timeout(const char *ver, int id, const char *want_name,
                          int want_idle)
{
    uint32_t wei = WEI(PG_WAIT_TIMEOUT, id);
    const char *got = pgwt_event_name(wei);
    CHECK(got != NULL && strcmp(got, want_name) == 0,
          "%s Timeout id %d: name expected \"%s\", got \"%s\"",
          ver, id, want_name, got ? got : "(null)");
    CHECK((pgwt_is_idle_event(wei) != 0) == (want_idle != 0),
          "%s Timeout:%s (id %d): idle expected %d, got %d",
          ver, want_name, id, want_idle, pgwt_is_idle_event(wei) != 0);
    CHECK(pgwt_is_hidden_event(wei) == 0,
          "%s Timeout:%s (id %d) must stay VISIBLE (not hidden)",
          ver, want_name, id);
}

/* ── 0. the rule, as a pure name predicate ─────────────────────────────── */
static void test_pacing_name_rule(void)
{
    printf("--- pacing rule by name ---\n");
    const char *pacing[] = {
        "CheckpointWriteDelay", "VacuumDelay", "BaseBackupThrottle",
        "RecoveryApplyDelay", "RecoveryRetrieveRetryInterval",
        "WalSummarizerError",
    };
    for (size_t i = 0; i < sizeof(pacing)/sizeof(pacing[0]); i++)
        CHECK(pgwt_timeout_name_is_pacing(pacing[i]) == 1,
              "\"%s\" must be pacing", pacing[i]);

    /* Owner decision 2026-10-06: PgSleep stays DB Time. A client ASKED for the
     * sleep and the server is inside that command. This assertion is the guard
     * on the one-line switch in src/idle_rule.c: flipping it fails here first,
     * before it reaches tests/test_aas_accuracy.py (4 backends in pg_sleep(10),
     * asserts Client AAS ~= 4.0) where the failure would look like noise. */
    CHECK(pgwt_timeout_name_is_pacing("PgSleep") == 0,
          "PgSleep stays DB Time (owner decision 2026-10-06)");
    /* The three outstanding-request backoffs. */
    CHECK(pgwt_timeout_name_is_pacing("SpinDelay") == 0, "SpinDelay is DB Time");
    CHECK(pgwt_timeout_name_is_pacing("RegisterSyncRequest") == 0,
          "RegisterSyncRequest is DB Time");
    CHECK(pgwt_timeout_name_is_pacing("VacuumTruncate") == 0,
          "VacuumTruncate is DB Time");

    /* BYPASS: an event this build has never heard of must stay in DB Time, and
     * a NULL name must not crash or be treated as pacing. Over-counting load is
     * visible in the UI; silently dropping a future Timeout event out of DB
     * Time would not be. */
    CHECK(pgwt_timeout_name_is_pacing("SomeFuturePgTimerEvent") == 0,
          "unknown Timeout name defaults to DB Time (fail-safe direction)");
    CHECK(pgwt_timeout_name_is_pacing(NULL) == 0, "NULL name is not pacing");
    /* Exact match only: a prefix must not satisfy it. */
    CHECK(pgwt_timeout_name_is_pacing("VacuumDelayExtra") == 0,
          "pacing match is exact, not a prefix");
    CHECK(pgwt_timeout_name_is_pacing("") == 0, "empty name is not pacing");
}

/* ── 1. PRE-INIT. Must run before any pgwt_init_event_names call. ───────── */
static void test_idle_pre_init(void)
{
    printf("--- pacing mask before any init ---\n");
    /* Documented default (src/idle_rule.h): the PG18 mask, because
     * wait_event.c's hardcoded tables also default to PG18. Any other default
     * would make the predicate disagree with the names printed beside it.
     * tests below assert init(18) REPRODUCES this constant from the table, so
     * the constant is checked rather than believed. */
    CHECK(pgwt_idle_rule_timeout_mask() == PGWT_IDLE_TIMEOUT_MASK_PG18,
          "pre-init mask is the documented PG18 default (0x%03x, got 0x%03x)",
          PGWT_IDLE_TIMEOUT_MASK_PG18, pgwt_idle_rule_timeout_mask());
    /* And the predicate is usable with no init at all — tests/test_anomaly.c
     * and the pure cores call it directly. */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 1)) != 0,
          "pre-init: PG18 id 1 (CheckpointWriteDelay) is idle");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 2)) == 0,
          "pre-init: PG18 id 2 (PgSleep) is not idle");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_CLIENT, 0)) != 0,
          "pre-init: Client:ClientRead is idle");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_ACTIVITY, 4)) != 0,
          "pre-init: Activity is idle");
}

/* ── 2. every Timeout id on PG18 and on PG13, by name ──────────────────── */
static void test_timeout_idle_pg18(void)
{
    printf("--- Timeout idleness: PG18 ---\n");
    pgwt_init_event_names(18);
    check_timeout("PG18", 0, "BaseBackupThrottle",            1);
    check_timeout("PG18", 1, "CheckpointWriteDelay",          1);
    check_timeout("PG18", 2, "PgSleep",                       0);
    check_timeout("PG18", 3, "RecoveryApplyDelay",            1);
    check_timeout("PG18", 4, "RecoveryRetrieveRetryInterval", 1);
    check_timeout("PG18", 5, "RegisterSyncRequest",           0);
    check_timeout("PG18", 6, "SpinDelay",                     0);
    check_timeout("PG18", 7, "VacuumDelay",                   1);
    check_timeout("PG18", 8, "VacuumTruncate",                0);
    check_timeout("PG18", 9, "WalSummarizerError",            1);

    /* The mask the table produced must equal the documented default. This is
     * what keeps PGWT_IDLE_TIMEOUT_MASK_PG18 honest: it is derived here from
     * the real table and compared, never trusted. */
    CHECK(pgwt_idle_rule_timeout_mask() == PGWT_IDLE_TIMEOUT_MASK_PG18,
          "PG18 table-derived mask == PGWT_IDLE_TIMEOUT_MASK_PG18 "
          "(0x%03x vs 0x%03x)", pgwt_idle_rule_timeout_mask(),
          PGWT_IDLE_TIMEOUT_MASK_PG18);
    /* BYPASS: a mask of 0 would make every assertion above that expects
     * "not idle" pass vacuously, and only the six positive ones would fail.
     * Pin the POPCOUNT so a mask that is empty, all-ones, or off by an entry
     * is a separate, named failure. */
    CHECK(popcount32(pgwt_idle_rule_timeout_mask()) == 6,
          "PG18: exactly 6 pacing ids (got %d, mask 0x%03x)",
          popcount32(pgwt_idle_rule_timeout_mask()),
          pgwt_idle_rule_timeout_mask());

    /* Ids past the end of the table resolve to no name, so they are NOT
     * pacing — never pacing-by-default. */
    CHECK(pgwt_event_name(WEI(PG_WAIT_TIMEOUT, 10)) == NULL,
          "PG18 Timeout id 10 has no name");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 10)) == 0,
          "PG18 Timeout id 10 (unnamed) is not idle");
    /* Mask width: the predicate can only represent ids 0..31, so an active
     * table that ever grew past 31 would silently classify the tail as DB
     * Time. Assert the table does not reach there. */
    CHECK(pgwt_event_name(WEI(PG_WAIT_TIMEOUT, 32)) == NULL,
          "PG18 Timeout table stays inside the 32-bit mask width");
}

static void test_timeout_idle_pg13(void)
{
    printf("--- Timeout idleness: PG13 ---\n");
    pgwt_init_event_names(13);
    check_timeout("PG13", 0, "BaseBackupThrottle",            1);
    check_timeout("PG13", 1, "PgSleep",                       0);
    check_timeout("PG13", 2, "RecoveryApplyDelay",            1);
    check_timeout("PG13", 3, "RecoveryRetrieveRetryInterval", 1);
    check_timeout("PG13", 4, "VacuumDelay",                   1);
    check_timeout("PG13", 5, "RegisterSyncRequest",           0);

    CHECK(pgwt_idle_rule_timeout_mask() == 0x1Du,
          "PG13 mask is 0x01D {0,2,3,4} (got 0x%03x)",
          pgwt_idle_rule_timeout_mask());
    CHECK(popcount32(pgwt_idle_rule_timeout_mask()) == 4,
          "PG13: exactly 4 pacing ids (got %d)",
          popcount32(pgwt_idle_rule_timeout_mask()));

    /* PG13 has no CheckpointWriteDelay, SpinDelay, VacuumTruncate or
     * WalSummarizerError at all. Ids 6..9 are unnamed and must be DB Time —
     * a PG18-derived id mask would have marked 7 and 9 pacing here. */
    for (int id = 6; id <= 9; id++) {
        CHECK(pgwt_event_name(WEI(PG_WAIT_TIMEOUT, id)) == NULL,
              "PG13 Timeout id %d has no name", id);
        CHECK(pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, id)) == 0,
              "PG13 Timeout id %d (unnamed) is not idle", id);
    }
    CHECK(pgwt_event_name(WEI(PG_WAIT_TIMEOUT, 32)) == NULL,
          "PG13 Timeout table stays inside the 32-bit mask width");
    pgwt_init_event_names(18);
}

/* ── 3. THE INDEX COLLISION, stated as its own test ────────────────────────
 *
 * Criterion 2 of the task: prove WEI(PG_WAIT_TIMEOUT, 1) is not
 * idle-for-the-wrong-reason on either version. Ids 1 and 2 carry OPPOSITE
 * classifications on PG13 and PG18, so this is a real discriminator and not a
 * restatement: any implementation that hardcodes one version's ids fails it. */
static void test_timeout_index_collision(void)
{
    printf("--- PG13/PG18 Timeout index collision ---\n");

    pgwt_init_event_names(18);
    uint32_t mask18 = pgwt_idle_rule_timeout_mask();
    int id1_18 = pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 1)) != 0;
    int id2_18 = pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 2)) != 0;
    const char *n1_18 = pgwt_event_name(WEI(PG_WAIT_TIMEOUT, 1));
    const char *n2_18 = pgwt_event_name(WEI(PG_WAIT_TIMEOUT, 2));

    pgwt_init_event_names(13);
    uint32_t mask13 = pgwt_idle_rule_timeout_mask();
    int id1_13 = pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 1)) != 0;
    int id2_13 = pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 2)) != 0;
    const char *n1_13 = pgwt_event_name(WEI(PG_WAIT_TIMEOUT, 1));
    const char *n2_13 = pgwt_event_name(WEI(PG_WAIT_TIMEOUT, 2));
    pgwt_init_event_names(18);

    CHECK(strcmp(n1_18, "CheckpointWriteDelay") == 0 && id1_18 == 1,
          "id 1 on PG18 is CheckpointWriteDelay and IS idle (got %s, idle=%d)",
          n1_18, id1_18);
    CHECK(strcmp(n1_13, "PgSleep") == 0 && id1_13 == 0,
          "id 1 on PG13 is PgSleep and is NOT idle (got %s, idle=%d)",
          n1_13, id1_13);
    CHECK(strcmp(n2_18, "PgSleep") == 0 && id2_18 == 0,
          "id 2 on PG18 is PgSleep and is NOT idle (got %s, idle=%d)",
          n2_18, id2_18);
    CHECK(strcmp(n2_13, "RecoveryApplyDelay") == 0 && id2_13 == 1,
          "id 2 on PG13 is RecoveryApplyDelay and IS idle (got %s, idle=%d)",
          n2_13, id2_13);

    /* The two masks MUST differ. If a future refactor made the mask
     * version-independent again, every by-name assertion above would still
     * pass on whichever version happened to be active last; this one would
     * not. */
    CHECK(mask18 != mask13,
          "PG13 and PG18 masks must differ (0x%03x vs 0x%03x)",
          mask18, mask13);
}

/* ── 4. the ORDERING bug: init(18) then a PG13 sidecar ─────────────────────
 *
 * pgwt-server calls pgwt_init_event_names(18) (src/server.c) and THEN
 * pgwt_load_names_json(trace_dir). Before this change pgwt_load_names_json
 * assigned pg_version WITHOUT re-selecting the active tables and never
 * rebuilt the mask, so a PG13 trace kept PG18's classification: its
 * Timeout:PgSleep (id 1) would have been reported as idle and dropped out of
 * DB Time. This is the one assertion in the file whose subject is ORDER. */
static void test_pg13_sidecar_idle_roundtrip(void)
{
    printf("--- PG13 sidecar: classification after load ---\n");

    char dir[] = "/tmp/pgwt_idle_sc_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp failed");

    /* Daemon side: a PG13 host writes the sidecar. */
    pgwt_init_event_names(13);
    CHECK(pgwt_write_names_json(dir) == 0, "write PG13 sidecar");

    /* Reader side, in pgwt-server's real order. */
    pgwt_init_event_names(18);
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 1)) != 0,
          "after init(18), id 1 is CheckpointWriteDelay => idle");

    CHECK(pgwt_load_names_json(dir) == 0, "load PG13 sidecar");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 1)) == 0,
          "after loading the PG13 sidecar, id 1 is PgSleep => NOT idle");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 2)) != 0,
          "after loading the PG13 sidecar, id 2 is RecoveryApplyDelay => idle");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 4)) != 0,
          "after loading the PG13 sidecar, id 4 is VacuumDelay => idle");
    CHECK(pgwt_idle_rule_timeout_mask() == 0x1Du,
          "sidecar load installs the PG13 mask (got 0x%03x)",
          pgwt_idle_rule_timeout_mask());

    /* BYPASS 1: a load that FAILS must not leave a mask from nowhere. The
     * PG13 mask installed above has to survive, because nothing changed. */
    CHECK(pgwt_load_names_json("/nonexistent/pgwt/dir") == -1,
          "missing sidecar reports -1");
    CHECK(pgwt_idle_rule_timeout_mask() == 0x1Du,
          "a failed sidecar load leaves the previous mask in place "
          "(got 0x%03x)", pgwt_idle_rule_timeout_mask());

    /* BYPASS 2: an UNPARSEABLE buffer makes the dynamic loader clear its
     * tables and return -1. The mask must then describe the hardcoded tables
     * that are now in force again — a stale mask from the cleared dynamic
     * names would be a silent wrong answer with nothing to point at. */
    CHECK(pgwt_load_event_names_from_buffer("no separators here") == -1,
          "unparseable dyn buffer reports -1");
    CHECK(pgwt_idle_rule_timeout_mask() == 0x1Du,
          "after a failed dyn load the mask matches the active PG13 tables "
          "(got 0x%03x)", pgwt_idle_rule_timeout_mask());

    /* BYPASS 3: a sidecar whose Timeout array is present but EMPTY-STRINGED
     * must fall back to the hardcoded table rather than classify nothing. */
    char path[700];
    snprintf(path, sizeof(path), "%s/wait_event_names.json", dir);
    FILE *fp = fopen(path, "w");
    CHECK(fp != NULL, "rewrite sidecar");
    if (fp) {
        fputs("{\"Timeout\":[\"\",\"\",\"\"],\"pg_version\":13}\n", fp);
        fclose(fp);
    }
    CHECK(pgwt_load_names_json(dir) == 0, "load blank-name sidecar");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 1)) == 0,
          "blank sidecar names fall back to the PG13 table: id 1 = PgSleep, "
          "not idle");
    CHECK(pgwt_idle_rule_timeout_mask() == 0x1Du,
          "blank sidecar still yields the PG13 mask (got 0x%03x)",
          pgwt_idle_rule_timeout_mask());

    /* BYPASS 4: dynamic names from a live PG must drive the mask too — the
     * ids there come from the server, not from any table in this build. A
     * one-entry Timeout list makes id 0 CheckpointWriteDelay, which no
     * hardcoded table ever says. */
    CHECK(pgwt_load_event_names_from_buffer(
              "Timeout|CheckpointWriteDelay\nTimeout|PgSleep\n") == 0,
          "load dynamic Timeout names");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 1)) != 0,
          "dyn names: CheckpointWriteDelay keeps its hardcoded id 1 and is "
          "idle");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_TIMEOUT, 2)) == 0,
          "dyn names: PgSleep at id 2 is not idle");

    remove(path);
    remove(dir);
    /* Leave the process in the state the remaining tests expect. */
    CHECK(pgwt_load_event_names_from_buffer("no separators here") == -1,
          "dyn reset");
    pgwt_init_event_names(18);
}

/* ── 5. the pacing events stay VISIBLE (criterion 4) ──────────────────────
 * pgwt_is_hidden_event is UNCHANGED: Activity class only. Pacing events are
 * excluded from load and still render in every list. */
static void test_pacing_visibility(void)
{
    printf("--- pacing events stay visible ---\n");
    pgwt_init_event_names(18);
    const int pacing_ids[] = {0, 1, 3, 4, 7, 9};
    for (size_t i = 0; i < sizeof(pacing_ids)/sizeof(pacing_ids[0]); i++) {
        uint32_t wei = WEI(PG_WAIT_TIMEOUT, pacing_ids[i]);
        CHECK(pgwt_is_idle_event(wei) != 0 && pgwt_is_hidden_event(wei) == 0,
              "Timeout id %d: idle AND visible", pacing_ids[i]);
        /* It must also still have a printable full name — an idle row with no
         * name is the "anonymous Idle number" this change exists to remove. */
        char buf[64];
        pgwt_event_full_name(wei, buf, sizeof(buf));
        CHECK(strncmp(buf, "Timeout:", 8) == 0 && strchr(buf, '=') == NULL,
              "Timeout id %d renders a real name (%s)", pacing_ids[i], buf);
    }
    /* Unchanged neighbours. */
    CHECK(pgwt_is_hidden_event(WEI(PG_WAIT_CLIENT, 0)) == 0,
          "Client:ClientRead still visible");
    CHECK(pgwt_is_hidden_event(WEI(PG_WAIT_ACTIVITY, 4)) != 0,
          "Activity still hidden");
}

/* ── 6. the NARROW session-idle predicate ─────────────────────────────────
 * src/sampler.c (query_id inheritance) and src/summary_writer.c (command
 * boundary) must NOT follow the load rule. A pacing sleep happens inside a
 * running command. */
static void test_session_idle_narrower(void)
{
    printf("--- session-idle is narrower than load-idle ---\n");
    pgwt_init_event_names(18);
    CHECK(pgwt_is_session_idle_event(WEI(PG_WAIT_ACTIVITY, 4)) != 0,
          "Activity is session-idle");
    CHECK(pgwt_is_session_idle_event(WEI(PG_WAIT_CLIENT, 0)) != 0,
          "Client:ClientRead is session-idle");
    const int pacing_ids[] = {0, 1, 3, 4, 7, 9};
    for (size_t i = 0; i < sizeof(pacing_ids)/sizeof(pacing_ids[0]); i++) {
        uint32_t wei = WEI(PG_WAIT_TIMEOUT, pacing_ids[i]);
        CHECK(pgwt_is_idle_event(wei) != 0 &&
              pgwt_is_session_idle_event(wei) == 0,
              "Timeout id %d is load-idle but NOT session-idle",
              pacing_ids[i]);
    }
    CHECK(pgwt_is_session_idle_event(WEI(PG_WAIT_TIMEOUT, 2)) == 0,
          "PgSleep is neither");
    CHECK(pgwt_is_session_idle_event(0) == 0, "CPU is not session-idle");
}

int main(void)
{
    printf("=== test_wait_event ===\n");
    /* FIRST of all: the pre-init default mask is only observable before any
     * pgwt_init_event_names() call, and this test reads nothing else. */
    test_idle_pre_init();
    test_pg13_sidecar_roundtrip();   /* needs fresh-process dyn state */
    pgwt_init_event_names(18);
    test_cpu();
    test_io_events();
    test_lock_events();
    test_lwlock_events();
    test_timeout_events();
    test_client_events();
    test_activity_events();
    test_ipc_events();
    test_bufferpin();
    test_extension();
    test_pg13_names();
    test_unknown_fallbacks();
    /* Timer-sleep / idle accounting (2026-10-06). */
    test_pacing_name_rule();
    test_timeout_idle_pg18();
    test_timeout_idle_pg13();
    test_timeout_index_collision();
    test_pg13_sidecar_idle_roundtrip();
    test_pacing_visibility();
    test_session_idle_narrower();
    test_dynamic_name_mapping();  /* must run last: sets dyn_loaded */

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
