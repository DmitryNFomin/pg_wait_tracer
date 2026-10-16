/* summary_writer.c — Per-second summary file writer
 *
 * Accumulates trace events into 1-second snapshots. Each snapshot contains
 * time model, per-event stats (with histograms), per-session stats, and
 * per-query stats. Snapshots are serialized and LZ4-compressed to
 * .summary.lz4 files with the same hourly rotation as trace files. */
#include "summary_writer.h"
#include "event_writer.h"   /* block index entry, varint, trace file header reuse */
#include <stddef.h>         /* offsetof: widen_file_start rewrites two header
                             * fields in place (#447) */
/* pgwt_duration_to_bucket: defined in map_reader.c (daemon) but not available
 * in the server build. Use a local copy for server compilation. */
#ifdef PGWT_SERVER
static uint32_t pgwt_duration_to_bucket(uint64_t ns)
{
    /* Hardcoded log2 buckets matching daemon (map_reader.c) exactly.
     * 0: <1us, 1: 1-2us, ..., 15: >=16ms */
    uint64_t us = ns / 1000;
    if (us < 1)     return 0;
    if (us < 2)     return 1;
    if (us < 4)     return 2;
    if (us < 8)     return 3;
    if (us < 16)    return 4;
    if (us < 32)    return 5;
    if (us < 64)    return 6;
    if (us < 128)   return 7;
    if (us < 256)   return 8;
    if (us < 512)   return 9;
    if (us < 1024)  return 10;
    if (us < 2048)  return 11;
    if (us < 4096)  return 12;
    if (us < 8192)  return 13;
    if (us < 16384) return 14;
    return 15;
}
#else
#include "map_reader.h"     /* pgwt_duration_to_bucket */
#endif
#include "wait_event.h"
#include "idle_rule.h"   /* pgwt_is_idle_event / pgwt_is_session_idle_event */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <grp.h>
#include <lz4.h>

/* ── Wait class index (local copy to avoid compute.c dependency) ── */

static int summary_wait_class_index(uint32_t event_id)
{
    if (event_id == 0)
        return PGWT_CLASS_CPU;
    uint8_t cls = (event_id >> 24) & 0xFF;
    switch (cls) {
    case 0x0A: return PGWT_CLASS_IO;
    case 0x03: return PGWT_CLASS_LOCK;
    case 0x01: return PGWT_CLASS_LWLOCK;
    case 0x08: return PGWT_CLASS_IPC;
    case 0x06: return PGWT_CLASS_CLIENT;
    case 0x09: return PGWT_CLASS_TIMEOUT;
    case 0x04: return PGWT_CLASS_BUFFERPIN;
    case 0x05: return PGWT_CLASS_ACTIVITY;
    case 0x07: return PGWT_CLASS_EXTENSION;
    default:   return PGWT_CLASS_UNKNOWN;
    }
}

/* ── Accumulator helpers ──────────────────────────────────── */

static void accum_reset(struct pgwt_summary_accum *acc)
{
    memset(acc, 0, sizeof(*acc));
}

/* End the current second. #277: clearing only accum_active left every
 * counter in place, so the next event in that same second resumed counting
 * on top of a record already on disk and the second was written again as a
 * superset. The accumulator must never carry counts across a flush, so the
 * reset and the flag live together in one place. */
static void accum_close(struct pgwt_summary_writer *w)
{
    accum_reset(&w->accum);
    w->accum_active = false;
}

static uint32_t hash32(uint32_t x)
{
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x;
}

static uint64_t hash64(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

/* Find or insert event in open-addressing hash table.
 * Returns pointer to slot, or NULL if table full. */
static struct pgwt_summary_event *find_or_insert_event(
    struct pgwt_summary_accum *acc, uint32_t event_id)
{
    uint32_t idx = hash32(event_id) & (SUMMARY_MAX_EVENTS - 1);
    for (int i = 0; i < SUMMARY_MAX_EVENTS; i++) {
        struct pgwt_summary_event *e = &acc->events[idx];
        /* For event_id=0 (CPU), an empty slot looks identical to "found"
         * since both have event_id==0. Distinguish by CONTENT.
         *
         * v9 (#424): the witness is `count > 0 || total_ns > 0`, not
         * `count > 0`. An open on-CPU gap's per-second credit adds TIME and
         * no count (counts are per-occurrence and ride the closing record),
         * so with the old test the id-0 row read as empty on every later
         * lookup: the second credit would re-insert into the same slot and
         * bump acc->num_events again, and the block header's num_events
         * would then exceed the entries pgwt_summary_serialize emits --
         * the reader walks a count it was given, so that is a decode
         * failure, not a cosmetic one. pgwt_summary_serialize's skip test
         * carries the identical clause for the same reason. */
        if (e->event_id == event_id &&
            (event_id != 0 || e->count > 0 || e->total_ns > 0))
            return e;
        if (e->event_id == 0 && e->count == 0 && e->total_ns == 0) {
            /* Empty slot — insert */
            e->event_id = event_id;
            acc->num_events++;
            return e;
        }
        idx = (idx + 1) & (SUMMARY_MAX_EVENTS - 1);
    }
    return NULL; /* table full */
}

static struct pgwt_summary_session *find_or_insert_session(
    struct pgwt_summary_accum *acc, uint32_t pid)
{
    uint32_t idx = hash32(pid) & (SUMMARY_MAX_SESSIONS - 1);
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++) {
        struct pgwt_summary_session *s = &acc->sessions[idx];
        if (s->pid == pid)
            return s;
        if (s->pid == 0 && s->db_time_ns == 0) {
            s->pid = pid;
            acc->num_sessions++;
            return s;
        }
        idx = (idx + 1) & (SUMMARY_MAX_SESSIONS - 1);
    }
    return NULL;
}

static struct pgwt_summary_query *find_or_insert_query(
    struct pgwt_summary_accum *acc, uint64_t query_id)
{
    if (query_id == 0) return NULL;  /* skip unknown queries */

    uint32_t idx = (uint32_t)(hash64(query_id) & (SUMMARY_MAX_QUERIES - 1));
    for (int i = 0; i < SUMMARY_MAX_QUERIES; i++) {
        struct pgwt_summary_query *q = &acc->queries[idx];
        if (q->query_id == query_id)
            return q;
        if (q->query_id == 0 && q->count == 0) {
            q->query_id = query_id;
            acc->num_queries++;
            return q;
        }
        idx = (idx + 1) & (SUMMARY_MAX_QUERIES - 1);
    }
    return NULL;
}

/* Per-query part of the accumulation: `count` intervals of `old_ev`
 * totalling `total_ns` (longest `max_ns`) attributed to `query_id`. Called
 * once per record with count 1, and from the #128 resolver with a deferred
 * aggregate once the pid's command reported its id. */
static void accum_query_add(struct pgwt_summary_accum *acc, uint64_t query_id,
                            uint32_t old_ev, uint64_t count,
                            uint64_t total_ns, uint64_t max_ns)
{
    struct pgwt_summary_query *sq = find_or_insert_query(acc, query_id);
    if (!sq)
        return;
    int cls = summary_wait_class_index(old_ev);
    /* v3: the per-query TOTALS exclude idle, matching the raw top-queries path
     * (src/compute.c pgwt_compute_top_queries skips idle records). Before v3
     * the summary path accumulated them and the raw path did not, so the same
     * query reported a different total depending on which path answered.
     * top_events[] below deliberately still records idle events: the Events
     * tab under a query filter must keep showing them (they are excluded from
     * load by pgwt_is_idle_event, not hidden by pgwt_is_hidden_event). */
    int idle = pgwt_is_idle_event(old_ev);
    /* v3: exact, and NOT derived from top_events[] -- that list holds 8
     * entries, so a 9th distinct event would vanish from the query's Idle
     * total. See struct pgwt_summary_query::idle_ns. */
    if (idle)
        sq->idle_ns += total_ns;
    if (!idle) {
        sq->count += count;
        sq->total_ns += total_ns;
        /* Per-class breakdown */
        if (cls >= 0 && cls < PGWT_NUM_CLASSES)
            sq->class_ns[cls] += total_ns;
    }
    /* Per-event top-8 tracking.
     *
     * #317 (v5): event 0 -- CPU -- IS filed here. It used to be guarded out
     * by `if (old_ev != 0)`, with no reason recorded, while every sibling
     * accumulator files it (the system-wide events[] via
     * find_or_insert_event, and sessions[].cpu_ns). top_events[] is the ONLY
     * source the summary path has for a per-query event breakdown, so the
     * Events tab drilled into one query over a >= 120 s window came back
     * missing the whole CPU* row and its db_time_ms was short by that
     * query's entire CPU time: measured raw 7002.426 ms vs summary 2949.426
     * ms, -57.9%, on the cross-check fixture. REACHABLE -- handle_top_events
     * adds no query guard, unlike handle_top_sessions and handle_heatmap.
     *
     * Filing it, rather than guarding the handler into the raw path, because
     * the Events tab is the one users drill and this is the content the
     * summary is for; and because nothing else needed changing to read it:
     * the slot sentinel is not event_id but num_top_events (the lookup loop
     * and the serializer both bound themselves by it), and te_summary_visitor
     * probes its hash table on `count > 0`, so an occupied slot holding
     * event_id 0 is unambiguous on the wire and in the reader.
     *
     * It does consume one of the 8 slots. CPU is normally among a query's
     * largest consumers, so that is the right slot to spend; the bounded-list
     * caveat is unchanged and the exact per-query totals (count, total_ns,
     * class_ns, idle_ns) do not come from this list.
     *
     * NOT changed: the top_wait selection below still skips event 0. The raw
     * path does the same (pgwt_compute_top_sessions sends event 0 to cpu_ns
     * rather than to its per-wait table and renders "no wait" as "CPU*"), so
     * the two paths already agree there -- see #319 for the part of top_wait
     * that does NOT agree. */
    {
        int found = -1;
        for (int j = 0; j < sq->num_top_events; j++) {
            if (sq->top_events[j].event_id == old_ev) { found = j; break; }
        }
        if (found >= 0) {
            sq->top_events[found].count += count;
            sq->top_events[found].total_ns += total_ns;
        } else if (sq->num_top_events < SUMMARY_QUERY_TOP_EVENTS) {
            sq->top_events[sq->num_top_events].event_id = old_ev;
            sq->top_events[sq->num_top_events].count = count;
            sq->top_events[sq->num_top_events].total_ns = total_ns;
            sq->num_top_events++;
        }
    }
    /* v3: top wait is never an idle event -- see the per-session note above. */
    if (old_ev != 0 && !idle && max_ns > sq->top_wait_ns) {
        sq->top_wait_id = old_ev;
        sq->top_wait_ns = max_ns;
    }
}

/* Accumulate a single trace event into the per-second snapshot. `query_id`
 * is the id its per-query part is filed under (0 = none / deferred).
 * `io_worker` is the producer's resolved verdict on the pid's category
 * (pgwt_summary_push_event's cat_flag) -- see the v6 note in summary_writer.h.
 * Returns 1 if the record was dropped BECAUSE it belongs to an io_worker, so
 * the caller can count it; 0 otherwise (accumulated, or dropped as a
 * marker/EXIT, which have their own long-standing reasons). */
/* What the per-second open credits already placed for the interval this
 * record closes -- the matching subtraction, without which the credits
 * would be counted twice. v8 carried only `already_ns` as a bare argument;
 * v9 needs four more facts about the same interval, and passing them as one
 * named struct is what stops a caller from supplying three of them.
 *
 * All-zero (the `{ 0 }` a non-open-interval caller passes) is exactly the
 * v7 close: nothing subtracted, no label override. */
struct accum_close_ctx {
    uint64_t already_ns;       /* wall already credited to earlier seconds */
    uint64_t already_cpu_ns;   /* MEASURED CPU already credited (v9)       */
    uint32_t frozen_eff;       /* v9: the label the written seconds used   */
    uint8_t  cpu_credited;     /* v9: this interval WAS credited open      */
    uint8_t  tier_unknown;     /* v9: those credits went to the UNKNOWN tier */
};

static int accum_event(struct pgwt_summary_accum *acc,
                        const struct pgwt_trace_event *evt,
                        uint64_t query_id, bool io_worker, uint32_t eff_event,
                        const struct accum_close_ctx *cc)
{
    /* v7 (#408): the EFFECTIVE id, as the producer resolved it (validated by
     * pgwt_summary_push_event, which is also where an unresolved or illegal
     * value is counted). It replaces evt->old_event for EVERY accumulator
     * below -- class_ns, idle_ns, events[], sessions[] and queries[] -- for
     * the same reason v6's io_worker drop covers all five: the raw paths
     * REWRITE the record's old_event (pgwt_tag_events) so every row and
     * bucket downstream sees the relabel, and a partial application here
     * would not fix the divergence, it would relocate it. */
    uint32_t old_ev = eff_event;
    /* v9 (#424), piece 3: an interval whose earlier seconds are already on
     * disk under a FROZEN label keeps that label for its closing record
     * too. The producer's answer is a majority over the open prefix and can
     * have moved since; the written seconds cannot. Overriding here, before
     * every accumulator, is what keeps the count and the time on the SAME
     * events[] row -- and the override stays inside v7's rule, because the
     * frozen value is one of the same two ids (0, PGWT_WEI_NONCMD_CPU)
     * eff_event itself is restricted to. */
    if (cc->cpu_credited)
        old_ev = cc->frozen_eff;
    uint64_t dur = evt->duration_ns;
    /* v8 (#379): the part of this record's duration the per-second watermark
     * has NOT already credited to the earlier seconds the interval occupied.
     * `dur` stays the FULL duration and keeps every per-OCCURRENCE quantity
     * honest -- count, histogram bucket, max_ns, top_wait and the whole of
     * queries[] (never credited open; see the v8 note) -- while `tns` is what
     * every TIME accumulator takes, so the interval's nanoseconds land in the
     * seconds they happened in and are counted exactly once in total.
     * cc->already_ns is 0 unless a producer noted this pid's interval,
     * which is what makes "no notes" byte-identical to v7. */
    uint64_t tns = dur > cc->already_ns ? dur - cc->already_ns : 0;

    if (old_ev == PGWT_EVENT_EXIT)
        return 0;

    /* Markers must never be accumulated (FID-4): exec/plan markers inflate
     * per-query counts (skewing avg low) and escalation markers insert
     * bogus PGWT_ESC_PACK query_ids. This is the write-side chokepoint —
     * event_stream pushes every record (markers included, so they land in
     * the trace) before its own marker check. */
    if (PGWT_IS_MARKER(old_ev))
        return 0;

    /* v6 (#315, #376): io_worker records are OUTSIDE the model, exactly as on
     * the raw paths (src/compute.c: the time model drops their idle time and
     * files their busy time under PGWT_CAT_IO_WORKER; Top Events, Top
     * Sessions, Top Queries and the AAS loop skip them outright).
     *
     * ALL FIVE accumulators, deliberately: class_ns, idle_ns, events[],
     * sessions[] and queries[]. A partial exclusion does not fix the
     * divergence, it relocates it -- events[] is where every read path gets
     * the per-event rows and the HISTOGRAMS from, so leaving it in would move
     * the disagreement to the Events tab and to the heatmap
     * (pgwt_compute_heatmap_from_summaries reads se->histogram; the matching
     * raw-side exclusion in pgwt_compute_heatmap ships with this change,
     * because handle_heatmap routes both ways).
     *
     * This is the one and only place the drop happens, so there is no second
     * copy of the rule to drift. */
    if (io_worker)
        return 1;

    acc->total_events++;

    /* Time model: classify old_event into wait class.
     *
     * v3: IDLE EVENTS ARE EXCLUDED HERE, at the writer. Before v3 this line
     * had no idle check at all (while ss->db_time_ns below did), so the class
     * totals were LUMPED: Client carried ClientRead and Timeout carried the
     * pacing sleeps, and every read path had to subtract them back out by
     * hand. It subtracted only ClientRead, and only by hunting
     * WEI(PG_WAIT_CLIENT,0) in the events table -- which also meant the
     * subtraction silently did nothing if that event had not claimed a
     * SUMMARY_MAX_EVENTS slot. Excluding at the writer removes the lumping,
     * the three hardcoded copies of the rule, and that failure mode together.
     *
     * The idle time is NOT lost: `events[]` (below) records every event
     * including the idle ones, so readers recover both the idle total and the
     * per-event named Idle sub-rows from there. */
    int cls = summary_wait_class_index(old_ev);
    int ev_idle = pgwt_is_idle_event(old_ev);
    if (cls >= 0 && cls < PGWT_NUM_CLASSES && !ev_idle)
        acc->class_ns[cls] += tns;
    /* v3: the exact system-wide idle total, independent of events[] so a full
     * event table cannot under-report it (see acc->events_overflow). */
    if (ev_idle)
        acc->idle_ns += tns;

    /* v7 (#408): the measured-CPU split, accumulated with the SAME arithmetic
     * pgwt_compute_time_model applies per record, so the reader can reproduce
     * its two CPU rows from these scalars instead of reporting
     * has_measured_cpu = 0 and one combined row.
     *
     * The clamp is per-record and therefore has to happen HERE: cpu_ns comes
     * from sum_exec_runtime, which is only current at a tick or a context
     * switch, so a sub-ms burst can leak into the following interval's delta
     * and make one record's cpu_ns exceed its own wall. Raw clamps each
     * record to its wall and counts the excess (cpu_clamped_ms); clamping a
     * whole second's SUM instead would hide a per-record inconsistency behind
     * the other records' headroom. Idle records are excluded above by the
     * same `ev_idle` rule the raw path applies before its CPU branch -- a
     * relabelled non-command gap (PGWT_WEI_NONCMD_CPU, v7's other half) is
     * idle, so it contributes to neither CPU scalar, exactly as on raw. */
    if (!ev_idle) {
        if (cls == PGWT_CLASS_CPU) {
            /* v9 (#424), piece 4: an interval whose written seconds went to
             * the UNKNOWN tier must not now add its closing cpu_ns to the
             * MEASURED one -- CPU* is measured + unknown, so that would
             * count the credited wall twice. The tail is unknown like its
             * head; the flip itself is counted by the caller. */
            if (cc->cpu_credited && cc->tier_unknown) {
                acc->cpu_unknown_ns += tns;
            } else if (cc->cpu_credited &&
                       evt->cpu_ns != PGWT_CPU_NS_UNKNOWN) {
                /* v9, piece 2: THE CLAMP DOMAIN IS THE WHOLE INTERVAL, not
                 * the remaining second.
                 *
                 * The clamp exists to catch a cpu_ns that exceeds the wall
                 * it was measured over (sum_exec_runtime is only current at
                 * a tick or a context switch, so a sub-ms burst leaks into
                 * the next interval's delta). Once part of the interval has
                 * been credited, `tns` is no longer that wall -- it is only
                 * the tail. Clamping against the tail would throw away
                 * (cpu_ns - credited measurement) every time the credits
                 * covered the whole wall, which is the COMMON case: a gap
                 * whose last tick landed before it closed has tns == 0, and
                 * the entire unplaced measurement would vanish into
                 * cpu_clamped_ns. That is v8's objection with the sign
                 * flipped -- the exact failure this version exists to
                 * prevent, reintroduced by the fix.
                 *
                 * So: clamp against `dur`, then subtract what the open
                 * seconds already carry. The identity
                 *   sum(credits) + measured here + clamped here == cpu_ns
                 * then holds exactly, per interval, which is the
                 * conservation leg of tests/test_open_cpu_accounting.c.
                 *
                 * The remainder can exceed this second's own wall share. At
                 * WINDOW level that is exactly right (both parts are in the
                 * window). At a window EDGE the reader's residual clamp
                 * absorbs it, the same clamp it already applies -- and far
                 * less of it than v7, which put the gap's whole wall AND
                 * whole measurement in this one second. */
                uint64_t m = evt->cpu_ns;
                if (m > dur) {
                    acc->cpu_clamped_ns += m - dur;
                    m = dur;
                }
                m = m > cc->already_cpu_ns ? m - cc->already_cpu_ns : 0;
                acc->cpu_measured_ns += m;
                acc->cpu_measured_records++;
            } else if (evt->cpu_ns != PGWT_CPU_NS_UNKNOWN) {
                uint64_t m = evt->cpu_ns;
                if (m > tns) {
                    acc->cpu_clamped_ns += m - tns;
                    m = tns;
                }
                acc->cpu_measured_ns += m;
                acc->cpu_measured_records++;
            } else {
                /* UNKNOWN (sampled / v2 / no CPU capability): gap-inference,
                 * the whole gap is CPU* and there is no runqueue residual for
                 * it. Kept SEPARATE from cpu_measured_ns so a window mixing
                 * the two tiers still reports the measured part honestly.
                 *
                 * v9 also lands here for the OTHER tier flip -- credits went
                 * to the measured tier and the closing record has no
                 * measurement. The tail genuinely has none, so it is
                 * unknown; CPU* (measured + unknown) still totals at most
                 * the interval's wall, and only the tier attribution is
                 * split. Counted by the caller. */
                acc->cpu_unknown_ns += tns;
            }
        } else if (evt->cpu_ns != PGWT_CPU_NS_UNKNOWN) {
            /* A wait's measured on-CPU spin keeps its WAIT label (the label
             * is the diagnostic); raw counts it only as an observability sum
             * and as evidence that this window CAN split CPU. Both of those
             * are reproduced here. */
            acc->wait_gap_cpu_ns += evt->cpu_ns;
            acc->cpu_measured_records++;
        }
    }

    /* Per-event stats */
    struct pgwt_summary_event *se = find_or_insert_event(acc, old_ev);
    if (!se) {
        /* v4: recorded in the block AND reported once per process. Without
         * the log line the only reader of this counter was a unit test, which
         * made "never a silent drop" an overstatement: the drop was recorded
         * but nothing ever said so. */
        acc->events_overflow++;
        static int warned_overflow;
        if (!warned_overflow) {
            warned_overflow = 1;
            fprintf(stderr, "WARN: summary per-event table full (%d slots) -- "
                    "per-event breakdowns and class/event-FILTERED totals for "
                    "affected seconds are incomplete; unfiltered DB Time and "
                    "Idle totals stay exact (events_overflow in each record)\n",
                    SUMMARY_MAX_EVENTS);
        }
    }
    if (se) {
        se->count++;
        se->total_ns += tns;
        if (dur > se->max_ns) se->max_ns = dur;
        uint32_t bucket = pgwt_duration_to_bucket(dur);
        if (bucket < HISTOGRAM_BUCKETS)
            se->histogram[bucket]++;
    }

    /* Per-session stats */
    struct pgwt_summary_session *ss = find_or_insert_session(acc, evt->pid);
    if (ss) {
        if (!pgwt_is_idle_event(old_ev))
            ss->db_time_ns += tns;
        if (old_ev == 0)
            ss->cpu_ns += tns;
        /* Track top wait per session. v3: never an IDLE event -- "this
         * session's top wait is Timeout:CheckpointWriteDelay" names the thing
         * that is explicitly NOT load, which is the opposite of what the
         * column is for. */
        if (old_ev != 0 && !pgwt_is_idle_event(old_ev) && dur > ss->top_wait_ns) {
            ss->top_wait_id = old_ev;
            ss->top_wait_ns = dur;
        }
    }

    /* Per-query stats */
    accum_query_add(acc, query_id, old_ev, 1, dur, dur);
    return 0;
}

/* ── #128: deferred per-query attribution (query_attr.h) ──────────────────
 * The summary keys per-query stats by the id BPF resolved when the interval
 * closed, so a parse-phase Lock:relation wait (closed before
 * pgstat_report_query_id) was filed under query_id 0 = dropped. Same rule
 * as the live accumulators and pgwt_tag_events: hold the record's
 * per-query part on its pid until the command reports an id (or ends).
 * Resolved aggregates land in the accumulator current at resolution time
 * (at most the wait-to-report delay later — µs for the lock case). */

static struct pgwt_summary_qattr_slot *qattr_slot(struct pgwt_summary_writer *w,
                                                  uint32_t pid)
{
    if (!w->qattr || pid == 0)
        return NULL;
    uint32_t idx = hash32(pid) & (SUMMARY_QATTR_SLOTS - 1);
    for (int i = 0; i < SUMMARY_QATTR_SLOTS; i++) {
        struct pgwt_summary_qattr_slot *s = &w->qattr[idx];
        if (s->pid == pid)
            return s;
        if (s->pid == 0) {
            s->pid = pid;
            memset(&s->q, 0, sizeof(s->q));
            return s;
        }
        idx = (idx + 1) & (SUMMARY_QATTR_SLOTS - 1);
    }
    /* Table full: this record keeps its emission-time id. Never silent —
     * exported as summary_query_attr_table_full_total. */
    w->qattr_table_full_total++;
    return NULL;
}

/* Reclaim a pid's slot on backend exit (linear probing: backward-shift
 * deletion keeps every other pid's probe chain intact), so the table holds
 * LIVE backends only and connection churn cannot exhaust it. */
static void qattr_slot_free(struct pgwt_summary_writer *w,
                            struct pgwt_summary_qattr_slot *s)
{
    uint32_t mask = SUMMARY_QATTR_SLOTS - 1;
    uint32_t hole = (uint32_t)(s - w->qattr);
    uint32_t i = hole;
    /* Bounded: a table with NO empty slot (1024 live pids, or slots whose
     * exits were never seen) has no natural stop — one full lap visits every
     * other entry exactly once (each is moved at most once, so none is
     * duplicated), then the last hole is cleared. */
    for (uint32_t step = 0; step < SUMMARY_QATTR_SLOTS - 1; step++) {
        i = (i + 1) & mask;
        struct pgwt_summary_qattr_slot *c = &w->qattr[i];
        if (c->pid == 0)
            break;
        uint32_t home = hash32(c->pid) & mask;
        /* c may move into the hole iff its home is not in (hole, i]. */
        bool between = hole <= i ? (home > hole && home <= i)
                                 : (home > hole || home <= i);
        if (!between) {
            w->qattr[hole] = *c;
            hole = i;
        }
    }
    memset(&w->qattr[hole], 0, sizeof(w->qattr[hole]));
}

static void summary_qattr_emit(void *ctx, uint64_t query_id, uint32_t we,
                               const struct pgwt_qattr_pending *p,
                               bool backfilled)
{
    struct pgwt_summary_writer *w = ctx;
    (void)backfilled;
    if (query_id == 0) {
        /* The summary's per-query table has no unattributed bucket (the
         * server's fast path reports unattributed_available: false); the
         * time stays in the event/session/class rows. Counted here so it
         * is never silent (summary_query_unattributed_ns_total). */
        w->qattr_unattributed_ns_total += p->total_ns;
        return;
    }
    if (!w->accum_active)
        return;
    accum_query_add(&w->accum, query_id, we, p->count, p->total_ns, p->max_ns);
}

/* Returns the id to file evt's per-query part under NOW (0 = none, or
 * deferred on the pid). A record whose new_event is PGWT_EVENT_EXIT closes
 * the pid: pending resolves by rule 2/3 and the slot is reclaimed. */
static uint64_t summary_qattr_step(struct pgwt_summary_writer *w,
                                   const struct pgwt_trace_event *evt)
{
    uint32_t we = evt->old_event;
    uint64_t qid = 0;
    if (we == PGWT_MARKER_ESCALATE_START || we == PGWT_MARKER_ESCALATE_END)
        return evt->query_id;
    struct pgwt_summary_qattr_slot *s = qattr_slot(w, evt->pid);
    if (!s)
        return evt->query_id;
    if (PGWT_IS_MARKER(we)) {
        if (we == PGWT_MARKER_CMD_START)
            pgwt_qattr_boundary(&s->q, true, summary_qattr_emit, w);
        else if (evt->query_id != 0)
            pgwt_qattr_observe(&s->q, evt->query_id, summary_qattr_emit, w);
        else if (we == PGWT_MARKER_CMD_END)
            pgwt_qattr_boundary(&s->q, false, summary_qattr_emit, w);
    } else if (evt->query_id != 0) {
        pgwt_qattr_observe(&s->q, evt->query_id, summary_qattr_emit, w);
        qid = evt->query_id;
    } else if (pgwt_is_session_idle_event(we)) {
        /* COMMAND BOUNDARY, so deliberately the NARROW predicate (Activity /
         * Client:ClientRead) and not the load rule. A Timeout pacing sleep is
         * excluded from DB Time but happens INSIDE a running command -- a
         * checkpointer in CheckpointWriteDelay is mid-checkpoint. Using the
         * load rule here would flush the pid's deferred per-query attribution
         * as "the command ended" every time an in-command sleep went by.
         * src/idle_rule.h explains why the two predicates differ. */
        pgwt_qattr_boundary(&s->q, false, summary_qattr_emit, w);
    } else {
        pgwt_qattr_defer(&s->q, we, evt->duration_ns, summary_qattr_emit, w);
    }
    if (evt->new_event == PGWT_EVENT_EXIT) {
        pgwt_qattr_boundary(&s->q, true, summary_qattr_emit, w);
        qattr_slot_free(w, s);
    }
    return qid;
}

/* ── Serialization ────────────────────────────────────────── */

size_t pgwt_summary_serialize(const struct pgwt_summary_accum *acc,
                               uint8_t *out, size_t out_size)
{
    uint8_t *p = out;
    /* The buffer is sized from PGWT_SUMMARY_SERIALIZE_MAX, which is the
     * worst case for the compiled-in table bounds -- so this can only fire if
     * a caller passes a smaller buffer. Refusing (0 bytes, no write) is the
     * fail-safe: a short record would deserialize as garbage. out_size was
     * previously ignored outright. */
    if (out_size < PGWT_SUMMARY_SERIALIZE_MAX)
        return 0;

    /* Time model: 11 × 8 = 88 bytes */
    memcpy(p, acc->class_ns, sizeof(acc->class_ns));
    p += sizeof(acc->class_ns);

    /* Per-event entries: only non-empty */
    for (int i = 0; i < SUMMARY_MAX_EVENTS; i++) {
        const struct pgwt_summary_event *e = &acc->events[i];
        /* v9: `|| total_ns` -- see find_or_insert_event. An open on-CPU
         * gap's credit gives the id-0 row time with no count, and skipping
         * it here while acc->num_events counted it desynchronises the
         * block header from the payload. */
        if (e->event_id == 0 && e->count == 0 && e->total_ns == 0) continue;

        memcpy(p, &e->event_id, 4);  p += 4;
        memcpy(p, &e->count, 8);     p += 8;
        memcpy(p, &e->total_ns, 8);  p += 8;
        memcpy(p, &e->max_ns, 8);    p += 8;
        memcpy(p, e->histogram, HISTOGRAM_BUCKETS * 8);
        p += HISTOGRAM_BUCKETS * 8;
        /* 4 + 8 + 8 + 8 + 128 = 156 bytes per event */
    }

    /* Per-session entries: only non-empty */
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++) {
        const struct pgwt_summary_session *s = &acc->sessions[i];
        if (s->pid == 0 && s->db_time_ns == 0) continue;

        memcpy(p, &s->pid, 4);           p += 4;
        memcpy(p, &s->db_time_ns, 8);    p += 8;
        memcpy(p, &s->cpu_ns, 8);        p += 8;
        memcpy(p, &s->top_wait_id, 4);   p += 4;
        memcpy(p, &s->top_wait_ns, 8);   p += 8;
        /* 4 + 8 + 8 + 4 + 8 = 32 bytes per session */
    }

    /* Per-query entries (v2): base 36 + class_ns 88 + 1 + top_events n×20 */
    for (int i = 0; i < SUMMARY_MAX_QUERIES; i++) {
        const struct pgwt_summary_query *q = &acc->queries[i];
        if (q->query_id == 0 && q->count == 0) continue;

        memcpy(p, &q->query_id, 8);      p += 8;
        memcpy(p, &q->count, 8);         p += 8;
        memcpy(p, &q->total_ns, 8);      p += 8;
        memcpy(p, &q->top_wait_id, 4);   p += 4;
        memcpy(p, &q->top_wait_ns, 8);   p += 8;
        /* v2: class_ns */
        memcpy(p, q->class_ns, sizeof(q->class_ns));
        p += sizeof(q->class_ns);
        /* v2: top_events (variable length) */
        uint8_t nte = (uint8_t)q->num_top_events;
        *p = nte;  p += 1;
        for (int j = 0; j < nte; j++) {
            memcpy(p, &q->top_events[j].event_id, 4);   p += 4;
            memcpy(p, &q->top_events[j].count, 8);       p += 8;
            memcpy(p, &q->top_events[j].total_ns, 8);    p += 8;
        }
        /* v4: appended AFTER the variable-length list, so a v2 reader that
         * stops at the list is unaffected and a v3 reader finds it by the
         * same walk. */
        memcpy(p, &q->idle_ns, 8);       p += 8;
    }

    /* v4 record-level trailer, appended last for the same reason. */
    memcpy(p, &acc->idle_ns, 8);          p += 8;
    memcpy(p, &acc->events_overflow, 4);  p += 4;

    /* v7 (#408) measured-CPU trailer, after the v4 one for the same reason:
     * every older reader stops before it, and the version gate (the reader
     * refuses any file that is not PGWT_SUMMARY_VERSION) is what keeps a v6
     * record -- which ends 40 bytes earlier -- from being read as if these
     * fields were there. */
    memcpy(p, &acc->cpu_measured_ns, 8);      p += 8;
    memcpy(p, &acc->cpu_unknown_ns, 8);       p += 8;
    memcpy(p, &acc->cpu_clamped_ns, 8);       p += 8;
    memcpy(p, &acc->wait_gap_cpu_ns, 8);      p += 8;
    memcpy(p, &acc->cpu_measured_records, 8); p += 8;

    return (size_t)(p - out);
}

int pgwt_summary_deserialize(const uint8_t *in, size_t in_size,
                              struct pgwt_summary_accum *acc,
                              int version)
{
    const uint8_t *p = in;
    const uint8_t *end = in + in_size;

    /* Caller sets: acc->second_wall_ns, acc->num_events, acc->num_sessions,
     * acc->num_queries from the block header. Save counts before we iterate,
     * since find_or_insert modifies num_events/sessions/queries. */
    int n_events   = acc->num_events;
    int n_sessions = acc->num_sessions;
    int n_queries  = acc->num_queries;

    /* Reset counters — find_or_insert will re-count */
    acc->num_events   = 0;
    acc->num_sessions = 0;
    acc->num_queries  = 0;

    /* Time model */
    if (p + sizeof(acc->class_ns) > end) return -1;
    memcpy(acc->class_ns, p, sizeof(acc->class_ns));
    p += sizeof(acc->class_ns);

    /* Per-event entries */
    for (int i = 0; i < n_events; i++) {
        if (p + 156 > end) return -1;

        uint32_t event_id;
        memcpy(&event_id, p, 4);  p += 4;

        struct pgwt_summary_event *e = find_or_insert_event(acc, event_id);
        if (!e) { p += 152; continue; }

        memcpy(&e->count, p, 8);     p += 8;
        memcpy(&e->total_ns, p, 8);  p += 8;
        memcpy(&e->max_ns, p, 8);    p += 8;
        memcpy(e->histogram, p, HISTOGRAM_BUCKETS * 8);
        p += HISTOGRAM_BUCKETS * 8;
    }

    /* Per-session entries */
    for (int i = 0; i < n_sessions; i++) {
        if (p + 32 > end) return -1;

        uint32_t pid;
        memcpy(&pid, p, 4);  p += 4;

        struct pgwt_summary_session *s = find_or_insert_session(acc, pid);
        if (!s) { p += 28; continue; }

        memcpy(&s->db_time_ns, p, 8);    p += 8;
        memcpy(&s->cpu_ns, p, 8);        p += 8;
        memcpy(&s->top_wait_id, p, 4);   p += 4;
        memcpy(&s->top_wait_ns, p, 8);   p += 8;
    }

    /* Per-query entries */
    for (int i = 0; i < n_queries; i++) {
        if (p + 36 > end) return -1;

        uint64_t query_id;
        memcpy(&query_id, p, 8);  p += 8;

        struct pgwt_summary_query *q = find_or_insert_query(acc, query_id);
        if (!q) {
            p += 28;  /* skip base fields */
            if (version >= 2) {
                p += sizeof(uint64_t) * PGWT_NUM_CLASSES;  /* class_ns */
                if (p + 1 <= end) {
                    uint8_t nte = *p;  p += 1;
                    p += nte * 20;     /* top_events */
                }
                if (version >= 4)
                    p += 8;            /* idle_ns */
            }
            continue;
        }

        memcpy(&q->count, p, 8);         p += 8;
        memcpy(&q->total_ns, p, 8);      p += 8;
        memcpy(&q->top_wait_id, p, 4);   p += 4;
        memcpy(&q->top_wait_ns, p, 8);   p += 8;

        if (version >= 2) {
            if (p + sizeof(q->class_ns) + 1 > end) return -1;
            memcpy(q->class_ns, p, sizeof(q->class_ns));
            p += sizeof(q->class_ns);
            q->num_top_events = *p;  p += 1;
            if (q->num_top_events > SUMMARY_QUERY_TOP_EVENTS)
                q->num_top_events = SUMMARY_QUERY_TOP_EVENTS;
            for (int j = 0; j < q->num_top_events; j++) {
                if (p + 20 > end) return -1;
                memcpy(&q->top_events[j].event_id, p, 4);   p += 4;
                memcpy(&q->top_events[j].count, p, 8);       p += 8;
                memcpy(&q->top_events[j].total_ns, p, 8);    p += 8;
            }
            if (version >= 4) {
                if (p + 8 > end) return -1;
                memcpy(&q->idle_ns, p, 8);  p += 8;
            }
        }
    }

    if (version >= 4) {
        if (p + 12 > end) return -1;
        memcpy(&acc->idle_ns, p, 8);          p += 8;
        memcpy(&acc->events_overflow, p, 4);  p += 4;
    }

    /* v7 (#408): REFUSE a short record rather than leaving the measured-CPU
     * scalars at 0. A zero here is not neutral -- it is the exact shape of
     * the bug this version fixes (cpu_measured_records == 0 reads as "this
     * window cannot split CPU" and the reader emits one combined CPU* row),
     * so a truncated block must fail to decode and be skipped as a decode
     * error, not silently answer with the v6 behaviour for part of a
     * window. */
    if (version >= 7) {
        if (p + 40 > end) return -1;
        memcpy(&acc->cpu_measured_ns, p, 8);      p += 8;
        memcpy(&acc->cpu_unknown_ns, p, 8);       p += 8;
        memcpy(&acc->cpu_clamped_ns, p, 8);       p += 8;
        memcpy(&acc->wait_gap_cpu_ns, p, 8);      p += 8;
        memcpy(&acc->cpu_measured_records, p, 8); p += 8;
    }

    return 0;
}

/* ── Startup recovery (DUR-1) ─────────────────────────────── */

/* Same contract as the trace writer's recovery: a leftover current.summary
 * is finalized (torn tail dropped, footer appended) and renamed aside to a
 * collision-safe archive — NEVER truncated. */
static void recover_current_summary(struct pgwt_summary_writer *w)
{
    char cur[512], meta[600], meta_tmp[600];
    snprintf(cur, sizeof(cur), "%s/current.summary", w->trace_dir);
    snprintf(meta, sizeof(meta), "%s/current.summary.meta", w->trace_dir);
    snprintf(meta_tmp, sizeof(meta_tmp), "%s/current.summary.meta.tmp",
             w->trace_dir);

    unlink(meta_tmp);   /* torn meta write — always stale */

    struct stat st;
    if (stat(cur, &st) != 0) {
        unlink(meta);
        return;
    }

    int committed = 0;
    {
        FILE *mf = fopen(meta, "r");
        if (mf) {
            if (fscanf(mf, "%d", &committed) != 1)
                committed = 0;
            fclose(mf);
        }
    }

    FILE *fp = fopen(cur, "r+b");
    struct pgwt_trace_file_header hdr;
    if (!fp) {
        fprintf(stderr, "WARN: cannot open %s for recovery: %s "
                "(leaving it in place)\n", cur, strerror(errno));
        return;
    }
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1 ||
        hdr.magic != PGWT_SUMMARY_MAGIC ||
        /* Recovery only has to decide "is this header intact", not "can I
         * COMPUTE from it": accept every version this project has ever
         * written so an intact v2 file left by an older daemon is ARCHIVED
         * normally instead of being renamed .corrupt. Refusing v2 for
         * computation is the reader's job (src/summary_reader.c). */
        hdr.version < 1 || hdr.version > PGWT_SUMMARY_VERSION) {
        fclose(fp);
        char aside[600];
        snprintf(aside, sizeof(aside), "%s.corrupt.%lld", cur,
                 (long long)time(NULL));
        if (rename(cur, aside) == 0)
            fprintf(stderr, "WARN: %s has an unreadable header — preserved "
                    "as %s\n", cur, aside);
        unlink(meta);
        pgwt_fsync_dir(w->trace_dir);
        return;
    }

    long fsize = (long)st.st_size;
    struct pgwt_block_index_entry *idx = NULL;
    int nidx = 0, cap = 0;
    long pos = (long)sizeof(hdr);
    struct pgwt_summary_block_header bh;
    while (nidx < PGWT_MAX_READ_BLOCKS) {
        if (fseek(fp, pos, SEEK_SET) != 0 ||
            fread(&bh, sizeof(bh), 1, fp) != 1)
            break;
        if (bh.compressed_size == 0 || bh.uncompressed_size == 0 ||
            bh.compressed_size > PGWT_MAX_BLOCK_COMPRESSED ||
            bh.uncompressed_size > PGWT_MAX_BLOCK_COMPRESSED)
            break;
        long end = pos + (long)sizeof(bh) + (long)bh.compressed_size;
        if (end > fsize)
            break;   /* torn tail */
        if (nidx >= cap) {
            int newcap = cap ? cap * 2 : 256;
            struct pgwt_block_index_entry *tmp =
                realloc(idx, newcap * sizeof(*tmp));
            if (!tmp)
                break;
            idx = tmp;
            cap = newcap;
        }
        idx[nidx++] = (struct pgwt_block_index_entry){
            .timestamp_ns = bh.wall_ns,
            .file_offset = (uint64_t)pos,
        };
        pos = end;
    }

    if (committed > nidx)
        fprintf(stderr, "ERROR: recovery of %s found %d complete blocks but "
                "the meta high-watermark says %d were committed\n",
                cur, nidx, committed);

    if (nidx == 0) {
        free(idx);
        fclose(fp);
        if (fsize > (long)sizeof(hdr)) {
            char aside[600];
            snprintf(aside, sizeof(aside), "%s.corrupt.%lld", cur,
                     (long long)time(NULL));
            if (rename(cur, aside) == 0)
                fprintf(stderr, "WARN: %s had no complete block — preserved "
                        "as %s\n", cur, aside);
        } else {
            unlink(cur);
        }
        unlink(meta);
        pgwt_fsync_dir(w->trace_dir);
        return;
    }

    int finalize_failed = 0;
    if (ftruncate(fileno(fp), (off_t)pos) != 0)
        finalize_failed = 1;
    if (!finalize_failed && fseek(fp, pos, SEEK_SET) == 0) {
        for (int i = 0; i < nidx && !finalize_failed; i++)
            if (fwrite(&idx[i], sizeof(idx[i]), 1, fp) != 1)
                finalize_failed = 1;
        uint32_t nb = (uint32_t)nidx;
        if (!finalize_failed && fwrite(&nb, sizeof(nb), 1, fp) != 1)
            finalize_failed = 1;
    } else {
        finalize_failed = 1;
    }
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    free(idx);
    if (finalize_failed)
        fprintf(stderr, "WARN: could not append a footer to recovered %s "
                "(%s) — archiving anyway\n", cur, strerror(errno));

    time_t start_sec = (time_t)(hdr.start_time_ns / 1000000000ULL);
    struct tm tm;
    localtime_r(&start_sec, &tm);
    char dest[512];
    if (pgwt_archive_path_nonclobber(w->trace_dir, &tm, ".summary.lz4",
                                     dest, sizeof(dest)) != 0 ||
        rename(cur, dest) != 0) {
        fprintf(stderr, "WARN: cannot archive recovered %s: %s "
                "(leaving it in place; it will NOT be truncated)\n",
                cur, strerror(errno));
        return;
    }
    unlink(meta);
    pgwt_fsync_dir(w->trace_dir);
    fprintf(stderr, "INFO: recovered previous current.summary: %d blocks "
            "-> %s\n", nidx, dest);
}

/* ── File operations ──────────────────────────────────────── */

static int write_footer(struct pgwt_summary_writer *w)
{
    for (int i = 0; i < w->num_blocks; i++) {
        if (fwrite(&w->block_index[i], sizeof(w->block_index[i]), 1, w->fp) != 1)
            return -1;
    }
    uint32_t nb = (uint32_t)w->num_blocks;
    if (fwrite(&nb, sizeof(nb), 1, w->fp) != 1)
        return -1;
    return 0;
}

static int open_summary_file(struct pgwt_summary_writer *w)
{
    snprintf(w->current_path, sizeof(w->current_path),
             "%s/current.summary", w->trace_dir);

    /* DUR-1: never truncate an existing current.summary ("wbx"); recover
     * and retry if one (re)appeared after init. */
    w->fp = fopen(w->current_path, "wbxe");
    if (!w->fp && errno == EEXIST) {
        recover_current_summary(w);
        w->fp = fopen(w->current_path, "wbxe");
    }
    if (!w->fp) {
        fprintf(stderr, "WARN: cannot create %s: %s\n",
                w->current_path, strerror(errno));
        return -1;
    }

    /* Set group ownership and permissions */
    if (w->trace_gid != (gid_t)-1) {
        fchown(fileno(w->fp), (uid_t)-1, w->trace_gid);
        fchmod(fileno(w->fp), 0640);
    }

    /* Capture wall-clock and monotonic timestamps */
    struct timespec wall, mono;
    clock_gettime(CLOCK_REALTIME, &wall);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    /* An offline producer may have PINNED the anchor to a synthetic
     * timestamp domain before the first push (pgwt_summary_pin_clock). It
     * has to be able to: the file opens lazily on the first record, and v8
     * can write a per-second record for a second EARLIER than that record
     * (the seconds a long interval was already open in). Fixing the offsets
     * up afterwards, as the generator used to, is too late for those. The
     * daemon never pins, so its behaviour here is unchanged. */
    if (!w->clock_anchor_pinned) {
        w->clock_offset_wall_ns =
            (uint64_t)wall.tv_sec * 1000000000ULL + wall.tv_nsec;
        w->clock_offset_mono_ns =
            (uint64_t)mono.tv_sec * 1000000000ULL + mono.tv_nsec;
    }

    /* Record current hour for rotation */
    time_t now = wall.tv_sec;
    struct tm tm;
    localtime_r(&now, &tm);
    w->current_hour = tm.tm_yday * 24 + tm.tm_hour;

    /* Write file header (same struct as trace files, different magic) */
    struct pgwt_trace_file_header hdr = {
        .magic = PGWT_SUMMARY_MAGIC,
        .version = PGWT_SUMMARY_VERSION,
        .flags = PGWT_FLAG_LZ4,
        .pg_version = 0,
        .start_time_ns = w->clock_offset_wall_ns,
        .clock_offset_ns = w->clock_offset_mono_ns,
    };
    if (fwrite(&hdr, sizeof(hdr), 1, w->fp) != 1) {
        fprintf(stderr, "WARN: cannot write summary file header: %s\n",
                strerror(errno));
        fclose(w->fp);
        w->fp = NULL;
        return -1;
    }

    w->num_blocks = 0;
    w->file_ever_opened = true;   /* #430: the clock anchor now exists */
    /* #447: what this file's header currently CLAIMS, which widen_file_start
     * may move earlier as records older than the open instant arrive. Kept
     * apart from the writer's own anchor above on purpose -- see
     * widen_file_start. */
    w->file_start_wall_ns = hdr.start_time_ns;
    w->file_start_mono_ns = hdr.clock_offset_ns;
    return 0;
}

/* Convert monotonic timestamp to wall-clock using clock offsets. */
static uint64_t mono_to_wall(const struct pgwt_summary_writer *w,
                              uint64_t mono_ns)
{
    return w->clock_offset_wall_ns + (mono_ns - w->clock_offset_mono_ns);
}

/* #447: WIDEN THIS FILE'S HEADER START so it covers a record older than the
 * header says the file begins.
 *
 * pgwt_scan_summary_files places a file by its header's start_time_ns and
 * summary_file_in_window prunes any file whose start is after the requested
 * window's end. A file's header is written when the file is OPENED, but the
 * records written into it can be OLDER than that instant:
 *
 *   - the lazy open happens on the first PUSH, and an event's second began up
 *     to a second before it arrived;
 *   - #430's seal reopen happens on a TICK, and the second it seals completed
 *     PGWT_SUMMARY_FLUSH_LAG_NS + 1 s earlier -- so every reopened file's
 *     earliest record is ~1.1 s before its own header.
 *
 * Without this, #430's recovered seconds are written and still unreachable: a
 * reopen at 10:00:07.2 writing the owed 10:00:06 second is pruned out of a
 * query for [10:00:06, 10:00:07), which is how every view reads. Crediting
 * them would have been pointless.
 *
 * BOTH header fields move by the SAME delta, because the reader derives
 * `mono_to_wall = start_time_ns - clock_offset_ns` (src/summary_reader.c) and
 * that difference must not change. Widening only ever moves the start
 * EARLIER, which only widens coverage -- it can never hide a record, so a
 * reader part-way through a live current.summary cannot be made to miss
 * anything by it.
 *
 * The WRITER's own anchor (w->clock_offset_wall_ns / _mono_ns) is deliberately
 * NOT touched, which is why the header's values are tracked separately in
 * file_start_wall_ns / file_start_mono_ns. Moving the writer's anchor would
 * have changed two unrelated things with it: mono_to_wall(), so every later
 * record in the file would be stamped differently from the earlier ones; and
 * the rotation's archive NAME, which is derived from clock_offset_wall_ns and
 * would have jumped back into the previous hour for every reopened file.
 *
 * It cannot make the file's span exceed the hour the reader assumes: the
 * earliest record a reopened file can hold is the second before the rotation
 * boundary that created it, and the latest is the second before the NEXT
 * boundary, so the span is at most one hour and summary_file_in_window's end
 * test (a strict `<`) still includes it. The one way to exceed that is a
 * flush that failed an hour earlier and left older seconds owed, which
 * flush_failures_total already counts.
 *
 * Called from flush_accum, which is the single funnel every record passes
 * through -- so this holds for every path (push, seal, rotation) rather than
 * for the one that prompted it. */
static void widen_file_start(struct pgwt_summary_writer *w, uint64_t wall_ns)
{
    if (!w->fp || wall_ns == 0 || wall_ns >= w->file_start_wall_ns)
        return;
    uint64_t delta = w->file_start_wall_ns - wall_ns;
    /* clock_offset_ns is CLOCK_MONOTONIC, i.e. small just after boot. Refuse
     * rather than wrap: a wrapped anchor is a wrong answer everywhere, where
     * a refusal is one file placed conservatively late and counted. */
    if (delta > w->file_start_mono_ns) {
        w->file_start_widen_failures_total++;
        return;
    }
    uint64_t new_start = wall_ns;
    uint64_t new_mono  = w->file_start_mono_ns - delta;
    long pos = ftell(w->fp);
    if (pos < 0 ||
        fseek(w->fp, (long)offsetof(struct pgwt_trace_file_header,
                                    start_time_ns), SEEK_SET) != 0 ||
        fwrite(&new_start, sizeof(new_start), 1, w->fp) != 1 ||
        fwrite(&new_mono, sizeof(new_mono), 1, w->fp) != 1 ||
        fseek(w->fp, pos, SEEK_SET) != 0) {
        /* The header may now be half-written, so the file must not be
         * trusted to place itself: leave file_start_wall_ns alone (so a
         * later record retries) and count it. pgwt_scan_summary_files falls
         * back to the NAME for an archived file and
         * pgwt_summaries_window_current counts a current.summary it cannot
         * place as unusable, which becomes a raw recompute -- never a
         * silent partial window. */
        w->file_start_widen_failures_total++;
        fprintf(stderr, "WARN: could not widen %s's header start to cover a "
                "record %llu ns older than it: %s\n", w->current_path,
                (unsigned long long)delta, strerror(errno));
        return;
    }
    w->file_start_mono_ns = new_mono;
    w->file_start_wall_ns = new_start;
    w->file_start_widened_total++;
}

/* Flush current accumulator as one LZ4-compressed block. */
static int flush_accum(struct pgwt_summary_writer *w)
{
    if (!w->accum_active || !w->fp)
        return 0;

    struct pgwt_summary_accum *acc = &w->accum;
    uint64_t flushed_second_mono_ns = acc->second_mono_ns;

    /* #447: before anything is written, make sure the file's header claims a
     * start early enough for the reader to find this record. Done here, at
     * the single point every record is written, so no path can bypass it --
     * and before the ftell below, whose offset must be the real one. */
    widen_file_start(w, acc->second_wall_ns);

    /* Serialize */
    size_t encoded_size = pgwt_summary_serialize(acc, w->encode_buf,
                                                  w->encode_buf_size);
    if (encoded_size == 0) {
        /* The serializer refused (buffer smaller than
         * PGWT_SUMMARY_SERIALIZE_MAX). Unreachable with the writer's own
         * buffer, but it must not fall through: LZ4_compress_default() on 0
         * bytes returns 1, so this used to write a block whose header still
         * claimed the record's event/session/query counts over an empty
         * payload -- which the reader turns into a silently dropped second.
         * Refuse loudly and leave the accumulator open so the second is
         * retried rather than lost. */
        fprintf(stderr, "ERROR: summary serialize refused (buffer %zu < "
                "required %zu) -- second not written, accumulator kept\n",
                w->encode_buf_size, (size_t)PGWT_SUMMARY_SERIALIZE_MAX);
        return -1;
    }

    /* LZ4 compress */
    int compressed_size = LZ4_compress_default(
        (const char *)w->encode_buf, (char *)w->compress_buf,
        (int)encoded_size, (int)w->compress_buf_size);
    if (compressed_size <= 0) {
        fprintf(stderr, "WARN: summary LZ4 compression failed\n");
        /* Nothing reached the disk, so there is no duplicate to avoid here:
         * leave the accumulator open and let the real second boundary (or
         * the next tick) write it, rather than dropping the second. Also
         * leaves last_flushed alone -- that second has not been written.
         * Unreachable in practice: compress_buf is LZ4_compressBound(
         * encode_buf_size) and encoded_size never exceeds encode_buf_size. */
        return -1;
    }

    /* Grow block index if needed */
    if (w->num_blocks >= w->block_index_cap) {
        int new_cap = w->block_index_cap * 2;
        struct pgwt_block_index_entry *new_idx =
            realloc(w->block_index, new_cap * sizeof(*new_idx));
        if (!new_idx) {
            fprintf(stderr, "WARN: cannot grow summary block index\n");
            w->enabled = false;
            return -1;
        }
        w->block_index = new_idx;
        w->block_index_cap = new_cap;
    }

    /* Record block index entry */
    long file_offset = ftell(w->fp);
    uint64_t wall_ns = mono_to_wall(w, acc->second_mono_ns);
    w->block_index[w->num_blocks++] = (struct pgwt_block_index_entry){
        .timestamp_ns = wall_ns,
        .file_offset = (uint64_t)file_offset,
    };

    /* Write block header */
    struct pgwt_summary_block_header bh = {
        .wall_ns = wall_ns,
        .num_events = (uint32_t)acc->num_events,
        .num_sessions = (uint16_t)acc->num_sessions,
        .num_queries = (uint16_t)acc->num_queries,
        .compressed_size = (uint32_t)compressed_size,
        .uncompressed_size = (uint32_t)encoded_size,
    };
    if (fwrite(&bh, sizeof(bh), 1, w->fp) != 1 ||
        fwrite(w->compress_buf, 1, compressed_size, w->fp) != (size_t)compressed_size) {
        fprintf(stderr, "WARN: summary file write failed: %s\n", strerror(errno));
        w->enabled = false;
        return -1;
    }

    w->total_records_written++;
    w->total_bytes_written += sizeof(bh) + compressed_size;
    /* Roll the per-record overflow count up to the writer so the control
     * socket's `metrics` can report it; the per-record field stays, because a
     * reader needs to know WHICH seconds are affected.
     *
     * This has to happen AFTER every early return above, not before them: a
     * failed flush leaves the accumulator open to be retried, so rolling up
     * first counted the same acc->events_overflow again on each attempt.
     * Diagnostic only and bounded by the retry count, but a self-check that
     * inflates under failure is worse than useless -- it reads as a second,
     * independent problem. Placed with the other success-side counters, and
     * before accum_close() clears the accumulator.
     *
     * CONSEQUENCE, stated rather than glossed: a second that is LOST never has
     * its overflow rolled up. The rotate path discards a failed second
     * deliberately (see pgwt_summary_maybe_rotate) and pgwt_summary_close()
     * logs and writes the footer, so neither reaches here. So
     * events_overflow_total counts dropped events in WRITTEN seconds only; it
     * is not a complete total, and flush_failures_total is the signal for the
     * seconds it cannot speak for. The two must be read together. */
    w->events_overflow_total += acc->events_overflow;
    /* This second is now on disk and immutable (#277). */
    w->last_flushed_second_mono_ns = flushed_second_mono_ns;
    w->have_flushed_second = true;
    accum_close(w);

    /* Flush + committed block count (same pattern as event_writer) */
    fflush(w->fp);
    {
        char meta_path[512], tmp_path[512];
        snprintf(meta_path, sizeof(meta_path), "%s/current.summary.meta",
                 w->trace_dir);
        snprintf(tmp_path, sizeof(tmp_path), "%s/current.summary.meta.tmp",
                 w->trace_dir);
        FILE *mf = fopen(tmp_path, "w");
        if (mf) {
            fprintf(mf, "%d\n", w->num_blocks);
            fclose(mf);
            rename(tmp_path, meta_path);
        }
    }

    return 0;
}

/* ── v8 (#379): per-second credit of OPEN intervals ───────────────────────
 * See the v8 note in summary_writer.h for the rule and for everything this
 * deliberately does NOT spread. The code below is only the mechanism:
 *   open_slot()           find/create a pid's watermark entry
 *   accum_open_credit()   put one second's share into one second's record
 *   credit_open_second()  do that for every fresh note, for one second
 *   writer_seal_through() seal every second strictly before a target,
 *                         materialising open-credit-only records for the
 *                         seconds in which nothing closed
 * Every refusal and every nanosecond that could not be placed is counted. */

#define PGWT_NS_PER_SEC 1000000000ULL

static struct pgwt_summary_open *open_slot(struct pgwt_summary_writer *w,
                                           uint32_t pid, bool create)
{
    if (!w->open || pid == 0)
        return NULL;
    uint32_t h = hash32(pid) & (SUMMARY_MAX_OPEN - 1);
    for (int i = 0; i < SUMMARY_MAX_OPEN; i++) {
        struct pgwt_summary_open *o =
            &w->open[(h + (uint32_t)i) & (SUMMARY_MAX_OPEN - 1)];
        if (o->pid == pid)
            return o;
        if (o->pid == 0) {
            if (!create)
                return NULL;
            o->pid = pid;
            return o;
        }
    }
    if (create)
        w->open_table_full_total++;
    return NULL;
}

/* One open interval's share of ONE second, into that second's accumulator.
 *
 * TIME ONLY, and only the four accumulators a window clips: class_ns,
 * idle_ns, events[].total_ns and sessions[].db_time_ns. No count, no
 * histogram bucket, no max_ns, no top_wait, no queries[] -- those are
 * per-OCCURRENCE and are recorded once, whole, when the interval closes
 * (summary_writer.h's v8 note says why for each). io_worker intervals never
 * reach here: pgwt_summary_note_open_ex refuses them.
 *
 * v9 (#424): on-CPU gaps DO reach here now, with `we` set to the interval's
 * FROZEN effective id -- 0 (CPU) or PGWT_WEI_NONCMD_CPU (idle). The two CPU
 * SCALARS are not touched here: they need the per-interval measurement
 * bookkeeping and are handled by the caller, which is the only place that
 * has it. The one addition is sessions[].cpu_ns, which accum_event also
 * fills for a we == 0 record and which the Sessions view reads. */
static void accum_open_credit(struct pgwt_summary_accum *acc, uint32_t pid,
                              uint32_t we, uint64_t ns)
{
    if (ns == 0)
        return;
    int cls = summary_wait_class_index(we);
    int ev_idle = pgwt_is_idle_event(we);
    if (ev_idle)
        acc->idle_ns += ns;
    else if (cls >= 0 && cls < PGWT_NUM_CLASSES)
        acc->class_ns[cls] += ns;

    struct pgwt_summary_event *se = find_or_insert_event(acc, we);
    if (se)
        se->total_ns += ns;
    else
        acc->events_overflow++;

    if (!ev_idle) {
        struct pgwt_summary_session *ss = find_or_insert_session(acc, pid);
        if (ss) {
            ss->db_time_ns += ns;
            if (we == 0)
                ss->cpu_ns += ns;
        }
    }
}

/* Is this slot describing something the writer may credit right now?
 *
 * v9 (#424): `we == 0` is BOTH "nothing creditable" (v8's sentinel) and the
 * raw id of an on-CPU gap, which v9 does credit -- so every loop asks this
 * ONE predicate instead of testing `o->we` and leaving the new state
 * half-handled wherever someone forgot. */
static inline bool open_creditable(const struct pgwt_summary_open *o)
{
    return o->pid != 0 && (o->we != 0 || o->cpu_slot);
}

/* Stop crediting this slot: the note expired, the interval closed, or the
 * producer withdrew it. The per-PID watermark deliberately SURVIVES (it is
 * what keeps a later record from double counting); only the description of
 * what is open is cleared. */
static void open_stop_crediting(struct pgwt_summary_open *o)
{
    o->we = 0;
    o->cpu_slot = 0;
    o->cpu_frozen = 0;
    o->cpu_tier_known = 0;
    o->cpu_eff_we = 0;
    o->cpu_prefix_ns = PGWT_CPU_NS_UNKNOWN;
    o->cpu_credited_ns = 0;
}

/* The measured-CPU half of one per-second credit of an open on-CPU gap.
 *
 * The producer's observation is a PREFIX -- measured CPU over
 * [o->start_ns, its last note) -- while the credit is a SLICE of one
 * completed second, and the note is always NEWER than the second being
 * sealed. So the quantity credited is the DIFFERENCE of two cumulative
 * numbers, capped by THIS SECOND'S OWN WALL:
 *
 *     credit = min(prefix - already credited, slice)
 *
 * The cap is what makes this safe. Without it the prefix's extra CPU --
 * burned in the part of the gap that is NOT yet sealed -- lands in a second
 * whose own wall cannot hold it. MEASURED on the rotation fixture in
 * tests/test_open_cpu_accounting.c with the cap removed: one second carried
 * 2480 ms of measured CPU inside 1000 ms of wall, which makes the reader's
 * DB - CPU* - Sigma waits residual NEGATIVE for that second and surfaces to
 * a user as phantom clamp. With the cap the same 3000 ms spreads
 * 1000/1000/1000 across the three seconds that can hold it and the window
 * total is unchanged. Several seconds ARE sealed in one pass against one
 * much newer observation in practice: after an hourly rotation the file
 * reopens lazily on the next pushed record, so the ticks in between seal
 * nothing.
 *
 * The excess is CARRIED, never clamped away, and the carry always drains:
 * each second releases up to its own wall, and the producer clamps its
 * prefix to the prefix's own wall, so the prefix can never outrun the sum
 * of the slices by more than the not-yet-sealed tail -- which the closing
 * record takes. Hence sum(credits) <= prefix and
 * sum(credits) <= wall credited, both without a second cap: a per-slice
 * bound implies the cumulative one.
 *
 * Returns the ns credited; 0 for the UNKNOWN tier, which the caller files
 * in cpu_unknown_ns instead. */
static uint64_t credit_open_cpu_measured(struct pgwt_summary_accum *acc,
                                         struct pgwt_summary_open *o,
                                         uint64_t slice)
{
    if (o->cpu_prefix_ns == PGWT_CPU_NS_UNKNOWN)
        return 0;
    if (o->cpu_prefix_ns <= o->cpu_credited_ns)
        return 0;                       /* non-monotone observation: carry */
    uint64_t add = o->cpu_prefix_ns - o->cpu_credited_ns;
    if (add > slice)
        add = slice;                    /* this second's own wall: carry */
    o->cpu_credited_ns += add;
    acc->cpu_measured_ns += add;
    return add;
}

/* Credit [sec, sec+1s) of every creditable open interval into the
 * accumulator, which the caller guarantees is the record for `sec`. */
static void credit_open_second(struct pgwt_summary_writer *w, uint64_t sec)
{
    if (!w->open)
        return;
    uint64_t end = sec + PGWT_NS_PER_SEC;
    for (int i = 0; i < SUMMARY_MAX_OPEN; i++) {
        struct pgwt_summary_open *o = &w->open[i];
        if (!open_creditable(o))
            continue;
        /* FAIL-SAFE (the one way this could fabricate time): a note the
         * producer stopped refreshing describes a pid that may no longer
         * exist. Stop crediting it, count the expiry, and keep the pid's
         * watermark so a later record still cannot double count. */
        if (o->noted_ns + PGWT_SUMMARY_OPEN_STALE_NS < end) {
            open_stop_crediting(o);
            w->open_notes_expired_total++;
            continue;
        }
        uint64_t from = o->start_ns > o->credited_until_ns
                      ? o->start_ns : o->credited_until_ns;
        if (from >= end)
            continue;
        if (from < sec) {
            /* The seconds before this one are already on disk and immutable
             * (#277), so that part cannot be credited to the seconds it
             * happened in. It is NOT lost and NOT silently dropped: it stays
             * un-credited, so the closing record still carries it (the v7
             * answer for that slice), and it is counted here because a
             * producer that notes an interval late is exactly the condition
             * under which v8 degrades towards v7. */
            w->open_credit_late_ns_total += sec - from;
            from = sec;
        }
        uint64_t slice = end - from;
        if (o->cpu_slot) {
            /* v9 (#424), piece 3: FREEZE the classification and the
             * measurement tier at the FIRST second this interval is written
             * into. Up to this instant the producer's answer was still free
             * to change (it is a majority over a growing prefix); from here
             * it is on disk and immutable (#277), so every later credit and
             * the closing record must use the same one or one interval's
             * seconds contradict each other. */
            if (!o->cpu_frozen) {
                o->cpu_frozen = 1;
                o->cpu_tier_known =
                    (o->cpu_prefix_ns != PGWT_CPU_NS_UNKNOWN) ? 1 : 0;
            }
            accum_open_credit(&w->accum, o->pid, o->cpu_eff_we, slice);
            w->open_cpu_credited_ns_total += slice;
            /* An idle-classified gap (PGWT_WEI_NONCMD_CPU) contributes to
             * NEITHER CPU scalar, exactly as accum_event's `ev_idle` guard
             * does for a closed one: the raw paths file post/between-command
             * time as Idle, not as CPU. */
            if (!pgwt_is_idle_event(o->cpu_eff_we)) {
                if (o->cpu_tier_known) {
                    uint64_t m = credit_open_cpu_measured(&w->accum, o,
                                                          slice);
                    w->open_cpu_measured_ns_total += m;
                    /* A CPU OBSERVATION, which is what this counter means
                     * since v9: without it a window whose only CPU time is
                     * one still-open gap reports has_measured_cpu = 0 and
                     * the reader discards the split it was just handed. */
                    w->accum.cpu_measured_records++;
                } else {
                    w->accum.cpu_unknown_ns += slice;
                    w->open_cpu_unknown_ns_total += slice;
                }
            }
        } else {
            accum_open_credit(&w->accum, o->pid, o->we, slice);
        }
        w->open_credited_ns_total += slice;
        o->credited_ns += slice;
        o->credited_until_ns = end;
    }
}

/* The earliest second any fresh open interval still owes credit for, or
 * UINT64_MAX if nothing is open. Only consulted when no accumulator exists,
 * i.e. when a whole second passed with no event at all. */
static uint64_t earliest_open_second(struct pgwt_summary_writer *w,
                                     uint64_t now_hint)
{
    uint64_t best = UINT64_MAX;
    if (!w->open)
        return best;
    for (int i = 0; i < SUMMARY_MAX_OPEN; i++) {
        struct pgwt_summary_open *o = &w->open[i];
        if (!open_creditable(o))
            continue;
        if (o->noted_ns + PGWT_SUMMARY_OPEN_STALE_NS < now_hint) {
            /* The SAME fail-safe credit_open_second applies, retired here
             * too -- and counted here too. Without this, a note that goes
             * stale while no accumulator is open is silently skipped
             * forever and open_notes_expired_total reads 0: the fail-safe
             * would be working and invisible, which is the state in which
             * nobody notices it stop working. */
            open_stop_crediting(o);
            w->open_notes_expired_total++;
            continue;
        }
        uint64_t from = o->start_ns > o->credited_until_ns
                      ? o->start_ns : o->credited_until_ns;
        uint64_t sec = (from / PGWT_NS_PER_SEC) * PGWT_NS_PER_SEC;
        if (sec < best)
            best = sec;
    }
    return best;
}

/* Can the seal write RIGHT NOW, reopening the file if that is all that is
 * missing? (#430)
 *
 * `w->fp == NULL` used to be read as one state and it is two:
 *
 *   NEVER ANCHORED -- no file has ever been opened and no offline producer
 *     pinned the anchor, so clock_offset_* are zero and mono_to_wall would
 *     stamp every record with garbage. Here the refusal is right, and it is
 *     the only reason the original guard existed.
 *
 *   ROTATED -- a file WAS open, pgwt_summary_check_rotation archived it and
 *     left fp NULL on purpose so open_summary_file fills it in lazily on the
 *     next PUSHED record. The clocks are anchored and the open table, the
 *     per-pid watermarks and the per-interval freeze all live on the writer,
 *     not on the file. Refusing here made every tick in that window seal
 *     nothing: crediting paused until a record happened to arrive, and if
 *     the daemon stopped first (pgwt_summary_close also returns early on a
 *     NULL fp) those seconds were never written at all.
 *
 * `file_ever_opened` separates them, and `current_hour` deliberately does
 * NOT -- see the field's comment in summary_writer.h for why the obvious
 * choice is the wrong one.
 *
 * This is called only once the caller KNOWS it has a second to write, so a
 * quiet writer with nothing open never creates a file -- the empty-record
 * refusal above it is unchanged. A failed reopen is COUNTED and credits
 * nothing: credited_until_ns is not advanced, so the owed seconds are still
 * owed and a later successful seal places them.
 *
 * It deliberately does NOT do what the push path does on an open failure --
 * pgwt_summary_push_event sets w->enabled = false and gives up. A seal runs
 * once per tick, so a disk that is full for a minute would permanently
 * disable summaries for the rest of the capture; instead this retries each
 * tick, and because nothing is advanced the retry is lossless. The cost is
 * open_summary_file's own WARN once a second while the condition lasts,
 * which is the loudness this project wants for a write it could not do, and
 * it stops the instant the reopen succeeds. */
/* Open the file if it is not open, and COUNT the outcome. Every caller that
 * can open a summary file goes through here, so there is no path that stops
 * recording without moving a number.
 *
 * It deliberately does not care WHO is asking -- what it counts is which of
 * the two opens this is. A FIRST open failing means the capture never
 * produced a summary file at all; a REOPEN failing means the file existed, a
 * rotation closed it, and recording has stopped mid-capture with seconds
 * still owed. The second is the one #430 cares about and the one that used to
 * be invisible: pgwt_summary_push_event kept its own open, so a rotation
 * followed by an event arriving before the next seal tick disabled the
 * writer with NEITHER seal counter moving, the later ticks returned in
 * silence, and the daemon discards the push's return value. There was a
 * warning on stderr and no number anywhere. */
static bool writer_open_counted(struct pgwt_summary_writer *w)
{
    if (w->fp)
        return true;
    bool reopen = w->file_ever_opened;
    if (open_summary_file(w) != 0) {
        if (reopen)
            w->open_seal_reopen_failures_total++;
        else
            w->first_open_failures_total++;
        return false;
    }
    if (reopen)
        w->open_seal_reopens_total++;
    return true;
}

static bool seal_can_write(struct pgwt_summary_writer *w)
{
    if (w->fp)
        return true;
    if (!w->file_ever_opened)
        return false;      /* clocks never anchored: invent nothing */
    return writer_open_counted(w);
}

/* Seal every second STRICTLY BEFORE `to_sec`: credit the open intervals into
 * it, write it, and open the next one if anything is still open there.
 *
 * This replaces the bare second-boundary flush. The loop is what makes a
 * long interval appear in the seconds it spans even when no event closes in
 * them -- without it a 100 s ClientRead on an otherwise quiet system would
 * still land whole in its closing second, because nothing would have written
 * the 99 seconds before it.
 *
 * #430: there is deliberately NO `if (!w->fp) return` here. That line was the
 * defect, and an early-out spelling the same refusal a second time would be
 * worse than the defect: the refusal would then live in two places, the one
 * in seal_can_write would be unreachable, and the bypass leg written to prove
 * the unanchored writer invents nothing would be satisfied by this line while
 * testing none of the logic it names. MEASURED: with that early-out present,
 * deleting seal_can_write's refusal outright left
 * tests/test_rotation_open_credit.c §3 fully GREEN -- a dead gate. One
 * refusal, one site. The loop below reaches seal_can_write only once it knows
 * it has a second to write, so a writer with nothing open still creates no
 * file and no record. */
static void writer_seal_through(struct pgwt_summary_writer *w, uint64_t to_sec)
{
    int guard = PGWT_SUMMARY_OPEN_FILL_MAX_SECONDS;
    for (;;) {
        uint64_t sec;
        if (w->accum_active) {
            sec = w->accum.second_mono_ns;
            if (sec >= to_sec)
                return;
            /* An ACTIVE accumulator with no file should not be reachable (the
             * rotation closes the accumulator before it closes the file, and
             * the push path opens the file first), but if it ever is, the old
             * code dropped the second in silence: flush_accum returns 0 --
             * SUCCESS -- when w->fp is NULL. Reopening here makes the
             * unreachable case correct instead of quietly lossy. */
            if (!seal_can_write(w))
                return;
        } else {
            sec = earliest_open_second(w, to_sec);
            if (sec >= to_sec)
                return;    /* nothing open: no empty record is invented */
            if (w->have_flushed_second &&
                sec <= w->last_flushed_second_mono_ns)
                sec = w->last_flushed_second_mono_ns + PGWT_NS_PER_SEC;
            if (sec >= to_sec)
                return;
            /* BEFORE second_wall_ns is derived: open_summary_file re-reads
             * the clock anchor (unless an offline producer pinned it), and
             * the record's wall must come from the anchor of the file it is
             * actually written into. */
            if (!seal_can_write(w))
                return;
            w->accum.second_mono_ns = sec;
            w->accum.second_wall_ns = mono_to_wall(w, sec);
            w->accum_active = true;
            w->open_credit_records_total++;
        }
        credit_open_second(w, sec);
        if (flush_accum(w) != 0) {
            /* The accumulator stays open and is retried at the next
             * boundary (flush_accum's contract). Returning rather than
             * spinning on a failing write is the difference between a
             * counted gap and a hang. */
            w->flush_failures_total++;
            return;
        }
        if (--guard <= 0) {
            /* Not reached by a producer that ticks; a clock jump or a
             * stopped daemon can. Give up on the span rather than spend
             * unbounded time in one call, and make the give-up visible. */
            w->open_fill_capped_total++;
            return;
        }
    }
}

void pgwt_summary_note_open(struct pgwt_summary_writer *w,
                             uint32_t pid, uint32_t we,
                             uint64_t start_mono_ns, uint32_t cat_flag,
                             uint64_t now_mono_ns)
{
    /* The v8 signature IS "I have no CPU observation". Not a deprecated
     * shim: the lightweight and sampled tiers, and every offline producer
     * driving the writer from a record stream, genuinely have none, and for
     * them the v8 refusal is the correct answer rather than merely the old
     * one. */
    pgwt_summary_note_open_ex(w, pid, we, start_mono_ns, cat_flag,
                              now_mono_ns, NULL);
}

void pgwt_summary_note_open_ex(struct pgwt_summary_writer *w,
                                uint32_t pid, uint32_t we,
                                uint64_t start_mono_ns, uint32_t cat_flag,
                                uint64_t now_mono_ns,
                                const struct pgwt_summary_open_cpu *cpu)
{
    if (!w->enabled || !w->open || pid == 0)
        return;
    /* v6's rule, on the open path: io_worker time is outside the model, so
     * there is nothing to spread. Refused at the note rather than at the
     * credit so the refusal is attributable to a pid. */
    if (cat_flag & PGWT_EVENT_FLAG_IO_WORKER) {
        w->open_io_worker_refused_total++;
        struct pgwt_summary_open *ex = open_slot(w, pid, false);
        if (ex)
            open_stop_crediting(ex);
        return;
    }
    struct pgwt_summary_open *o = open_slot(w, pid, true);
    if (!o)
        return;            /* table full, already counted */
    o->noted_ns = now_mono_ns;
    o->cat_flag = cat_flag;
    if (we == 0 || summary_wait_class_index(we) == PGWT_CLASS_CPU) {
        /* AN ON-CPU GAP.
         *
         * WITHOUT an observation this is v8 exactly: not spread, because
         * the record carries its measured cpu_ns only at close and
         * crediting the wall alone would clamp the whole measurement away
         * and destroy v7's split. The wall that stays unspread is measured
         * at close (cpu_open_uncredited_ns_total), so the residual is a
         * number rather than a belief, and the pid's watermark is left
         * alone.
         *
         * WITH one (v9, #424) the obstacle is gone: the producer has
         * already measured the open prefix, so the writer can place wall
         * AND measurement second by second. */
        if (!cpu) {
            open_stop_crediting(o);
            o->start_ns = start_mono_ns;
            w->open_cpu_refused_total++;
            return;
        }
        /* v7's rule on the open path: the ONLY relabel this parameter may
         * express is a we == 0 gap becoming PGWT_WEI_NONCMD_CPU. Anything
         * else is a producer bug; accepting it would let a caller refile
         * arbitrary time into an arbitrary class through plumbing. Refuse
         * to the RAW id and count, exactly as push_event does. */
        uint32_t eff = cpu->eff_we;
        if (eff != 0 && eff != PGWT_WEI_NONCMD_CPU) {
            eff = 0;
            w->open_cpu_eff_rejected_total++;
        }
        if (!o->cpu_slot || o->we != 0 || o->start_ns != start_mono_ns) {
            /* A DIFFERENT interval. Same pre-charge as the wait path below:
             * any part of this interval's span the pid's monotone watermark
             * already credited (under the PREVIOUS interval) is charged
             * against this one's wall remainder, so the at-close
             * subtraction stays exact in both tick/close orders. The
             * MEASURED side needs no pre-charge: its credit is bounded per
             * second by that second's own slice, never by a cumulative
             * figure a previous interval contributed to. */
            open_stop_crediting(o);
            o->credited_ns = 0;
            if (o->credited_until_ns > start_mono_ns)
                o->credited_ns = o->credited_until_ns - start_mono_ns;
            o->cpu_slot = 1;
            o->start_ns = start_mono_ns;
        }
        /* Until the first second of this interval is WRITTEN the answer is
         * still free, so take the producer's latest; after that
         * credit_open_second has frozen it and this must not move it. */
        if (!o->cpu_frozen)
            o->cpu_eff_we = eff;
        o->cpu_prefix_ns = cpu->cpu_prefix_ns;
        return;
    }
    /* A WAIT. If this pid's slot was describing an on-CPU gap, that gap is
     * over as far as the producer is concerned: clear the CPU state
     * outright so no part of it can survive into a later gap, and let the
     * branch below treat this as a new interval (o->we, still 0, != we). */
    if (o->cpu_slot)
        open_stop_crediting(o);
    if (o->we != we || o->start_ns != start_mono_ns) {
        /* A DIFFERENT interval from the one last noted. Its per-interval
         * credit starts at zero -- except for any part of it the pid's
         * monotone watermark says was already credited (a tick that ran
         * past the previous close credits into what turns out to be this
         * interval's span). Pre-charging that overlap is what makes the
         * at-close subtraction exact in both tick/close orders instead of
         * counting the overlap twice. */
        o->credited_ns = 0;
        if (o->credited_until_ns > start_mono_ns)
            o->credited_ns = o->credited_until_ns - start_mono_ns;
        o->we = we;
        o->start_ns = start_mono_ns;
    }
}

void pgwt_summary_pin_clock(struct pgwt_summary_writer *w,
                             uint64_t mono_anchor_ns, uint64_t wall_anchor_ns)
{
    w->clock_offset_mono_ns = mono_anchor_ns;
    w->clock_offset_wall_ns = wall_anchor_ns;
    w->clock_anchor_pinned = true;
}

void pgwt_summary_forget_open(struct pgwt_summary_writer *w, uint32_t pid)
{
    struct pgwt_summary_open *o = open_slot(w, pid, false);
    if (!o)
        return;
    /* Tombstone, not a hole: open addressing cannot free a middle slot
     * without breaking the probe chain of every key that passed through it.
     * Clearing `we` stops the credit, which is the point; the slot is reused
     * by the same pid and reclaimed when the writer is destroyed. */
    open_stop_crediting(o);
    o->start_ns = 0;
    o->credited_until_ns = 0;
    o->credited_ns = 0;
    o->noted_ns = 0;
}

/* ── Public API ───────────────────────────────────────────── */

int pgwt_summary_writer_init(struct pgwt_summary_writer *w,
                               const char *trace_dir,
                               int retention_hours,
                               const char *group_name)
{
    memset(w, 0, sizeof(*w));
    snprintf(w->trace_dir, sizeof(w->trace_dir), "%s", trace_dir);
    w->retention_hours = retention_hours;
    w->enabled = true;
    w->current_hour = -1;
    w->trace_gid = (gid_t)-1;

    /* Resolve trace group → GID */
    if (group_name) {
        struct group *gr = getgrnam(group_name);
        if (gr)
            w->trace_gid = gr->gr_gid;
        /* Don't warn here — event_writer already warned */
    }

    /* Directory already created by event_writer */

    /* DUR-1: recover (finalize + archive) any current.summary a previous
     * daemon left behind — never truncate it. */
    recover_current_summary(w);

    /* Allocate scratch buffers.
     * Worst case: 1024 events × 156 + 1024 sessions × 32 + 2048 queries × 36
     *           = 159744 + 32768 + 73728 = ~260 KB uncompressed */
    /* Derived from the table bounds, not a round number: see
     * PGWT_SUMMARY_SERIALIZE_MAX. */
    w->encode_buf_size = PGWT_SUMMARY_SERIALIZE_MAX;
    w->encode_buf = malloc(w->encode_buf_size);

    w->compress_buf_size = LZ4_compressBound((int)w->encode_buf_size);
    w->compress_buf = malloc(w->compress_buf_size);

    w->block_index_cap = 4096;  /* one per second, ~1 hour */
    w->block_index = malloc(w->block_index_cap * sizeof(*w->block_index));

    if (!w->encode_buf || !w->compress_buf || !w->block_index) {
        pgwt_summary_destroy(w);
        return -1;
    }

    /* #128 deferred per-query attribution state (heap: the writer may live
     * on a caller's stack). NULL is non-fatal: emission-time ids as before. */
    w->qattr = calloc(SUMMARY_QATTR_SLOTS, sizeof(*w->qattr));

    /* v8 (#379) open-interval watermarks (heap, same reason). NULL is
     * non-fatal and means exactly the v7 accounting rule: nothing is noted,
     * so nothing is credited per second and every duration lands whole in
     * its closing second. */
    w->open = calloc(SUMMARY_MAX_OPEN, sizeof(*w->open));

    return 0;
}

int pgwt_summary_push_event(struct pgwt_summary_writer *w,
                              const struct pgwt_trace_event *evt,
                              uint32_t cat_flag, uint32_t eff_event)
{
    if (!w->enabled) return 0;

    /* v7 (#408): VALIDATE the producer's effective id before anything uses
     * it. The parameter may express exactly one rewrite -- a foreground
     * we == 0 interval outside a command becomes PGWT_WEI_NONCMD_CPU, which
     * is what both raw paths do (pgwt_tag_events, pgwt_live_effective_event)
     * -- so anything else is a producer bug, and accepting it would let a
     * caller refile arbitrary time into an arbitrary class through a
     * parameter that looks like plumbing. Both degradations keep the v6
     * behaviour (evt->old_event) and are COUNTED, because "the writer quietly
     * classified this the old way" is precisely the kind of silence #408 was:
     * the resolve existed 65 lines below the push and nothing said so. */
    uint32_t eff = eff_event;
    if (eff == PGWT_SUMMARY_EFF_UNRESOLVED) {
        eff = evt->old_event;
        w->eff_unresolved_events_total++;
    } else if (eff != evt->old_event && eff != PGWT_WEI_NONCMD_CPU) {
        eff = evt->old_event;
        w->eff_rejected_events_total++;
    }

    /* v6 (#315, #376): the producer's verdict on this pid's category. The
     * drop itself stays in accum_event -- the one write-side chokepoint,
     * where markers are dropped too -- so there is no second copy of the
     * rule, and the guard is on the path every io_worker record takes
     * instead of being short-circuited here where only a test could reach
     * it.
     *
     * evt->flags is deliberately NOT consulted: nothing that feeds this
     * writer sets a category bit (the daemon does not, and the server's
     * pgwt_tag_events runs at raw LOAD time, long after), so a flags check
     * here would be DEAD in every producer that exists -- which is the N7
     * trap in tests/test_agg_raw_crosscheck.c, where a pre-flagged fixture
     * greens a guard that does nothing in production. An offline producer
     * that does tag its events passes `evt->flags & PGWT_EVENT_FLAG_IO_WORKER`
     * as cat_flag at the call site, where it is visible. */
    bool io_worker = (cat_flag & PGWT_EVENT_FLAG_IO_WORKER) != 0;

    /* Lazy file open on first event -- and on the first event after a
     * rotation, which is the same call and a different failure. #430: it goes
     * through writer_open_counted so that failure is a NUMBER; it used to
     * call open_summary_file directly and move no counter at all. */
    if (!writer_open_counted(w)) {
        w->enabled = false;
        return -1;
    }

    /* Determine which second this event belongs to (monotonic, floored) */
    uint64_t evt_second = (evt->timestamp_ns / 1000000000ULL) * 1000000000ULL;

    /* #277: a second whose record is already on disk can never be reopened
     * — the reader sums every record it finds, so a second record for the
     * same second is counted twice. An event that arrives after its second
     * was written is folded into the oldest second still open, which is
     * exactly what already happens to an out-of-order event inside the
     * stream (the boundary test below is `>`, so a backwards event joins
     * the current accumulator). Counts are conserved; only the event's
     * second moves, by less than one second. */
    if (w->have_flushed_second &&
        evt_second <= w->last_flushed_second_mono_ns) {
        evt_second = w->last_flushed_second_mono_ns + 1000000000ULL;
        w->late_events_folded_total++;
    }

    /* Second boundary detection.
     *
     * v8 (#379): sealing is what credits each completed second's share of
     * every OPEN interval, so it has to happen here -- before this event's
     * second becomes the accumulator -- and it has to cover EVERY second in
     * between, not just the previous one: a 100 s wait is only in the record
     * for second 40 if second 40 was written while that wait was open.
     *
     * A failed flush leaves the accumulator OPEN on purpose (see
     * flush_accum): the second is retried at the next boundary rather than
     * silently becoming a hole in the window. Counted so the gap is
     * attributable instead of merely absent. */
    writer_seal_through(w, evt_second);
    if (w->accum_active && evt_second > w->accum.second_mono_ns) {
        /* Only reachable when writer_seal_through gave up (a failed write),
         * since it seals every second below evt_second. Kept so a sealing
         * failure still cannot leave two seconds in one accumulator. */
        if (flush_accum(w) != 0)
            w->flush_failures_total++;
    }

    /* Start new accumulator if needed */
    if (!w->accum_active) {
        w->accum.second_mono_ns = evt_second;
        w->accum.second_wall_ns = mono_to_wall(w, evt_second);
        w->accum_active = true;
    }

    /* #128: the per-query part may be deferred until the pid's command
     * reports its id (see summary_qattr_step).
     *
     * v6: an io_worker record skips the deferral entirely. io_workers are
     * structurally query-less (src/compute.c says so where it skips them in
     * Top Queries), and the deferral is a SECOND route into queries[]:
     * pgwt_qattr_defer parks the time on the pid and summary_qattr_emit
     * files it with accum_query_add later, which would walk straight past
     * the exclusion in accum_event. Excluding in one accumulator and not the
     * other is how a partial fix relocates a divergence. */
    uint64_t qid = evt->query_id;
    if (w->qattr && !io_worker) {
        qid = summary_qattr_step(w, evt);
    } else if (w->qattr && io_worker && evt->new_event == PGWT_EVENT_EXIT) {
        /* LIFECYCLE, not accounting. Skipping summary_qattr_step also skips
         * its EXIT-time qattr_slot_free, so a slot this pid created BEFORE
         * its metadata resolved (the pgwt_live_pid_cat_flag residual: an
         * io_worker reads as foreground until discovery parses it) would
         * never be reclaimed. Dropping the parked time is right -- io_workers
         * are query-less -- but the slot must still go, or pid reuse inside
         * one writer lifetime hands a client backend a stale entry and it
         * emits the parked io_worker time into queries[] on its first
         * CMD_START. Free it without resolving anything. */
        struct pgwt_summary_qattr_slot *s = qattr_slot(w, evt->pid);
        if (s)
            qattr_slot_free(w, s);
    }
    /* v8 (#379): close the pid's open interval against its watermark. The
     * record contributes only the part of its duration no completed second
     * has already been credited for -- `close - max(start, credited_until)`,
     * clamped at both ends so the tick and the close can arrive in EITHER
     * order without double counting or losing a nanosecond:
     *   - tick first (the normal case): credited_until sits at a second
     *     boundary inside the interval, and the remainder is the tail;
     *   - close first: credited_until is below the record's start, nothing
     *     was credited, and the whole duration lands here (v7 behaviour);
     *   - a tick that already saw the NEXT interval and credited past this
     *     close: credited_until > close, the remainder clamps to 0, and the
     *     overlap stays filed under the previous event. Attribution moves by
     *     at most one tick; the total does not move at all.
     * Markers and EXIT records are excluded because they are not intervals
     * (accum_event drops them); an EXIT also retires the pid's note. */
    uint64_t already = 0;
    struct accum_close_ctx cc = { 0 };
    bool is_marker = PGWT_IS_MARKER(evt->old_event) ||
                     evt->old_event == PGWT_EVENT_EXIT;
    if (w->open && !is_marker) {
        uint64_t start = evt->timestamp_ns >= evt->duration_ns
                       ? evt->timestamp_ns - evt->duration_ns : 0;
        struct pgwt_summary_open *o = open_slot(w, evt->pid, false);
        if (o) {
            /* IDENTITY CHECK, and it is load-bearing. Only subtract credits
             * when the note that was credited IS the interval this record
             * closes -- same pid, same wait id, same start. A producer whose
             * records do NOT tile one pid's timeline (overlapping intervals;
             * a synthetic scenario that interleaves a long wait with short
             * ones for the same pid) would otherwise have the long record's
             * duration cancelled by credits that belonged to the short ones,
             * turning an over-report into a 39.5 s UNDER-report. Measured on
             * the #410 parity fixture's pid 1001 before this check existed:
             * IO:DataFileRead 89,000 ms raw vs 49,499 ms summary on the
             * interior window. The watermark itself still floors every
             * credit, so a mismatch can only ever charge the record WHOLE
             * (the v7 answer) -- never twice. */
            if (o->we == evt->old_event && o->start_ns == start) {
                already = o->credited_ns;
                if (already > evt->duration_ns)
                    already = evt->duration_ns;
                /* v9 (#424): the CPU half of the same subtraction. The
                 * identity check above is the SAME one -- an on-CPU slot
                 * has o->we == 0, which matches a we == 0 record's
                 * old_event -- so the measurement is only ever subtracted
                 * from the interval it was credited for. cpu_frozen, not
                 * cpu_slot, is the gate: an interval noted but never
                 * written into has placed nothing on disk and must keep the
                 * plain v7/v8 close. */
                if (o->cpu_slot && o->cpu_frozen) {
                    cc.cpu_credited    = 1;
                    cc.frozen_eff      = o->cpu_eff_we;
                    cc.tier_unknown    = o->cpu_tier_known ? 0 : 1;
                    cc.already_cpu_ns  = o->cpu_credited_ns;
                    /* The measured identity's one escape hatch, counted
                     * rather than hidden: a prefix observation that ran
                     * past the closing record's own cpu_ns cannot be taken
                     * back out of an immutable second. */
                    if (evt->cpu_ns != PGWT_CPU_NS_UNKNOWN) {
                        uint64_t cap = evt->cpu_ns < evt->duration_ns
                                     ? evt->cpu_ns : evt->duration_ns;
                        if (o->cpu_credited_ns > cap)
                            w->open_cpu_overcredit_ns_total +=
                                o->cpu_credited_ns - cap;
                    }
                    /* Piece 3's measured cost: the producer's answer at
                     * close differs from the one frozen on disk. The frozen
                     * one wins (the written seconds cannot move), and the
                     * whole interval's wall is counted as divergent. */
                    if (eff != o->cpu_eff_we)
                        w->open_cpu_class_divergent_ns_total +=
                            evt->duration_ns;
                    /* Piece 4: the tier flipped between the first credit
                     * and the close. Both directions conserve CPU* (it is
                     * measured + unknown); only the attribution moves. */
                    if ((evt->cpu_ns != PGWT_CPU_NS_UNKNOWN) !=
                        (o->cpu_tier_known != 0))
                        w->open_cpu_tier_mismatch_total++;
                }
            } else {
                /* No matching note. Subtract only what the pid's MONOTONE
                 * watermark proves was already placed inside this record's
                 * span -- normally nothing. This is the anti-double-count
                 * floor for the cases the identity check refuses: a
                 * producer whose open-interval view is not interval-exact,
                 * or two intervals overlapping on one pid. */
                if (o->credited_until_ns > start) {
                    already = o->credited_until_ns - start;
                    if (already > evt->duration_ns)
                        already = evt->duration_ns;
                }
                /* v9: `|| o->cpu_slot` -- an open on-CPU slot also has
                 * we == 0, so without it the one disagreement the new
                 * state can produce would be the only uncounted one. */
                if (o->we != 0 || o->cpu_slot)
                    w->open_note_mismatch_total++;
            }
            /* The interval is over: nothing is creditable for this pid until
             * the producer notes the next one. credited_until_ns is NOT
             * bumped here -- it advances only where credit was actually
             * placed, which is what keeps a mismatched record from eating
             * another interval's accounting. */
            open_stop_crediting(o);
            o->start_ns = evt->timestamp_ns;
            o->credited_ns = 0;
        }
        /* The #379 residual, MEASURED rather than asserted: an on-CPU gap
         * longer than a second that landed WHOLE in its closing second
         * because nothing credited it while it was open. v9 shrinks this to
         * the gaps no producer observed (no notes at all, the lightweight
         * and sampled tiers, a gap that opened and closed between two
         * ticks); it does not redefine it, so the number stays comparable
         * across versions and `credited > 0 && uncredited > 0` is a
         * meaningful "some producers observe, some do not". */
        if (already == 0 && evt->duration_ns > PGWT_NS_PER_SEC &&
            summary_wait_class_index(evt->old_event) == PGWT_CLASS_CPU)
            w->cpu_open_uncredited_ns_total += evt->duration_ns;
    }
    if (evt->new_event == PGWT_EVENT_EXIT || evt->old_event == PGWT_EVENT_EXIT)
        pgwt_summary_forget_open(w, evt->pid);

    cc.already_ns = already;
    if (accum_event(&w->accum, evt, qid, io_worker, eff, &cc)) {
        w->io_worker_events_excluded_total++;
        w->io_worker_ns_excluded_total += evt->duration_ns;
    }
    return 0;
}

/* Both public flush entry points route through here so a failure is counted
 * exactly once wherever it came from. Previously these returned flush_accum's
 * status without touching the counter, so only the internal second-boundary
 * path incremented it -- and the daemon discards the periodic flush result, so
 * a failing tick reached nobody at all. */
static int flush_counted(struct pgwt_summary_writer *w)
{
    int rc = flush_accum(w);
    if (rc != 0)
        w->flush_failures_total++;
    return rc;
}

int pgwt_summary_flush(struct pgwt_summary_writer *w)
{
    return flush_counted(w);
}

int pgwt_summary_flush_completed(struct pgwt_summary_writer *w,
                                   uint64_t now_mono_ns)
{
    if (!w->enabled)
        return 0;

    /* A second is written once it is COMPLETE: writing it earlier would put
     * a partial record on disk that the real second boundary writes again as
     * a superset (#277). The decision is on the clock, not on event arrival,
     * so a second that simply stops receiving events is still written on the
     * next tick — this can delay a record, never withhold one.
     *
     * `complete_before` is the first second that is NOT complete, i.e. every
     * second S with S + 1 s + PGWT_SUMMARY_FLUSH_LAG_NS <= now_mono_ns is
     * sealed. That is the identical rule the single-accumulator version
     * applied; what v8 adds is that the seal also credits each of those
     * seconds with its share of every open interval, and that a second in
     * which NOTHING closed is still written when something was open in it —
     * which is the whole of #379 for a system that goes quiet. */
    if (now_mono_ns < PGWT_NS_PER_SEC + PGWT_SUMMARY_FLUSH_LAG_NS)
        return 0;
    uint64_t latest = now_mono_ns - PGWT_SUMMARY_FLUSH_LAG_NS
                    - PGWT_NS_PER_SEC;          /* last complete second's ts */
    uint64_t complete_before = (latest / PGWT_NS_PER_SEC) * PGWT_NS_PER_SEC
                             + PGWT_NS_PER_SEC; /* first INcomplete second  */
    writer_seal_through(w, complete_before);
    return 0;
}

int pgwt_summary_check_rotation(struct pgwt_summary_writer *w)
{
    if (!w->enabled || !w->fp) return 0;

    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    struct tm tm;
    time_t now = wall.tv_sec;
    localtime_r(&now, &tm);
    int current_hour = tm.tm_yday * 24 + tm.tm_hour;

    if (current_hour == w->current_hour)
        return 0;

    /* Hour changed — rotate. Refusing to rotate on a failed flush would
     * strand the writer on a file it can no longer append to, so the rotation
     * proceeds -- but loudly, because the un-flushed second is lost at this
     * point (the accumulator belongs to the file being closed). */
    if (flush_accum(w) != 0) {
        w->flush_failures_total++;
        /* BEHAVIOUR FIX, not just wording: accum_close() runs only on
         * flush_accum's success path, so a failed rotate flush left the
         * accumulator ACTIVE and it was written into the NEXT hour's file at
         * the following boundary -- carrying a second_wall_ns earlier than
         * that file's own name-derived start, which the reader's window
         * filter then places wrongly. A lost second is a short window; a
         * misfiled one is a wrong answer in two windows. So discard it
         * explicitly, which also makes the message below true. */
        accum_close(w);
        fprintf(stderr, "ERROR: summary rotation could not write the open "
                "second; it is DISCARDED (not carried into the next hour's "
                "file) and that window is short by one second\n");
    }
    write_footer(w);

    if (w->verbose) {
        fprintf(stderr, "INFO: summary file closed: %lu records, %lu bytes\n",
                (unsigned long)w->total_records_written,
                (unsigned long)w->total_bytes_written);
    }

    /* DUR-5: archives are durable once rotated. */
    fflush(w->fp);
    fsync(fileno(w->fp));
    fclose(w->fp);
    w->fp = NULL;

    /* Rename current.summary → YYYY-MM-DD_HH[.N].summary.lz4 (DUR-2:
     * collision-safe — see the trace writer's rotation comment). */
    time_t file_start_sec = (time_t)(w->clock_offset_wall_ns / 1000000000ULL);
    struct tm file_tm;
    localtime_r(&file_start_sec, &file_tm);
    char new_path[512];
    if (pgwt_archive_path_nonclobber(w->trace_dir, &file_tm, ".summary.lz4",
                                     new_path, sizeof(new_path)) == 0)
        rename(w->current_path, new_path);
    else
        fprintf(stderr, "WARN: no free archive name for %s — leaving it as "
                "current.summary (it will be recovered on next start)\n",
                w->current_path);

    /* Remove meta file */
    {
        char meta_path[512];
        snprintf(meta_path, sizeof(meta_path), "%s/current.summary.meta",
                 w->trace_dir);
        unlink(meta_path);
    }
    pgwt_fsync_dir(w->trace_dir);

    /* Reset stats */
    w->total_records_written = 0;
    w->total_bytes_written = 0;

    return 0;
}

int pgwt_summary_close(struct pgwt_summary_writer *w)
{
    if (!w->fp) return 0;

    if (flush_accum(w) != 0) {
        w->flush_failures_total++;
        fprintf(stderr, "ERROR: summary close lost the open second "
                "(flush failed)\n");
    }
    write_footer(w);

    if (w->verbose) {
        fprintf(stderr, "INFO: summary file closed: %lu records, %lu bytes\n",
                (unsigned long)w->total_records_written,
                (unsigned long)w->total_bytes_written);
    }

    fflush(w->fp);
    fsync(fileno(w->fp));
    fclose(w->fp);
    w->fp = NULL;
    return 0;
}

int pgwt_summary_cleanup_old_files(struct pgwt_summary_writer *w)
{
    if (w->retention_hours <= 0) return 0;

    DIR *dir = opendir(w->trace_dir);
    if (!dir) return -1;

    time_t cutoff = time(NULL) - (time_t)w->retention_hours * 3600;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strstr(ent->d_name, ".summary.lz4"))
            continue;

        struct tm ftm = {0};
        int hour;
        if (sscanf(ent->d_name, "%4d-%2d-%2d_%2d.summary.lz4",
                   &ftm.tm_year, &ftm.tm_mon, &ftm.tm_mday, &hour) != 4)
            continue;

        ftm.tm_year -= 1900;
        ftm.tm_mon -= 1;
        ftm.tm_hour = hour;
        ftm.tm_isdst = -1;
        time_t file_time = mktime(&ftm);

        if (file_time > 0 && file_time < cutoff) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", w->trace_dir, ent->d_name);
            if (unlink(path) == 0 && w->verbose)
                fprintf(stderr, "INFO: deleted old summary file %s\n", ent->d_name);
        }
    }
    closedir(dir);
    return 0;
}

void pgwt_summary_destroy(struct pgwt_summary_writer *w)
{
    free(w->encode_buf);
    w->encode_buf = NULL;
    free(w->compress_buf);
    w->compress_buf = NULL;
    free(w->block_index);
    w->block_index = NULL;
    free(w->qattr);
    w->qattr = NULL;
    free(w->open);
    w->open = NULL;
}
