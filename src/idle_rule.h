/* idle_rule.h — THE one definition of "this wait is not the database doing
 * work", and the one place the Timeout pacing set is written down.
 *
 * Why this is its own translation unit and not part of wait_event.c:
 * wait_event.c carries ~2000 lines of version-specific name tables plus a
 * cJSON dependency, which the BPF-free pure cores (src/anomaly.c and its
 * unit test) cannot link. That is exactly why src/anomaly.c used to carry an
 * INLINE COPY of the rule with a comment asking the next person to keep it in
 * sync by hand. This file is small, dependency-free (pg_wait_tracer.h for the
 * WEI/WE_CLASS macros only) and linkable from everywhere, so the copy is gone.
 *
 * ── The rule (owner, 2026-10-06) ─────────────────────────────────────────
 *
 *   "there is DB time when database doing smth - not just waiting for timer"
 *
 * It is NOT a foreground/background rule. A process asleep on a timer is not
 * doing work, whoever it is. The operative test is "timer sleep with NO
 * OUTSTANDING RESOURCE REQUEST", not "calls pg_usleep": Timeout:SpinDelay,
 * Timeout:RegisterSyncRequest and Timeout:VacuumTruncate are all pg_usleep
 * backoffs, but each is part of an in-flight attempt to acquire something
 * (a spinlock, room in the fsync queue, a lock), so the database IS
 * contending and that time is DB Time. See docs/AAS_SEMANTICS_DECISION.md.
 *
 * ── Why the set is derived from NAMES, never from ids ────────────────────
 *
 * The Timeout event ids are VERSION-DEPENDENT. PG18 (src/wait_event.c
 * timeout_events[]) has id 1 = CheckpointWriteDelay; PG13
 * (src/wait_event_pg13.inc timeout_events_pg13[]) has id 1 = PgSleep, and no
 * CheckpointWriteDelay or SpinDelay at all. So a hardcoded id list is a
 * SILENT WRONG ANSWER on PG13 — it would classify PgSleep as pacing on PG13
 * and nothing as pacing where the names moved. The live tier runs PG
 * 13/16/17/18.
 *
 * So: wait_event.c derives an id-indexed bitmask from the ACTIVE name table
 * (via pgwt_timeout_name_is_pacing) and installs it here, and it rebuilds it
 * at the end of BOTH pgwt_init_event_names() AND pgwt_load_names_json() —
 * pgwt-server calls them in that order, so the sidecar's (trace-supplied)
 * names must get the last word.
 *
 * pgwt_is_idle_event() is on hot loops in compute.c (~45 call sites), so the
 * predicate itself is a shift and a mask, never a strcmp.
 */
#ifndef PGWT_IDLE_RULE_H
#define PGWT_IDLE_RULE_H

#include <stdint.h>

/* ── The pacing set, by name ─────────────────────────────────────────────
 *
 * Returns 1 if `name` (a Timeout-class event name, without the "Timeout:"
 * prefix) is a pure pacing sleep: a timer with no outstanding resource
 * request. Returns 0 for every other Timeout name, INCLUDING names this
 * build has never heard of — a Timeout event added by a future PostgreSQL
 * stays in DB Time rather than silently vanishing from it. That is the
 * fail-safe direction: over-counting load is visible, under-counting is not.
 *
 * To add or remove one event, edit the ONE table in src/idle_rule.c
 * (pacing_timeout_names[]) and nothing else — the mask, every read path and
 * every view follow from it.
 */
int pgwt_timeout_name_is_pacing(const char *name);

/* How many names pacing_timeout_names[] holds. The maximum number of VISIBLE
 * idle events is therefore this + 1 (Client:ClientRead); Activity is hidden
 * and never gets a row. src/compute.c static-asserts its Idle child-row budget
 * against this, so adding a name to the table cannot silently overflow the
 * breakdown. */
#define PGWT_PACING_TIMEOUT_COUNT 6
#define PGWT_MAX_VISIBLE_IDLE_EVENTS (PGWT_PACING_TIMEOUT_COUNT + 1)

/* The Timeout majors this build has a VERIFIED enum table for. PG14/15/16 use
 * the PG18 table as a best-effort fallback (src/wait_event.c's header comment
 * says so), which is a cosmetic mislabel for DISPLAY but would be a silent
 * WRONG ANSWER for classification -- so those majors get an EMPTY mask
 * instead. See rebuild_idle_mask() in src/wait_event.c. */
#define PGWT_TIMEOUT_TABLE_VERIFIED(major) \
    ((major) == 13 || (major) >= 17)

/* Install the id-indexed Timeout pacing mask (bit N set => Timeout event id
 * N is a pacing sleep). Called by wait_event.c only, from
 * pgwt_init_event_names() and pgwt_load_names_json(). Ids >= 32 are not
 * representable; PostgreSQL has had at most 10 Timeout events, and
 * pgwt_idle_rule_rebuild's loop bound is asserted against the active table
 * in tests/test_wait_event.c. */
void pgwt_idle_rule_set_timeout_mask(uint32_t mask);

/* The currently installed mask. Exposed so a unit test can assert the mask
 * a version selection produced, rather than inferring it from behaviour. */
uint32_t pgwt_idle_rule_timeout_mask(void);

/* The mask in force before either entry point has run.
 *
 * It is the PG18 mask, because the hardcoded name tables in wait_event.c
 * also default to PG18 (io_events = io_events_pg18, timeout_events_active =
 * timeout_events). Any other default would make the predicate disagree with
 * the names printed next to it. tests/test_wait_event.c asserts that
 * pgwt_init_event_names(18) reproduces this constant exactly, so the two can
 * never drift: the constant is checked against the table, not trusted.
 *
 * PG18 timeout_events[]: 0 BaseBackupThrottle, 1 CheckpointWriteDelay,
 * 2 PgSleep, 3 RecoveryApplyDelay, 4 RecoveryRetrieveRetryInterval,
 * 5 RegisterSyncRequest, 6 SpinDelay, 7 VacuumDelay, 8 VacuumTruncate,
 * 9 WalSummarizerError  =>  pacing ids {0,1,3,4,7,9} = 0x29B.
 */
#define PGWT_IDLE_TIMEOUT_MASK_PG18  0x29Bu

/* LOAD accounting: returns true if this event must be EXCLUDED from
 * DB Time / AAS / active load. True for:
 *   - Activity-class events (background processes parked in their main loop);
 *   - Client:ClientRead (idle between commands — Oracle's "SQL*Net message
 *     from client");
 *   - Timeout-class PACING events (see above).
 * Use this anywhere you are deciding how much LOAD an event represents.
 * These events stay VISIBLE — see pgwt_is_hidden_event. */
int pgwt_is_idle_event(uint32_t wait_event_info);

/* VISIBILITY: returns true if this event must NOT be displayed in event
 * lists / graphs / breakdowns. True for Activity-class events ONLY.
 *
 * Deliberately does NOT include Client:ClientRead or the Timeout pacing
 * events: they are excluded from load but must remain visible in event
 * lists, timelines, transition graphs, histograms, class drill-downs and
 * (since 2026-10-06) as named sub-rows under the time model's Idle row.
 * Conflating load with visibility is what previously forced ClientRead to be
 * counted as DB Time, because marking it idle deleted it from every view. */
int pgwt_is_hidden_event(uint32_t wait_event_info);

/* SESSION STATE, deliberately NARROWER than pgwt_is_idle_event: returns true
 * only if the event means "this session has NO command running".
 *
 * Activity (a background process in its main loop) and Client:ClientRead (a
 * backend between commands) qualify. The Timeout pacing events do NOT: a
 * checkpointer in CheckpointWriteDelay is mid-checkpoint, and
 * Timeout:PgSleep happens INSIDE a command. Two callers need this and only
 * this:
 *
 *  - src/sampler.c, which INHERITS the previous query_id across an idle span.
 *    Inheriting across an in-command sleep would attribute that sleep to the
 *    PREVIOUS statement.
 *  - src/summary_writer.c, which treats such an event as a COMMAND BOUNDARY
 *    and flushes the pid's deferred per-query attribution. Firing that on an
 *    in-command pacing sleep would close a command that is still running.
 *
 * Both used to inline their own copy of the load rule; they are now wired to
 * this one, which is why widening the load rule does not silently widen them.
 */
int pgwt_is_session_idle_event(uint32_t wait_event_info);

#endif /* PGWT_IDLE_RULE_H */
