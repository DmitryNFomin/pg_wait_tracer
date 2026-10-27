/* summary_writer.h — Per-second summary file writer
 *
 * Accumulates raw trace events into per-second snapshots containing:
 * - Time model (11 wait classes)
 * - Per-event stats with histograms
 * - Per-session (PID) stats
 * - Per-query stats
 *
 * Writes .summary.lz4 files alongside .trace.lz4 files.
 * Server uses summaries for time ranges >= 120s (instant response). */
#ifndef PGWT_SUMMARY_WRITER_H
#define PGWT_SUMMARY_WRITER_H

#include "pg_wait_tracer.h"
#include "query_attr.h"

/* Import class index definitions from compute.h without pulling full header */
#ifndef PGWT_NUM_CLASSES
#define PGWT_NUM_CLASSES 11

#define PGWT_CLASS_CPU       0
#define PGWT_CLASS_IO        1
#define PGWT_CLASS_LOCK      2
#define PGWT_CLASS_LWLOCK    3
#define PGWT_CLASS_IPC       4
#define PGWT_CLASS_CLIENT    5
#define PGWT_CLASS_TIMEOUT   6
#define PGWT_CLASS_BUFFERPIN 7
#define PGWT_CLASS_ACTIVITY  8
#define PGWT_CLASS_EXTENSION 9
#define PGWT_CLASS_UNKNOWN   10
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

/* ── On-disk format constants ─────────────────────────────── */

#define PGWT_SUMMARY_MAGIC    0x53574750   /* "PGWS" little-endian */
/* v3 (2026-10-06, the timer-sleep/idle accounting change): `class_ns`,
 * `queries[].class_ns`, `queries[].count`, `queries[].total_ns`,
 * `sessions[].top_wait_*` and `queries[].top_wait_*` now EXCLUDE idle events
 * (pgwt_is_idle_event -- Activity, Client:ClientRead and the Timeout pacing
 * sleeps). `events[]` is unchanged and still carries EVERY event including the
 * idle ones, which is where every read path gets both the idle totals and the
 * named Idle sub-rows from.
 *
 * Why the version had to move: v1/v2 precomputed those per-second totals at
 * WRITE time under the OLD rule, so a trace directory holding v2 seconds and
 * v3 seconds would BLEND two accounting rules inside one window and report a
 * DB Time that is neither. The reader therefore refuses v1/v2 for computation
 * (src/summary_reader.c) and pgwt-server preflights the window so a refusal
 * becomes a RAW recompute, never a plausible-looking partial answer
 * (should_use_summaries / pgwt_summaries_window_current). Startup recovery
 * still archives an intact older file rather than calling it corrupt. */
/* v4 (2026-10-07): the SAME change as v3 plus the exact idle scalars --
 * `pgwt_summary_query::idle_ns` (8 bytes per query) and the record trailer
 * (`idle_ns` + `events_overflow`, 12 bytes).
 *
 * Why this is a second bump rather than an edit to v3: v3 had already been
 * written to disk by an earlier build of this branch, with the OLD layout,
 * under the same version number. A reader that assumes the new bytes exist
 * mis-parses those files -- and it fails in the shape v3 was introduced to
 * close, because the file-level preflight only inspects the header, approves
 * it, and then the visitor silently SKIPS the blocks that fail to decode,
 * yielding a plausible partial window with no error and no raw fallback.
 * "No such file should exist anywhere" is a weaker guarantee than "an old
 * file is refused", so the version moves and the preflight refuses v3. */
/* v5 (2026-10-08, aggregate-vs-raw cross-check fix #317): ONE content change
 * at the writer, no layout change. `queries[].top_events[]` now also carries
 * event 0 (CPU), which accum_query_add's `old_ev != 0` guard had been
 * dropping -- so the Events tab drilled into a single query over a >= 120 s
 * window was missing that query's whole CPU* row and its db_time_ms was short
 * by the query's entire CPU time (measured raw 7002.426 ms vs summary
 * 2949.426 ms, -57.9%).
 *
 * WHY THE VERSION HAS TO MOVE even though the byte layout is identical: the
 * version is what tells a reader which ACCOUNTING RULE the precomputed
 * per-second totals were written under -- that is the whole reason v3 and v4
 * exist. A v4 second and a v5 second answer "what is in this query's event
 * list" differently, so a window holding both would report a per-query event
 * breakdown that is neither: the v4 seconds contribute no CPU row and the v5
 * seconds do, and the CPU* row would silently carry only part of the window.
 *
 * SCOPE, stated because it is easy to over-read: v5 says nothing about
 * io_worker records. The summary records still CONTAIN io_worker time (see
 * the N7 hole in tests/test_agg_raw_crosscheck.c and issue #315) -- excluding
 * it needs a daemon-side fix that does not exist yet, and claiming otherwise
 * here would be a false contract that a later version bump could only
 * withdraw by refusing these files too.
 *
 * COMPATIBILITY FOR FILES ALREADY ON DISK, handled by machinery that already
 * exists and is not changed here:
 *   - pgwt_summary_reader_open refuses any file whose header version is not
 *     PGWT_SUMMARY_VERSION, with a WARN naming both versions
 *     (src/summary_reader.c), so a v4 file is never decoded for computation;
 *   - pgwt-server PREFLIGHTS the window (should_use_summaries ->
 *     pgwt_summaries_window_current) and takes the RAW path for any window
 *     containing a non-current file. That is the part that matters: refusing
 *     alone would have left pgwt_visit_summaries silently SKIPPING those
 *     files and answering from the rest -- a plausible partial window
 *     labelled "fidelity":"exact". So an existing v4 capture is still fully
 *     readable, it is just recomputed from its raw events, which is the
 *     source of truth and does carry every query's CPU time;
 *   - startup recovery still FINALIZES and archives an intact older
 *     current.summary rather than calling it corrupt, so nothing is deleted.
 * No migration is possible or needed: a v4 record simply does not contain the
 * per-query CPU entry, and the raw trace already holds what is required to
 * recompute it. */
/* v6 (2026-10-13, issues #315 and #376): io_worker records are EXCLUDED at
 * the writer, from every accumulator -- class_ns, idle_ns, events[]
 * (histograms included), sessions[] and queries[]. ONE content change, no
 * layout change.
 *
 * WHY: docs/AAS_SEMANTICS_DECISION.md records that io_worker time is outside
 * DB Time and outside Idle (it is surfaced as an io_worker busy %, because
 * the requesting backend already shows AioIoCompletion for the same logical
 * I/O). src/compute.c implements that on the raw paths. The summary writer
 * did not, so the same window answered differently depending on which path
 * served it: +400% on DB Time on a synthetic probe (#315), and on the PG18
 * demo capture (tests/results/demo_rehearsal/trace, 1851 s, 3 io_workers)
 * summary Idle ran +5,549,649.64 ms over raw -- +23.36% on the whole
 * capture, +308.16% on a 150 s window containing the capture end, +2.43% on
 * the same window one second earlier -- because each io_worker's
 * Activity:IoWorkerMain interval closes at capture end and is bucketed whole
 * into that second. (#376's issue text quotes a different pair,
 * 406,476.45 vs 528,770.62 ms; that window could not be reconstructed, so
 * the numbers here are the ones this branch measured, with the harness kept
 * in tests/results/issue376-measurement.)
 *
 * WHY THE VERSION HAS TO MOVE even though the byte layout is identical: the
 * version is what tells a reader which ACCOUNTING RULE the precomputed
 * per-second totals were written under -- the same reason v3, v4 and v5
 * exist. A v5 second counts io_worker time and a v6 second does not, so a
 * window holding both reports a DB Time and an Idle that is neither. The
 * existing machinery handles files already on disk and is NOT changed here:
 * pgwt_summary_reader_open refuses a non-current version with a WARN, and
 * pgwt-server preflights the window (should_use_summaries ->
 * pgwt_summaries_window_current) so a refusal becomes a RAW recompute rather
 * than a plausible partial answer; startup recovery still archives an intact
 * older file instead of calling it corrupt. A v5 capture therefore stays
 * fully readable, from its raw events, which are the source of truth and
 * already carry the pid categories (pgwt_tag_events + backends.jsonl).
 *
 * HOW the writer knows: it is TOLD. `cat_flag` is a parameter of
 * pgwt_summary_push_event, resolved by the producer before the push
 * (src/event_stream.c hoists pgwt_live_pid_cat_flag above it;
 * src/escalation.c resolves in flush_open_intervals). `evt->flags` is EMPTY
 * of category bits on the live path, so a flags-only check inside the writer
 * would be dead code in the daemon -- see hole N7 in
 * tests/test_agg_raw_crosscheck.c.
 *
 * RESIDUAL, stated because it is real and small: pgwt_live_pid_cat_flag
 * returns 0 (foreground) while a pid's metadata is still unparsed, so an
 * io_worker's earliest records can be written as foreground, while the raw
 * path tags them post hoc from backends.jsonl and excludes them. That window
 * is bounded by discovery's first metadata parse for that pid. MEASURED on
 * the demo capture: 0 ms of 5,553,133.69 ms of io_worker time (no io_worker
 * record closes inside the capture's first second) -- small there, but not
 * zero by construction. The writer counts what it excluded
 * (io_worker_events_excluded_total / io_worker_ns_excluded_total, exported by
 * the control socket) so the residual stays observable rather than assumed.
 *
 * NOT fixed by v6: the summary path still reports no io_worker BUSY total
 * (cat_ms[]/io_worker_busy_pct are raw-path-only, hole N1 -- they were 0 on
 * the summary path before this change too, so nothing was lost), and the
 * writer still buckets a long idle interval WHOLE into the second in which
 * it closes while the raw path clips it to the requested window
 * (event_window_ns). That straddler mechanism is general -- a long
 * Client:ClientRead behaves the same way -- so "the summary equals raw on
 * any window" is NOT what this version claims; it claims the io_worker
 * accounting rule now matches. */
/* v7 (2026-10-14, issue #408): the per-second record now carries the
 * MEASURED-CPU SPLIT, and the writer applies the non-command-CPU relabel the
 * raw paths apply. Two content changes, one layout change (a 40-byte record
 * trailer).
 *
 * WHAT WAS WRONG, measured on the PG18 demo capture run.id 1791973533
 * (1929 s, 7,416,121 records; harness and output in
 * tests/results/issue408-measurement):
 *
 *   1. THE SPLIT WAS NOT IN THE FILE. The raw time model reports CPU as two
 *      rows -- "CPU (running)" (the conserved sum of each interval's measured
 *      cpu_ns) and "CPU (waiting for a core)" (the runqueue residual
 *      DB - CPU* - Sigma waits) -- whenever any record carried a measured
 *      cpu_ns. The summary record stored neither quantity, so
 *      pgwt_compute_time_model_from_summaries could only report
 *      has_measured_cpu = 0 and emit ONE combined "CPU*" row. At the full
 *      span that is 340,625.997 + 225,927.106 ms shown as one 586,586.148 ms
 *      row: the runqueue-wait signal -- the subject of #386's whole Off-CPU
 *      investigation -- simply disappears from the Overview on every window
 *      of 120 s or more, which is every window the demo shows.
 *
 *   2. THE NON-COMMAND RELABEL WAS NOT APPLIED. A foreground backend's
 *      we == 0 interval OUTSIDE a command is post/between-command idle time,
 *      not CPU (docs/AAS_SEMANTICS_DECISION.md). Both raw paths file it under
 *      PGWT_WEI_NONCMD_CPU: the server's tagging pass (pgwt_tag_events) and
 *      the live accumulators (pgwt_live_effective_event, issue #97). The
 *      writer never did -- it saw the raw ringbuf id, and the daemon resolved
 *      the command gate ~65 lines BELOW the summary push -- so that time sat
 *      in the summary's CPU class and therefore inside DB Time, while raw
 *      counts it as Idle. MEASURED: +20,033.044 ms on the full span, which is
 *      the WHOLE of the DB Time divergence there (raw 4,210,812.782 vs
 *      summary 4,230,845.827 = +20,033.044, and Idle is low by exactly the
 *      same -20,033.044) -- +3.54% on the CPU row itself. The identity holds
 *      to three decimals, which is why this is not a pid-categorisation
 *      effect: no other mechanism contributes at the full span.
 *
 * HOW the writer knows, both times: it is TOLD, the same way v6 is told a
 * pid's category. pgwt_summary_push_event takes `eff_event` -- the wait id
 * the PRODUCER resolved through the shared classifier
 * (pgwt_live_effective_event in src/event_stream.c, which now runs the
 * command-gate sweep ABOVE the push; pgwt_tag_events' output for offline
 * producers). A flags- or gate-only check inside the writer would be dead
 * code in the daemon, exactly as hole N7 in tests/test_agg_raw_crosscheck.c
 * records for the category bits. PGWT_SUMMARY_EFF_UNRESOLVED means "the
 * producer did not resolve one": the writer then uses evt->old_event (the v6
 * behaviour) and COUNTS it (eff_unresolved_events_total), so the residual is
 * observable rather than assumed. An eff_event that is neither
 * evt->old_event nor PGWT_WEI_NONCMD_CPU is refused the same way
 * (eff_rejected_events_total) -- the relabel is the only rewrite this
 * parameter may express.
 *
 * WHY THE VERSION HAS TO MOVE: both changes are ACCOUNTING-RULE changes, the
 * same reason v3, v4, v5 and v6 exist. A v6 second charges non-command CPU to
 * DB Time and a v7 second charges it to Idle, so a window holding both
 * reports a DB Time that is neither; and a v6 second cannot answer "what was
 * measured CPU" at all, so a mixed window would emit the two-row split
 * carrying only part of its span. The layout also grows, which makes the bump
 * mandatory rather than conventional: a v6 record ends after the 12-byte v4
 * trailer, so a reader expecting the v7 trailer mis-parses it.
 *
 * FILES ALREADY ON DISK are handled by machinery that exists and is unchanged
 * here: pgwt_summary_reader_open refuses any file whose version is not
 * PGWT_SUMMARY_VERSION (with a WARN naming both), and pgwt-server preflights
 * the window (should_use_summaries -> pgwt_summaries_window_current) so a
 * refusal becomes a RAW RECOMPUTE, never a plausible partial answer. Since
 * #403 that fallback is correct rather than a silent zero. Startup recovery
 * still finalizes and archives an intact older file instead of calling it
 * corrupt. A v6 capture therefore stays fully readable from its raw events,
 * which carry cpu_ns and the CMD markers and are the source of truth for
 * both rules. No migration is possible: the measured-CPU scalars are simply
 * not in a v6 record.
 *
 * SCOPE, stated because it is easy to over-read:
 *   - The split is reported on the UNFILTERED window only. The stored
 *     scalars are record-wide, which IS the unfiltered population; a
 *     class/event/query-filtered request asks about a different population
 *     the file cannot answer, so that path keeps has_measured_cpu = 0 and the
 *     single "CPU*" row (the v6 behaviour, honest about what it cannot
 *     split). Per-query and per-session CPU are unchanged.
 *   - v7 says NOTHING about #379: the writer still charges a long interval
 *     WHOLE to the second in which it closes, while the raw path clips it to
 *     the window. On the capture above that is +4,699 ms of the +14,153 ms
 *     DB Time divergence at a 900 s window (the rest being the relabel) and
 *     most of the Idle divergence there. "The summary equals raw on any
 *     window" is NOT what this version claims; at the FULL span, where no
 *     record straddles an edge, the two paths now agree to 0.000 ms. */
/* v8 (2026-10-14, issue #379): a wait/idle interval's DURATION is now spread
 * across the seconds it actually occupies, instead of being charged WHOLE to
 * the second in which it closes. ONE content change, no layout change.
 *
 * WHAT WAS WRONG. The raw path clips every record to the requested window
 * (event_window_ns, src/compute.c); the writer charged the whole duration to
 * floor(close). So a window containing a long record's CLOSING second but not
 * its whole span over-reported by the out-of-window part, and a window
 * containing the SPAN but not the closing second reported none of it. On the
 * #410 parity fixture, a 130 s window straddling a 100 s Client:ClientRead's
 * closing second reports DB Time +60,000 ms = +23.80% across 29 fields; on
 * the PG18 demo capture an interior 900 s window is +1.14%. Dragging the time
 * range across such a boundary moves DB Time by a quarter with the workload
 * unchanged.
 *
 * HOW: a WATERMARK, not a retro-spread. The file is append-only -- a second
 * is immutable the moment it is written (#277) -- so a 100 s interval cannot
 * be back-charged to the 100 already-written seconds it covered. Instead the
 * writer is TOLD which interval each pid currently has OPEN (
 * pgwt_summary_note_open, from the producer's own open-interval source: the
 * daemon's state_map read, src/map_reader.c) and, as each second COMPLETES,
 * credits that second's share of every open interval into that second's
 * record. When the interval finally closes, the record contributes only the
 * REMAINDER it has not already been credited for
 * (close - max(start, credited_until)). Every per-pid watermark is monotone,
 * so the two halves can never overlap and can never leave a gap, whichever
 * order a tick and a close arrive in.
 *
 * NOT the live view's rule. pgwt_live_account_open_stretch adds the WHOLE
 * open stretch every tick; that is correct only because d->accum is rebuilt
 * from scratch into a throwaway display copy each time. A persistent writer
 * doing the same would charge a 100 s ClientRead as 1+2+...+100 s.
 *
 * WHAT IS SPREAD AND WHAT IS NOT -- the distinction is the subtle part:
 *   - SPREAD (per second): class_ns, idle_ns, events[].total_ns,
 *     sessions[].db_time_ns. These are TIME, and time is what a window clips.
 *   - ONCE, AT CLOSE, WITH THE FULL DURATION: events[].count,
 *     events[].histogram, events[].max_ns, sessions[].top_wait_*,
 *     queries[].*. A latency histogram of a 100 s wait must show one 100 s
 *     observation, not 100 one-second ones, and `max_ns` means the longest
 *     single wait. The raw path does the same: it counts a straddler once and
 *     measures it whole (pgwt_compute_heatmap, top_wait).
 *   - NEVER CREDITED OPEN: queries[]. An open interval's query attribution is
 *     unresolved at credit time (#128's deferral exists precisely because the
 *     id can arrive after the wait ends), and Queries always raw-loads
 *     (#324), so there is no divergence to close and a guess would create one.
 *   - NEVER CREDITED OPEN: an on-CPU (we == 0) interval. SUPERSEDED BY v9
 *     (#424), which credits it from a producer-supplied CPU OBSERVATION --
 *     the constraint stated below still holds and v9 SATISFIES it rather
 *     than bypassing it; a producer that supplies no observation still gets
 *     exactly the v8 refusal described here. A per-second credit
 *     has no cpu_ns -- se.sum_exec_runtime is only current at a tick or a
 *     context switch, and the record carries its measurement only at close --
 *     so spreading a CPU gap's wall while its measured cpu_ns stayed whole at
 *     close would put the whole measurement into cpu_clamped_ns and ZERO into
 *     cpu_measured_ns, destroying v7's split. A long CPU gap therefore keeps
 *     the v7 whole-at-close behaviour and remains a #379 residual; it is
 *     COUNTED (cpu_open_uncredited_ns_total, exported by the control socket)
 *     so the residual is observable rather than assumed, and it is bounded by
 *     how long a backend runs with no wait event at all.
 *   - NEVER CREDITED OPEN: an io_worker pid, for the same reason v6 drops its
 *     records (counted: open_io_worker_refused_total).
 *
 * NOTHING IS INVENTED. The writer never derives an open interval from the
 * record stream; it only credits what a producer explicitly noted, and only
 * while that note stays fresh (PGWT_SUMMARY_OPEN_STALE_NS) on periodic ticks.
 * At close, credit extends through each interval's own last note, including
 * a partial final second, without applying the staleness window.
 * A producer that notes nothing degrades EXACTLY to v7 -- which is what
 * pgwt_summary_push_event does on its own, and what the lightweight/sampled
 * tiers (no state_map intervals) get. The alternative -- inferring the open
 * interval from each record's new_event -- would fabricate time on any stream
 * whose records do not tile the timeline, which is every synthetic fixture
 * and every mode where records are dropped.
 *
 * WHY THE VERSION HAS TO MOVE: it is an ACCOUNTING-RULE change, the same
 * reason v3-v7 exist. A v7 second holds a long interval's whole duration and a
 * v8 second holds one second of it, so a window holding both double-counts the
 * overlap and reports a DB Time that is neither. The layout is unchanged, so
 * the bump is conventional rather than forced -- which is exactly the case
 * v5's note argues is the most dangerous one to skip. Files already on disk
 * are handled by machinery that is unchanged here: pgwt_summary_reader_open
 * refuses any file whose version is not PGWT_SUMMARY_VERSION, and pgwt-server
 * preflights the window so a refusal becomes a RAW RECOMPUTE rather than a
 * plausible partial answer; startup recovery still archives an intact older
 * file instead of calling it corrupt.
 *
 * RESIDUALS, stated because they are real:
 *   1. the on-CPU gap above;
 *   2. ATTRIBUTION (never conservation) can be off by up to one tick at an
 *      interval boundary: if a tick credits [x, floor(now)) under the open
 *      interval's event and the closing record then shows the interval ended
 *      earlier, the overlap stays filed under the older event. The watermark
 *      clamps the close to 0 remainder, so no nanosecond is counted twice or
 *      lost;
 *   3. a second that completed while the writer had no note yet (the pid's
 *      first tick, or a producer that notes late) cannot be back-credited.
 *      That slice is not lost and not double counted -- the closing record
 *      still carries it, i.e. it keeps the v7 answer -- and it is counted
 *      (open_credit_late_ns_total). */
/* v9 (2026-10-15, issue #424): an OPEN ON-CPU interval is credited per
 * second too, WITHOUT destroying v7's measured-CPU split. ONE content
 * change, no layout change.
 *
 * WHAT WAS WRONG. v8 refused on-CPU intervals outright (the bullet above),
 * so an in-flight pure-CPU statement was ABSENT from the CPU rows of the
 * summaries path until it ended -- on the 15-minute Overview, the demo's
 * own headline view, a backend that had been burning CPU for 90 s showed
 * nothing at all. The unspread wall was counted
 * (cpu_open_uncredited_ns_total) but never placed.
 *
 * WHY v8 REFUSED, and how v9 satisfies the same constraint. v8's argument
 * was that a per-second credit has no cpu_ns: se.sum_exec_runtime is only
 * current at a tick or a context switch, and the closing RECORD carries its
 * measurement whole, so crediting wall per second while the whole
 * measurement arrived at close would clamp the entire measurement into
 * cpu_clamped_ns and leave ZERO in cpu_measured_ns. Every word of that is
 * still true about the RECORD STREAM. It is not true about the PRODUCER: at
 * the exact point where src/map_reader.c notes the open interval it has
 * already computed, for the live view, the measured on-CPU ns of the open
 * stretch (cpu_ns_total + the current stretch - the seeded base, clamped to
 * the stretch's wall). v9 does not GUESS the missing measurement and does
 * not drop it -- it is TOLD, the same way v6 is told a pid's category and v7
 * is told the effective wait id, through
 * pgwt_summary_note_open_ex(..., const struct pgwt_summary_open_cpu *).
 * A producer that passes NULL -- or the v8-signature
 * pgwt_summary_note_open(), which is exactly that call -- gets the v8
 * refusal unchanged, counted in open_cpu_refused_total.
 *
 * THE FIVE PIECES, because "credit the wall" alone would be wrong four
 * different ways:
 *
 *  1. THE CPU OBSERVATION IS A PREFIX, NOT A SLICE. The producer measures
 *     [start, now), cumulative; the writer credits [from, end) for one
 *     completed second. So the writer keeps the interval's CUMULATIVE
 *     credited measurement and credits the DIFFERENCE,
 *     min(prefix, wall credited so far) - already credited. The min() is
 *     what keeps a prefix that runs past `end` (the observation is always
 *     newer than the second being sealed) from exceeding the second's own
 *     wall and manufacturing the very clamp this version exists to avoid:
 *     the excess is CARRIED to the next second instead of being clamped
 *     away, and the carry always drains because the producer clamps its
 *     prefix to the prefix wall.
 *
 *  2. THE CLOSE SUBTRACTS WHAT WAS CREDITED -- for the MEASUREMENT as well
 *     as for the wall. v8 subtracted only the wall (already_credited_ns);
 *     accum_event then clamped the FULL closing cpu_ns against the small
 *     remaining wall, which is precisely v8's objection, reproduced. v9
 *     subtracts the credited measurement first and clamps the REMAINDER:
 *     sum(per-second measured credits) + close measured + close clamped
 *     == the closing record's cpu_ns, exactly, which is the conservation
 *     leg tests/test_open_cpu_accounting.c asserts.
 *
 *  3. CLASSIFICATION IS FROZEN AT THE FIRST CREDIT. A we == 0 gap is CPU or
 *     (foreground, outside a command) idle PGWT_WEI_NONCMD_CPU, and the
 *     producer's answer is a MAJORITY over the open prefix [start, now) --
 *     so it CAN CHANGE as the prefix grows (a gap that starts outside a
 *     command and later spends most of itself inside one flips from idle to
 *     CPU). A written second is immutable (#277), so persisting each tick's
 *     answer would leave one interval's seconds disagreeing with each other
 *     and with its own closing record. Instead the writer takes the
 *     producer's latest answer up to the moment the FIRST second of this
 *     interval is written, freezes it there, and uses it for every later
 *     credit AND for the closing record -- overriding the record's
 *     eff_event, whose only other legal value is the same pair. One
 *     interval therefore carries ONE label, which is also the shape the raw
 *     path produces (pgwt_tag_events labels a record once); the two can
 *     disagree about WHICH label, never about how many, and when they
 *     disagree the whole interval's wall is counted in
 *     open_cpu_class_divergent_ns_total. Freezing at the first CREDIT
 *     rather than at the first NOTE is deliberate: it is the last instant
 *     at which the answer is still free, so it uses the most information
 *     that can still be acted on.
 *
 *  4. THE TIER IS FROZEN WITH IT. An interval whose credits went to
 *     cpu_unknown_ns (the producer had no measurement) must not also have
 *     its closing cpu_ns added to cpu_measured_ns -- CPU* is
 *     measured + unknown, so that would double count the credited wall.
 *     The tier is frozen at the first credit exactly as the class is, a
 *     flip is counted (open_cpu_tier_mismatch_total), and BOTH directions
 *     conserve CPU*: only the tier attribution moves.
 *
 *  5. THE PER-OCCURRENCE QUANTITIES STILL HAPPEN ONCE, AT CLOSE. count, the
 *     histogram bucket, max_ns, top_wait and queries[] are unchanged from
 *     v8 -- they ride the closing record with the FULL duration. Only time
 *     is spread. Because the label is frozen, the count and the time land
 *     on the SAME events[] row; splitting them would give the Events tab a
 *     row with time and no count and another with count and no time.
 *
 * NOTHING IS INVENTED, same as v8: the writer credits only what a producer
 * explicitly noted, with periodic ticks bounded by PGWT_SUMMARY_OPEN_STALE_NS
 * and restricted to completed seconds. At close, each interval's own last
 * note bounds credit, including a partial final second, rather than the
 * staleness window.
 *
 * WHAT IS NOT IN THIS VERSION, stated because it is easy to over-read:
 *   - A CLOSED CPU interval is still charged to its CLOSING second, exactly
 *     as v7/v8 charge it. v9 changes the OPEN prefix only. A long CPU gap
 *     that was never noted (no producer notes, lightweight/sampled tiers,
 *     a pid that appeared and closed between two ticks) therefore keeps the
 *     v7 answer and keeps being counted in cpu_open_uncredited_ns_total.
 *   - ATTRIBUTION can still move by up to one tick at an interval boundary,
 *     and a second that completed before the producer's first note still
 *     cannot be back-credited (open_credit_late_ns_total). Both are v8
 *     residuals, unchanged.
 *   - AN HOURLY ROTATION USED TO PAUSE CREDITING until the next pushed
 *     record; FIXED in #430, and the claim that it was attribution-only was
 *     WRONG. pgwt_summary_check_rotation closes the file and leaves w->fp
 *     NULL for open_summary_file to fill in lazily, and
 *     writer_seal_through refused to invent records with no file -- so the
 *     ticks between the rotation and the next event sealed nothing. If a
 *     record did arrive, the seconds were credited in ONE pass and only
 *     ATTRIBUTION moved. But pgwt_summary_close() also returns early on a
 *     NULL fp, so a daemon STOPPED inside that window wrote none of those
 *     seconds at all: on a 35-minute capture that rotates, the whole tail
 *     after the boundary was LOST, not misattributed
 *     (tests/test_rotation_open_credit.c §1 measures 6 of 11 s on disk
 *     before the fix). writer_seal_through now reopens the file ITSELF once
 *     it knows it has a second to write -- never before, so a writer whose
 *     clocks were never anchored still invents nothing -- and both outcomes
 *     are counted (open_seal_reopens_total,
 *     open_seal_reopen_failures_total). The v9-specific hazard the pause
 *     created -- one second receiving several seconds' worth of measured
 *     CPU against its own one second of wall -- is what the per-second
 *     slice cap in credit_open_cpu_measured exists for; the cap stays,
 *     because the catch-up pass still happens whenever a seal is delayed
 *     for any other reason (a failed write, open_fill_capped_total).
 *   - If the producer's prefix measurement for a tick EXCEEDS the closing
 *     record's own cpu_ns (schedstat jitter between the two reads), the
 *     over-credited part cannot be taken back out of an immutable second.
 *     It is counted (open_cpu_overcredit_ns_total) and is the one way the
 *     per-interval measured identity in piece 2 can fail to hold exactly;
 *     the WALL identity never can.
 *
 * WHY THE VERSION WILL HAVE TO MOVE, AND WHY IT HAS NOT YET. The rule above
 * is an ACCOUNTING-RULE change, v3-v8's reason: a v8 second holds a long CPU
 * gap's whole wall and measurement, a v9 second holds one second of each, and
 * a window holding both double-counts the overlap.
 *
 * But NO PRODUCER CALLS pgwt_summary_note_open_ex. The live wiring was
 * reverted (agent/424-daemon fc7d7f5) once its counters showed it had never
 * been reached, so nothing credits an open on-CPU interval and v9's only
 * difference from v8 -- the widened cpu_measured_records -- never
 * materialises. A build shipping this constant at 9 writes a v8 file wearing
 * a 9.
 *
 * That is worse than merely untidy. pgwt_summaries_window_current compares
 * the NUMBER, so if 9 ships now and the producer is wired later without
 * another bump, 9 names two different accounting rules and no reader can
 * tell the resulting files apart -- the exact mixing the version exists to
 * prevent. The project's rule is never to ship a version whose contract is
 * not yet true, so the constant stays at 8 and the commit that WIRES the
 * producer is the one that sets it to 9. The writer-side capability below
 * is dormant, not absent: it is implemented and unit-tested
 * (tests/test_open_cpu_accounting.c), and tests/test_v9_open_cpu_wired.sh
 * is its acceptance test, deliberately registered nowhere until then.
 *
 * The machinery for files already on disk is unchanged either way:
 * pgwt_summary_reader_open refuses any version that is not
 * PGWT_SUMMARY_VERSION and the server preflights the window, so a refusal
 * becomes a RAW RECOMPUTE, never a plausible partial answer. */
#define PGWT_SUMMARY_VERSION  8

/* `eff_event` sentinel: the producer resolved no effective wait id, so the
 * writer uses evt->old_event and counts the push. Not 0 -- 0 is a valid id
 * (the on-CPU gap), which is exactly the id this parameter exists to
 * reclassify. */
#define PGWT_SUMMARY_EFF_UNRESOLVED  0xFFFFFFFFu

/* #277: how long after a second ends it is treated as complete by the
 * periodic flush. The daemon's timer handler runs BEFORE the event-ring
 * drain in the same main-loop pass (and the ring is polled every 10 ms), so
 * an event belonging to the just-ended second can still be in flight. Late
 * arrivals beyond this are still counted exactly once -- they fold into the
 * next open second -- this only keeps them in the right second. */
#define PGWT_SUMMARY_FLUSH_LAG_NS  100000000ULL   /* 100 ms */

/* v8 (#379): how long a producer's open-interval note stays creditable
 * without being refreshed. The live producer re-notes every pid on every
 * display tick (1 s), so three ticks of slack costs nothing there -- and it
 * is the FAIL-SAFE that bounds the one way this mechanism could fabricate
 * time: a pid that vanishes from the producer's view without an EXIT record
 * (SIGKILL, a dropped map entry, a mode change) stops being credited within
 * this long instead of accruing forever. Expiry is COUNTED
 * (open_notes_expired_total), never silent. */
#define PGWT_SUMMARY_OPEN_STALE_NS  3000000000ULL   /* 3 s */

/* v8 (#379): the most consecutive event-less seconds one advance may
 * materialise as open-credit-only records. Reached only if the writer is not
 * ticked for that long (a stopped daemon, a synthetic producer jumping its
 * clock); beyond it the credit for the skipped span is dropped and counted
 * (open_credit_late_ns_total, open_fill_capped_total) rather than spending
 * unbounded time in one call. */
#define PGWT_SUMMARY_OPEN_FILL_MAX_SECONDS  3600

/* Limits for per-second accumulator hash tables */
#define SUMMARY_MAX_EVENTS    1024
#define SUMMARY_MAX_SESSIONS  MAX_BACKENDS   /* 1024 */
#define SUMMARY_MAX_QUERIES   2048

/* Worst-case serialized size of one per-second record, COMPUTED from the
 * table bounds rather than guessed.
 *
 * pgwt_summary_serialize() writes into a fixed buffer and (before v3) ignored
 * its out_size entirely, so the only thing standing between a full record and
 * a heap overrun was that 800 KB happened to be enough. v3 adds 8 bytes per
 * query (idle_ns) and 12 per record, which cut the old margin to ~26 KB --
 * close enough that the next field to be added would have been the one that
 * silently overran. The bound is now derived here and the writer both sizes
 * its buffer from it and refuses to write past out_size.
 *
 *   class_ns   PGWT_NUM_CLASSES * 8
 *   events     SUMMARY_MAX_EVENTS   * (4+8+8+8 + HISTOGRAM_BUCKETS*8)
 *   sessions   SUMMARY_MAX_SESSIONS * (4+8+8+4+8)
 *   queries    SUMMARY_MAX_QUERIES  * (36 + PGWT_NUM_CLASSES*8 + 1
 *                                      + SUMMARY_QUERY_TOP_EVENTS*20 + 8)
 *   trailer    8 (idle_ns) + 4 (events_overflow)
 *              + v7: 5 * 8 (the measured-CPU scalars)
 */
#define PGWT_SUMMARY_SERIALIZE_MAX                                            \
    ((size_t)(PGWT_NUM_CLASSES * 8)                                           \
     + (size_t)SUMMARY_MAX_EVENTS   * (28 + HISTOGRAM_BUCKETS * 8)            \
     + (size_t)SUMMARY_MAX_SESSIONS * 32                                      \
     + (size_t)SUMMARY_MAX_QUERIES  * (36 + PGWT_NUM_CLASSES * 8 + 1          \
                                       + SUMMARY_QUERY_TOP_EVENTS * 20 + 8)   \
     + 12 + 40)

/* ── Per-second accumulator ───────────────────────────────── */

struct pgwt_summary_event {
    uint32_t event_id;         /* wait_event_info, 0 = unused slot */
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t histogram[HISTOGRAM_BUCKETS];
};

struct pgwt_summary_session {
    uint32_t pid;              /* 0 = unused slot */
    uint64_t db_time_ns;
    uint64_t cpu_ns;
    uint32_t top_wait_id;      /* event_id of highest-duration wait */
    uint64_t top_wait_ns;
};

#define SUMMARY_QUERY_TOP_EVENTS 8

struct pgwt_summary_query_event {
    uint32_t event_id;
    uint64_t count;
    uint64_t total_ns;
};

struct pgwt_summary_query {
    uint64_t query_id;         /* 0 = unused slot */
    uint64_t count;
    uint64_t total_ns;
    uint32_t top_wait_id;
    uint64_t top_wait_ns;
    /* v2: per-class time breakdown + top events */
    uint64_t class_ns[PGWT_NUM_CLASSES];
    struct pgwt_summary_query_event top_events[SUMMARY_QUERY_TOP_EVENTS];
    int      num_top_events;
    /* v3: EXACT idle total for this query. top_events[] holds only 8 entries,
     * so a query with 8 busier non-idle events would otherwise report Idle = 0
     * while its DB Time was right -- DB Time + Idle would stop accounting for
     * the window under a query filter. The named Idle CHILDREN still come from
     * top_events (bounded); the difference is emitted as one labelled
     * remainder row, so the total is exact even when the breakdown is not. */
    uint64_t idle_ns;
};

struct pgwt_summary_accum {
    uint64_t second_wall_ns;   /* wall-clock second boundary (rounded down) */
    uint64_t second_mono_ns;   /* monotonic second boundary */
    uint32_t total_events;     /* total events accumulated this second */

    /* Time model (system-wide per-class totals) */
    uint64_t class_ns[PGWT_NUM_CLASSES];

    /* Per-event hash table (open addressing) */
    struct pgwt_summary_event events[SUMMARY_MAX_EVENTS];
    int num_events;            /* count of occupied slots */
    /* v3: events[] could not take an entry because all SUMMARY_MAX_EVENTS
     * slots were occupied. Since v3 moved idle time OUT of class_ns, events[]
     * is the only per-event source left, so a full table under-reports the
     * Idle breakdown and any class/event-FILTERED total while unfiltered
     * DB Time stays correct -- a mismatch with no signal. This is that
     * signal. ~270 distinct PG wait events against 1024 slots is headroom,
     * not a proof, and it is not a proof at all for Extension events. */
    uint32_t events_overflow;

    /* v3: EXACT system-wide idle total (Activity + Client:ClientRead + the
     * Timeout pacing sleeps), accumulated at the writer independently of
     * events[] so it survives a full table. */
    uint64_t idle_ns;

    /* v7 (#408): the MEASURED-CPU SPLIT for this second, i.e. everything
     * pgwt_compute_time_model derives its two CPU rows from. Without these
     * the summary path could only report has_measured_cpu = 0 and one
     * combined "CPU*" row -- see the v7 note above.
     *
     * The CPU* total is cpu_measured_ns + cpu_unknown_ns, exactly as the raw
     * path's single cpu_measured_ns accumulator holds both tiers; the
     * runqueue row is then the residual DB - CPU* - Sigma waits, computed by
     * the reader from these and class_ns (the same leak-immune arithmetic the
     * raw path uses -- never a per-interval wall-minus-cpu split, because
     * se.sum_exec_runtime is only current at a tick or a context switch).
     *
     * cpu_measured_records is NOT derivable from the sums: a MEASURED ZERO is
     * a signal ("this window can separate running from waiting, and nothing
     * ran") while an absent measurement is not, and the raw path distinguishes
     * them by counting records, not nanoseconds.
     *
     * v9 (#424) WIDENS cpu_measured_records from "records" to CPU
     * OBSERVATIONS: a closed record with a cpu_ns, OR a per-second credit of
     * an open on-CPU gap whose producer supplied a measurement. It had to
     * widen. Its single consumer is `rec->cpu_measured_records > 0 =>
     * has_measured_cpu` (src/compute.c), and a window whose only CPU time is
     * one still-open gap would otherwise carry cpu_measured_ns while
     * reporting has_measured_cpu = 0 -- the reader would then ignore the
     * split it was just given and emit one combined CPU* row, which is the
     * v8 symptom reintroduced one layer up. Nothing compares it to a record
     * count. */
    uint64_t cpu_measured_ns;       /* Sigma clamped cpu_ns over we==0 gaps
                                     * + v9 per-second open-gap credits      */
    uint64_t cpu_unknown_ns;        /* Sigma wall over UNKNOWN we==0 gaps     */
    uint64_t cpu_clamped_ns;        /* Sigma (cpu_ns - wall) clamped away     */
    uint64_t wait_gap_cpu_ns;       /* Sigma cpu_ns on WAIT records (obs.)    */
    uint64_t cpu_measured_records;  /* CPU observations: records with
                                     * cpu_ns != UNKNOWN, plus v9 credits     */

    /* Per-session hash table (open addressing) */
    struct pgwt_summary_session sessions[SUMMARY_MAX_SESSIONS];
    int num_sessions;
    /* #555: sessions[] could not take an entry because all
     * SUMMARY_MAX_SESSIONS slots were occupied, so that pid's time was
     * DROPPED from sessions[].db_time_ns while class_ns and idle_ns kept it.
     * The visible symptom is Sigma Top Sessions < Overview's DB Time, which a
     * reader cannot tell apart from a broken accounting -- measured on the
     * RAW path the two agree to the last bit (2,495,784.951108 ms either way
     * over an 1851 s / 4.3M-event trace), so on the summary path any such gap
     * IS this bound and nothing said so. This is that signal, the exact
     * counterpart of events_overflow above.
     *
     * COUNTS DENIALS THAT COST sessions[] SOMETHING, not nanoseconds: one
     * per push whose pid could not claim a slot AND whose event was not
     * idle. The idle clause is the counter's MEANING -- every write into a
     * session slot is already closed to idle events, so a slot denied to an
     * idle-only pid loses nothing, and counting it fired the WARN and the
     * metric (both of which assert a DB-time shortfall) on precisely the
     * workload that trips the bound: a >1024-connection instance is mostly
     * backends in Client:ClientRead. src/summary_writer.c's accum_event has
     * the full argument; section 14g of tests/test_idle_summary.c pins it
     * both ways.
     *
     * Deliberately NOT the same rule as events_overflow, which counts every
     * denial: events[] DOES carry idle events (it is where every read path
     * gets the Idle breakdown from), so an idle denial there loses real data.
     * Both counters count what was lost. Also not guarded on `tns > 0`: a
     * non-idle denial still loses this record's top_wait candidacy, so the
     * counter answers "was session data dropped" and the lost NANOSECONDS
     * are the separate, exact class_ns - Sigma sessions[].db_time_ns.
     *
     * NOT YET SERIALIZED -- a SEQUENCED GAP, not a permanent limitation.
     * events_overflow is in the on-disk record trailer; putting this field
     * there is a FORMAT change, i.e. a PGWT_SUMMARY_VERSION bump and a raw
     * recompute of every retained capture (see the version notes above).
     * The owner APPROVED that bump on 2026-10-25; it is deliberately NOT in
     * this change, because it is being landed as ONE consolidated 8 -> 9 bump
     * together with the measured-CPU split for queries[].top_events, so that
     * two format changes cost one recompute instead of two. Do not read the
     * paragraph below as "captures will never carry this".
     *
     * UNTIL THAT LANDS: A CAPTURE ON DISK CARRIES NO RECORD OF ITS DROPPED
     * PIDS AT ALL. Not "cannot attribute them to individual seconds" -- none,
     * anywhere. sessions_overflow_total lives on the writer struct, is never
     * logged at exit, and pgwt-server instantiates no writer, so the only
     * path by which the count leaves the daemon is control.c's `metrics`
     * reply, polled while the daemon is still running. After the fact, the
     * WARN lines in the daemon's stderr are the only evidence. That is why
     * both drop sites emit one.
     *
     * THE SEAM, so the 8 -> 9 bump is a small diff. This field is already
     * shaped exactly like events_overflow: same name pattern, zeroed by
     * accum_reset with the rest of the accumulator, rolled up at the one
     * place in flush_accum that sits after every early return. What that
     * change has to add is only:
     *   - PGWT_SUMMARY_VERSION 8 -> 9 and its version note;
     *   - 4 bytes in pgwt_summary_serialize's trailer, after
     *     acc->events_overflow;
     *   - the matching read in pgwt_summary_deserialize under
     *     `if (version >= 9)`, REFUSING a short record (the v7 note above
     *     says why a zero there is not neutral);
     *   - the `+ 12 + 40` trailer term in PGWT_SUMMARY_SERIALIZE_MAX, and the
     *     layout comment above it.
     *
     * ONE TRAP IN THAT CHANGE, stated here because it is silent and it is the
     * same class of bug as #555 itself: pgwt_summary_deserialize already
     * WRITES this field, counting the entries it had to drop, and it does so
     * in the session loop -- which runs BEFORE the trailer is read. A plain
     * `memcpy(&acc->sessions_overflow, p, 4)` in the trailer would therefore
     * OVERWRITE the reader's own count with the writer's, losing exactly the
     * drops that the reader is the only witness to. Read the trailer value
     * into a local and ADD it, or move the reader's increment to a separate
     * field. tests/test_idle_summary.c section 14e pins the reader's count
     * and will go red if it is clobbered.
     *
     * The field is also set by pgwt_summary_deserialize when a record claims
     * more sessions than the table can hold -- and that site has its own
     * one-per-process WARN, because on the READ side nothing consumes this
     * field either: the counter alone would have left a forged block decoding
     * rc=0 and being served labelled exact with entries missing. */
    uint32_t sessions_overflow;

    /* Per-query hash table (open addressing) */
    struct pgwt_summary_query queries[SUMMARY_MAX_QUERIES];
    int num_queries;
};

/* ── On-disk record header (precedes compressed payload) ──── */

struct pgwt_summary_block_header {
    uint64_t wall_ns;          /* wall-clock timestamp of this second */
    uint32_t num_events;       /* distinct event types */
    uint16_t num_sessions;     /* distinct PIDs */
    uint16_t num_queries;      /* distinct query_ids */
    uint32_t compressed_size;
    uint32_t uncompressed_size;
} __attribute__((packed));

/* ── Writer state ─────────────────────────────────────────── */

/* v8 (#379): one pid's OPEN interval, as the producer last described it,
 * plus the monotone watermark that keeps the per-second credits and the
 * at-close remainder from overlapping.
 *
 * `credited_until_ns` outlives the interval it was accrued for: it is a
 * per-PID high-water mark over the pid's whole timeline, which is what makes
 * the tick/close race safe in BOTH orders. BPF stamps
 * `duration = now - last_ts; last_ts = now` on every transition
 * (src/bpf/pg_wait_tracer.bpf.c), so a pid's records TILE its timeline
 * exactly -- one watermark per pid therefore partitions that timeline with
 * no gap and no overlap.
 *
 * `we == 0` means "no creditable interval open for this pid": either the
 * producer has not noted one since the last close, or what is open is an
 * on-CPU gap, which v8 deliberately does not spread (see the version note).
 * The entry survives so the watermark does. */
#define SUMMARY_MAX_OPEN  MAX_BACKENDS   /* 1024 */
struct pgwt_summary_open {
    uint32_t pid;                 /* 0 = empty slot */
    uint32_t we;                  /* open wait id; 0 = nothing creditable
                                   * UNLESS cpu_slot says this is an open
                                   * on-CPU gap (v9) */
    uint32_t cat_flag;            /* producer's T2 category for this pid */
    uint64_t start_ns;            /* monotonic start of the open interval */
    uint64_t credited_until_ns;   /* monotone per-PID watermark */
    uint64_t credited_ns;         /* credited for the CURRENT interval only */
    uint64_t noted_ns;            /* when the producer last refreshed it */

    /* v9 (#424): the open ON-CPU gap. `we` is 0 for such an interval, which
     * is also the "nothing creditable" sentinel, so creditability is carried
     * by its own flag rather than overloaded onto `we` -- see
     * open_creditable() in summary_writer.c, the ONE predicate every loop
     * uses so a new state cannot be half-handled. */
    uint8_t  cpu_slot;            /* 1 = this slot is an open on-CPU gap */
    uint8_t  cpu_frozen;          /* 1 = class/tier fixed by a written second */
    uint8_t  cpu_tier_known;      /* frozen tier: 1 = measured, 0 = unknown */
    uint32_t cpu_eff_we;          /* producer's LATEST classification
                                   * (0 or PGWT_WEI_NONCMD_CPU) while the
                                   * interval is still unfrozen; the FROZEN
                                   * one once cpu_frozen is set */
    uint64_t cpu_prefix_ns;       /* producer's measured CPU over
                                   * [start_ns, its last note), or
                                   * PGWT_CPU_NS_UNKNOWN */
    uint64_t cpu_credited_ns;     /* measured CPU credited for THIS interval */
};

/* v9 (#424): a producer's OBSERVATION of one open on-CPU gap at tick time.
 * Passing NULL to pgwt_summary_note_open_ex (and calling the v8-signature
 * pgwt_summary_note_open, which is that call) means "I have no observation"
 * and keeps the v8 refusal.
 *
 * `eff_we` is the EFFECTIVE id for the gap as the producer's own classifier
 * resolved it -- pgwt_live_effective_event(we=0, cat_flag, cmd_gate_active,
 * in_cmd) on the live path. Exactly two values are legal, 0 (CPU) and
 * PGWT_WEI_NONCMD_CPU (idle, foreground outside a command); anything else is
 * refused and counted (open_cpu_eff_rejected_total), the same contract
 * pgwt_summary_push_event's `eff_event` has and for the same reason -- the
 * command gate lives in the producer and a writer-side copy would drift.
 * It is a PREFIX answer and may change between ticks; see piece 3 of the v9
 * note for what the writer does about that.
 *
 * `cpu_prefix_ns` is the measured on-CPU ns of [start_mono_ns, now_mono_ns),
 * CUMULATIVE and already clamped by the producer to that prefix's wall --
 * i.e. exactly the `cpu_open` that src/map_reader.c already computes for the
 * live view. PGWT_CPU_NS_UNKNOWN means the tier has no measurement at all
 * (cpu_accounting off, no CPU capability): the credit then goes to
 * cpu_unknown_ns, which is what the raw path does with an UNKNOWN record. */
struct pgwt_summary_open_cpu {
    uint32_t eff_we;
    uint64_t cpu_prefix_ns;
};

#define SUMMARY_QATTR_SLOTS 1024
struct pgwt_summary_qattr_slot {
    uint32_t pid;                 /* 0 = empty */
    struct pgwt_qattr_pid q;
};

struct pgwt_summary_writer {
    /* Configuration */
    char          trace_dir[256];
    int           retention_hours;
    gid_t         trace_gid;
    bool          enabled;
    bool          verbose;

    /* Clock offset: wall_ns - mono_ns at file creation */
    uint64_t      clock_offset_wall_ns;
    uint64_t      clock_offset_mono_ns;
    /* v8: the offsets above were set by pgwt_summary_pin_clock BEFORE the
     * first record, so the lazy file open must not overwrite them. Offline
     * producers only; the daemon leaves this false. */
    bool          clock_anchor_pinned;

    /* Current file */
    FILE         *fp;
    char          current_path[512];
    int           current_hour;
    /* #430: has a summary file EVER been opened by this writer? `fp == NULL`
     * alone cannot answer that, and the seal needs the answer: NULL because
     * nothing was ever opened means the clock anchor does not exist and no
     * record may be invented, while NULL because pgwt_summary_check_rotation
     * archived the file means the anchor is fine and the seal must reopen
     * (see writer_seal_through / seal_can_write).
     *
     * `current_hour` looks like it could serve -- init sets -1,
     * open_summary_file sets a real hour -- but it CANNOT: the deterministic
     * rotation seam every test uses is to set current_hour = -1 so the
     * cached hour differs from localtime(), and check_rotation itself never
     * writes the field back. So after a forced rotation current_hour is -1
     * with a file very much having been opened, i.e. exactly the state the
     * sentinel is supposed to exclude. Measured: using current_hour left
     * tests/test_rotation_open_credit.c §1 at 5000 of 11000 ms with the fix
     * applied. A separate flag cannot be aliased by a seam. */
    bool          file_ever_opened;
    /* #447: what the OPEN file's header currently claims, which is not the
     * same as the writer's anchor above. The reader places a file by
     * start_time_ns and prunes any file whose start is after the window's
     * end, but a file's records can be OLDER than the instant it was opened
     * -- an event's second began before it arrived, and #430's seal reopen
     * writes a second that completed ~1.1 s earlier. widen_file_start moves
     * these two earlier, in step, so the header keeps covering every record
     * in the file; the writer's own clock_offset_* must NOT move with them
     * (mono_to_wall and the archive name both depend on those). */
    uint64_t      file_start_wall_ns;
    uint64_t      file_start_mono_ns;

    /* Block index (footer) */
    struct pgwt_block_index_entry *block_index;
    int           num_blocks;
    int           block_index_cap;

    /* Per-second accumulator */
    struct pgwt_summary_accum accum;
    bool          accum_active;    /* has any events been accumulated? */

    /* #277: the monotonic second of the last record actually written. A
     * record is immutable once on disk, so a second that has been written
     * can never be reopened -- the reader would sum it twice. An event that
     * arrives late for such a second is folded into the oldest second still
     * open (late_events_folded_total counts those, so the fold is never
     * silent; the control socket publishes it as
     * summary_late_events_folded_total). have_flushed_second distinguishes
     * "nothing written yet" from "wrote the second at monotonic 0".
     *
     * The clamp runs before accum_event, which returns early for markers
     * (PGWT_IS_MARKER) and for PGWT_EVENT_EXIT -- so a late marker is
     * counted here while contributing no counts and no time. The counter is
     * a skew signal, not an exact count of mis-timed wait events. */
    uint64_t      last_flushed_second_mono_ns;
    bool          have_flushed_second;
    uint64_t      late_events_folded_total;

    /* Scratch buffers */
    uint8_t      *encode_buf;
    size_t        encode_buf_size;
    uint8_t      *compress_buf;
    size_t        compress_buf_size;

    /* Stats */
    /* Seconds that could not be serialised/compressed. Non-zero means the
     * window has holes; it was previously impossible to tell, because every
     * caller of flush_accum discarded its return value. */
    uint64_t      flush_failures_total;
    /* Σ of every record's events_overflow, so the control socket can report
     * one number instead of a reader having to sum the blocks. */
    uint64_t      events_overflow_total;
    /* #555: Σ of every WRITTEN record's sessions_overflow. Read it exactly
     * the way events_overflow_total is read: WRITTEN seconds only, so a
     * second lost to a failed rotation or close never reaches it and
     * flush_failures_total is the signal for the seconds it cannot speak
     * for. Unlike events_overflow_total this is the ONLY place the count
     * survives the flush -- the per-record field is not on disk (see
     * pgwt_summary_accum::sessions_overflow).
     *
     * SCOPE, because it is easy to over-read: this is LIVE-DAEMON state. It
     * is never written to the trace directory and never logged at exit, so it
     * is reachable only by a `metrics` query to the control socket of a
     * daemon that is still running. A capture analysed later carries nothing
     * from it -- until the approved PGWT_SUMMARY_VERSION 8 -> 9 bump puts the
     * per-record field on disk (see pgwt_summary_accum::sessions_overflow for
     * the seam and the one trap in it). This total stays useful after that:
     * it is what spares a reader summing every block. */
    uint64_t      sessions_overflow_total;
    uint64_t      total_records_written;
    uint64_t      total_bytes_written;
    /* v6 (#315/#376): records dropped because their pid is an io_worker, and
     * their wall ns. Not a diagnostic afterthought: it is the only way to see
     * the pgwt_live_pid_cat_flag residual in production (a pid whose metadata
     * is not parsed yet resolves to foreground, so its earliest records are
     * accumulated while the raw path -- tagging post hoc from backends.jsonl
     * -- excludes them). Compare against the raw io_worker total for the same
     * capture: the difference IS the residual. Exported by the control socket
     * as summary_io_worker_events_excluded_total /
     * summary_io_worker_ns_excluded_total. */
    uint64_t      io_worker_events_excluded_total;
    uint64_t      io_worker_ns_excluded_total;
    /* v7 (#408): pushes whose producer resolved no effective wait id
     * (PGWT_SUMMARY_EFF_UNRESOLVED -- the record keeps evt->old_event, i.e.
     * the v6 behaviour), and pushes whose eff_event was neither
     * evt->old_event nor PGWT_WEI_NONCMD_CPU and was therefore REFUSED. Both
     * are the non-command relabel's residual made observable, the same role
     * io_worker_*_excluded_total play for v6: a non-zero
     * eff_unresolved_events_total means some of this file's we==0 time is
     * classified the pre-v7 way, and eff_rejected_events_total should be 0 in
     * any build (it means a producer is passing an id the rule does not
     * allow). Exported by the control socket as
     * summary_eff_unresolved_events_total /
     * summary_eff_rejected_events_total. */
    uint64_t      eff_unresolved_events_total;
    uint64_t      eff_rejected_events_total;

    /* #128 deferred per-query attribution (query_attr.h), per pid; heap
     * (SUMMARY_QATTR_SLOTS entries, open addressing, reclaimed on backend
     * exit), NULL = disabled. The two counters are exported by the control
     * socket (summary_query_attr_table_full_total,
     * summary_query_unattributed_ns_total): records that kept their
     * emission-time id because the table was full, and deferred time no
     * command claimed (the summary has no unattributed bucket). */
    struct pgwt_summary_qattr_slot *qattr;
    uint64_t      qattr_table_full_total;
    uint64_t      qattr_unattributed_ns_total;

    /* v8 (#379): the open-interval watermarks, heap (SUMMARY_MAX_OPEN
     * entries, open addressing), NULL = the mechanism is off and the writer
     * behaves exactly as v7. */
    struct pgwt_summary_open *open;
    /* Every way this mechanism can do something other than spread a
     * duration, counted so none of them is silent. Exported by the control
     * socket as summary_open_*:
     *   credited_ns     Sigma of every per-second open credit. ZERO on a
     *                   capture that contains a multi-second wait means the
     *                   producer is not feeding notes -- i.e. v7 behaviour
     *                   with a v8 version stamp, which is the one failure
     *                   this whole change could have that still looks green.
     *   records         open-credit-only per-second records materialised
     *                   (seconds in which nothing closed but something was
     *                   open).
     *   late_ns         span an interval was open for in seconds that were
     *                   already written when the producer first noted it.
     *                   Not lost: the closing record still carries it, i.e.
     *                   that slice keeps the v7 answer.
     *   notes_expired   notes that went stale (PGWT_SUMMARY_OPEN_STALE_NS)
     *                   before being refreshed -- the fail-safe firing.
     *   table_full      note_open refused because every slot was taken.
     *   io_worker_refused / cpu_refused  notes the rule declines to spread.
     *   cpu_open_uncredited_ns_total (field below; BREAKS the summary_open_*
     *                   wire naming above -- its control-socket key is
     *                   summary_cpu_open_uncredited_ns_total, "cpu_open_"
     *                   first, not "open_cpu_"). It is the #379 residual:
     *                   CUMULATIVE over the whole capture, not scoped to any
     *                   later-queried window, and it is the gap's WALL time
     *                   recorded once, AT CLOSE -- not a measured-CPU
     *                   quantity and not what is currently missing from an
     *                   in-flight window. See the increment site
     *                   (pgwt_summary_push_event, "#379 residual") for the
     *                   exact condition. */
    uint64_t      open_credited_ns_total;
    uint64_t      open_credit_records_total;
    uint64_t      open_credit_late_ns_total;
    uint64_t      open_notes_expired_total;
    uint64_t      open_table_full_total;
    uint64_t      open_io_worker_refused_total;
    uint64_t      open_cpu_refused_total;
    uint64_t      cpu_open_uncredited_ns_total;
    uint64_t      open_fill_capped_total;
    /* #430: the SEAL's own file lifecycle. pgwt_summary_check_rotation closes
     * the file and leaves w->fp NULL for a LAZY reopen, so the seal has to be
     * able to reopen it itself -- otherwise every tick between the rotation
     * and the next PUSHED record seals nothing (writer_seal_through). The two
     * outcomes are not interchangeable, so they are two numbers:
     *   seal_reopens_total          the seal reopened the file because it had
     *                               a second to write and the rotation had
     *                               closed it. Non-zero is the proof this
     *                               path is REACHED; ZERO on a capture that
     *                               rotated with an interval open means the
     *                               pause is back and the gate stopped
     *                               looking.
     *   seal_reopen_failures_total  the reopen FAILED (no space, a
     *                               current.summary that cannot be created).
     *                               Nothing is fabricated and nothing is
     *                               advanced: credited_until_ns is untouched,
     *                               so the seconds stay OWED and a later
     *                               successful seal still places them. This
     *                               is therefore the number that says a
     *                               window is short -- a gap as a number
     *                               rather than a belief. */
    uint64_t      open_seal_reopens_total;
    uint64_t      open_seal_reopen_failures_total;
    /* The FIRST open failing, as opposed to a reopen: the capture never
     * produced a summary file at all. Separate because the two mean different
     * things to whoever reads them -- "there is nothing" vs "recording
     * stopped mid-capture with seconds owed" -- and because the push path
     * reaches both. Both are published as summary_*. */
    uint64_t      first_open_failures_total;
    /* #447: times this file's header start had to be moved EARLIER so the
     * reader would still find a record older than the open instant, and times
     * that could not be done (a write failure, or a monotonic anchor too
     * small to shift). A non-zero failure count means some file is placed
     * conservatively LATE and a narrow window may miss its earliest records;
     * it is the number that says so rather than leaving it to be assumed. */
    uint64_t      file_start_widened_total;
    uint64_t      file_start_widen_failures_total;
    /*   note_mismatch   a closing record whose pid had a note describing a
     *                   DIFFERENT interval (different wait id or start).
     *                   Nothing is subtracted in that case -- the record is
     *                   charged whole, the v7 answer -- so this counts how
     *                   often the producer's open-interval view and the
     *                   record stream disagree. Expected to be 0 from the
     *                   daemon, where BPF's records tile each pid's
     *                   timeline; non-zero only means some records keep the
     *                   old accounting, never that time was double counted. */
    uint64_t      open_note_mismatch_total;

    /* v9 (#424): the open-ON-CPU credit, and every way it can do something
     * other than spread wall+measurement. Same discipline as the v8 block
     * above -- nothing here is silent. Intended control-socket names
     * summary_open_cpu_*:
     *   credited_ns          Sigma of the WALL credited to seconds from open
     *                        on-CPU gaps. ZERO on a capture that contains a
     *                        multi-second CPU gap means no producer is
     *                        passing an observation -- i.e. v8 behaviour
     *                        under a v9 stamp, the one failure this change
     *                        could have that still looks green. Pair it with
     *                        cpu_open_uncredited_ns_total, which counts the
     *                        complement.
     *   measured_ns          Sigma of the MEASURED CPU credited to seconds.
     *                        The quantity v8 argued could not exist.
     *   unknown_ns           Sigma of wall credited to the UNKNOWN tier.
     *   eff_rejected         notes whose eff_we was neither 0 nor
     *                        PGWT_WEI_NONCMD_CPU; treated as 0 and counted.
     *                        Should be 0 in any build.
     *   class_divergent_ns   wall of intervals whose FROZEN class differed
     *                        from the producer's answer at close -- the
     *                        measured cost of freezing (piece 3).
     *   tier_mismatch        intervals whose measurement tier flipped
     *                        between the first credit and the close.
     *   overcredit_ns        measured CPU credited while open that the
     *                        closing record's own cpu_ns did not cover
     *                        (schedstat jitter): the one place the measured
     *                        identity can fail. The WALL identity cannot.
     */
    uint64_t      open_cpu_credited_ns_total;
    uint64_t      open_cpu_measured_ns_total;
    uint64_t      open_cpu_unknown_ns_total;
    uint64_t      open_cpu_eff_rejected_total;
    uint64_t      open_cpu_class_divergent_ns_total;
    uint64_t      open_cpu_tier_mismatch_total;
    uint64_t      open_cpu_overcredit_ns_total;
};

/* ── Public API ───────────────────────────────────────────── */

int  pgwt_summary_writer_init(struct pgwt_summary_writer *w,
                               const char *trace_dir,
                               int retention_hours,
                               const char *group_name);

/* Push a raw trace event. Handles second-boundary detection and flushing.
 *
 * `cat_flag` is the pid's T2 category, as the PRODUCER resolved it: 0 =
 * foreground/unresolved, else PGWT_EVENT_FLAG_IO_WORKER / _MAINT /
 * _BACKGROUND (the values pgwt_live_pid_cat_flag and the server's
 * bm_type_to_cat_flag return). v6: an io_worker record is excluded from
 * every accumulator, matching the raw paths in src/compute.c.
 *
 * It is a PARAMETER rather than a check on evt->flags because evt->flags
 * carries no category bits on the live path -- the daemon never sets
 * PGWT_EVENT_FLAG_IO_WORKER, the server applies it at raw LOAD time via
 * pgwt_tag_events. A flags-only check in the writer is therefore dead in the
 * daemon (hole N7, tests/test_agg_raw_crosscheck.c). Making it an argument
 * also means no caller can inherit the wrong answer silently: every producer
 * must say what it resolved. Pass 0 for a pid-less marker (the writer drops
 * markers anyway, and resolving a category for pid 0 would be meaningless).
 *
 * `eff_event` (v7, #408) is the EFFECTIVE wait id the producer resolved
 * through the shared classifier -- pgwt_live_effective_event() on the live
 * path, pgwt_tag_events()' rewrite offline. Its only legal value other than
 * evt->old_event is PGWT_WEI_NONCMD_CPU: a foreground we==0 interval outside
 * a command is post/between-command IDLE time, not CPU, and both raw paths
 * file it that way (docs/AAS_SEMANTICS_DECISION.md, issues #97 and #408). It
 * is a PARAMETER for the same reason cat_flag is: the command gate lives in
 * the producer's per-pid accumulator, and a writer-side guess would be a
 * second copy of the rule that could drift from the one the raw paths use.
 * PGWT_SUMMARY_EFF_UNRESOLVED means "not resolved" -- the writer uses
 * evt->old_event and counts the push (eff_unresolved_events_total), so a
 * producer that cannot resolve degrades to the v6 behaviour VISIBLY instead
 * of silently. Anything else is refused and counted
 * (eff_rejected_events_total). */
int  pgwt_summary_push_event(struct pgwt_summary_writer *w,
                              const struct pgwt_trace_event *evt,
                              uint32_t cat_flag,
                              uint32_t eff_event);

/* v8 (#379): tell the writer what interval `pid` currently has OPEN, so the
 * seconds it occupies are credited as they complete instead of the whole
 * duration landing in the second it closes in. Call it for every pid with a
 * live interval on every tick, BEFORE pgwt_summary_flush_completed: a note is
 * only creditable while it is fresher than PGWT_SUMMARY_OPEN_STALE_NS, which
 * is what stops a pid that vanishes without an EXIT record from accruing
 * fabricated time forever.
 *
 * `we` is the RAW wait id of the open interval (state_map's last_event), not
 * an effective one: the only relabel v7 allows is a we == 0 gap becoming
 * PGWT_WEI_NONCMD_CPU. v8 declined to spread we == 0 at all; v9 spreads it
 * when -- and only when -- the producer supplies the CPU observation that
 * makes the split possible, which THIS entry point does not (see
 * pgwt_summary_note_open_ex below). Calling this function for a we == 0 gap
 * is therefore the v8 refusal, counted in open_cpu_refused_total.
 * `start_mono_ns` is the interval's start on the SAME clock as
 * evt->timestamp_ns (bpf_ktime_get_ns / CLOCK_MONOTONIC) -- state_map's
 * last_ts, which is exactly where the closing record's
 * timestamp_ns - duration_ns comes from. `now_mono_ns` is the producer's
 * current clock read, used only for freshness.
 *
 * Calling it never ADDS time by itself: time is credited only as whole
 * seconds complete, and only for the part of the interval that the pid's
 * monotone watermark has not already been credited for. Not calling it at
 * all is the exact v7 behaviour. */
void pgwt_summary_note_open(struct pgwt_summary_writer *w,
                             uint32_t pid, uint32_t we,
                             uint64_t start_mono_ns, uint32_t cat_flag,
                             uint64_t now_mono_ns);

/* v9 (#424): the same note, plus the producer's CPU OBSERVATION of an open
 * ON-CPU gap. This is the full interface; pgwt_summary_note_open() above is
 * exactly this call with `cpu == NULL`.
 *
 * `cpu` is read ONLY when the noted interval is on-CPU (we == 0, or a `we`
 * whose class is CPU). For every wait id it is ignored, so one producer loop
 * can pass it unconditionally. `cpu == NULL` on an on-CPU gap keeps the v8
 * refusal exactly -- no credit, counted in open_cpu_refused_total -- which
 * is what makes "a producer that has no measurement" a documented
 * degradation rather than a silent mis-split.
 *
 * The live call site (src/map_reader.c) already computes both members for
 * the live view, a few lines below where it notes the interval: `cpu_open`
 * (the clamped schedstat delta over the open stretch) and
 * pgwt_live_effective_event(we, cat_flag, d->cmd_gate_active, in_cmd) with
 * in_cmd from pgwt_live_cmd_gate_classify over [last_ts, now). Pass those,
 * not a second copy of either rule. */
void pgwt_summary_note_open_ex(struct pgwt_summary_writer *w,
                                uint32_t pid, uint32_t we,
                                uint64_t start_mono_ns, uint32_t cat_flag,
                                uint64_t now_mono_ns,
                                const struct pgwt_summary_open_cpu *cpu);

/* Drop `pid`'s open-interval note and its watermark (backend exit, GC, or a
 * producer that knows the pid is gone). Idempotent. The staleness rule is the
 * fail-safe for the pids this is never called for; this is the exact one. */
void pgwt_summary_forget_open(struct pgwt_summary_writer *w, uint32_t pid);

/* Pin the mono->wall anchor BEFORE the first record, for an OFFLINE producer
 * driving the writer with synthetic timestamps (tests/gen_test_traces.c).
 * The file opens lazily on the first push and stamps the anchor from the
 * real clocks; v8 can write a per-second record for a second EARLIER than
 * that first record (every second a long interval was already open in), so
 * re-anchoring afterwards is too late for exactly the records this change
 * adds. The daemon never calls this and is unaffected. */
void pgwt_summary_pin_clock(struct pgwt_summary_writer *w,
                             uint64_t mono_anchor_ns, uint64_t wall_anchor_ns);

/* Force-flush the current accumulator whatever its state (rotation, close,
 * and offline generators that drive the writer with synthetic timestamps).
 * NOT for the periodic daemon tick: see pgwt_summary_flush_completed. */
int  pgwt_summary_flush(struct pgwt_summary_writer *w);

/* Periodic (daemon-tick) flush. Writes the accumulated second only once it
 * is complete -- i.e. once now_mono_ns has passed its end plus
 * PGWT_SUMMARY_FLUSH_LAG_NS -- and leaves a second still receiving events
 * alone. #277: the unconditional flush wrote the in-progress second, and
 * the rest of that same second was then written again as a superset, so
 * every summaries-path aggregate counted part of each second twice.
 *
 * now_mono_ns is CLOCK_MONOTONIC, the same clock as evt->timestamp_ns
 * (bpf_ktime_get_ns). It is a parameter, not a clock read, so the decision
 * is testable without depending on wall time. */
int  pgwt_summary_flush_completed(struct pgwt_summary_writer *w,
                                   uint64_t now_mono_ns);

/* Check for hourly rotation. Call from timer handler. */
int  pgwt_summary_check_rotation(struct pgwt_summary_writer *w);

/* Close current file (flush + write footer). */
int  pgwt_summary_close(struct pgwt_summary_writer *w);

/* Delete old .summary.lz4 files beyond retention. */
int  pgwt_summary_cleanup_old_files(struct pgwt_summary_writer *w);

/* Free allocated buffers. */
void pgwt_summary_destroy(struct pgwt_summary_writer *w);

/* ── Serialization (exposed for reader/testing) ───────────── */

/* Serialize accumulator to buffer. Returns bytes written. */
size_t pgwt_summary_serialize(const struct pgwt_summary_accum *acc,
                               uint8_t *out, size_t out_size);

/* Deserialize buffer into accumulator. Returns 0 on success. */
int pgwt_summary_deserialize(const uint8_t *in, size_t in_size,
                              struct pgwt_summary_accum *acc,
                              int version);

#endif /* PGWT_SUMMARY_WRITER_H */
