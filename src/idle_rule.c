/* idle_rule.c — see idle_rule.h for the rule and why it lives in its own TU. */
#include "idle_rule.h"
#include "pg_wait_tracer.h"   /* WE_CLASS, WEI, PG_WAIT_* */

#include <string.h>

/* ── THE pacing set ──────────────────────────────────────────────────────
 *
 * ONE table. Adding or removing an event here is the whole change: the
 * id-indexed mask, every DB Time / AAS read path, the Idle sub-rows and the
 * per-query totals all derive from it.
 *
 * Timeout:PgSleep is DELIBERATELY ABSENT (owner decision 2026-10-06, final)
 * and is NOT an oversight — see the block comment below. THE ONE-LINE SWITCH:
 * to make PgSleep pacing, add
 *     "PgSleep",
 * to this table; to undo it, delete that line. Nothing else in the tree
 * changes — the mask, every DB Time / AAS read path, the Idle sub-rows and the
 * per-query totals all derive from this table.
 */
static const char *const pacing_timeout_names[] = {
    "CheckpointWriteDelay",             /* checkpointer spreading its writes */
    "VacuumDelay",                      /* vacuum_cost_delay throttle */
    "BaseBackupThrottle",               /* base-backup rate limit */
    "RecoveryApplyDelay",               /* recovery_min_apply_delay */
    "RecoveryRetrieveRetryInterval",    /* wait before retrying WAL fetch */
    "WalSummarizerError",               /* retry delay after an error */
};

/* NOT pacing, and each for a stated reason (the operative test is "timer
 * sleep with NO OUTSTANDING RESOURCE REQUEST", not "calls pg_usleep"):
 *
 *   SpinDelay           — a pg_usleep backoff inside an OUTSTANDING attempt to
 *                         acquire a contended spinlock. The database is
 *                         contending; that is load, and the busiest possible
 *                         diagnostic.
 *   RegisterSyncRequest — backoff inside an outstanding attempt to push a
 *                         request through a FULL fsync queue.
 *   VacuumTruncate      — backoff inside an outstanding lock acquisition.
 *
 *   PgSleep             — OWNER DECISION 2026-10-06, final: stays DB Time. A
 *                         client ASKED for the sleep and the server is inside
 *                         that command, so the narrower reading applies — it
 *                         is not the database pacing itself. The switch below
 *                         exists so the decision is one line, but it ships
 *                         OFF. (It is also the premise of several live
 *                         accuracy tests: tests/test_aas_accuracy.py starts 4
 *                         backends in pg_sleep(10) and asserts Client AAS
 *                         ~= 4.0; likewise test_accuracy.py,
 *                         test_deterministic.py, test_query_accuracy.py,
 *                         test_capture_smoke.py. Flipping it would not
 *                         re-baseline those tests, it would destroy their
 *                         meaning.)
 */

/* PGWT_PACING_TIMEOUT_COUNT is consumed by src/compute.c to size the Idle
 * breakdown. Tie it to the table here so adding a name without updating the
 * constant is a COMPILE error rather than a silently truncated breakdown. */
_Static_assert(sizeof(pacing_timeout_names) / sizeof(pacing_timeout_names[0])
                   == PGWT_PACING_TIMEOUT_COUNT,
               "PGWT_PACING_TIMEOUT_COUNT must equal pacing_timeout_names[]; "
               "update src/idle_rule.h when you add or remove a pacing event");

int pgwt_timeout_name_is_pacing(const char *name)
{
    if (!name)
        return 0;
    for (size_t i = 0; i < sizeof(pacing_timeout_names) /
                           sizeof(pacing_timeout_names[0]); i++)
        if (strcmp(name, pacing_timeout_names[i]) == 0)
            return 1;
    return 0;
}

/* UNINITIALISED = EMPTY = everything stays in DB Time.
 *
 * This used to default to the PG18 mask on the reasoning that wait_event.c's
 * name tables also default to PG18. But that version default exists for
 * DISPLAY ("render something rather than Unknown"), and reusing it for
 * CLASSIFICATION made "no one has said which PostgreSQL this is" silently
 * equivalent to "this is PG18" -- so any process that never called
 * pgwt_init_event_names got a real mask derived from an assumption. Empty is
 * the fail-safe direction named at the top of idle_rule.h: over-counting load
 * is visible on screen, under-counting is not. */
static uint32_t timeout_pacing_mask = 0;

void pgwt_idle_rule_set_timeout_mask(uint32_t mask)
{
    timeout_pacing_mask = mask;
}

uint32_t pgwt_idle_rule_timeout_mask(void)
{
    return timeout_pacing_mask;
}

int pgwt_is_idle_event(uint32_t wei)
{
    if (WE_CLASS(wei) == PG_WAIT_ACTIVITY)
        return 1;
    if (wei == WEI(PG_WAIT_CLIENT, 0))          /* Client:ClientRead */
        return 1;
    if (WE_CLASS(wei) == PG_WAIT_TIMEOUT) {
        uint32_t id = WE_EVENT(wei);
        /* Ids >= 32 are not representable in the mask and are therefore NOT
         * pacing — the fail-safe direction (stays in DB Time). PostgreSQL
         * has never had more than 10 Timeout events; test_wait_event.c
         * asserts the active table never exceeds the mask width. */
        return id < 32 && ((timeout_pacing_mask >> id) & 1u);
    }
    return 0;
}

int pgwt_is_hidden_event(uint32_t wei)
{
    /* Activity-class only — never hides Client:ClientRead or Timeout pacing. */
    return WE_CLASS(wei) == PG_WAIT_ACTIVITY;
}

int pgwt_is_session_idle_event(uint32_t wei)
{
    return WE_CLASS(wei) == PG_WAIT_ACTIVITY ||
           wei == WEI(PG_WAIT_CLIENT, 0);
}
