/* compute.c — Server-side compute functions for pgwt-server
 *
 * Direct port of client/src/compute.rs. Works on raw pgwt_trace_event arrays.
 * All result structs use malloc'd arrays — caller frees with free(result->rows).
 */
#include "compute.h"
#include "summary_writer.h"
#include "summary_reader.h"
#include "wait_event.h"
#include "percentile.h"
#include "pid_index.h"
#include "triple_map.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Window clipping ──────────────────────────────────────────────────────
 *
 * The raw loader (server_load_events_fi) selects events by their END
 * timestamp, so a wait already running when the window opened arrives whole.
 * pgwt_compute_aas has always clipped such a straddler to its in-window part;
 * the Overview/Top-N summers did not, and counted it in full. On a 60 s
 * window of an uncontended rehearsal that was +1.28% on DB Time (Timeout
 * 931.110 ms + Lock 894.018 ms of 142,059.216 ms) — unbounded in a lock
 * pileup, where one 10 s Lock:relation ending 1 s into the window adds 9 s.
 *
 * `from_ns == 0` / `to_ns == 0` mean "unbounded on that side". The server
 * forwards req->from_ns / req->to_ns verbatim, and both are 0 on a
 * whole-capture request, so those results are bit-identical to pre-fix.
 * Returns 0 for an event wholly outside the window (never a wrapped
 * uint64 difference), and 0 for a record pgwt_filter_matches would refuse
 * as impossible (duration > timestamp).
 */
static inline uint64_t event_window_ns(const struct pgwt_trace_event *ev,
                                       uint64_t from_ns, uint64_t to_ns)
{
    /* Defence in depth for the impossible record pgwt_filter_matches already
     * refuses (duration > its own absolute timestamp): return 0 rather than
     * clamp the start to 0. Clamping would have made the UNBOUNDED path
     * return timestamp_ns instead of duration_ns — the one exception to
     * "a whole-capture request is bit-identical to pre-fix". Refusing keeps
     * that claim exact and keeps this helper agreeing with the filter for
     * any future caller that reaches it without one. */
    if (ev->duration_ns > ev->timestamp_ns)
        return 0;
    uint64_t start = ev->timestamp_ns - ev->duration_ns;
    uint64_t end   = ev->timestamp_ns;
    if (from_ns != 0 && start < from_ns) start = from_ns;
    if (to_ns   != 0 && end   > to_ns)   end   = to_ns;
    return end > start ? end - start : 0;
}

/* Fraction of an event that lies inside the window, for quantities measured
 * per-event rather than in wall time (cpu_ns). Matches the ov/dur split
 * pgwt_compute_aas applies per bucket, so CPU* agrees between the two paths.
 * Exactly 1.0 when the window does not cut the event, which keeps the
 * no-window path bit-identical. */
static inline double event_window_frac(uint64_t clip_ns, uint64_t dur_ns)
{
    if (dur_ns == 0) return 0.0;
    if (clip_ns == dur_ns) return 1.0;
    return (double)clip_ns / (double)dur_ns;
}

/* pgwt_duration_to_bucket: log2 latency bucket for heatmap.
 * In daemon build this comes from map_reader.c; inline it here for server. */
#ifdef PGWT_SERVER
static uint32_t compute_duration_to_bucket(uint64_t ns)
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
#include "map_reader.h"
#define compute_duration_to_bucket pgwt_duration_to_bucket
#endif

/* ── Hash helpers for O(1) event/PID/query lookups ─────────── */

/* Hash a uint32 key to a table slot (power-of-2 table size) */
static inline int hash32(uint32_t key, int mask)
{
    key = ((key >> 16) ^ key) * 0x45d9f3b;
    key = ((key >> 16) ^ key) * 0x45d9f3b;
    key = (key >> 16) ^ key;
    return (int)(key & mask);
}

/* Hash a uint64 key to a table slot (power-of-2 table size) */
static inline int hash64(uint64_t key, int mask)
{
    key = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ULL;
    key = (key ^ (key >> 27)) * 0x94d049bb133111ebULL;
    key = key ^ (key >> 31);
    return (int)(key & mask);
}

/*
 * Find-or-insert in an open-addressing hash table of event_accum entries.
 * Returns the index of the existing or newly-inserted entry, or -1 if full.
 * Table entries with event_id == 0 are considered empty (event_id 0 = CPU
 * is handled specially by callers before reaching the hash lookup).
 */
struct event_ht_entry {
    uint32_t event_id;    /* 0 = empty slot */
    double   total_ns;
};

#define EVENT_HT_SIZE 2048  /* must be power of 2 */
#define EVENT_HT_MASK (EVENT_HT_SIZE - 1)

static inline int event_ht_find_or_insert(struct event_ht_entry *ht,
                                          uint32_t event_id)
{
    int slot = hash32(event_id, EVENT_HT_MASK);
    for (int i = 0; i < EVENT_HT_SIZE; i++) {
        int idx = (slot + i) & EVENT_HT_MASK;
        if (ht[idx].event_id == event_id)
            return idx;
        if (ht[idx].event_id == 0) {
            ht[idx].event_id = event_id;
            return idx;
        }
    }
    return -1;  /* full (shouldn't happen with 2048 slots for ~300 events) */
}

/*
 * Find-or-insert for per-PID wait tracking within a session_accum.
 * Uses same open-addressing approach.
 */
#define WAIT_HT_SIZE 256   /* must be power of 2 */
#define WAIT_HT_MASK (WAIT_HT_SIZE - 1)

struct wait_ht_entry {
    uint32_t event_id;    /* 0 = empty */
    uint64_t total_ns;
};

static inline int wait_ht_find_or_insert(struct wait_ht_entry *ht,
                                         uint32_t event_id)
{
    int slot = hash32(event_id, WAIT_HT_MASK);
    for (int i = 0; i < WAIT_HT_SIZE; i++) {
        int idx = (slot + i) & WAIT_HT_MASK;
        if (ht[idx].event_id == event_id)
            return idx;
        if (ht[idx].event_id == 0) {
            ht[idx].event_id = event_id;
            return idx;
        }
    }
    return -1;
}

/* ── Wait class mapping ───────────────────────────────────── */

int pgwt_wait_class_index(uint32_t event_id)
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

/* ── Filter ───────────────────────────────────────────────── */

int pgwt_filter_matches(const struct pgwt_filter *f,
                        const struct pgwt_trace_event *ev)
{
    /* Markers (exec/plan/escalation) are structural records, never wait
     * data: duration 0, sentinel event ids, and — for escalation markers —
     * a PGWT_ESC_PACK payload in query_id. Excluding them here is the
     * single chokepoint for every raw compute path (FID-4): top_events,
     * top_queries, heatmap, sessions, AAS, time model, timeline,
     * fingerprints, concurrency, lock chains, interference. Consumers that
     * legitimately READ markers (variants, the exec/plan lifecycle stats)
     * handle them before calling the filter. */
    if (PGWT_IS_MARKER(ev->old_event))
        return 0;
    /* A record whose duration exceeds its own absolute END timestamp cannot
     * be true: timestamp_ns is CLOCK_REALTIME-derived (~1.8e18 now), so this
     * needs a wait longer than the epoch. REFUSED here, at the same
     * chokepoint, rather than reinterpreted — the two plausible repairs
     * disagree with each other (clamp the start to 0 and you invent ~58
     * years of wait time; let `end - duration` wrap and the event silently
     * vanishes), and picking either would make the compute paths answer
     * differently on the same bytes: pgwt_compute_aas derives its own
     * ev_start by subtraction and would drop the record, while the Overview
     * summers would count it. A record that cannot be true is refused, not
     * repaired, and refusing it HERE is what keeps every path agreeing. */
    if (ev->duration_ns > ev->timestamp_ns)
        return 0;
    if (f->class_name[0] != '\0') {
        const char *cls = pgwt_class_name(ev->old_event);
        if (strcasecmp(cls, f->class_name) != 0)
            return 0;
    }
    if (f->event_id != 0 && ev->old_event != f->event_id)
        return 0;
    if (f->pid != 0 && ev->pid != f->pid)
        return 0;
    if (f->query_id != 0 && ev->query_id != f->query_id)
        return 0;
    return 1;
}

/* ── T2: category tagging pass (docs/AAS_SEMANTICS_DECISION.md) ──────── */

/* Per-pid sweep state for pgwt_tag_events. Open-addressing hash on pid. */
#define TAG_HT_SIZE 4096
#define TAG_HT_MASK (TAG_HT_SIZE - 1)
struct tag_pid_state {
    uint32_t pid;          /* 0 = empty slot */
    uint32_t cat_flag;     /* category flag from the pid table */
    uint8_t  cat_resolved;
    uint8_t  plan_open, exec_open;
    uint8_t  cmd_open, seen_cmd;
    uint64_t cmd_anchor;   /* start of the current open run / last record end */
    uint64_t banked_ns;    /* closed command-open ns since the last record */
    /* #128 query attribution (query_attr.h rules, mirrored here for the
     * whole-array case): the last id reported since the command opened
     * (forward pass, rule 2) and the first id reported later in the same
     * command (backward pass, rule 1). */
    uint64_t cmd_qid;
    uint64_t next_qid;
};

static uint32_t pid_cat_lookup(const struct pgwt_pid_cat *cats, int n,
                               uint32_t pid)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (cats[mid].pid == pid)
            return cats[mid].flag;
        if (cats[mid].pid < pid)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;   /* unknown pid: foreground (conservative) */
}

static struct tag_pid_state *tag_pid_get(struct tag_pid_state *ht, uint32_t pid)
{
    int h = hash32(pid, TAG_HT_MASK);
    for (int probe = 0; probe < TAG_HT_SIZE; probe++) {
        if (ht[h].pid == pid)
            return &ht[h];
        if (ht[h].pid == 0) {
            ht[h].pid = pid;
            return &ht[h];
        }
        h = (h + 1) & TAG_HT_MASK;
    }
    return NULL;   /* table full — untagged records stay foreground */
}

void pgwt_tag_events(struct pgwt_trace_event *events, int count,
                     const struct pgwt_pid_cat *cats, int n_cats)
{
    struct tag_pid_state *ht = calloc(TAG_HT_SIZE, sizeof(*ht));
    if (!ht)
        return;   /* untagged: everything foreground, CPU stays CPU */
    /* #128: per-record cmd_qid at the record's time (rule 2 fallback for
     * the backward pass). NULL on allocation failure → no back-fill, the
     * records keep their emission-time id exactly as before. */
    uint64_t *fwd = count > 0 ? calloc((size_t)count, sizeof(*fwd)) : NULL;

    for (int i = 0; i < count; i++) {
        struct pgwt_trace_event *ev = &events[i];
        struct tag_pid_state *st = tag_pid_get(ht, ev->pid);
        if (!st)
            continue;

        /* Markers drive the per-pid windows and are otherwise left alone. */
        if (PGWT_IS_MARKER(ev->old_event)) {
            uint64_t ts = ev->timestamp_ns;
            /* #128 forward pass: a marker carrying an id observes it for
             * the command; CMD_START forgets the previous command's id
             * (the id BPF resolved AT CMD_START is the previous
             * statement's). Escalation markers pack a reason, not an id. */
            if (ev->old_event == PGWT_MARKER_CMD_START)
                st->cmd_qid = 0;
            else if (ev->old_event != PGWT_MARKER_ESCALATE_START &&
                     ev->old_event != PGWT_MARKER_ESCALATE_END &&
                     ev->query_id != 0)
                st->cmd_qid = ev->query_id;
            switch (ev->old_event) {
            case PGWT_MARKER_PLAN_START: st->plan_open = 1; break;
            case PGWT_MARKER_PLAN_END:   st->plan_open = 0; break;
            case PGWT_MARKER_EXEC_START: st->exec_open = 1; break;
            case PGWT_MARKER_EXEC_END:   st->exec_open = 0; break;
            case PGWT_MARKER_CMD_START:
                st->seen_cmd = 1;
                if (!st->cmd_open) {
                    st->cmd_open = 1;
                    st->cmd_anchor = ts;
                }
                break;
            case PGWT_MARKER_CMD_END:
                st->seen_cmd = 1;
                if (st->cmd_open) {
                    st->banked_ns += ts - st->cmd_anchor;
                    st->cmd_open = 0;
                }
                /* A command closing also closes any stale plan/exec window
                 * (an EXEC_END lost to a ringbuf drop must not leak the
                 * window across statements). */
                st->plan_open = 0;
                st->exec_open = 0;
                break;
            default:
                break;   /* escalation markers: pid 0, no per-pid state */
            }
            continue;
        }

        if (!st->cat_resolved) {
            st->cat_flag = pid_cat_lookup(cats, n_cats, ev->pid);
            st->cat_resolved = 1;
        }
        ev->flags |= st->cat_flag;

        /* #128 forward pass (query_attr.h rule 2): remember the command's
         * last reported id at this record; a record carrying one reports
         * it. An idle SAMPLE is the sampled tier's between-commands
         * boundary (no markers there): it carries the finished statement's
         * id (sampler.c build_batch) for the backward pass, but the next
         * command must not inherit it. */
        if (fwd)
            fwd[i] = st->cmd_qid;
        /* pgwt_is_session_idle_event, NOT the load rule: this ENDS THE
         * COMMAND, and it is the offline counterpart of src/sampler.c's live
         * rule, which uses the narrow predicate. Before this was fixed the
         * same decision was made two different ways depending on whether it
         * ran live or offline. A sampled Timeout:VacuumDelay is excluded from
         * DB Time but happens INSIDE a running VACUUM, and treating it as a
         * boundary dropped the command's parse-phase waits. Adjudicated by
         * tests/test_idle_accounting.c section 7, which failed here before
         * the change and passes its ClientRead contrast either way. */
        if ((ev->flags & PGWT_EVENT_FLAG_SAMPLE) &&
            pgwt_is_session_idle_event(ev->new_event ? ev->new_event
                                                     : ev->old_event))
            st->cmd_qid = 0;
        else if (ev->query_id != 0)
            st->cmd_qid = ev->query_id;
        /* The record closing an exiting backend is its last: nothing after
         * it (a reused pid number) belongs to this command. */
        if (ev->new_event == PGWT_EVENT_EXIT)
            st->cmd_qid = 0;

        if (ev->flags & PGWT_EVENT_FLAG_SAMPLE) {
            /* Sampled tier: no plan/exec markers exist. Cheap phase
             * attribution — a foreground sample carrying a query_id is
             * execution; without one it is command overhead. */
            if (st->cat_flag == 0 && ev->query_id != 0)
                ev->flags |= PGWT_EVENT_FLAG_EXEC;
            continue;
        }

        /* Exact record: interval [t1 - dur, t1). */
        uint64_t t1 = ev->timestamp_ns;
        uint64_t t0 = t1 - ev->duration_ns;
        if (st->plan_open) ev->flags |= PGWT_EVENT_FLAG_PLAN;
        if (st->exec_open) ev->flags |= PGWT_EVENT_FLAG_EXEC;

        if (st->seen_cmd) {
            /* Command-open ns inside this interval: the banked closed runs
             * plus the currently-open run clipped to the interval. Records
             * for one pid are contiguous in an exact span, so banked time
             * since the previous record belongs to this interval. */
            uint64_t open_ns = st->banked_ns;
            if (st->cmd_open) {
                uint64_t a = st->cmd_anchor > t0 ? st->cmd_anchor : t0;
                if (t1 > a)
                    open_ns += t1 - a;
            }
            st->banked_ns = 0;
            if (st->cmd_open)
                st->cmd_anchor = t1;

            /* Majority rule: an interval more than half inside a command
             * window is in-command (bounded by one interval's length —
             * we==0 intervals are µs-ms vs multi-second buckets). */
            int in_cmd = ev->duration_ns == 0
                       ? st->cmd_open
                       : (open_ns * 2 >= ev->duration_ns);
            if (in_cmd)
                ev->flags |= PGWT_EVENT_FLAG_CMD_OPEN;

            /* The decision table: we==0 OUTSIDE a command is idle
             * post/between-command time, not CPU. Foreground pids only —
             * background processes never report activity states. */
            if (ev->old_event == 0 && st->cat_flag == 0 && !in_cmd)
                ev->old_event = PGWT_WEI_NONCMD_CPU;
        }
    }

    /* #128 backward pass (query_attr.h): a foreground non-idle record that
     * closed with query_id 0 takes the first id reported LATER in the same
     * command (rule 1 — the parse-phase lock wait closes before
     * pgstat_report_query_id; the plan/exec markers and the run that
     * follows carry the id), else the last id reported earlier in the
     * command (rule 2, fwd[]), else it is UNATTRIBUTED — flagged so
     * top_queries counts it instead of dropping it. Boundaries mirror the
     * live consumers: CMD_START (and an idle sample) end the command in
     * both directions; CMD_END and an idle exact record end rule 1 only. */
    if (fwd) {
        for (int i = count - 1; i >= 0; i--) {
            struct pgwt_trace_event *ev = &events[i];
            struct tag_pid_state *st = tag_pid_get(ht, ev->pid);
            if (!st)
                continue;
            if (PGWT_IS_MARKER(ev->old_event)) {
                if (ev->old_event == PGWT_MARKER_ESCALATE_START ||
                    ev->old_event == PGWT_MARKER_ESCALATE_END)
                    continue;
                if (ev->old_event == PGWT_MARKER_CMD_START)
                    st->next_qid = 0;
                else if (ev->query_id != 0)
                    st->next_qid = ev->query_id;
                else if (ev->old_event == PGWT_MARKER_CMD_END)
                    st->next_qid = 0;
                continue;
            }
            uint32_t we = (ev->flags & PGWT_EVENT_FLAG_SAMPLE) && ev->new_event
                        ? ev->new_event : ev->old_event;
            /* Walking backwards, an exiting backend's closing record is
             * the first of its pid we meet: anything already in next_qid
             * came from a later reuse of the pid number. */
            if (ev->new_event == PGWT_EVENT_EXIT)
                st->next_qid = 0;
            if (ev->query_id != 0) {
                st->next_qid = ev->query_id;
                continue;
            }
            /* Same command-boundary question as the forward pass above, on
             * the backward (backfill) pass -- and this one is NOT gated on
             * SAMPLE, so it reached exact records too: an in-command pacing
             * sleep with query_id 0 cleared the carry AND skipped backfill,
             * losing the statement's parse-phase lock entirely. */
            if (pgwt_is_session_idle_event(we)) {
                st->next_qid = 0;
                continue;
            }
            if (st->cat_flag != 0)
                continue;   /* background / maintenance / io_worker: query-less */
            uint64_t q = st->next_qid ? st->next_qid : fwd[i];
            if (q != 0) {
                ev->query_id = q;
                ev->flags |= PGWT_EVENT_FLAG_QUERY_BACKFILL;
            } else {
                ev->flags |= PGWT_EVENT_FLAG_QUERY_UNATTRIB;
            }
        }
    }
    free(fwd);
    free(ht);
}

int pgwt_event_category(const struct pgwt_trace_event *ev)
{
    if (ev->flags & PGWT_EVENT_FLAG_IO_WORKER)
        return PGWT_CAT_IO_WORKER;
    if (pgwt_is_idle_event(ev->old_event))
        return -1;
    if (ev->flags & PGWT_EVENT_FLAG_MAINT)
        return PGWT_CAT_MAINT;
    if (ev->flags & PGWT_EVENT_FLAG_BACKGROUND)
        return PGWT_CAT_BG;
    if (ev->flags & PGWT_EVENT_FLAG_PLAN)
        return PGWT_CAT_FG_PLAN;
    if (ev->flags & PGWT_EVENT_FLAG_EXEC)
        return PGWT_CAT_FG_EXEC;
    return PGWT_CAT_FG_CMD;
}

/* ── AAS Buckets ──────────────────────────────────────────── */

/* Hash table entry for per-event total accumulation (pass 1) */
struct aas_event_accum {
    uint32_t event_id;
    uint64_t total_ns;
};

#define AAS_EVT_HT_SIZE 512
#define AAS_EVT_HT_MASK (AAS_EVT_HT_SIZE - 1)

static int cmp_aas_event_series(const void *a, const void *b)
{
    double da = ((const struct pgwt_aas_event_series *)a)->total_aas;
    double db = ((const struct pgwt_aas_event_series *)b)->total_aas;
    return (db > da) - (db < da);
}

void pgwt_compute_aas(const struct pgwt_trace_event *events, int count,
                      const struct pgwt_filter *f,
                      uint64_t from_ns, uint64_t to_ns, int num_buckets,
                      int detail_events, int max_series,
                      struct pgwt_aas_result *out)
{
    memset(out, 0, sizeof(*out));

    uint64_t range_ns = to_ns - from_ns;
    if (range_ns == 0 || num_buckets <= 0) {
        out->bucket_ns = 1000000000ULL;
        return;
    }

    /* Raw events have ns precision — no minimum bucket width */
    uint64_t bucket_ns = (range_ns + (uint64_t)num_buckets - 1) / (uint64_t)num_buckets;
    if (bucket_ns == 0)
        bucket_ns = 1;

    int actual_buckets = (int)((range_ns + bucket_ns - 1) / bucket_ns);
    struct pgwt_aas_bucket *buckets = calloc(actual_buckets, sizeof(*buckets));
    if (!buckets)
        return;

    for (int i = 0; i < actual_buckets; i++)
        buckets[i].start_ns = from_ns + (uint64_t)i * bucket_ns;

    /* Pass 1: accumulate class AAS + per-event totals (if detail requested) */
    struct aas_event_accum *evt_ht = NULL;
    if (detail_events) {
        evt_ht = calloc(AAS_EVT_HT_SIZE, sizeof(*evt_ht));
        if (!evt_ht) { free(buckets); return; }
    }

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
            continue;

        /* T2: io_worker records never enter the class AAS (or max_aas) —
         * they appear only in their own category slot, so the headline AAS
         * keeps the "≤ sessions doing work" invariant and stays comparable
         * across io_method settings. */
        int cat = pgwt_event_category(ev);
        int io_worker = (cat == PGWT_CAT_IO_WORKER);

        /* Event covers [timestamp_ns - duration_ns, timestamp_ns) */
        uint64_t ev_start = ev->timestamp_ns - ev->duration_ns;
        uint64_t ev_end   = ev->timestamp_ns;

        if (ev_end <= from_ns || ev_start >= to_ns)
            continue;

        int class_idx = pgwt_wait_class_index(ev->old_event);

        /* T8 revision: measured cpu_ns for this interval (per-bucket, split by
         * overlap fraction). Counted toward CPU* for BOTH we==0 gaps and wait
         * intervals (a wait's cpu_ns is boundary-leaked pre-wait CPU). Off-CPU*
         * is a residual, computed per bucket after the loop from exact totals
         * (leak-immune) — NOT a per-interval dur-cpu split. UNKNOWN records use
         * gap-inference: a we==0 gap is wholly CPU, a wait contributes 0 CPU.
         * `buckets[].offcpu_aas` is reused as the per-bucket DB-ns accumulator
         * during the loop and converted to the residual in the normalize pass. */
        int is_measured = (ev->cpu_ns != PGWT_CPU_NS_UNKNOWN);
        double dur_d = (double)ev->duration_ns;

        int first_b = (ev_start <= from_ns) ? 0
                     : (int)((ev_start - from_ns) / bucket_ns);
        int last_b  = (ev_end >= to_ns) ? actual_buckets - 1
                     : (int)(((ev_end - from_ns) - 1) / bucket_ns);
        if (last_b >= actual_buckets)
            last_b = actual_buckets - 1;

        for (int b = first_b; b <= last_b; b++) {
            uint64_t b_start = from_ns + (uint64_t)b * bucket_ns;
            uint64_t b_end   = b_start + bucket_ns;
            uint64_t o_start = ev_start > b_start ? ev_start : b_start;
            uint64_t o_end   = ev_end < b_end ? ev_end : b_end;
            if (o_end > o_start) {
                double ov = (double)(o_end - o_start);
                if (!io_worker) {
                    buckets[b].offcpu_aas += ov;   /* DB-ns accumulator */
                    if (class_idx == PGWT_CLASS_CPU) {
                        /* we==0 gap: measured on-CPU → CPU*, the rest is the
                         * Off-CPU* residual (computed in the normalize pass). */
                        double cpu_c;
                        if (is_measured)
                            cpu_c = dur_d > 0 ? (double)ev->cpu_ns * (ov/dur_d) : 0.0;
                        else
                            cpu_c = ov;   /* UNKNOWN → gap-inference */
                        if (cpu_c > ov) cpu_c = ov;
                        buckets[b].class_aas[PGWT_CLASS_CPU] += cpu_c;
                    } else {
                        /* wait: full wall time under its own label (any on-CPU
                         * spin stays here, not in CPU* — see time_model). */
                        buckets[b].class_aas[class_idx] += ov;
                    }
                }
                if (cat >= 0)
                    buckets[b].cat_aas[cat] += ov;
            }
        }

        /* Per-event total for sorting (pass 1) */
        if (evt_ht && !io_worker) {
            uint32_t h = ev->old_event & AAS_EVT_HT_MASK;
            while (evt_ht[h].total_ns > 0 && evt_ht[h].event_id != ev->old_event)
                h = (h + 1) & AAS_EVT_HT_MASK;
            evt_ht[h].event_id = ev->old_event;
            evt_ht[h].total_ns += ev->duration_ns;
        }
    }

    /* Convert accumulated ns → AAS (class + category buckets). T8 revision:
     * offcpu_aas held the per-bucket DB-ns total during accumulation; convert
     * it to the residual Off-CPU* = max(0, DB - CPU* - Σ waits) from exact
     * per-bucket totals (leak-immune), then normalize everything by bucket_ns. */
    double max_aas = 0.0;
    for (int i = 0; i < actual_buckets; i++) {
        double db_ns_bucket = buckets[i].offcpu_aas;
        double wait_ns_bucket = 0.0;
        for (int c = 0; c < PGWT_NUM_CLASSES; c++)
            if (c != PGWT_CLASS_CPU)
                wait_ns_bucket += buckets[i].class_aas[c];
        double off_ns = db_ns_bucket - buckets[i].class_aas[PGWT_CLASS_CPU]
                        - wait_ns_bucket;
        if (off_ns < 0) off_ns = 0.0;
        buckets[i].offcpu_aas = off_ns;

        double total = 0.0;
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            buckets[i].class_aas[c] /= (double)bucket_ns;
            total += buckets[i].class_aas[c];
        }
        /* Off-CPU* is part of active DB Time — include it in the total. */
        buckets[i].offcpu_aas /= (double)bucket_ns;
        total += buckets[i].offcpu_aas;
        for (int c = 0; c < PGWT_NUM_CATS; c++)
            buckets[i].cat_aas[c] /= (double)bucket_ns;
        if (total > max_aas)
            max_aas = total;
    }

    out->buckets     = buckets;
    out->num_buckets = actual_buckets;
    out->bucket_ns   = bucket_ns;
    out->max_aas     = max_aas;

    /* Per-event breakdown: sort top N, then pass 2 for per-bucket data */
    if (evt_ht) {
        if (max_series <= 0) max_series = AAS_MAX_EVENT_SERIES;
        if (max_series > AAS_MAX_EVENT_SERIES) max_series = AAS_MAX_EVENT_SERIES;

        /* Collect all events from hash table into event_series for sorting */
        struct pgwt_aas_event_series all[AAS_EVT_HT_SIZE];
        int n_all = 0;
        for (int i = 0; i < AAS_EVT_HT_SIZE; i++) {
            if (evt_ht[i].total_ns == 0) continue;
            all[n_all].event_id = evt_ht[i].event_id;
            all[n_all].total_aas = (double)evt_ht[i].total_ns / (double)range_ns;
            pgwt_event_full_name(evt_ht[i].event_id, all[n_all].name,
                                 sizeof(all[n_all].name));
            n_all++;
        }
        free(evt_ht);

        qsort(all, n_all, sizeof(all[0]), cmp_aas_event_series);

        int ns = n_all < max_series ? n_all : max_series;
        out->num_event_series = ns;
        for (int i = 0; i < ns; i++)
            out->event_series[i] = all[i];

        if (ns > 0) {
            /* Build lookup: event_id → series index (linear, ns is small) */
            out->event_aas = calloc((size_t)actual_buckets * ns, sizeof(double));
            if (!out->event_aas) { out->num_event_series = 0; return; }

            /* Pass 2: accumulate per-event per-bucket */
            for (int i = 0; i < count; i++) {
                const struct pgwt_trace_event *ev = &events[i];
                if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
                    continue;
                if (ev->flags & PGWT_EVENT_FLAG_IO_WORKER)
                    continue;   /* T2: excluded from AAS in any view */

                /* Find series index for this event */
                int si = -1;
                for (int s = 0; s < ns; s++) {
                    if (out->event_series[s].event_id == ev->old_event) {
                        si = s; break;
                    }
                }
                if (si < 0) continue;  /* not in top N */

                uint64_t ev_start = ev->timestamp_ns - ev->duration_ns;
                uint64_t ev_end   = ev->timestamp_ns;
                if (ev_end <= from_ns || ev_start >= to_ns)
                    continue;

                int first_b = (ev_start <= from_ns) ? 0
                             : (int)((ev_start - from_ns) / bucket_ns);
                int last_b  = (ev_end >= to_ns) ? actual_buckets - 1
                             : (int)(((ev_end - from_ns) - 1) / bucket_ns);
                if (last_b >= actual_buckets)
                    last_b = actual_buckets - 1;

                for (int b = first_b; b <= last_b; b++) {
                    uint64_t b_start = from_ns + (uint64_t)b * bucket_ns;
                    uint64_t b_end   = b_start + bucket_ns;
                    uint64_t o_start = ev_start > b_start ? ev_start : b_start;
                    uint64_t o_end   = ev_end < b_end ? ev_end : b_end;
                    if (o_end > o_start)
                        out->event_aas[b * ns + si] += (double)(o_end - o_start);
                }
            }

            /* Convert ns → AAS and compute max */
            double evt_max = 0.0;
            for (int b = 0; b < actual_buckets; b++) {
                double total = 0.0;
                for (int s = 0; s < ns; s++) {
                    out->event_aas[b * ns + s] /= (double)bucket_ns;
                    total += out->event_aas[b * ns + s];
                }
                if (total > evt_max) evt_max = total;
            }
            out->max_aas = evt_max;  /* override with event-level max */
        }
    }
}

/* ── Time Model ───────────────────────────────────────────── */

/* ── The Idle row and its NAMED sub-rows ──────────────────────────────────
 *
 * Before 2026-10-06 every idle interval was summed into one scalar and the
 * time model emitted no Idle row at all, so `idle_time_ms` was a number with
 * nothing behind it. Making the Timeout pacing sleeps idle would have turned
 * ~807 s of Timeout:CheckpointWriteDelay into an UNEXPLAINED figure on the
 * Overview, which is the opposite of the point.
 *
 * So the time model now emits:
 *
 *     Idle                              1,207,000 ms     indent 0
 *       Timeout:CheckpointWriteDelay      807,549 ms     indent 2
 *       Client:ClientRead                 399,000 ms     indent 2
 *
 * INDENTS, and why they are 0 and 2 rather than the more obvious 1 and 2:
 *
 *  - indent 0. tests/demo_rehearsal_lib.py time_model_conservation() sums
 *    EVERY indent==1 row and requires the total to equal db_time_ms within
 *    tolerance. An indent-1 Idle row is time that is BY DEFINITION not in
 *    db_time_ms, so it would break that check on contact -- and that file
 *    belongs to another branch. indent 0 also happens to be what the UI
 *    already expects: web/static/views/overview.js looks the row up with
 *    `r.indent === 0 && r.name.indexOf('Idle') >= 0`, and
 *    tests/mock_server.py has shipped `{"indent": 0, "name": "Idle"}` in its
 *    fixture all along. The real server simply never emitted it.
 *  - indent 2 for the children, which is what
 *    demo_rehearsal_lib.REQUIRED_WORKLOAD_EVENTS reads sub-event rows at.
 *    Adding rows there is additive: those names are "Lock:relation" and
 *    "Timeout:PgSleep", and PgSleep is not idle.
 *
 * HIDDEN events are deliberately NOT broken out. pgwt_is_hidden_event (the
 * Activity class) means "never display this row", and that has not changed:
 * background processes parked in their main loop are not a diagnostic. So the
 * Activity share of Idle stays inside the parent total, and the children
 * explain the VISIBLE idle events -- at most 7 of them today (ClientRead plus
 * the six pacing sleeps), which is why they are not truncated to 5 the way
 * class sub-events are.
 */
/* One row per VISIBLE idle event, plus one for the labelled remainder.
 * Tied to the rule rather than guessed: adding a name to
 * pacing_timeout_names[] without widening this is a COMPILE error, not a
 * silently truncated breakdown. */
#define MAX_IDLE_SUB_ROWS (PGWT_MAX_VISIBLE_IDLE_EVENTS + 1)
_Static_assert(MAX_IDLE_SUB_ROWS >= PGWT_MAX_VISIBLE_IDLE_EVENTS + 1,
               "the Idle breakdown must hold every visible idle event plus "
               "the remainder row");

struct idle_accum {
    uint32_t event_id;
    double   total_ns;
};

/* Add `ns` of `eid` to the visible-idle breakdown. Hidden events are counted
 * in the parent total by the caller but never given a row. */
static void idle_accum_add(struct idle_accum *ia, int *n, uint32_t eid,
                           double ns)
{
    if (pgwt_is_hidden_event(eid))
        return;
    for (int i = 0; i < *n; i++) {
        if (ia[i].event_id == eid) { ia[i].total_ns += ns; return; }
    }
    if (*n >= MAX_IDLE_SUB_ROWS)
        return;
    ia[*n].event_id = eid;
    ia[*n].total_ns = ns;
    (*n)++;
}

static int cmp_idle_desc(const void *a, const void *b)
{
    const struct idle_accum *x = a, *y = b;
    if (x->total_ns < y->total_ns) return 1;
    if (x->total_ns > y->total_ns) return -1;
    return 0;
}

/* Emit the Idle parent row plus one named row per visible idle event.
 * `rows`/`nr` are the time-model row array being built; the caller sized it
 * with 1 + MAX_IDLE_SUB_ROWS spare slots. pct_db_time and aas are deliberately
 * 0 on these rows: idle time has no share OF DB Time (it is excluded from it),
 * and reporting a percentage of a total it is not part of is how an excluded
 * number sneaks back into a reader's mental sum. */
/* `idle_time_ns` is the EXACT total; `ia` holds the per-event breakdown of
 * the VISIBLE part of it. The two differ by the hidden (Activity) share, and
 * on the summary path also by anything the bounded per-event tables could not
 * hold. The difference is emitted as one labelled row so the children always
 * sum to the parent -- an unexplained gap in a visible total is the thing
 * this whole change exists to remove, and leaving one would have reintroduced
 * it at the next level down. */
static int emit_idle_rows(struct pgwt_tm_row *rows, int nr,
                          double idle_time_ns,
                          struct idle_accum *ia, int n_ia,
                          double *excess_ns)
{
    if (excess_ns) *excess_ns = 0.0;
    if (idle_time_ns <= 0)
        return nr;

    snprintf(rows[nr].name, sizeof(rows[nr].name), "Idle");
    rows[nr].time_ms     = idle_time_ns / 1e6;
    rows[nr].pct_db_time = 0.0;
    rows[nr].aas         = 0.0;
    rows[nr].indent      = 0;
    nr++;

    qsort(ia, n_ia, sizeof(ia[0]), cmp_idle_desc);
    for (int i = 0; i < n_ia; i++) {
        if (ia[i].total_ns <= 0) continue;
        char buf[64];
        pgwt_event_full_name(ia[i].event_id, buf, sizeof(buf));
        snprintf(rows[nr].name, sizeof(rows[nr].name), "%s", buf);
        rows[nr].time_ms     = ia[i].total_ns / 1e6;
        rows[nr].pct_db_time = 0.0;
        rows[nr].aas         = 0.0;
        rows[nr].indent      = 2;
        nr++;
    }

    /* The remainder: hidden background parking (Activity) plus, on the
     * summary path, any visible idle event the bounded tables dropped. */
    double named = 0;
    for (int i = 0; i < n_ia; i++)
        if (ia[i].total_ns > 0) named += ia[i].total_ns;
    double other = idle_time_ns - named;
    /* ANY positive remainder gets a row. This was `> 1e3` -- "ignore float
     * dust" -- which silently dropped up to 1 us: a record whose only hidden
     * Activity was 500 ns produced an Idle parent with children that did not
     * sum to it, which is the precise contract this row exists to keep. The
     * values are sums of integer nanosecond durations, so there is no dust to
     * ignore. */
    if (other > 0) {
        snprintf(rows[nr].name, sizeof(rows[nr].name), "Other (background)");
        rows[nr].time_ms     = other / 1e6;
        rows[nr].pct_db_time = 0.0;
        rows[nr].aas         = 0.0;
        rows[nr].indent      = 2;
        nr++;
    } else if (other < 0 && excess_ns) {
        /* Children exceed the parent. Impossible on all three paths as they
         * stand (raw sums the same events it names; the summary totals are
         * writer-side scalars over a superset of what the bounded lists
         * name), so this is reported rather than clamped: it would mean the
         * writer and the reader disagree about what is idle. */
        *excess_ns = -other;
    }
    return nr;
}


/* Internal accumulator for class/event totals */
struct class_accum {
    char     name[32];
    double   total_ns;
};

struct event_accum {
    char     class_name[32];
    uint32_t event_id;
    double   total_ns;
};

static int cmp_class_desc(const void *a, const void *b)
{
    double da = ((const struct class_accum *)a)->total_ns;
    double db = ((const struct class_accum *)b)->total_ns;
    return (db > da) - (db < da);
}

static int cmp_event_desc(const void *a, const void *b)
{
    double da = ((const struct event_accum *)a)->total_ns;
    double db = ((const struct event_accum *)b)->total_ns;
    return (db > da) - (db < da);
}

void pgwt_compute_time_model(const struct pgwt_trace_event *events, int count,
                             const struct pgwt_filter *f,
                             uint64_t from_ns, uint64_t to_ns, double wall_ms,
                             struct pgwt_tm_result *out)
{
    memset(out, 0, sizeof(*out));
    out->wall_ms = wall_ms;

    /* Phase 1: accumulate per-class and per-(class, event) totals */
    struct class_accum classes[PGWT_NUM_CLASSES];
    memset(classes, 0, sizeof(classes));
    for (int i = 0; i < PGWT_NUM_CLASSES; i++)
        snprintf(classes[i].name, sizeof(classes[i].name), "%s",
                 pgwt_class_names[i]);

    /* Hash table for per-event accumulation — O(1) lookup */
    struct event_ht_entry *ev_ht = calloc(EVENT_HT_SIZE, sizeof(*ev_ht));

    double db_time_ns  = 0.0;
    double idle_time_ns = 0.0;
    /* Named breakdown of idle_time_ns -- see emit_idle_rows. */
    struct idle_accum idle_ev[MAX_IDLE_SUB_ROWS];
    int n_idle_ev = 0;
    /* T8 measured-CPU accumulators. cpu_measured_ns is the CPU* total (measured
     * where v3 cpu_ns exists, else the legacy full gap); offcpu_ns its off-CPU
     * sibling; the two sum to the old full CPU-class total, so DB Time is
     * unchanged. cpu_clamped/wait_gap are the self-checks (5.6). */
    double cpu_measured_ns = 0.0, offcpu_ns = 0.0;
    double cpu_clamped_ns = 0.0, wait_gap_cpu_ns = 0.0;
    int    has_measured_cpu = 0;
    double cat_ns[PGWT_NUM_CATS] = {0};
    /* io_worker busy%% needs the worker-pool size: count distinct io_worker
     * pids seen in the window (a handful — io_workers defaults to 3). */
    uint32_t iw_pids[64];
    int n_iw_pids = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev))
            continue;

        /* Every accumulator below is "time inside the requested window", so
         * a straddler contributes only its overlap (see event_window_ns).
         * clip_ns == duration_ns whenever the window does not cut the event,
         * which is always true for a whole-capture request. */
        uint64_t clip_u = event_window_ns(ev, from_ns, to_ns);
        double dur_ns = (double)clip_u;
        double frac   = event_window_frac(clip_u, ev->duration_ns);

        /* T2: io_worker time is OUTSIDE DB Time/idle — busy time goes to
         * its category slot (the utilization signal), idle time (their
         * instrumented IoWorkerMain wait) is dropped from the model. */
        if (ev->flags & PGWT_EVENT_FLAG_IO_WORKER) {
            int seen = 0;
            for (int k = 0; k < n_iw_pids; k++)
                if (iw_pids[k] == ev->pid) { seen = 1; break; }
            if (!seen && n_iw_pids < 64)
                iw_pids[n_iw_pids++] = ev->pid;
            if (!pgwt_is_idle_event(ev->old_event))
                cat_ns[PGWT_CAT_IO_WORKER] += dur_ns;
            continue;
        }

        if (pgwt_is_idle_event(ev->old_event)) {
            idle_time_ns += dur_ns;
            idle_accum_add(idle_ev, &n_idle_ev, ev->old_event, dur_ns);
            continue;
        }

        int cat = pgwt_event_category(ev);
        if (cat >= 0)
            cat_ns[cat] += dur_ns;

        db_time_ns += dur_ns;
        int cls_idx = pgwt_wait_class_index(ev->old_event);
        if (cls_idx == PGWT_CLASS_CPU) {
            /* T8 revision (docs/ROADMAP_AND_STATUS.md): CPU* is the SUM
             * of every interval's measured cpu_ns (conserved = true CPU);
             * Off-CPU* is a GLOBAL RESIDUAL computed after the loop, never a
             * per-interval dur-cpu split. `se.sum_exec_runtime` is only current
             * at a tick (1ms) or a context switch, so a sub-ms CPU burst before
             * a wait leaks (whole) into the FOLLOWING wait interval's delta; a
             * per-interval split therefore double-counts that leak. Summing all
             * cpu_ns (here + the wait branch below) is leak-immune, and the
             * residual recovers the real runqueue time from exact totals. A
             * stale-0 on a we==0 gap now correctly adds 0 (its burst is counted
             * in the interval it leaked to) — no full-gap fallback (which
             * over-stated CPU* past physical cores, Finding #1). */
            if (ev->cpu_ns != PGWT_CPU_NS_UNKNOWN) {
                /* Split measured CPU by the in-window fraction, the same
                 * ov/dur split pgwt_compute_aas uses per bucket. */
                double m = (double)ev->cpu_ns * frac;
                if (m > dur_ns) { cpu_clamped_ns += m - dur_ns; m = dur_ns; }
                cpu_measured_ns += m;
                has_measured_cpu = 1;
            } else {
                /* UNKNOWN: sampled/v2/legacy — never measured. Gap-inference:
                 * the whole gap is CPU*, and no Off-CPU* for this window. */
                cpu_measured_ns += dur_ns;
            }
        } else {
            /* The wait keeps its FULL wall time and its LABEL. With S3, a
             * backend that spins on-CPU during e.g. an LWLock has that spin
             * measured (ev->cpu_ns > 0), but it stays attributed to the wait
             * class — the label is the diagnostic ("LWLock:LockManager" tells
             * you it's lock contention; folding that CPU into an anonymous CPU*
             * bucket would hide the cause). So CPU* excludes wait spin; the
             * spin is tracked only as an observability sum (a future release
             * can sub-split each wait class into on-CPU-spin vs off-CPU-blocked
             * WITHIN the label — docs/S3 §9). */
            classes[cls_idx].total_ns += dur_ns;
            int slot = event_ht_find_or_insert(ev_ht, ev->old_event);
            if (slot >= 0)
                ev_ht[slot].total_ns += dur_ns;
            if (ev->cpu_ns != PGWT_CPU_NS_UNKNOWN) {
                wait_gap_cpu_ns += (double)ev->cpu_ns * frac; /* observability only */
                has_measured_cpu = 1;
            }
        }
    }

    /* The CPU class total is the measured (or legacy-inferred) CPU. */
    classes[PGWT_CLASS_CPU].total_ns = cpu_measured_ns;

    /* T8 revision: Off-CPU* = DB Time - CPU* - Σ wait durations (the residual
     * runqueue/unaccounted remainder of the on-CPU gaps). Computed from EXACT
     * totals only (DB Time and wait durations are timestamp-exact; CPU* is a
     * conserved sum), so it is immune to the per-interval sub-ms CPU leak. Only
     * meaningful where some cpu_ns was measured; UNKNOWN-only windows show CPU*
     * alone. Clamp at 0 (a positive clamp = CPU* exceeded DB-waits, a
     * measurement inconsistency worth counting). */
    if (has_measured_cpu) {
        double wait_total_ns = 0.0;
        for (int c = 0; c < PGWT_NUM_CLASSES; c++)
            if (c != PGWT_CLASS_CPU)
                wait_total_ns += classes[c].total_ns;
        double resid = db_time_ns - cpu_measured_ns - wait_total_ns;
        if (resid < 0) { cpu_clamped_ns += -resid; resid = 0.0; }
        offcpu_ns = resid;
    } else {
        offcpu_ns = 0.0;
    }

    for (int c = 0; c < PGWT_NUM_CATS; c++)
        out->cat_ms[c] = cat_ns[c] / 1e6;
    out->io_worker_busy_pct =
        (n_iw_pids > 0 && wall_ms > 0)
            ? 100.0 * (cat_ns[PGWT_CAT_IO_WORKER] / 1e6)
              / ((double)n_iw_pids * wall_ms)
            : 0.0;

    double db_time_ms  = db_time_ns / 1e6;
    double idle_ms     = idle_time_ns / 1e6;

    /* Phase 2: build result rows */
    /* Max rows: 1 (DB Time) + 1 (Off-CPU*) + NUM_CLASSES * (1 class + 5 sub)
     * + 1 (Idle) + MAX_IDLE_SUB_ROWS (its named children). */
    int max_rows = 2 + PGWT_NUM_CLASSES * 6 + 1 + MAX_IDLE_SUB_ROWS;
    struct pgwt_tm_row *rows = calloc(max_rows, sizeof(*rows));
    int nr = 0;

    /* DB Time row */
    snprintf(rows[nr].name, sizeof(rows[nr].name), "DB Time");
    rows[nr].time_ms     = db_time_ms;
    rows[nr].pct_db_time = 100.0;
    rows[nr].aas         = wall_ms > 0 ? db_time_ms / wall_ms : 0;
    rows[nr].indent      = 0;
    nr++;

    /* Sort classes by total descending */
    qsort(classes, PGWT_NUM_CLASSES, sizeof(classes[0]), cmp_class_desc);

    /* Convert hash table to sorted array for Phase 2 sub-event lookup */
    #define MAX_DISTINCT_EVENTS 4096
    struct event_accum *ev_accum = calloc(MAX_DISTINCT_EVENTS, sizeof(*ev_accum));
    int num_ev_accum = 0;
    for (int i = 0; i < EVENT_HT_SIZE && num_ev_accum < MAX_DISTINCT_EVENTS; i++) {
        if (ev_ht[i].event_id == 0) continue;
        int cls_idx = pgwt_wait_class_index(ev_ht[i].event_id);
        snprintf(ev_accum[num_ev_accum].class_name,
                 sizeof(ev_accum[num_ev_accum].class_name), "%s",
                 pgwt_class_names[cls_idx]);
        ev_accum[num_ev_accum].event_id = ev_ht[i].event_id;
        ev_accum[num_ev_accum].total_ns = ev_ht[i].total_ns;
        num_ev_accum++;
    }
    free(ev_ht);
    qsort(ev_accum, num_ev_accum, sizeof(ev_accum[0]), cmp_event_desc);

    for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
        if (classes[c].total_ns <= 0)
            continue;

        double cls_ms = classes[c].total_ns / 1e6;
        double pct = db_time_ms > 0 ? cls_ms / db_time_ms * 100.0 : 0;

        /* Class display name from lookup table */
        const char *display = classes[c].name;
        for (int k = 0; k < PGWT_NUM_CLASSES; k++) {
            if (strcasecmp(classes[c].name, pgwt_class_names[k]) == 0) {
                display = pgwt_class_display[k];
                break;
            }
        }
        /* T8 revision (#187 two-band split): the CPU *group* keeps its
         * historical meaning (running + waiting for a processor — see
         * docs/AAS_SEMANTICS_DECISION.md), rendered as two rows where the
         * exact tier can measure the split. Where it cannot (has_measured_cpu
         * unset — legacy full-gap inference, no sibling row below), the row
         * stays "CPU*": the asterisk's footnote now carries BOTH caveats
         * (uninstrumented code paths, and "this window cannot separate
         * running from waiting"), so a single un-split row is never
         * mislabeled as "running only". */
        if (strcasecmp(classes[c].name, "cpu") == 0)
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%s",
                     has_measured_cpu ? "CPU (running)" : "CPU*");
        else
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%.31s", display);
        rows[nr].time_ms     = cls_ms;
        rows[nr].pct_db_time = pct;
        rows[nr].aas         = wall_ms > 0 ? cls_ms / wall_ms : 0;
        rows[nr].indent      = 1;
        nr++;

        /* T8: "CPU (waiting for a core)" is a sibling of "CPU (running)" (the
         * measured off-CPU/runqueue-unaccounted remainder of on-CPU gaps).
         * Emitted only where measured cpu_ns existed (v3 exact data) —
         * sampled/v2 windows show the combined "CPU*" row alone (waiting time
         * is unavailable to split out, not zero). It is part of DB Time. */
        if (strcasecmp(classes[c].name, "cpu") == 0 && has_measured_cpu) {
            double off_ms = offcpu_ns / 1e6;
            snprintf(rows[nr].name, sizeof(rows[nr].name),
                     "CPU (waiting for a core)");
            rows[nr].time_ms     = off_ms;
            rows[nr].pct_db_time = db_time_ms > 0 ? off_ms / db_time_ms * 100.0 : 0;
            rows[nr].aas         = wall_ms > 0 ? off_ms / wall_ms : 0;
            rows[nr].indent      = 1;
            nr++;
        }

        /* Top 5 sub-events for this class (skip CPU — no meaningful sub-events) */
        if (strcasecmp(classes[c].name, "cpu") == 0)
            continue;

        int sub_count = 0;
        for (int j = 0; j < num_ev_accum && sub_count < 5; j++) {
            if (strcasecmp(ev_accum[j].class_name, classes[c].name) != 0)
                continue;

            double sub_ms  = ev_accum[j].total_ns / 1e6;
            double sub_pct = db_time_ms > 0 ? sub_ms / db_time_ms * 100.0 : 0;

            char buf[64];
            pgwt_event_full_name(ev_accum[j].event_id, buf, sizeof(buf));
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%s", buf);
            rows[nr].time_ms     = sub_ms;
            rows[nr].pct_db_time = sub_pct;
            rows[nr].aas         = wall_ms > 0 ? sub_ms / wall_ms : 0;
            rows[nr].indent      = 2;
            nr++;
            sub_count++;
        }
    }

    free(ev_accum);

    /* Idle LAST, so rows[0] stays "DB Time" (overview.js relies on that). */
    double idle_excess_ns = 0.0;
    nr = emit_idle_rows(rows, nr, idle_time_ns, idle_ev, n_idle_ev,
                        &idle_excess_ns);
    out->idle_children_excess_ms = idle_excess_ns / 1e6;

    out->rows        = rows;
    out->num_rows    = nr;
    out->db_time_ms  = db_time_ms;
    out->idle_time_ms = idle_ms;
    out->aas         = wall_ms > 0 ? db_time_ms / wall_ms : 0;
    /* T8 measured-CPU decomposition + self-checks. */
    out->cpu_ms          = cpu_measured_ns / 1e6;
    out->offcpu_ms       = offcpu_ns / 1e6;
    out->has_measured_cpu = has_measured_cpu;
    out->cpu_clamped_ms  = cpu_clamped_ns / 1e6;
    out->wait_gap_cpu_ms = wait_gap_cpu_ns / 1e6;
}

/* ── Top Events ───────────────────────────────────────────── */

struct top_event_accum {
    uint32_t event_id;
    uint64_t count;
    uint64_t total_ns;
    /* Exact-only accumulation for the latency columns (FID-3): samples do
     * not carry real durations and must not feed avg/max/percentiles. */
    uint64_t exact_count;
    uint64_t exact_total_ns;
    uint64_t max_ns;
    uint64_t hist[HISTOGRAM_BUCKETS];
};

/* Reuses EVENT_HT_SIZE/EVENT_HT_MASK defined above */

/* Histogram bucket upper boundaries in microseconds. The LAST bucket is
 * open-ended: it holds every duration of 16384 us or more (see
 * compute_duration_to_bucket: `if (us < 16384) return 14; return 15;`), so
 * its entry repeats the previous edge — that number is a FLOOR, not an
 * upper bound. */
static const uint64_t hist_upper_us[HISTOGRAM_BUCKETS] = {
    1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 16384
};

/* Sum of a latency histogram's buckets — the population the percentiles are
 * over. ZERO means there is no latency distribution to report at all (the
 * per-query summary records carry counts and totals but no histogram and no
 * max), and the caller must then gate p50/p95/p99 AND max to null rather
 * than let a percentile be computed against an empty histogram. */
static uint64_t hist_population(const uint64_t hist[HISTOGRAM_BUCKETS])
{
    uint64_t total = 0;
    for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
        total += hist[b];
    return total;
}

/* Percentile from the latency histogram. `total` MUST be hist_population()
 * of the same histogram (never a count from another source) and must be > 0.
 *
 * Nearest-rank: the value at rank ceil(total * pct), never rank 0. With the
 * old floor() threshold a single observation asked for rank 0, which the
 * first bucket satisfied vacuously — one 100 ms wait reported P50 = 1 us.
 *
 * `overflow` (issue #103) reports whether the percentile landed in the
 * open-ended top bucket. There the returned number is only the bucket's
 * lower edge (16.384 ms) and the true percentile can be arbitrarily larger —
 * exactly the case that printed "P50 = P95 = P99 = 16.4 ms" next to an
 * Avg of 1.8 s. It cannot be derived client-side: bucket 14 (8192..16383 us)
 * and bucket 15 (>= 16384 us) both yield 16384, so the flag must come from
 * the side that knows which bucket was hit. */
static double hist_percentile(const uint64_t hist[HISTOGRAM_BUCKETS],
                              uint64_t total, double pct, int *overflow)
{
    if (overflow) *overflow = 0;
    if (total == 0) return 0;          /* caller must have gated this */
    double rank = (double)total * pct;
    uint64_t threshold = (uint64_t)rank;
    if ((double)threshold < rank) threshold++;   /* ceil */
    if (threshold == 0) threshold = 1;           /* nearest-rank is 1-based */
    uint64_t cumulative = 0;
    for (int b = 0; b < HISTOGRAM_BUCKETS; b++) {
        cumulative += hist[b];
        if (cumulative >= threshold) {
            if (overflow) *overflow = (b == HISTOGRAM_BUCKETS - 1);
            return (double)hist_upper_us[b];
        }
    }
    /* Unreachable: threshold <= total == sum(hist), so some bucket always
     * satisfies it. Returning the top edge here would be a fabricated
     * bound, so it reports the last bucket it actually walked. */
    if (overflow) *overflow = (hist[HISTOGRAM_BUCKETS - 1] > 0);
    return (double)hist_upper_us[HISTOGRAM_BUCKETS - 1];
}

static int cmp_event_row_desc(const void *a, const void *b)
{
    double da = ((const struct pgwt_event_row *)a)->total_ms;
    double db = ((const struct pgwt_event_row *)b)->total_ms;
    return (db > da) - (db < da);
}

void pgwt_compute_top_events(const struct pgwt_trace_event *events, int count,
                             const struct pgwt_filter *f,
                             uint64_t from_ns, uint64_t to_ns, double wall_ms,
                             struct pgwt_events_result *out)
{
    memset(out, 0, sizeof(*out));

    /* Hash table: open addressing, linear probe */
    struct top_event_accum *ht = calloc(EVENT_HT_SIZE, sizeof(*ht));
    int num_entries = 0;
    uint64_t db_time_ns = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_hidden_event(ev->old_event))
            continue;
        /* T2: io_worker time never enters the DB-Time-ranked event list —
         * it would double-represent the same I/O already counted in the
         * requesting backends' AioIoCompletion waits. Their records stay
         * in the raw trace/timeline; the utilization metric covers them. */
        if (ev->flags & PGWT_EVENT_FLAG_IO_WORKER)
            continue;

        /* Time-in-window for the DB-Time columns; the LATENCY columns below
         * keep the event's full duration on purpose (a wait's latency is a
         * property of the wait, not of the window — clipping it would report
         * a p99 no wait ever had). */
        uint64_t clip_u = event_window_ns(ev, from_ns, to_ns);

        /* Idle-but-visible events (Client:ClientRead) appear in the list
         * but are excluded from DB Time. Their time is not part of the
         * DB-Time denominator, so a numeric %DB is meaningless for them
         * (it overshoots and makes the column sum past 100%). Their %DB is
         * marked with a sentinel below and rendered as "—". */
        if (!pgwt_is_idle_event(ev->old_event))
            db_time_ns += clip_u;

        /* Hash by event_id */
        uint32_t h = ev->old_event & EVENT_HT_MASK;
        while (ht[h].count > 0 && ht[h].event_id != ev->old_event)
            h = (h + 1) & EVENT_HT_MASK;

        if (ht[h].count == 0) {
            ht[h].event_id = ev->old_event;
            num_entries++;
        }
        /* Exact-window subtraction can split one delayed sampled observation
         * into multiple timeline fragments. Preserve its duration pieces but
         * count the physical observation once. */
        if (!(ev->flags & PGWT_EVENT_FLAG_SAMPLE_CONT) || ht[h].count == 0)
            ht[h].count++;
        ht[h].total_ns += clip_u;
        /* Latency stats from exact records only (FID-3), on FULL durations. */
        if (!(ev->flags & PGWT_EVENT_FLAG_SAMPLE)) {
            ht[h].exact_count++;
            ht[h].exact_total_ns += ev->duration_ns;
            if (ev->duration_ns > ht[h].max_ns)
                ht[h].max_ns = ev->duration_ns;
            ht[h].hist[compute_duration_to_bucket(ev->duration_ns)]++;
        }
    }

    double db_time_ms = (double)db_time_ns / 1e6;

    /* Collect into result rows */
    struct pgwt_event_row *rows = calloc(num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < EVENT_HT_SIZE; i++) {
        if (ht[i].count == 0)
            continue;

        struct pgwt_event_row *r = &rows[nr];
        r->event_id = ht[i].event_id;
        r->count    = ht[i].count;
        r->total_ms = (double)ht[i].total_ns / 1e6;
        /* Latency columns from exact data only; exact_count == 0 flags
         * them as unavailable (sampled-only row). */
        r->exact_count = ht[i].exact_count;
        r->avg_us   = ht[i].exact_count > 0
                     ? (double)ht[i].exact_total_ns / (double)ht[i].exact_count / 1000.0
                     : 0;
        /* Percentiles come from the histogram's own population, never from
         * a count accumulated elsewhere; an empty histogram means there is
         * no distribution and no max to report (see hist_population). */
        uint64_t hpop = hist_population(ht[i].hist);
        r->has_latency_dist = hpop > 0;
        r->max_us   = (double)ht[i].max_ns / 1000.0;
        if (hpop > 0) {
            r->p50_us = hist_percentile(ht[i].hist, hpop, 0.50,
                                        &r->p50_overflow);
            r->p95_us = hist_percentile(ht[i].hist, hpop, 0.95,
                                        &r->p95_overflow);
            r->p99_us = hist_percentile(ht[i].hist, hpop, 0.99,
                                        &r->p99_overflow);
        }
        /* Idle-but-visible events have time but no meaningful share of DB
         * Time; flag their %DB with a sentinel so it renders as "—". */
        if (pgwt_is_idle_event(ht[i].event_id))
            r->pct_db = PGWT_PCT_DB_IDLE;
        else
            r->pct_db = db_time_ms > 0 ? r->total_ms / db_time_ms * 100.0 : 0;
        r->aas      = wall_ms > 0 ? r->total_ms / wall_ms : 0;

        if (ht[i].event_id == 0)
            snprintf(r->name, sizeof(r->name), "CPU*");
        else
            pgwt_event_full_name(ht[i].event_id, r->name, sizeof(r->name));
        nr++;
    }

    free(ht);

    qsort(rows, nr, sizeof(rows[0]), cmp_event_row_desc);

    out->rows        = rows;
    out->num_rows    = nr;
    out->db_time_ms  = db_time_ms;
}

/* ── Top Sessions ─────────────────────────────────────────── */

struct session_accum {
    uint32_t pid;
    uint64_t total_ns;
    uint64_t cpu_ns;
    /* Per-wait hash table — O(1) lookup */
    struct wait_ht_entry waits[WAIT_HT_SIZE];
};

#define SESSION_HT_SIZE MAX_BACKENDS

static int cmp_session_row_desc(const void *a, const void *b)
{
    double da = ((const struct pgwt_session_row *)a)->db_time_ms;
    double db = ((const struct pgwt_session_row *)b)->db_time_ms;
    return (db > da) - (db < da);
}

void pgwt_compute_top_sessions(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f,
                               uint64_t from_ns, uint64_t to_ns, double wall_ms,
                               struct pgwt_sessions_result *out)
{
    (void)wall_ms;
    memset(out, 0, sizeof(*out));

    /* Hash table keyed by PID, open addressing */
    struct session_accum *ht = calloc(SESSION_HT_SIZE, sizeof(*ht));
    int num_entries = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
            continue;
        if (ev->flags & PGWT_EVENT_FLAG_IO_WORKER)
            continue;   /* T2: io_workers are not sessions doing DB work */

        uint32_t h = ev->pid % SESSION_HT_SIZE;
        while (ht[h].pid != 0 && ht[h].pid != ev->pid)
            h = (h + 1) % SESSION_HT_SIZE;

        if (ht[h].pid == 0) {
            ht[h].pid = ev->pid;
            num_entries++;
        }
        /* Per-session DB time is time inside the window (see
         * event_window_ns); cpu_pct/wait_pct are ratios of the same clipped
         * total, so they stay consistent. */
        uint64_t clip_u = event_window_ns(ev, from_ns, to_ns);
        ht[h].total_ns += clip_u;

        if (ev->old_event == 0) {
            ht[h].cpu_ns += clip_u;
        } else {
            /* O(1) hash lookup for per-wait totals */
            int wslot = wait_ht_find_or_insert(ht[h].waits, ev->old_event);
            if (wslot >= 0)
                ht[h].waits[wslot].total_ns += clip_u;
        }
    }

    /* Collect results */
    struct pgwt_session_row *rows = calloc(num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < SESSION_HT_SIZE; i++) {
        if (ht[i].pid == 0)
            continue;

        struct pgwt_session_row *r = &rows[nr];
        r->pid        = ht[i].pid;
        r->db_time_ms = (double)ht[i].total_ns / 1e6;

        double db = (double)ht[i].total_ns;
        r->cpu_pct  = db > 0 ? (double)ht[i].cpu_ns / db * 100.0 : 0;
        r->wait_pct = 100.0 - r->cpu_pct;

        /* Find top wait from per-PID hash table */
        uint32_t top_id = 0;
        uint64_t top_ns = 0;
        for (int j = 0; j < WAIT_HT_SIZE; j++) {
            if (ht[i].waits[j].event_id != 0 &&
                ht[i].waits[j].total_ns > top_ns) {
                top_ns = ht[i].waits[j].total_ns;
                top_id = ht[i].waits[j].event_id;
            }
        }
        r->top_wait_id = top_id;
        if (top_id == 0)
            snprintf(r->top_wait, sizeof(r->top_wait), "CPU*");
        else
            pgwt_event_full_name(top_id, r->top_wait, sizeof(r->top_wait));

        nr++;
    }

    free(ht);

    qsort(rows, nr, sizeof(rows[0]), cmp_session_row_desc);

    out->rows     = rows;
    out->num_rows = nr;
}

/* ── Top Queries ──────────────────────────────────────────── */

struct query_accum {
    uint64_t query_id;
    uint64_t count;
    uint64_t total_ns;   /* time INSIDE the requested window (DB-Time column) */
    uint64_t full_ns;    /* sum of full durations — the avg latency column */
    uint64_t class_ns[PGWT_NUM_CLASSES]; /* per-class time breakdown */
    /* Per-wait hash table — O(1) lookup */
    struct wait_ht_entry waits[WAIT_HT_SIZE];
};

#define QUERY_HT_SIZE 2048
#define QUERY_HT_MASK (QUERY_HT_SIZE - 1)

static int cmp_query_row_desc(const void *a, const void *b)
{
    double da = ((const struct pgwt_query_row *)a)->total_ms;
    double db = ((const struct pgwt_query_row *)b)->total_ms;
    return (db > da) - (db < da);
}

void pgwt_compute_top_queries(const struct pgwt_trace_event *events, int count,
                              const struct pgwt_filter *f,
                              uint64_t from_ns, uint64_t to_ns, double wall_ms,
                              struct pgwt_queries_result *out)
{
    (void)wall_ms;
    memset(out, 0, sizeof(*out));

    struct query_accum *ht = calloc(QUERY_HT_SIZE, sizeof(*ht));
    int num_entries = 0;
    uint64_t db_time_ns = 0;
    uint64_t unattributed_ns = 0, backfilled_ns = 0;
    uint64_t unattributed_count = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
            continue;
        if (ev->flags & PGWT_EVENT_FLAG_IO_WORKER)
            continue;   /* T2: io_workers are structurally query-less */
        /* Time inside the requested window (see event_window_ns). Only the
         * avg column keeps the full duration — it is a per-observation
         * latency, not a share of the window. */
        uint64_t clip_u = event_window_ns(ev, from_ns, to_ns);
        if (ev->query_id == 0) {
            /* #128: foreground non-idle time whose command never reported
             * an id is reported, not dropped. */
            if (ev->flags & PGWT_EVENT_FLAG_QUERY_UNATTRIB) {
                unattributed_ns += clip_u;
                unattributed_count++;
            }
            continue;
        }
        if (ev->flags & PGWT_EVENT_FLAG_QUERY_BACKFILL)
            backfilled_ns += clip_u;

        db_time_ns += clip_u;

        uint32_t h = (uint32_t)(ev->query_id ^ (ev->query_id >> 32)) & QUERY_HT_MASK;
        while (ht[h].count > 0 && ht[h].query_id != ev->query_id)
            h = (h + 1) & QUERY_HT_MASK;

        if (ht[h].count == 0) {
            ht[h].query_id = ev->query_id;
            num_entries++;
        }
        /* Split sample continuations conserve time, not observation count. */
        if (!(ev->flags & PGWT_EVENT_FLAG_SAMPLE_CONT) || ht[h].count == 0)
            ht[h].count++;
        ht[h].total_ns += clip_u;
        ht[h].full_ns  += ev->duration_ns;
        ht[h].class_ns[pgwt_wait_class_index(ev->old_event)] += clip_u;

        /* O(1) hash lookup for per-wait totals */
        if (ev->old_event != 0) {
            int wslot = wait_ht_find_or_insert(ht[h].waits, ev->old_event);
            if (wslot >= 0)
                ht[h].waits[wslot].total_ns += clip_u;
        }
    }

    double db_time_ms = (double)db_time_ns / 1e6;

    struct pgwt_query_row *rows = calloc(num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < QUERY_HT_SIZE; i++) {
        if (ht[i].count == 0)
            continue;

        struct pgwt_query_row *r = &rows[nr];
        r->query_id = ht[i].query_id;
        r->count    = ht[i].count;
        r->total_ms = (double)ht[i].total_ns / 1e6;
        /* Latency, so from FULL durations (== total_ns for any window that
         * does not cut an event, hence unchanged for whole-capture). */
        r->avg_us   = ht[i].count > 0
                     ? (double)ht[i].full_ns / (double)ht[i].count / 1000.0
                     : 0;
        r->pct_db   = db_time_ms > 0 ? r->total_ms / db_time_ms * 100.0 : 0;

        /* Find top wait + build per-event breakdown from hash table */
        uint32_t top_id = 0;
        uint64_t top_ns = 0;

        /* Collect non-empty entries from wait hash table */
        struct { uint32_t eid; uint64_t ns; } wlist[WAIT_HT_SIZE];
        int nw = 0;
        for (int j = 0; j < WAIT_HT_SIZE; j++) {
            if (ht[i].waits[j].event_id == 0) continue;
            if (ht[i].waits[j].total_ns > top_ns) {
                top_ns = ht[i].waits[j].total_ns;
                top_id = ht[i].waits[j].event_id;
            }
            wlist[nw].eid = ht[i].waits[j].event_id;
            wlist[nw].ns  = ht[i].waits[j].total_ns;
            nw++;
        }

        r->top_wait_id = top_id;
        if (top_id == 0)
            snprintf(r->top_wait, sizeof(r->top_wait), "CPU*");
        else
            pgwt_event_full_name(top_id, r->top_wait, sizeof(r->top_wait));

        for (int c = 0; c < PGWT_NUM_CLASSES; c++)
            r->class_ms[c] = (double)ht[i].class_ns[c] / 1e6;

        /* Add CPU as a pseudo-event in the list */
        if (ht[i].class_ns[PGWT_CLASS_CPU] > 0) {
            wlist[nw].eid = 0;  /* CPU */
            wlist[nw].ns = ht[i].class_ns[PGWT_CLASS_CPU];
            if (wlist[nw].ns > top_ns) {
                top_ns = wlist[nw].ns;
                top_id = 0;
                snprintf(r->top_wait, sizeof(r->top_wait), "CPU*");
                r->top_wait_id = 0;
            }
            nw++;
        }

        /* Per-event breakdown: pick top 16 by time desc */
        r->num_events = 0;
        for (int k = 0; k < 16 && k < nw; k++) {
            /* Find max remaining */
            int best = k;
            for (int j = k + 1; j < nw; j++) {
                if (wlist[j].ns > wlist[best].ns)
                    best = j;
            }
            if (best != k) {
                uint32_t te = wlist[k].eid; uint64_t tn = wlist[k].ns;
                wlist[k].eid = wlist[best].eid; wlist[k].ns = wlist[best].ns;
                wlist[best].eid = te; wlist[best].ns = tn;
            }
            r->event_ids[k] = wlist[k].eid;
            r->event_ms[k]  = (double)wlist[k].ns / 1e6;
            r->num_events++;
        }

        nr++;
    }

    free(ht);

    qsort(rows, nr, sizeof(rows[0]), cmp_query_row_desc);

    out->rows       = rows;
    out->num_rows   = nr;
    out->db_time_ms = db_time_ms;
    out->unattributed_ms    = (double)unattributed_ns / 1e6;
    out->unattributed_count = unattributed_count;
    out->backfilled_ms      = (double)backfilled_ns / 1e6;
}

/* ── Heatmap (latency distribution over time) ─────────────── */

void pgwt_compute_heatmap(const struct pgwt_trace_event *events, int count,
                          const struct pgwt_filter *f,
                          uint64_t from_ns, uint64_t to_ns, int num_buckets,
                          struct pgwt_heatmap_result *out)
{
    memset(out, 0, sizeof(*out));

    uint64_t range_ns = to_ns - from_ns;
    if (range_ns == 0 || num_buckets <= 0)
        return;

    /* Raw events have ns precision — no minimum bucket width */
    uint64_t bucket_ns = (range_ns + (uint64_t)num_buckets - 1) / (uint64_t)num_buckets;
    if (bucket_ns == 0)
        bucket_ns = 1;

    int actual_buckets = (int)((range_ns + bucket_ns - 1) / bucket_ns);
    if (actual_buckets > 100000) actual_buckets = 100000;  /* sanity clamp */
    size_t grid_size = (size_t)actual_buckets * HISTOGRAM_BUCKETS;

    uint64_t *grid = calloc(grid_size, sizeof(uint64_t));
    uint64_t *times = malloc((size_t)actual_buckets * sizeof(uint64_t));
    if (!grid || !times) {
        free(grid);
        free(times);
        return;
    }

    for (int i = 0; i < actual_buckets; i++)
        times[i] = from_ns + (uint64_t)i * bucket_ns;

    uint64_t total = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        /* Visibility filter: keep Client:ClientRead in the latency heatmap. */
        if (!pgwt_filter_matches(f, ev) || pgwt_is_hidden_event(ev->old_event))
            continue;
        /* Samples carry no real durations — a latency distribution built
         * from normalized sample periods is fabricated (FID-3). */
        if (ev->flags & PGWT_EVENT_FLAG_SAMPLE)
            continue;

        uint64_t ev_ts = ev->timestamp_ns;
        if (ev_ts < from_ns || ev_ts >= to_ns)
            continue;

        int time_b = (int)((ev_ts - from_ns) / bucket_ns);
        if (time_b >= actual_buckets)
            time_b = actual_buckets - 1;

        int lat_b = (int)compute_duration_to_bucket(ev->duration_ns);

        grid[time_b * HISTOGRAM_BUCKETS + lat_b]++;
        total++;
    }

    /* Find max cell for color scaling */
    uint64_t max_count = 0;
    for (size_t i = 0; i < grid_size; i++) {
        if (grid[i] > max_count)
            max_count = grid[i];
    }

    out->grid         = grid;
    out->num_buckets  = actual_buckets;
    out->bucket_ns    = bucket_ns;
    out->times        = times;
    out->max_count    = max_count;
    out->total_events = total;
}

/* ══════════════════════════════════════════════════════════════
 * Summary-based compute functions
 * Each pgwt_summary_accum record represents 1 second of pre-aggregated data.
 * ══════════════════════════════════════════════════════════════ */

/* Helper: check if a summary event matches a filter.
 * Marker event ids are rejected outright: summaries written before the
 * FID-4 write-side fix may still contain accumulated marker rows. */
static int summary_event_matches_filter(const struct pgwt_filter *f,
                                         uint32_t event_id)
{
    if (PGWT_IS_MARKER(event_id))
        return 0;
    if (f->class_name[0] != '\0') {
        const char *cls = pgwt_class_name(event_id);
        if (strcasecmp(cls, f->class_name) != 0)
            return 0;
    }
    if (f->event_id != 0 && event_id != f->event_id)
        return 0;
    return 1;
}

/* ── AAS from summaries ───────────────────────────────────── */

struct aas_summary_ctx {
    struct pgwt_aas_bucket *buckets;
    int      actual_buckets;
    uint64_t from_ns;
    uint64_t bucket_ns;
    const struct pgwt_filter *f;
    int has_class_filter;
    int has_event_filter;
    int has_pid_filter;
    int has_query_filter;
};

/* summary_clientread_ns() / summary_query_clientread_ns() USED TO LIVE HERE.
 *
 * They subtracted Client:ClientRead back out of the LUMPED `class_ns` that
 * summary v1/v2 wrote, because the writer accumulated idle events into the
 * class totals with no idle check at all. Three problems, all gone in v3:
 *
 *  1. They were a THIRD hardcoded copy of the idle rule --
 *     `WEI(PG_WAIT_CLIENT, 0)`, spelled out, in compute.c. Widening the rule
 *     (the Timeout pacing sleeps) would have left the Timeout class total
 *     flowing into db_time_ns untouched: the predicate would say "idle" and
 *     this path would keep charging it as DB Time. The demo's 900 s window
 *     takes this path (should_use_summaries), so the predicate change alone
 *     would have reported the OLD DB Time there.
 *  2. The system-wide one only worked if ClientRead had claimed one of the
 *     1024 SUMMARY_MAX_EVENTS slots; if it had not, the subtraction silently
 *     did nothing.
 *  3. The per-query one read `top_events`, which holds at most 8 entries, so
 *     a query with 8 busier events subtracted zero.
 *
 * v3 excludes idle at the WRITER (src/summary_writer.c accum_event /
 * accum_query_add), so `class_ns` is already load-only and there is nothing to
 * subtract. Idle totals and the named Idle sub-rows come from `events[]` /
 * `top_events[]`, which still record every event. Mixed-version windows cannot
 * reach here: the reader refuses non-current files and pgwt-server preflights
 * the window (pgwt_summaries_window_current).
 */

static int aas_summary_visitor(const struct pgwt_summary_accum *rec, void *arg)
{
    struct aas_summary_ctx *ctx = arg;
    uint64_t wall = rec->second_wall_ns;

    int bi = (int)((wall - ctx->from_ns) / ctx->bucket_ns);
    if (bi >= ctx->actual_buckets) bi = ctx->actual_buckets - 1;
    if (bi < 0) bi = 0;

    /* PID filter: summaries don't have per-PID class breakdown */
    if (ctx->has_pid_filter)
        return 0;

    /* Query filter: use per-query class_ns (v2 data) */
    if (ctx->has_query_filter) {
        for (int q = 0; q < SUMMARY_MAX_QUERIES; q++) {
            const struct pgwt_summary_query *sq = &rec->queries[q];
            if (sq->query_id == 0 && sq->count == 0) continue;
            if (sq->query_id != ctx->f->query_id) continue;
            /* v3: sq->class_ns is already load-only (idle excluded at the
             * writer), so it is used as-is. The ACTIVITY skip is kept as a
             * belt: that class is idle by definition and must never add load
             * even if a future writer change put something there. */
            for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
                if (c == PGWT_CLASS_ACTIVITY) continue;
                ctx->buckets[bi].class_aas[c] += (double)sq->class_ns[c];
            }
            break;
        }
        return 0;
    }

    if (!ctx->has_class_filter && !ctx->has_event_filter) {
        /* v3: rec->class_ns is already load-only -- see the note where
         * summary_clientread_ns used to be. */
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            if (c == PGWT_CLASS_ACTIVITY) continue;
            ctx->buckets[bi].class_aas[c] += (double)rec->class_ns[c];
        }
    } else {
        for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
            const struct pgwt_summary_event *se = &rec->events[e];
            if (se->event_id == 0 && se->count == 0) continue;
            if (pgwt_is_idle_event(se->event_id)) continue;
            if (!summary_event_matches_filter(ctx->f, se->event_id)) continue;
            int cls = pgwt_wait_class_index(se->event_id);
            ctx->buckets[bi].class_aas[cls] += (double)se->total_ns;
        }
    }
    return 0;
}

void pgwt_compute_aas_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, int num_buckets,
    struct pgwt_aas_result *out)
{
    memset(out, 0, sizeof(*out));

    uint64_t range_ns = to_ns - from_ns;
    if (range_ns == 0 || num_buckets <= 0) {
        out->bucket_ns = 1000000000ULL;
        return;
    }

    uint64_t bucket_ns = (range_ns + (uint64_t)num_buckets - 1) / (uint64_t)num_buckets;
    if (bucket_ns < 1000000000ULL)
        bucket_ns = 1000000000ULL;

    int actual_buckets = (int)((range_ns + bucket_ns - 1) / bucket_ns);
    struct pgwt_aas_bucket *buckets = calloc(actual_buckets, sizeof(*buckets));
    if (!buckets) return;

    for (int i = 0; i < actual_buckets; i++)
        buckets[i].start_ns = from_ns + (uint64_t)i * bucket_ns;

    struct aas_summary_ctx ctx = {
        .buckets = buckets,
        .actual_buckets = actual_buckets,
        .from_ns = from_ns,
        .bucket_ns = bucket_ns,
        .f = f,
        .has_class_filter = (f->class_name[0] != '\0'),
        .has_event_filter = (f->event_id != 0),
        .has_pid_filter   = (f->pid != 0),
        .has_query_filter = (f->query_id != 0),
    };

    pgwt_visit_summaries(trace_dir, from_ns, to_ns, aas_summary_visitor, &ctx);

    /* Convert ns → AAS */
    double max_aas = 0.0;
    for (int i = 0; i < actual_buckets; i++) {
        double total = 0.0;
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            buckets[i].class_aas[c] /= (double)bucket_ns;
            total += buckets[i].class_aas[c];
        }
        if (total > max_aas) max_aas = total;
    }

    out->buckets     = buckets;
    out->num_buckets = actual_buckets;
    out->bucket_ns   = bucket_ns;
    out->max_aas     = max_aas;
}

/* ── Time Model from summaries ────────────────────────────── */

struct tm_summary_ctx {
    struct class_accum classes[PGWT_NUM_CLASSES];
    struct event_accum *ev_accum;
    int    num_ev_accum;
    double db_time_ns;
    double idle_time_ns;
    struct idle_accum idle_ev[MAX_IDLE_SUB_ROWS];
    int    n_idle_ev;
    const struct pgwt_filter *f;
};

static int tm_summary_visitor(const struct pgwt_summary_accum *rec, void *arg)
{
    struct tm_summary_ctx *ctx = arg;
    const struct pgwt_filter *f = ctx->f;

    /* Query filter: use per-query class_ns + top_events */
    if (f->query_id != 0) {
        for (int q = 0; q < SUMMARY_MAX_QUERIES; q++) {
            const struct pgwt_summary_query *sq = &rec->queries[q];
            if (sq->query_id == 0 && sq->count == 0) continue;
            if (sq->query_id != f->query_id) continue;

            /* v3: sq->class_ns is load-only already. The idle time for this
             * query comes from top_events[], which still records idle events
             * -- so it is both counted into the Idle total and given a NAMED
             * sub-row, instead of being an anonymous scalar. */
            for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
                if (c == PGWT_CLASS_ACTIVITY) continue;   /* idle by definition */
                ctx->classes[c].total_ns += (double)sq->class_ns[c];
                ctx->db_time_ns += (double)sq->class_ns[c];
            }
            /* v3: the query's idle TOTAL is the exact scalar, never the sum
             * of top_events[] -- that list holds 8 entries, so a query with 8
             * busier non-idle events reported Idle = 0 beside a correct DB
             * Time and the two stopped accounting for the window. The named
             * children below are still drawn from the bounded list; whatever
             * they do not cover becomes the "Other (background)" row. */
            ctx->idle_time_ns += (double)sq->idle_ns;
            for (int j = 0; j < sq->num_top_events; j++) {
                uint32_t eid = sq->top_events[j].event_id;
                if (pgwt_is_idle_event(eid)) {
                    idle_accum_add(ctx->idle_ev, &ctx->n_idle_ev, eid,
                                   (double)sq->top_events[j].total_ns);
                    continue;
                }
                double ns = (double)sq->top_events[j].total_ns;
                int cls_idx = pgwt_wait_class_index(eid);
                int found = -1;
                for (int k = 0; k < ctx->num_ev_accum; k++) {
                    if (ctx->ev_accum[k].event_id == eid) { found = k; break; }
                }
                if (found >= 0) {
                    ctx->ev_accum[found].total_ns += ns;
                } else if (ctx->num_ev_accum < MAX_DISTINCT_EVENTS) {
                    snprintf(ctx->ev_accum[ctx->num_ev_accum].class_name,
                             sizeof(ctx->ev_accum[ctx->num_ev_accum].class_name),
                             "%s", pgwt_class_names[cls_idx]);
                    ctx->ev_accum[ctx->num_ev_accum].event_id = eid;
                    ctx->ev_accum[ctx->num_ev_accum].total_ns = ns;
                    ctx->num_ev_accum++;
                }
            }
            break;
        }
        return 0;
    }

    /* If no class/event filter: use class_ns directly */
    int unfiltered = (f->class_name[0] == '\0' && f->event_id == 0);
    if (unfiltered) {
        /* v3: rec->class_ns is load-only already -- see the note where
         * summary_clientread_ns used to be. */
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            if (c == PGWT_CLASS_ACTIVITY) continue;   /* idle by definition */
            ctx->classes[c].total_ns += (double)rec->class_ns[c];
            ctx->db_time_ns += (double)rec->class_ns[c];
        }
        /* EXACT, and deliberately not the sum of events[]: that table is
         * bounded (rec->events_overflow says when it filled) and since v3 it
         * is the only per-event idle source, so summing it would under-report
         * Idle while DB Time stayed right. */
        ctx->idle_time_ns += (double)rec->idle_ns;
    }

    /* Per-event stats: iterate event entries */
    for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
        const struct pgwt_summary_event *se = &rec->events[e];
        if (se->event_id == 0 && se->count == 0) continue;
        if (!summary_event_matches_filter(f, se->event_id)) continue;
        if (pgwt_is_idle_event(se->event_id)) {
            /* UNFILTERED: the total already came from rec->idle_ns above, so
             * this only builds the named breakdown.
             *
             * FILTERED: the exact scalar covers the whole record, which is a
             * different population from what the caller asked for, so the
             * total is summed from the matching events instead. The raw path
             * applies pgwt_filter_matches before its own idle branch, so a
             * class=timeout request reports only the Timeout class's idle time
             * there too -- the two paths must agree, filter or no filter,
             * because cross_validate compares them. */
            if (!unfiltered)
                ctx->idle_time_ns += (double)se->total_ns;
            idle_accum_add(ctx->idle_ev, &ctx->n_idle_ev,
                           se->event_id, (double)se->total_ns);
            continue;
        }

        int cls_idx = pgwt_wait_class_index(se->event_id);

        /* With filter: accumulate class totals from events */
        if (f->class_name[0] != '\0' || f->event_id != 0) {
            ctx->classes[cls_idx].total_ns += (double)se->total_ns;
            ctx->db_time_ns += (double)se->total_ns;
        }

        /* Sub-event accumulation */
        int found = -1;
        for (int j = 0; j < ctx->num_ev_accum; j++) {
            if (ctx->ev_accum[j].event_id == se->event_id) {
                found = j; break;
            }
        }
        if (found >= 0) {
            ctx->ev_accum[found].total_ns += (double)se->total_ns;
        } else if (ctx->num_ev_accum < MAX_DISTINCT_EVENTS) {
            snprintf(ctx->ev_accum[ctx->num_ev_accum].class_name,
                     sizeof(ctx->ev_accum[ctx->num_ev_accum].class_name),
                     "%s", pgwt_class_names[cls_idx]);
            ctx->ev_accum[ctx->num_ev_accum].event_id = se->event_id;
            ctx->ev_accum[ctx->num_ev_accum].total_ns = (double)se->total_ns;
            ctx->num_ev_accum++;
        }
    }
    return 0;
}

void pgwt_compute_time_model_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_tm_result *out)
{
    memset(out, 0, sizeof(*out));
    out->wall_ms = wall_ms;

    struct tm_summary_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.f = f;
    for (int i = 0; i < PGWT_NUM_CLASSES; i++)
        snprintf(ctx.classes[i].name, sizeof(ctx.classes[i].name), "%s",
                 pgwt_class_names[i]);
    ctx.ev_accum = calloc(MAX_DISTINCT_EVENTS, sizeof(*ctx.ev_accum));

    pgwt_visit_summaries(trace_dir, from_ns, to_ns, tm_summary_visitor, &ctx);

    double db_time_ms = ctx.db_time_ns / 1e6;
    double idle_ms    = ctx.idle_time_ns / 1e6;

    /* Build result rows (same format as raw compute) */
    int max_rows = 1 + PGWT_NUM_CLASSES * 6 + 1 + MAX_IDLE_SUB_ROWS;
    struct pgwt_tm_row *rows = calloc(max_rows, sizeof(*rows));
    int nr = 0;

    snprintf(rows[nr].name, sizeof(rows[nr].name), "DB Time");
    rows[nr].time_ms     = db_time_ms;
    rows[nr].pct_db_time = 100.0;
    rows[nr].aas         = wall_ms > 0 ? db_time_ms / wall_ms : 0;
    rows[nr].indent      = 0;
    nr++;

    qsort(ctx.classes, PGWT_NUM_CLASSES, sizeof(ctx.classes[0]), cmp_class_desc);
    qsort(ctx.ev_accum, ctx.num_ev_accum, sizeof(ctx.ev_accum[0]), cmp_event_desc);

    for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
        if (ctx.classes[c].total_ns <= 0) continue;

        double cls_ms = ctx.classes[c].total_ns / 1e6;
        double pct = db_time_ms > 0 ? cls_ms / db_time_ms * 100.0 : 0;

        const char *display2 = ctx.classes[c].name;
        for (int k = 0; k < PGWT_NUM_CLASSES; k++) {
            if (strcasecmp(ctx.classes[c].name, pgwt_class_names[k]) == 0) {
                display2 = pgwt_class_display[k];
                break;
            }
        }
        if (strcasecmp(ctx.classes[c].name, "cpu") == 0)
            snprintf(rows[nr].name, sizeof(rows[nr].name), "CPU*");
        else
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%.31s", display2);
        rows[nr].time_ms     = cls_ms;
        rows[nr].pct_db_time = pct;
        rows[nr].aas         = wall_ms > 0 ? cls_ms / wall_ms : 0;
        rows[nr].indent      = 1;
        nr++;

        if (strcasecmp(ctx.classes[c].name, "cpu") == 0) continue;

        int sub_count = 0;
        for (int j = 0; j < ctx.num_ev_accum && sub_count < 5; j++) {
            if (strcasecmp(ctx.ev_accum[j].class_name, ctx.classes[c].name) != 0)
                continue;
            double sub_ms  = ctx.ev_accum[j].total_ns / 1e6;
            double sub_pct = db_time_ms > 0 ? sub_ms / db_time_ms * 100.0 : 0;

            char buf[64];
            pgwt_event_full_name(ctx.ev_accum[j].event_id, buf, sizeof(buf));
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%s", buf);
            rows[nr].time_ms     = sub_ms;
            rows[nr].pct_db_time = sub_pct;
            rows[nr].aas         = wall_ms > 0 ? sub_ms / wall_ms : 0;
            rows[nr].indent      = 2;
            nr++;
            sub_count++;
        }
    }

    free(ctx.ev_accum);

    /* Idle LAST, so rows[0] stays "DB Time" -- same as the raw path. */
    double idle_excess_ns = 0.0;
    nr = emit_idle_rows(rows, nr, ctx.idle_time_ns, ctx.idle_ev,
                        ctx.n_idle_ev, &idle_excess_ns);
    out->idle_children_excess_ms = idle_excess_ns / 1e6;

    out->rows         = rows;
    out->num_rows     = nr;
    out->db_time_ms   = db_time_ms;
    out->idle_time_ms = idle_ms;
    out->aas          = wall_ms > 0 ? db_time_ms / wall_ms : 0;
}

/* ── Top Events from summaries ────────────────────────────── */

struct te_summary_ctx {
    struct top_event_accum *ht;
    int      num_entries;
    uint64_t db_time_ns;
    const struct pgwt_filter *f;
};

static int te_summary_visitor(const struct pgwt_summary_accum *rec, void *arg)
{
    struct te_summary_ctx *ctx = arg;

    /* Query filter: use per-query top_events */
    if (ctx->f->query_id != 0) {
        for (int q = 0; q < SUMMARY_MAX_QUERIES; q++) {
            const struct pgwt_summary_query *sq = &rec->queries[q];
            if (sq->query_id == 0 && sq->count == 0) continue;
            if (sq->query_id != ctx->f->query_id) continue;

            for (int j = 0; j < sq->num_top_events; j++) {
                uint32_t eid = sq->top_events[j].event_id;
                if (pgwt_is_hidden_event(eid)) continue;
                if (!summary_event_matches_filter(ctx->f, eid)) continue;

                /* Idle-but-visible (ClientRead): list it, exclude from DB Time. */
                if (!pgwt_is_idle_event(eid))
                    ctx->db_time_ns += sq->top_events[j].total_ns;

                uint32_t h = eid & EVENT_HT_MASK;
                while (ctx->ht[h].count > 0 && ctx->ht[h].event_id != eid)
                    h = (h + 1) & EVENT_HT_MASK;

                if (ctx->ht[h].count == 0) {
                    ctx->ht[h].event_id = eid;
                    ctx->num_entries++;
                }
                ctx->ht[h].count += sq->top_events[j].count;
                ctx->ht[h].total_ns += sq->top_events[j].total_ns;
                /* Summaries are built from exact records only. */
                ctx->ht[h].exact_count += sq->top_events[j].count;
                ctx->ht[h].exact_total_ns += sq->top_events[j].total_ns;
            }
            break;
        }
        return 0;
    }

    for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
        const struct pgwt_summary_event *se = &rec->events[e];
        if (se->event_id == 0 && se->count == 0) continue;
        if (pgwt_is_hidden_event(se->event_id)) continue;
        if (!summary_event_matches_filter(ctx->f, se->event_id)) continue;

        /* Idle-but-visible (ClientRead): list it, exclude from DB Time. */
        if (!pgwt_is_idle_event(se->event_id))
            ctx->db_time_ns += se->total_ns;

        uint32_t h = se->event_id & EVENT_HT_MASK;
        while (ctx->ht[h].count > 0 && ctx->ht[h].event_id != se->event_id)
            h = (h + 1) & EVENT_HT_MASK;

        if (ctx->ht[h].count == 0) {
            ctx->ht[h].event_id = se->event_id;
            ctx->num_entries++;
        }
        ctx->ht[h].count += se->count;
        ctx->ht[h].total_ns += se->total_ns;
        /* Summaries are built from exact records only. */
        ctx->ht[h].exact_count += se->count;
        ctx->ht[h].exact_total_ns += se->total_ns;
        if (se->max_ns > ctx->ht[h].max_ns)
            ctx->ht[h].max_ns = se->max_ns;
        for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
            ctx->ht[h].hist[b] += se->histogram[b];
    }
    return 0;
}

void pgwt_compute_top_events_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_events_result *out)
{
    memset(out, 0, sizeof(*out));

    struct te_summary_ctx ctx = {
        .ht = calloc(EVENT_HT_SIZE, sizeof(*ctx.ht)),
        .num_entries = 0,
        .db_time_ns = 0,
        .f = f,
    };
    if (!ctx.ht) return;

    pgwt_visit_summaries(trace_dir, from_ns, to_ns, te_summary_visitor, &ctx);

    double db_time_ms = (double)ctx.db_time_ns / 1e6;

    struct pgwt_event_row *rows = calloc(ctx.num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < EVENT_HT_SIZE; i++) {
        if (ctx.ht[i].count == 0) continue;

        struct pgwt_event_row *row = &rows[nr];
        row->event_id = ctx.ht[i].event_id;
        row->count    = ctx.ht[i].count;
        row->total_ms = (double)ctx.ht[i].total_ns / 1e6;
        row->exact_count = ctx.ht[i].exact_count;
        row->avg_us   = ctx.ht[i].exact_count > 0
                       ? (double)ctx.ht[i].exact_total_ns / (double)ctx.ht[i].exact_count / 1000.0 : 0;
        /* The per-QUERY summary records (struct pgwt_summary_query_event)
         * carry count and total_ns only — no histogram, no max. Such a row
         * has a real count/total/avg but NO latency distribution: gate
         * p50/p95/p99 and max to null rather than answer a percentile from
         * an all-zero histogram, which used to produce ">= 16.4ms" next to
         * Max 0us on every query-drilled Events row (#103 review). */
        uint64_t hpop = hist_population(ctx.ht[i].hist);
        row->has_latency_dist = hpop > 0;
        row->max_us   = (double)ctx.ht[i].max_ns / 1000.0;
        if (hpop > 0) {
            row->p50_us = hist_percentile(ctx.ht[i].hist, hpop, 0.50,
                                          &row->p50_overflow);
            row->p95_us = hist_percentile(ctx.ht[i].hist, hpop, 0.95,
                                          &row->p95_overflow);
            row->p99_us = hist_percentile(ctx.ht[i].hist, hpop, 0.99,
                                          &row->p99_overflow);
        }
        /* Idle-but-visible events have time but no meaningful share of DB
         * Time; flag their %DB with a sentinel so it renders as "—". */
        if (pgwt_is_idle_event(ctx.ht[i].event_id))
            row->pct_db = PGWT_PCT_DB_IDLE;
        else
            row->pct_db = db_time_ms > 0 ? row->total_ms / db_time_ms * 100.0 : 0;
        row->aas      = wall_ms > 0 ? row->total_ms / wall_ms : 0;

        if (ctx.ht[i].event_id == 0)
            snprintf(row->name, sizeof(row->name), "CPU*");
        else
            pgwt_event_full_name(ctx.ht[i].event_id, row->name, sizeof(row->name));
        nr++;
    }

    free(ctx.ht);
    qsort(rows, nr, sizeof(rows[0]), cmp_event_row_desc);

    out->rows       = rows;
    out->num_rows   = nr;
    out->db_time_ms = db_time_ms;
}

/* ── Top Sessions from summaries ──────────────────────────── */

struct ts_summary_ht_entry {
    uint32_t pid;
    uint64_t db_time_ns;
    uint64_t cpu_ns;
    uint32_t top_wait_id;
    uint64_t top_wait_ns;
};

struct ts_summary_ctx {
    struct ts_summary_ht_entry *ht;
    int num_entries;
    const struct pgwt_filter *f;
};

static int ts_summary_visitor(const struct pgwt_summary_accum *rec, void *arg)
{
    struct ts_summary_ctx *ctx = arg;

    for (int s = 0; s < SUMMARY_MAX_SESSIONS; s++) {
        const struct pgwt_summary_session *ss = &rec->sessions[s];
        if (ss->pid == 0 && ss->db_time_ns == 0) continue;
        if (ctx->f->pid != 0 && ss->pid != ctx->f->pid) continue;

        uint32_t h = ss->pid % SESSION_HT_SIZE;
        while (ctx->ht[h].pid != 0 && ctx->ht[h].pid != ss->pid)
            h = (h + 1) % SESSION_HT_SIZE;

        if (ctx->ht[h].pid == 0) {
            ctx->ht[h].pid = ss->pid;
            ctx->num_entries++;
        }
        ctx->ht[h].db_time_ns += ss->db_time_ns;
        ctx->ht[h].cpu_ns += ss->cpu_ns;
        if (ss->top_wait_ns > ctx->ht[h].top_wait_ns) {
            ctx->ht[h].top_wait_id = ss->top_wait_id;
            ctx->ht[h].top_wait_ns = ss->top_wait_ns;
        }
    }
    return 0;
}

void pgwt_compute_top_sessions_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_sessions_result *out)
{
    (void)wall_ms;
    memset(out, 0, sizeof(*out));

    struct ts_summary_ctx ctx = {
        .ht = calloc(SESSION_HT_SIZE, sizeof(*ctx.ht)),
        .num_entries = 0,
        .f = f,
    };
    if (!ctx.ht) return;

    pgwt_visit_summaries(trace_dir, from_ns, to_ns, ts_summary_visitor, &ctx);

    struct pgwt_session_row *rows = calloc(ctx.num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < SESSION_HT_SIZE; i++) {
        if (ctx.ht[i].pid == 0) continue;

        struct pgwt_session_row *row = &rows[nr];
        row->pid        = ctx.ht[i].pid;
        row->db_time_ms = (double)ctx.ht[i].db_time_ns / 1e6;
        double db = (double)ctx.ht[i].db_time_ns;
        row->cpu_pct  = db > 0 ? (double)ctx.ht[i].cpu_ns / db * 100.0 : 0;
        row->wait_pct = 100.0 - row->cpu_pct;
        row->top_wait_id = ctx.ht[i].top_wait_id;
        if (ctx.ht[i].top_wait_id == 0)
            snprintf(row->top_wait, sizeof(row->top_wait), "CPU*");
        else
            pgwt_event_full_name(ctx.ht[i].top_wait_id, row->top_wait,
                                  sizeof(row->top_wait));
        nr++;
    }

    free(ctx.ht);
    qsort(rows, nr, sizeof(rows[0]), cmp_session_row_desc);

    out->rows     = rows;
    out->num_rows = nr;
}

/* ── Top Queries from summaries ───────────────────────────── */

struct tq_summary_ht_entry {
    uint64_t query_id;
    uint64_t count;
    uint64_t total_ns;
    uint32_t top_wait_id;
    uint64_t top_wait_ns;
    uint64_t class_ns[PGWT_NUM_CLASSES];
};

struct tq_summary_ctx {
    struct tq_summary_ht_entry *ht;
    int      num_entries;
    uint64_t db_time_ns;
    const struct pgwt_filter *f;
};

static int tq_summary_visitor(const struct pgwt_summary_accum *rec, void *arg)
{
    struct tq_summary_ctx *ctx = arg;

    /* Resolve class filter index (-1 = no filter) */
    int filter_cls = -1;
    if (ctx->f->class_name[0] != '\0') {
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            if (strcasecmp(ctx->f->class_name, pgwt_class_names[c]) == 0) {
                filter_cls = c;
                break;
            }
        }
        if (filter_cls < 0) return 0;
    }

    for (int q = 0; q < SUMMARY_MAX_QUERIES; q++) {
        const struct pgwt_summary_query *sq = &rec->queries[q];
        if (sq->query_id == 0 && sq->count == 0) continue;
        if (ctx->f->query_id != 0 && sq->query_id != ctx->f->query_id) continue;

        /* Determine effective time and top wait, respecting class+event filters */
        uint64_t effective_ns = sq->total_ns;
        uint32_t rec_top_wait_id = sq->top_wait_id;
        uint64_t rec_top_wait_ns = sq->top_wait_ns;

        if (filter_cls >= 0 || ctx->f->event_id != 0) {
            /* Use top_events[] to compute class/event-filtered totals */
            effective_ns = 0;
            rec_top_wait_id = 0;
            rec_top_wait_ns = 0;

            for (int j = 0; j < sq->num_top_events; j++) {
                uint32_t eid = sq->top_events[j].event_id;
                /* IDLE IS NOT DB TIME, on this path either. top_events[]
                 * deliberately retains idle events so the Events tab can show
                 * them, so this branch has to filter them out itself -- it is
                 * the one place v3's writer-side exclusion does not reach,
                 * because it reads the per-event list rather than class_ns.
                 * The raw pgwt_compute_top_queries skips idle records
                 * outright; without this the same window answered differently
                 * depending on which path served it, charging a foreground
                 * VACUUM's Timeout:VacuumDelay (and, before 2026-10-06,
                 * Client:ClientRead) as DB Time under a class filter. */
                if (pgwt_is_idle_event(eid))
                    continue;
                /* Class filter */
                if (filter_cls >= 0 && pgwt_wait_class_index(eid) != filter_cls)
                    continue;
                /* Event filter */
                if (ctx->f->event_id != 0 && eid != ctx->f->event_id)
                    continue;

                effective_ns += sq->top_events[j].total_ns;
                if (sq->top_events[j].total_ns > rec_top_wait_ns) {
                    rec_top_wait_ns = sq->top_events[j].total_ns;
                    rec_top_wait_id = eid;
                }
            }

            /* If top_events didn't cover this class, fall back to class_ns */
            if (effective_ns == 0 && filter_cls >= 0 && ctx->f->event_id == 0) {
                int has_class_ns = 0;
                for (int c = 0; c < PGWT_NUM_CLASSES; c++)
                    if (sq->class_ns[c] > 0) { has_class_ns = 1; break; }
                if (has_class_ns)
                    effective_ns = sq->class_ns[filter_cls];
                else if (pgwt_wait_class_index(sq->top_wait_id) == filter_cls)
                    effective_ns = sq->total_ns;
                rec_top_wait_id = sq->top_wait_id;
                rec_top_wait_ns = effective_ns;
            }

            if (effective_ns == 0) continue;
        }

        ctx->db_time_ns += effective_ns;

        uint32_t h = (uint32_t)(sq->query_id ^ (sq->query_id >> 32)) & QUERY_HT_MASK;
        while (ctx->ht[h].count > 0 && ctx->ht[h].query_id != sq->query_id)
            h = (h + 1) & QUERY_HT_MASK;

        if (ctx->ht[h].count == 0) {
            ctx->ht[h].query_id = sq->query_id;
            ctx->num_entries++;
        }
        ctx->ht[h].count += sq->count;
        ctx->ht[h].total_ns += effective_ns;
        /* Per-class breakdown — when filtering, only accumulate the filtered class */
        if (filter_cls >= 0) {
            ctx->ht[h].class_ns[filter_cls] += effective_ns;
        } else {
            int has_class_ns = 0;
            for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
                ctx->ht[h].class_ns[c] += sq->class_ns[c];
                if (sq->class_ns[c] > 0) has_class_ns = 1;
            }
            if (!has_class_ns) {
                int cls = pgwt_wait_class_index(sq->top_wait_id);
                ctx->ht[h].class_ns[cls] += sq->total_ns;
            }
        }
        if (rec_top_wait_ns > ctx->ht[h].top_wait_ns) {
            ctx->ht[h].top_wait_id = rec_top_wait_id;
            ctx->ht[h].top_wait_ns = rec_top_wait_ns;
        }
    }
    return 0;
}

void pgwt_compute_top_queries_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_queries_result *out)
{
    (void)wall_ms;
    memset(out, 0, sizeof(*out));

    struct tq_summary_ctx ctx = {
        .ht = calloc(QUERY_HT_SIZE, sizeof(*ctx.ht)),
        .num_entries = 0,
        .db_time_ns = 0,
        .f = f,
    };
    if (!ctx.ht) return;

    pgwt_visit_summaries(trace_dir, from_ns, to_ns, tq_summary_visitor, &ctx);

    double db_time_ms = (double)ctx.db_time_ns / 1e6;

    struct pgwt_query_row *rows = calloc(ctx.num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < QUERY_HT_SIZE; i++) {
        if (ctx.ht[i].count == 0) continue;

        struct pgwt_query_row *row = &rows[nr];
        row->query_id = ctx.ht[i].query_id;
        row->count    = ctx.ht[i].count;
        row->total_ms = (double)ctx.ht[i].total_ns / 1e6;
        row->avg_us   = ctx.ht[i].count > 0
                       ? (double)ctx.ht[i].total_ns / (double)ctx.ht[i].count / 1000.0 : 0;
        row->pct_db   = db_time_ms > 0 ? row->total_ms / db_time_ms * 100.0 : 0;
        row->top_wait_id = ctx.ht[i].top_wait_id;
        if (ctx.ht[i].top_wait_id == 0)
            snprintf(row->top_wait, sizeof(row->top_wait), "CPU*");
        else
            pgwt_event_full_name(ctx.ht[i].top_wait_id, row->top_wait,
                                  sizeof(row->top_wait));
        for (int c = 0; c < PGWT_NUM_CLASSES; c++)
            row->class_ms[c] = (double)ctx.ht[i].class_ns[c] / 1e6;
        nr++;
    }

    free(ctx.ht);
    qsort(rows, nr, sizeof(rows[0]), cmp_query_row_desc);

    out->rows       = rows;
    out->num_rows   = nr;
    out->db_time_ms = db_time_ms;
}

/* ── Heatmap from summaries ──────────────────────────────── */

struct hm_summary_ctx {
    uint64_t *grid;        /* [num_buckets * HISTOGRAM_BUCKETS] */
    uint64_t *times;       /* [num_buckets] */
    int      num_buckets;
    uint64_t from_ns;
    uint64_t bucket_ns;
    uint64_t max_count;
    uint64_t total_events;
    const struct pgwt_filter *f;
};

static int hm_summary_visitor(const struct pgwt_summary_accum *rec, void *arg)
{
    struct hm_summary_ctx *ctx = arg;
    uint64_t wall = rec->second_wall_ns;

    int bi = (int)((wall - ctx->from_ns) / ctx->bucket_ns);
    if (bi >= ctx->num_buckets) bi = ctx->num_buckets - 1;
    if (bi < 0) bi = 0;

    for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
        const struct pgwt_summary_event *se = &rec->events[e];
        if (se->event_id == 0 && se->count == 0) continue;
        /* Visibility filter: keep Client:ClientRead in the latency heatmap. */
        if (pgwt_is_hidden_event(se->event_id)) continue;
        if (!summary_event_matches_filter(ctx->f, se->event_id)) continue;

        for (int b = 0; b < HISTOGRAM_BUCKETS; b++) {
            uint64_t v = se->histogram[b];
            if (v == 0) continue;

            uint64_t idx = (uint64_t)bi * HISTOGRAM_BUCKETS + b;
            ctx->grid[idx] += v;
            ctx->total_events += v;

            if (ctx->grid[idx] > ctx->max_count)
                ctx->max_count = ctx->grid[idx];
        }
    }
    return 0;
}

void pgwt_compute_heatmap_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, int num_buckets,
    struct pgwt_heatmap_result *out)
{
    memset(out, 0, sizeof(*out));

    uint64_t range_ns = to_ns - from_ns;
    if (range_ns == 0 || num_buckets <= 0) return;

    uint64_t bucket_ns = (range_ns + (uint64_t)num_buckets - 1) / (uint64_t)num_buckets;
    if (bucket_ns < 1000000000ULL)
        bucket_ns = 1000000000ULL;

    int actual_buckets = (int)((range_ns + bucket_ns - 1) / bucket_ns);
    uint64_t *grid = calloc((size_t)actual_buckets * HISTOGRAM_BUCKETS, sizeof(uint64_t));
    uint64_t *times = calloc(actual_buckets, sizeof(uint64_t));
    if (!grid || !times) { free(grid); free(times); return; }

    for (int i = 0; i < actual_buckets; i++)
        times[i] = from_ns + (uint64_t)i * bucket_ns;

    struct hm_summary_ctx ctx = {
        .grid = grid,
        .times = times,
        .num_buckets = actual_buckets,
        .from_ns = from_ns,
        .bucket_ns = bucket_ns,
        .max_count = 0,
        .total_events = 0,
        .f = f,
    };

    pgwt_visit_summaries(trace_dir, from_ns, to_ns, hm_summary_visitor, &ctx);

    out->grid         = grid;
    out->num_buckets  = actual_buckets;
    out->bucket_ns    = bucket_ns;
    out->times        = times;
    out->max_count    = ctx.max_count;
    out->total_events = ctx.total_events;
}

/* ── Transitions ──────────────────────────────────────────── */

struct trans_accum {
    uint32_t from_event;
    uint32_t to_event;
    uint64_t count;
    uint64_t total_ns;
};

#define TRANS_HT_SIZE 4096
#define TRANS_HT_MASK (TRANS_HT_SIZE - 1)

static int cmp_trans_desc(const void *a, const void *b)
{
    uint64_t ca = ((const struct trans_accum *)a)->count;
    uint64_t cb = ((const struct trans_accum *)b)->count;
    return (cb > ca) - (cb < ca);
}

void pgwt_compute_transitions(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f, int max_rows,
                               struct pgwt_transitions_result *out)
{
    memset(out, 0, sizeof(*out));

    /* Hash table for (from, to) pairs */
    struct trans_accum *ht = calloc(TRANS_HT_SIZE, sizeof(*ht));
    int num_entries = 0;
    uint64_t total = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        /* Exact-required view: normalized sample points are not state
         * transitions. In a mixed window they remain available to sampled
         * views only. */
        if (ev->flags & PGWT_EVENT_FLAG_SAMPLE)
            continue;
        if (!pgwt_filter_matches(f, ev))
            continue;
        /* Visibility filter: Client:ClientRead transitions stay in the graph. */
        if (pgwt_is_hidden_event(ev->old_event) || pgwt_is_hidden_event(ev->new_event))
            continue;
        if (ev->new_event == PGWT_EVENT_EXIT)
            continue;
        if (PGWT_IS_MARKER(ev->old_event) || PGWT_IS_MARKER(ev->new_event))
            continue;

        uint32_t from = ev->old_event;
        uint32_t to   = ev->new_event;

        /* Hash: combine from and to */
        uint32_t h = ((from * 0x45d9f3b) ^ (to * 0x9e3779b9)) & TRANS_HT_MASK;
        while (ht[h].count > 0 &&
               (ht[h].from_event != from || ht[h].to_event != to))
            h = (h + 1) & TRANS_HT_MASK;

        if (ht[h].count == 0) {
            ht[h].from_event = from;
            ht[h].to_event = to;
            num_entries++;
        }
        ht[h].count++;
        ht[h].total_ns += ev->duration_ns;
        total++;
    }

    /* Collect into flat array for sorting */
    struct trans_accum *arr = calloc(num_entries, sizeof(*arr));
    int n = 0;
    for (int i = 0; i < TRANS_HT_SIZE && n < num_entries; i++) {
        if (ht[i].count > 0)
            arr[n++] = ht[i];
    }
    free(ht);

    qsort(arr, n, sizeof(arr[0]), cmp_trans_desc);

    /* Build result rows */
    int nr = n < max_rows ? n : max_rows;
    struct pgwt_transition_row *rows = calloc(nr, sizeof(*rows));
    for (int i = 0; i < nr; i++) {
        rows[i].from_event = arr[i].from_event;
        rows[i].to_event   = arr[i].to_event;
        rows[i].count      = arr[i].count;
        rows[i].total_ns   = arr[i].total_ns;
        pgwt_event_full_name(arr[i].from_event, rows[i].from_name,
                             sizeof(rows[i].from_name));
        pgwt_event_full_name(arr[i].to_event, rows[i].to_name,
                             sizeof(rows[i].to_name));
    }
    free(arr);

    out->rows = rows;
    out->num_rows = nr;
    out->total_rows = n;
    out->total_transitions = total;
}

/* ── Fingerprints ─────────────────────────────────────────── */

struct fp_accum {
    uint64_t query_id;
    uint64_t class_ns[PGWT_NUM_CLASSES];
    uint64_t total_ns;
    uint64_t total_transitions;
    /* Top transition tracking */
    struct trans_accum top_trans[64];
    int num_trans;
};

#define FP_HT_SIZE 2048
#define FP_HT_MASK (FP_HT_SIZE - 1)

static int cmp_fp_desc(const void *a, const void *b)
{
    uint64_t ta = ((const struct pgwt_fingerprint_row *)a)->total_transitions;
    uint64_t tb = ((const struct pgwt_fingerprint_row *)b)->total_transitions;
    return (tb > ta) - (tb < ta);
}

void pgwt_compute_fingerprints(const struct pgwt_trace_event *events, int count,
                                const struct pgwt_filter *f,
                                struct pgwt_fingerprint_result *out)
{
    memset(out, 0, sizeof(*out));

    struct fp_accum *ht = calloc(FP_HT_SIZE, sizeof(*ht));
    int num_entries = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        /* Fingerprints describe exact transition structure; sampled records
         * (including clipped continuations) must not fabricate self-edges. */
        if (ev->flags & PGWT_EVENT_FLAG_SAMPLE)
            continue;
        if (!pgwt_filter_matches(f, ev))
            continue;
        if (pgwt_is_idle_event(ev->old_event))
            continue;
        if (ev->query_id == 0)
            continue;

        /* Find query in hash table */
        uint32_t h = hash64(ev->query_id, FP_HT_MASK);
        while (ht[h].total_ns > 0 && ht[h].query_id != ev->query_id)
            h = (h + 1) & FP_HT_MASK;

        if (ht[h].total_ns == 0) {
            ht[h].query_id = ev->query_id;
            num_entries++;
        }

        int cls = pgwt_wait_class_index(ev->old_event);
        ht[h].class_ns[cls] += ev->duration_ns;
        ht[h].total_ns += ev->duration_ns;

        /* Count transition if not exit */
        if (ev->new_event != PGWT_EVENT_EXIT &&
            !pgwt_is_idle_event(ev->new_event)) {
            ht[h].total_transitions++;

            /* Track top transition for this query */
            uint32_t from = ev->old_event;
            uint32_t to   = ev->new_event;
            int found = -1;
            for (int j = 0; j < ht[h].num_trans; j++) {
                if (ht[h].top_trans[j].from_event == from &&
                    ht[h].top_trans[j].to_event == to) {
                    found = j;
                    break;
                }
            }
            if (found >= 0) {
                ht[h].top_trans[found].count++;
            } else if (ht[h].num_trans < 64) {
                int n = ht[h].num_trans;
                ht[h].top_trans[n].from_event = from;
                ht[h].top_trans[n].to_event = to;
                ht[h].top_trans[n].count = 1;
                ht[h].num_trans++;
            }
        }
    }

    /* Build result rows */
    struct pgwt_fingerprint_row *rows = calloc(num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < FP_HT_SIZE; i++) {
        if (ht[i].total_ns == 0) continue;

        struct pgwt_fingerprint_row *r = &rows[nr];
        r->query_id = ht[i].query_id;
        r->total_transitions = ht[i].total_transitions;

        /* Compute class percentages */
        for (int c = 0; c < PGWT_NUM_CLASSES; c++)
            r->class_pct[c] = ht[i].total_ns > 0
                ? (double)ht[i].class_ns[c] / (double)ht[i].total_ns * 100.0
                : 0;

        /* Find top transition */
        uint64_t best_count = 0;
        for (int j = 0; j < ht[i].num_trans; j++) {
            if (ht[i].top_trans[j].count > best_count) {
                best_count = ht[i].top_trans[j].count;
                r->top_from = ht[i].top_trans[j].from_event;
                r->top_to = ht[i].top_trans[j].to_event;
            }
        }

        /* Build signature: "CPU:45%|IO:30%|Lock:25% → IO:DataFileRead→CPU*" */
        char sig[128] = {0};
        int pos = 0;
        /* Sort classes by percentage for consistent signatures */
        struct { int idx; double pct; } cpairs[PGWT_NUM_CLASSES];
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            cpairs[c].idx = c;
            cpairs[c].pct = r->class_pct[c];
        }
        /* Simple bubble sort for 11 elements */
        for (int a = 0; a < PGWT_NUM_CLASSES - 1; a++)
            for (int b = a + 1; b < PGWT_NUM_CLASSES; b++)
                if (cpairs[b].pct > cpairs[a].pct) {
                    int ti = cpairs[a].idx; double tp = cpairs[a].pct;
                    cpairs[a].idx = cpairs[b].idx; cpairs[a].pct = cpairs[b].pct;
                    cpairs[b].idx = ti; cpairs[b].pct = tp;
                }

        for (int c = 0; c < PGWT_NUM_CLASSES && cpairs[c].pct >= 1.0; c++) {
            if (pos > 0) pos += snprintf(sig + pos, sizeof(sig) - pos, "|");
            pos += snprintf(sig + pos, sizeof(sig) - pos, "%s:%.0f%%",
                           pgwt_class_display[cpairs[c].idx], cpairs[c].pct);
        }
        snprintf(r->signature, sizeof(r->signature), "%s", sig);

        nr++;
    }

    free(ht);
    qsort(rows, nr, sizeof(rows[0]), cmp_fp_desc);

    out->rows = rows;
    out->num_rows = nr;
}

/* ── Concurrency / Burst Detection ────────────────────────── */

/*
 * For each event, we know: PID was in old_event from (timestamp - duration)
 * to timestamp. Peak concurrency in a bucket is the maximum, over wait
 * events, of the number of DISTINCT pids whose interval overlaps that
 * bucket; a burst is burst_threshold+ distinct pids ENTERING the same wait
 * event within burst_window_ns of each other.
 *
 * #276 — what this used to do and why it was wrong. Candidate intervals were
 * materialised into a fixed 100,000-entry array filled in ARRIVAL order
 * ("limit for memory"), and four more fixed arrays bounded the rest:
 * ev_pids[64] distinct events per bucket, pids[128] distinct pids per event
 * per bucket, burst_cap=256 bursts, pids[64] pids per burst. Every one of
 * them dropped data silently. The interval cap was reached on every capture
 * worth looking at: at the UI's default 900 s window on the demo workload
 * the function examined the first ~60 s of events and reported the other
 * 825 s as `max: 0` — a flat zero line that reads as "the database was
 * idle", with "fidelity": "exact" beside it.
 *
 * Phase 1 therefore materialises nothing. Each qualifying event is bucketed
 * ON THE FLY into every bucket its interval overlaps, and the distinct
 * (bucket, event, pid) triples live in a hash map that grows
 * (src/triple_map.h). Memory is then a function of the chart's resolution
 * and of the workload's shape — buckets × distinct events × distinct pids —
 * never of the event count, so there is no cap left to declare: every bucket
 * sees every event in it, whether the window holds ten thousand events or
 * ten million.
 *
 * Phase 2 (bursts) needs entry times in time order at 10 ms resolution,
 * which no per-bucket pass supplies, so it does keep an array — but sized to
 * the qualifying intervals actually present (16 bytes each, against the
 * 48-byte event array the caller already holds and which load_max_events
 * already bounds), never to a fixed 100,000. Bursts are then selected the
 * way the rest of this codebase selects a bounded subset: the LARGEST burst
 * in each bucket, so the returned set covers the whole window at the
 * resolution the client asked for, with `bursts_total` declaring how many
 * onsets were detected. Selecting the top N by size alone would have put
 * every marker back at the left edge as soon as sizes tie, which on a steady
 * workload they do.
 *
 * Allocation failure anywhere sets `failed` and the caller reports the answer
 * as ABSENT (code "compute_failed"). A short answer that looks complete is
 * the whole shape of this issue.
 */

struct active_entry {
    uint32_t pid;
    uint32_t event_id;
    uint64_t start_ns;   /* timestamp_ns - duration_ns */
    uint64_t end_ns;      /* timestamp_ns */
};

static int cmp_active_by_start(const void *a, const void *b)
{
    uint64_t sa = ((const struct active_entry *)a)->start_ns;
    uint64_t sb = ((const struct active_entry *)b)->start_ns;
    return (sa > sb) - (sa < sb);
}

/* One pid ENTERING one wait event: all phase 2 needs, 16 bytes. */
struct burst_entry {
    uint64_t start_ns;
    uint32_t pid;
    uint32_t event_id;
};

/* Event first (phase 2 walks one event's entries as a contiguous group), then
 * entry time, then pid. The order is TOTAL on purpose: qsort is not stable,
 * and which burst a bucket reports must not depend on the libc. */
static int cmp_burst_entry(const void *a, const void *b)
{
    const struct burst_entry *x = (const struct burst_entry *)a;
    const struct burst_entry *y = (const struct burst_entry *)b;
    if (x->event_id != y->event_id)
        return (x->event_id > y->event_id) - (x->event_id < y->event_id);
    if (x->start_ns != y->start_ns)
        return (x->start_ns > y->start_ns) - (x->start_ns < y->start_ns);
    return (x->pid > y->pid) - (x->pid < y->pid);
}

/* Largest first; ties by time then event id, again for a total order. */
static int cmp_burst_desc(const void *a, const void *b)
{
    const struct pgwt_burst *x = (const struct pgwt_burst *)a;
    const struct pgwt_burst *y = (const struct pgwt_burst *)b;
    if (x->num_sessions != y->num_sessions)
        return (y->num_sessions > x->num_sessions) -
               (y->num_sessions < x->num_sessions);
    if (x->timestamp_ns != y->timestamp_ns)
        return (x->timestamp_ns > y->timestamp_ns) -
               (x->timestamp_ns < y->timestamp_ns);
    return (x->event_id > y->event_id) - (x->event_id < y->event_id);
}

/* Does this record contribute an interval? The predicate is shared by the
 * counting pass and the filling pass so the two can never disagree about the
 * size of the array. Markers and records whose duration exceeds their own end
 * timestamp are already refused by pgwt_filter_matches (the FID-4 chokepoint),
 * which is what makes `timestamp_ns - duration_ns` below safe. */
static inline int concurrency_qualifies(const struct pgwt_trace_event *ev,
                                        const struct pgwt_filter *f,
                                        uint64_t from_ns, uint64_t to_ns)
{
    if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
        return 0;
    if (ev->old_event == 0)
        return 0;        /* on-CPU: not a wait, never part of a burst */
    if (ev->timestamp_ns < from_ns || ev->timestamp_ns > to_ns)
        return 0;
    return 1;
}

void pgwt_compute_concurrency(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f,
                               uint64_t from_ns, uint64_t to_ns,
                               int num_buckets,
                               uint64_t burst_window_ns, int burst_threshold,
                               struct pgwt_concurrency_result *out)
{
    memset(out, 0, sizeof(*out));

    if (count == 0 || from_ns >= to_ns || num_buckets <= 0)
        return;

    uint64_t bucket_ns = (to_ns - from_ns) / num_buckets;
    if (bucket_ns == 0) bucket_ns = 1;

    out->num_buckets = num_buckets;
    out->bucket_ns = bucket_ns;
    out->peak_sessions = calloc(num_buckets, sizeof(int));
    out->peak_event = calloc(num_buckets, sizeof(uint32_t));
    if (!out->peak_sessions || !out->peak_event) {
        out->failed = 1;
        return;
    }

    /* Phase 1 state. Two maps so a pid key can never be read as a count key:
     *   seen:   (bucket, event, pid) -> presence
     *   counts: (bucket, event, 0)   -> distinct pids seen so far */
    struct pgwt_triple_map seen, counts;
    pgwt_triple_map_init(&seen);
    pgwt_triple_map_init(&counts);

    /* Phase 2 input: sized by a counting pass, so it is exactly as large as
     * the qualifying intervals (16 bytes each) with no doubling slack — the
     * cap it replaces was 100,000 entries of 24 bytes, and the array the
     * caller already holds is 48 bytes per EVENT. One predicate
     * (concurrency_qualifies) decides membership for both passes, so the two
     * cannot drift apart and overrun. */
    int failed = 0;
    size_t nqual = 0;
    for (int i = 0; i < count; i++)
        if (concurrency_qualifies(&events[i], f, from_ns, to_ns))
            nqual++;

    struct burst_entry *entries = NULL;
    if (nqual > 0) {
        if (!pgwt_test_alloc_fail("concurrency_entries"))
            entries = (struct burst_entry *)malloc(nqual * sizeof(*entries));
        if (!entries) {
            pgwt_triple_map_free(&seen);
            pgwt_triple_map_free(&counts);
            out->failed = 1;
            return;
        }
    }
    size_t nentries = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!concurrency_qualifies(ev, f, from_ns, to_ns))
            continue;

        uint64_t end_ns = ev->timestamp_ns;
        uint64_t start_ns = end_ns - ev->duration_ns;

        /* Buckets overlapped: start < bucket_end && end > bucket_start. The
         * window filter above guarantees end_ns <= to_ns and end_ns >= from_ns;
         * end_ns == from_ns overlaps no bucket. */
        if (end_ns > from_ns) {
            uint64_t into = end_ns - from_ns;
            int b_hi = (int)((into - 1) / bucket_ns);
            if (b_hi > num_buckets - 1) b_hi = num_buckets - 1;
            int b_lo = 0;
            if (start_ns > from_ns) {
                uint64_t q = (start_ns - from_ns) / bucket_ns;
                b_lo = q > (uint64_t)num_buckets ? num_buckets : (int)q;
            }
            for (int b = b_lo; b <= b_hi; b++) {
                int created = 0;
                int *slot = pgwt_triple_map_slot(&seen, (uint32_t)b,
                                                 ev->old_event, ev->pid,
                                                 &created);
                if (!slot) { failed = 1; break; }
                if (!created)
                    continue;      /* this pid is already counted here */
                int *cnt = pgwt_triple_map_slot(&counts, (uint32_t)b,
                                                ev->old_event, 0, &created);
                if (!cnt) { failed = 1; break; }
                (*cnt)++;
                /* Strictly greater: the first event to REACH a count owns the
                 * bucket, which is the pre-#276 tie-break (first-inserted
                 * wins) and is deterministic for a given event order. */
                if (*cnt > out->peak_sessions[b]) {
                    out->peak_sessions[b] = *cnt;
                    out->peak_event[b] = ev->old_event;
                }
            }
            if (failed) break;
        }

        if (nentries >= nqual)
            continue;    /* unreachable: same predicate as the counting pass */
        entries[nentries].start_ns = start_ns;
        entries[nentries].pid = ev->pid;
        entries[nentries].event_id = ev->old_event;
        nentries++;
    }

    pgwt_triple_map_free(&seen);
    pgwt_triple_map_free(&counts);

    if (failed) {
        free(entries);
        out->failed = 1;
        return;
    }

    /* `entries` is NULL when nothing in the window qualified, and qsort(NULL,
     * 0, ...) is formally undefined even though every libc tolerates it. */
    if (nentries > 0)
        qsort(entries, nentries, sizeof(entries[0]), cmp_burst_entry);

    /* Phase 2: one burst slot per bucket — the largest burst in it. Bounded
     * by the client's own chart resolution, not by an arbitrary 256. */
    struct pgwt_burst *per_bucket =
        (struct pgwt_burst *)calloc((size_t)num_buckets, sizeof(*per_bucket));
    int *has_burst = (int *)calloc((size_t)num_buckets, sizeof(int));
    if (!per_bucket || !has_burst) {
        free(per_bucket); free(has_burst); free(entries);
        out->failed = 1;
        return;
    }

    /* pid -> how many of its entries are inside the current window. A sliding
     * window with both ends monotone, so distinct-pid bookkeeping is
     * incremental: no rescan per anchor, and no 64-pid ceiling. */
    struct pgwt_triple_map win;
    pgwt_triple_map_init(&win);
    int total_bursts = 0;

    size_t g = 0;
    while (g < nentries) {
        uint32_t eid = entries[g].event_id;
        size_t gend = g;
        while (gend < nentries && entries[gend].event_id == eid)
            gend++;

        pgwt_triple_map_clear(&win);
        size_t i = g, j = g;
        int distinct = 0;

        while (i < gend) {
            uint64_t wend = entries[i].start_ns + burst_window_ns;
            while (j < gend && entries[j].start_ns <= wend) {
                int created = 0;
                int *m = pgwt_triple_map_slot(&win, entries[j].pid, 0, 0,
                                              &created);
                if (!m) { failed = 1; break; }
                if (*m == 0) distinct++;
                (*m)++;
                j++;
            }
            if (failed) break;

            size_t consume_to = i + 1;
            if (distinct >= burst_threshold) {
                total_bursts++;
                int b = 0;
                if (entries[i].start_ns > from_ns) {
                    uint64_t q = (entries[i].start_ns - from_ns) / bucket_ns;
                    b = q > (uint64_t)(num_buckets - 1)
                        ? num_buckets - 1 : (int)q;
                }
                if (!has_burst[b] || distinct > per_bucket[b].num_sessions) {
                    struct pgwt_burst *nb = &per_bucket[b];
                    memset(nb, 0, sizeof(*nb));
                    has_burst[b] = 1;
                    nb->timestamp_ns = entries[i].start_ns;
                    nb->event_id = eid;
                    nb->num_sessions = distinct;
                    pgwt_event_full_name(eid, nb->event_name,
                                         sizeof(nb->event_name));
                    /* pids[] is a DISPLAY SAMPLE: the first
                     * PGWT_BURST_PID_SAMPLE distinct pids of the burst in
                     * entry order. num_sessions is the exact count, so
                     * num_pids < num_sessions makes the bound visible
                     * instead of silent (the pre-#276 code capped
                     * num_sessions at 64 as well, so the count and the list
                     * agreed by being equally short). */
                    int np = 0;
                    for (size_t k = i; k < j && np < PGWT_BURST_PID_SAMPLE; k++) {
                        int dup = 0;
                        for (int p = 0; p < np; p++)
                            if (nb->pids[p] == entries[k].pid) { dup = 1; break; }
                        if (!dup) nb->pids[np++] = entries[k].pid;
                    }
                    nb->num_pids = np;
                }
                /* One onset is reported once. The pre-#276 code also skipped
                 * ahead, but stopped at the next entry of a DIFFERENT event,
                 * so a single onset could be re-reported dozens of times and
                 * fill the old 256-entry table by itself. */
                while (consume_to < gend && entries[consume_to].start_ns <= wend)
                    consume_to++;
            }
            for (; i < consume_to; i++) {
                int created = 0;
                int *m = pgwt_triple_map_slot(&win, entries[i].pid, 0, 0,
                                              &created);
                if (!m) { failed = 1; break; }
                if (--(*m) == 0) distinct--;
            }
            if (failed) break;
        }
        if (failed) break;
        g = gend;
    }

    pgwt_triple_map_free(&win);
    free(entries);

    if (failed) {
        free(per_bucket); free(has_burst);
        out->failed = 1;
        return;
    }

    /* Compact to the buckets that had a burst, largest first. */
    int nbursts = 0;
    for (int b = 0; b < num_buckets; b++)
        if (has_burst[b]) nbursts++;

    struct pgwt_burst *bursts = NULL;
    if (nbursts > 0) {
        bursts = (struct pgwt_burst *)calloc((size_t)nbursts, sizeof(*bursts));
        if (!bursts) {
            free(per_bucket); free(has_burst);
            out->failed = 1;
            return;
        }
        int w = 0;
        for (int b = 0; b < num_buckets; b++)
            if (has_burst[b]) bursts[w++] = per_bucket[b];
        qsort(bursts, nbursts, sizeof(bursts[0]), cmp_burst_desc);
    }
    free(per_bucket);
    free(has_burst);

    out->bursts = bursts;
    out->num_bursts = nbursts;
    out->bursts_total = total_bursts;
}

/* ── Lock Chains ──────────────────────────────────────────── */

/*
 * Lock chain detection: for each Lock wait event, find another PID that
 * was active (on CPU or in a different state) during the same time interval.
 * The "blocker" is the PID that was most likely on CPU while the waiter
 * was blocked on a Lock event.
 *
 * This is a heuristic: without pg_locks data, we can only infer blockers
 * from temporal overlap. A PID on CPU while another waits on Lock:transactionid
 * is likely holding the conflicting lock.
 */

static int cmp_lock_chain_desc(const void *a, const void *b)
{
    uint64_t wa = ((const struct pgwt_lock_chain_link *)a)->wait_ns;
    uint64_t wb = ((const struct pgwt_lock_chain_link *)b)->wait_ns;
    return (wb > wa) - (wb < wa);
}

void pgwt_compute_lock_chains(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f, int max_links,
                               struct pgwt_lock_chains_result *out)
{
    memset(out, 0, sizeof(*out));

    /* Collect Lock wait intervals */
    int lock_cap = 4096;
    struct active_entry *locks = malloc(lock_cap * sizeof(*locks));
    int nlocks = 0;

    /* Collect CPU intervals (potential blockers) */
    int cpu_cap = 8192;
    struct active_entry *cpus = malloc(cpu_cap * sizeof(*cpus));
    int ncpus = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev))
            continue;

        uint8_t cls = (ev->old_event >> 24) & 0xFF;

        if (cls == 0x03 && nlocks < lock_cap) {  /* Lock class */
            locks[nlocks].pid = ev->pid;
            locks[nlocks].event_id = ev->old_event;
            locks[nlocks].start_ns = ev->timestamp_ns - ev->duration_ns;
            locks[nlocks].end_ns = ev->timestamp_ns;
            nlocks++;
        }
        if (ev->old_event == 0 && ncpus < cpu_cap) {  /* CPU */
            cpus[ncpus].pid = ev->pid;
            cpus[ncpus].event_id = 0;
            cpus[ncpus].start_ns = ev->timestamp_ns - ev->duration_ns;
            cpus[ncpus].end_ns = ev->timestamp_ns;
            ncpus++;
        }
    }

    /* For each Lock wait, find the CPU interval from a different PID
     * that overlaps the most. That PID is the likely blocker. */
    int link_cap = max_links * 2;
    struct pgwt_lock_chain_link *links = calloc(link_cap, sizeof(*links));
    int nlinks = 0;

    for (int i = 0; i < nlocks && nlinks < link_cap; i++) {
        uint32_t best_pid = 0;
        uint64_t best_overlap = 0;

        for (int j = 0; j < ncpus; j++) {
            if (cpus[j].pid == locks[i].pid)
                continue;  /* skip self */

            /* Compute overlap */
            uint64_t ostart = locks[i].start_ns > cpus[j].start_ns
                            ? locks[i].start_ns : cpus[j].start_ns;
            uint64_t oend = locks[i].end_ns < cpus[j].end_ns
                          ? locks[i].end_ns : cpus[j].end_ns;

            if (oend > ostart) {
                uint64_t overlap = oend - ostart;
                if (overlap > best_overlap) {
                    best_overlap = overlap;
                    best_pid = cpus[j].pid;
                }
            }
        }

        if (best_pid != 0 && best_overlap > 0) {
            struct pgwt_lock_chain_link *l = &links[nlinks++];
            l->waiter_pid = locks[i].pid;
            l->blocker_pid = best_pid;
            l->lock_event = locks[i].event_id;
            l->wait_ns = locks[i].end_ns - locks[i].start_ns;
            l->timestamp_ns = locks[i].start_ns;
            pgwt_event_full_name(locks[i].event_id, l->lock_name,
                                 sizeof(l->lock_name));
        }
    }

    free(locks);
    free(cpus);

    qsort(links, nlinks, sizeof(links[0]), cmp_lock_chain_desc);

    int nr = nlinks < max_links ? nlinks : max_links;
    out->links = links;
    out->num_links = nr;
}

/* ── Interference Scoring ────────────────────────────────── */

/*
 * For each pair of PIDs, compute how much time they spent waiting on the
 * same wait event simultaneously. High overlap = "noisy neighbors" contending
 * on the same resource.
 *
 * Algorithm: for each event, build (pid, event_id, start_ns, end_ns) intervals.
 * For each pair of intervals on the same event from different PIDs, compute overlap.
 * Aggregate per PID pair.
 */

struct pair_accum {
    uint32_t pid_a;
    uint32_t pid_b;
    uint64_t overlap_ns;
    uint32_t top_event;
    uint64_t top_event_ns;
};

#define PAIR_HT_SIZE 4096
#define PAIR_HT_MASK (PAIR_HT_SIZE - 1)

static int cmp_interference_desc(const void *a, const void *b)
{
    double sa = ((const struct pgwt_interference_row *)a)->score;
    double sb = ((const struct pgwt_interference_row *)b)->score;
    return (sb > sa) - (sb < sa);
}

void pgwt_compute_interference(const struct pgwt_trace_event *events, int count,
                                const struct pgwt_filter *f, int max_rows,
                                struct pgwt_interference_result *out)
{
    memset(out, 0, sizeof(*out));

    /* Build active intervals (non-CPU, non-idle wait events only) */
    int cap = count < 50000 ? count : 50000;
    struct active_entry *active = malloc(cap * sizeof(*active));
    int nactive = 0;

    for (int i = 0; i < count && nactive < cap; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
            continue;
        if (ev->old_event == 0) continue;  /* skip CPU */

        active[nactive].pid = ev->pid;
        active[nactive].event_id = ev->old_event;
        active[nactive].start_ns = ev->timestamp_ns - ev->duration_ns;
        active[nactive].end_ns = ev->timestamp_ns;
        nactive++;
    }

    qsort(active, nactive, sizeof(active[0]), cmp_active_by_start);

    /* Find overlapping pairs on same event */
    struct pair_accum *ht = calloc(PAIR_HT_SIZE, sizeof(*ht));
    int num_pairs = 0;
    uint64_t max_overlap = 0;

    for (int i = 0; i < nactive; i++) {
        /* Look forward for overlapping intervals on same event */
        for (int j = i + 1; j < nactive; j++) {
            if (active[j].start_ns >= active[i].end_ns)
                break;  /* no more overlaps possible (sorted by start) */

            if (active[j].event_id != active[i].event_id)
                continue;
            if (active[j].pid == active[i].pid)
                continue;

            /* Compute overlap */
            uint64_t ostart = active[j].start_ns;  /* j starts after i (sorted) */
            uint64_t oend = active[i].end_ns < active[j].end_ns
                          ? active[i].end_ns : active[j].end_ns;
            if (oend <= ostart) continue;
            uint64_t overlap = oend - ostart;

            /* Normalize PID pair (smaller first) */
            uint32_t pa = active[i].pid < active[j].pid ? active[i].pid : active[j].pid;
            uint32_t pb = active[i].pid < active[j].pid ? active[j].pid : active[i].pid;

            /* Hash lookup */
            uint32_t h = ((pa * 0x45d9f3b) ^ (pb * 0x9e3779b9)) & PAIR_HT_MASK;
            while (ht[h].overlap_ns > 0 &&
                   (ht[h].pid_a != pa || ht[h].pid_b != pb))
                h = (h + 1) & PAIR_HT_MASK;

            if (ht[h].overlap_ns == 0) {
                ht[h].pid_a = pa;
                ht[h].pid_b = pb;
                num_pairs++;
            }
            ht[h].overlap_ns += overlap;
            if (overlap > ht[h].top_event_ns) {
                ht[h].top_event_ns = overlap;
                ht[h].top_event = active[i].event_id;
            }
            if (ht[h].overlap_ns > max_overlap)
                max_overlap = ht[h].overlap_ns;
        }
    }

    free(active);

    /* Build result rows */
    struct pgwt_interference_row *rows = calloc(num_pairs, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < PAIR_HT_SIZE; i++) {
        if (ht[i].overlap_ns == 0) continue;
        rows[nr].pid_a = ht[i].pid_a;
        rows[nr].pid_b = ht[i].pid_b;
        rows[nr].score = max_overlap > 0
                       ? (double)ht[i].overlap_ns / (double)max_overlap : 0;
        rows[nr].top_event = ht[i].top_event;
        rows[nr].overlap_ns = ht[i].overlap_ns;
        pgwt_event_full_name(ht[i].top_event, rows[nr].top_event_name,
                             sizeof(rows[nr].top_event_name));
        nr++;
    }

    free(ht);
    qsort(rows, nr, sizeof(rows[0]), cmp_interference_desc);

    int n = nr < max_rows ? nr : max_rows;
    out->rows = rows;
    out->num_rows = n;
}

/* ── Variants ─────────────────────────────────────────────── */

/* ── Execution waterfall / scatter data (U3/B6) ────────────────── */

struct exec_pid_state {
    uint32_t pid;
    /* Stack (LIFO) of row indices this pid currently has open, deepest
     * (innermost) execution last -- see pgwt_compute_executions's
     * EXEC_START/EXEC_END/CMD_END handling. A single `active_row` int
     * (the pre-#222-review shape) loses the OUTER row the moment a second
     * EXEC_START fires -- true nesting (SQL-level EXECUTE of a prepared
     * statement from inside plpgsql, PortalRun genuinely nested) and,
     * just as commonly, a backend that errors on one statement and the
     * client immediately sends another on the same connection with no
     * EXEC_END in between for the first. Pushing (never overwriting)
     * keeps every still-open row reachable so CMD_END -- the one marker
     * that fires only once this pid has actually gone idle, so it can
     * never cut a genuinely still-running statement -- can close every
     * one of them, however deep. */
    int *open_rows;
    int n_open, cap_open;
    /* #222 review: a wait event must attribute to the top-of-stack row
     * only while that row is genuinely the one that JUST started -- not
     * merely whatever an EXEC_END's pop happened to re-expose underneath
     * it. Set on every push (a fresh EXEC_START), cleared on every pop
     * (EXEC_END or CMD_END): once cleared, exec_pid_attributable_row()
     * returns -1 until the next EXEC_START, even though the stack itself
     * may still be non-empty. Without this, a pid that errors on A (A
     * stays stacked, unclosed), then runs and finishes B, has every wait
     * event AFTER B's EXEC_END mis-credited to A -- a long-dead orphan
     * that silently keeps accruing n_events (and matches_event_filter)
     * for as long as CMD_END never arrives to close it (see the CMD_END
     * comment below). Master attributed those same events to no row at
     * all; that is the safer default here too, since the marker stream
     * alone cannot distinguish "B is legitimately nested inside A" from
     * "B is an unrelated statement after A orphaned" -- CMD_END is the
     * only marker that can. */
    int top_attributable;
    uint64_t plan_start_ns;
    uint64_t plan_query_id;
    int plan_open;
    uint64_t ready_plan_start_ns;
    uint64_t ready_plan_end_ns;
    uint64_t ready_plan_query_id;
    int plan_ready;
};

static int exec_pid_state_get(struct exec_pid_state **states, int *n_states,
                              int *cap_states, uint32_t pid)
{
    for (int i = 0; i < *n_states; i++)
        if ((*states)[i].pid == pid)
            return i;

    if (*n_states >= *cap_states) {
        int new_cap = *cap_states ? *cap_states * 2 : 64;
        struct exec_pid_state *tmp = realloc(*states,
                                             new_cap * sizeof(**states));
        if (!tmp)
            return -1;
        *states = tmp;
        *cap_states = new_cap;
    }

    int idx = (*n_states)++;
    memset(&(*states)[idx], 0, sizeof((*states)[idx]));
    (*states)[idx].pid = pid;
    return idx;
}

/* Push a newly-opened row's index onto its pid's open stack. Marks it
 * freshly attributable -- see top_attributable's own comment. */
static int exec_pid_push_row(struct exec_pid_state *st, int row_idx)
{
    if (st->n_open >= st->cap_open) {
        int new_cap = st->cap_open ? st->cap_open * 2 : 4;
        int *tmp = realloc(st->open_rows, new_cap * sizeof(*tmp));
        if (!tmp)
            return -1;
        st->open_rows = tmp;
        st->cap_open = new_cap;
    }
    st->open_rows[st->n_open++] = row_idx;
    st->top_attributable = 1;
    return 0;
}

/* Pop the innermost (most recently opened) still-open row, or -1 if none.
 * Whatever the pop exposes underneath (if anything) is no longer
 * attributable until its own EXEC_START pushes it again -- see
 * top_attributable's own comment for why. */
static int exec_pid_pop_row(struct exec_pid_state *st)
{
    st->top_attributable = 0;
    if (st->n_open <= 0)
        return -1;
    return st->open_rows[--st->n_open];
}

/* The innermost still-open row without removing it -- the row a wait event
 * arriving right now belongs to. */
static int exec_pid_peek_row(const struct exec_pid_state *st)
{
    return st->n_open > 0 ? st->open_rows[st->n_open - 1] : -1;
}

/* Like exec_pid_peek_row, but -1 whenever the top was exposed by a pop
 * rather than freshly pushed -- see top_attributable's own comment. This
 * is the one callers use for wait-event attribution (n_events,
 * matches_event_filter, n_workers); exec_pid_peek_row alone stays
 * available for anything that legitimately wants "whatever's on top"
 * regardless (there is no such caller today, kept for symmetry with push/
 * pop). */
static int exec_pid_attributable_row(const struct exec_pid_state *st)
{
    return st->top_attributable ? exec_pid_peek_row(st) : -1;
}

static int execution_append(struct pgwt_execution **rows, int *n_rows,
                            int *cap_rows, const struct pgwt_execution *row)
{
    if (*n_rows >= *cap_rows) {
        int new_cap = *cap_rows ? *cap_rows * 2 : 128;
        struct pgwt_execution *tmp = realloc(*rows,
                                             new_cap * sizeof(**rows));
        if (!tmp)
            return -1;
        *rows = tmp;
        *cap_rows = new_cap;
    }
    (*rows)[*n_rows] = *row;
    return (*n_rows)++;
}

static int worker_link_index(const struct pgwt_backend_link *links, int n_links,
                             uint32_t pid)
{
    int lo = 0, hi = n_links;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (links[mid].pid < pid) lo = mid + 1;
        else                      hi = mid;
    }
    return lo < n_links && links[lo].pid == pid && links[lo].leader_pid != 0
         ? lo : -1;
}

static int interval_overlaps(uint64_t event_end, uint64_t duration,
                             uint64_t from_ns, uint64_t to_ns);

void pgwt_compute_executions(const struct pgwt_trace_event *events, int count,
                             uint64_t from_ns, uint64_t to_ns,
                             const struct pgwt_filter *f,
                             const struct pgwt_backend_link *links, int n_links,
                             struct pgwt_executions_result *out)
{
    memset(out, 0, sizeof(*out));

    struct pgwt_execution *rows = NULL;
    int n_rows = 0, cap_rows = 0;
    struct exec_pid_state *states = NULL;
    int n_states = 0, cap_states = 0;
    int *worker_last_row = NULL;
    if (n_links > 0) {
        worker_last_row = malloc(n_links * sizeof(*worker_last_row));
        if (!worker_last_row) {
            out->failed = 1;
            return;
        }
        for (int i = 0; i < n_links; i++) worker_last_row[i] = -1;
    }

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        int si = exec_pid_state_get(&states, &n_states, &cap_states, ev->pid);
        if (si < 0) {
            out->failed = 1;
            break;
        }
        struct exec_pid_state *st = &states[si];
        uint32_t marker = ev->old_event;

        if (marker == PGWT_MARKER_PLAN_START) {
            st->plan_start_ns = ev->timestamp_ns;
            st->plan_query_id = ev->query_id;
            st->plan_open = 1;
            continue;
        }
        if (marker == PGWT_MARKER_PLAN_END) {
            if (st->plan_open && ev->timestamp_ns >= st->plan_start_ns) {
                st->ready_plan_start_ns = st->plan_start_ns;
                st->ready_plan_end_ns = ev->timestamp_ns;
                st->ready_plan_query_id = st->plan_query_id
                                        ? st->plan_query_id : ev->query_id;
                st->plan_ready = 1;
            }
            st->plan_open = 0;
            continue;
        }
        if (marker == PGWT_MARKER_EXEC_START) {
            /* PUSH, never overwrite: PortalRun genuinely nests for
             * SQL-level EXECUTE of a prepared statement from inside
             * plpgsql, and a backend that errors on one statement (no
             * EXEC_END) commonly has the client send another right away
             * on the same connection -- both leave an earlier row still
             * open when this EXEC_START fires. Closing it here (or
             * discarding it by overwriting a single active_row slot)
             * would be wrong: the outer/earlier row might still be
             * genuinely running. Only CMD_END (below) is a safe place to
             * give up on a row this pid left open. */
            if (ev->timestamp_ns <= to_ns) {
                struct pgwt_execution row;
                memset(&row, 0, sizeof(row));
                row.pid = ev->pid;
                row.query_id = ev->query_id;
                row.start_ns = ev->timestamp_ns;
                row.in_progress = 1;
                row.started_before_window = ev->timestamp_ns < from_ns;
                if (st->plan_ready &&
                    (st->ready_plan_query_id == 0 || row.query_id == 0 ||
                     st->ready_plan_query_id == row.query_id)) {
                    row.plan_start_ns = st->ready_plan_start_ns;
                    row.plan_end_ns = st->ready_plan_end_ns;
                    row.has_plan = 1;
                }
                int row_idx = execution_append(&rows, &n_rows, &cap_rows,
                                               &row);
                if (row_idx < 0 || exec_pid_push_row(st, row_idx) < 0) {
                    out->failed = 1;
                    break;
                }
            } else {
                /* #222 review round 3, item 2: this pid HAS started a new
                 * statement; we simply are not keeping a row for it (its
                 * start is past the caller's window). The previous row
                 * must stop being attributable all the same -- otherwise
                 * top_attributable keeps whatever the last push left it
                 * at, and a later wait event is credited to an earlier,
                 * still-open row that the new statement has in fact
                 * superseded. Master cleared active_row unconditionally
                 * here for the same reason. Unreachable through the
                 * server today (server_load_events_fi stops at
                 * `ts > sample_to_m`, so no event after this one is even
                 * loaded, and interval_overlaps would gate it anyway) --
                 * this makes it unreachable BY CONSTRUCTION rather than
                 * by caller discipline, and pgwt_compute_executions is a
                 * public entry point that does not get to assume its
                 * caller pre-filtered. */
                st->top_attributable = 0;
            }
            /* A completed plan belongs to this next execution, including a
             * start outside the caller's output window. Never reuse it. */
            st->plan_ready = 0;
            continue;
        }
        if (marker == PGWT_MARKER_EXEC_END) {
            /* Pops the INNERMOST open row -- correct for nesting (the
             * statement that started last finishes first) and for the
             * plain non-nested case (stack depth 1). A real, measured
             * end: end_inferred stays 0. */
            int row_idx = exec_pid_pop_row(st);
            if (row_idx >= 0 && row_idx < n_rows &&
                ev->timestamp_ns >= rows[row_idx].start_ns) {
                struct pgwt_execution *row = &rows[row_idx];
                row->end_ns = ev->timestamp_ns;
                row->in_progress = 0;
                if (row->query_id == 0)
                    row->query_id = ev->query_id;
            }
            continue;
        }
        if (marker == PGWT_MARKER_CMD_END) {
            /* A command can close without ever reaching EXEC_END: an ERROR
             * longjmp, a cancel, statement_timeout, a client disconnect
             * mid-query, or an EXEC_END lost to a ringbuf drop all skip
             * src/daemon.c's query__execute__done probe. Left alone, that
             * row stays in_progress for the rest of the retained capture
             * with a duration fabricated from the caller's window bound
             * (execution_sort_duration_ns, src/server.c) -- sorted above
             * every genuinely slow COMPLETED execution once "longest
             * running first" ranks by duration (#222 review: the
             * demo-blocking failure #222 was filed to prevent,
             * reintroduced from the other direction -- concretely, a
             * single presenter Ctrl-C on an ad-hoc query pins one
             * "In progress" row at the top of the tab for the rest of the
             * session; n=1 is enough).
             *
             * CMD_END is the correct closing point, and closing every row
             * still open for this pid (not just the innermost) is safe,
             * BECAUSE of what CMD_END means: src/bpf/pg_wait_tracer.bpf.c
             * emits it only on the pgstat_report_activity gate flip to
             * IDLE, which happens after every nested portal for this
             * command has itself finished or errored -- the backend
             * cannot be RUNNING anything when this fires. So CMD_END can
             * never truncate a still-running statement, nested or not;
             * closing on a second EXEC_START instead would be wrong for
             * exactly that reason (see the EXEC_START comment above). Each
             * closed-here row's end_ns is a real wall-clock timestamp --
             * not fabricated from the window bound -- but DEDUCED from the
             * idle transition rather than measured at the query's own
             * completion, so end_inferred=1 (struct pgwt_execution,
             * compute.h): a cancelled statement is consequently
             * indistinguishable from a normally-completed one of the same
             * measured span in this field alone -- acceptable, but not
             * invisible: end_inferred is exactly the bit that says so.
             * Mirrors pgwt_tag_events's own CMD_END handling (this file,
             * ~line 267: "an EXEC_END lost to a ringbuf drop must not leak
             * the window across statements") for a different computation
             * over the same markers.
             *
             * Not a total fix: CMD_END itself is not guaranteed. It is
             * emitted only while a live watchpoint is open on this pid
             * (src/bpf/pg_wait_tracer.bpf.c: st->wp_live &&
             * exact_admission_open()) -- a pid that never escalated, or
             * whose CMD_END marker is itself lost to a ringbuf drop, still
             * leaves a genuine zombie. Nothing below claims otherwise. */
            int row_idx;
            while ((row_idx = exec_pid_pop_row(st)) >= 0) {
                if (row_idx < n_rows && rows[row_idx].in_progress &&
                    ev->timestamp_ns >= rows[row_idx].start_ns) {
                    struct pgwt_execution *row = &rows[row_idx];
                    row->end_ns = ev->timestamp_ns;
                    row->in_progress = 0;
                    row->end_inferred = 1;
                }
            }
            st->plan_open = 0;
            st->plan_ready = 0;
            continue;
        }
        if (PGWT_IS_MARKER(marker) ||
            (ev->flags & PGWT_EVENT_FLAG_SAMPLE))
            continue;

        /* The transition stream is ordered by interval end. Lifecycle END is
         * emitted after the final interval closes, so active-row accounting
         * is both linear and faithful to interval overlap at execution edges. */
        struct pgwt_filter event_filter = f ? *f : (struct pgwt_filter){0};
        /* A pid filter identifies the leader execution. Parallel-worker
         * membership supplies pid scope; the shared matcher applies the
         * event-level class/event_id/query_id dimensions. */
        event_filter.pid = 0;
        int event_matches =
            interval_overlaps(ev->timestamp_ns, ev->duration_ns,
                              from_ns, to_ns) &&
            pgwt_filter_matches(&event_filter, ev);
        int active_row = exec_pid_attributable_row(st);
        if (event_matches && active_row >= 0 && active_row < n_rows) {
            rows[active_row].n_events++;
            rows[active_row].matches_event_filter = 1;
        }

        int wi = worker_last_row
               ? worker_link_index(links, n_links, ev->pid) : -1;
        if (wi >= 0) {
            int li = exec_pid_state_get(&states, &n_states, &cap_states,
                                        links[wi].leader_pid);
            if (li < 0) {
                out->failed = 1;
                break;
            } else {
                int row_idx = exec_pid_attributable_row(&states[li]);
                if (event_matches && row_idx >= 0 && row_idx < n_rows &&
                    worker_last_row[wi] != row_idx) {
                    rows[row_idx].n_workers++;
                    rows[row_idx].matches_event_filter = 1;
                    worker_last_row[wi] = row_idx;
                }
            }
        }
    }

    free(worker_last_row);
    for (int i = 0; i < n_states; i++)
        free(states[i].open_rows);
    free(states);
    if (out->failed) {
        free(rows);
    } else {
        int kept = 0;
        for (int i = 0; i < n_rows; i++) {
            if (rows[i].start_ns > to_ns)
                continue;
            if (!rows[i].in_progress && rows[i].end_ns < from_ns)
                continue;
            rows[kept++] = rows[i];
        }
        out->rows = rows;
        out->num_rows = kept;
    }
}

static int interval_overlaps(uint64_t event_end, uint64_t duration,
                             uint64_t from_ns, uint64_t to_ns)
{
    uint64_t event_start = event_end >= duration ? event_end - duration : 0;
    return event_start < to_ns && event_end > from_ns;
}

static int detail_worker_leader(const struct pgwt_backend_link *links,
                                int n_links, uint32_t pid)
{
    int idx = worker_link_index(links, n_links, pid);
    return idx >= 0 ? (int)links[idx].leader_pid : 0;
}

static struct pgwt_exec_lane *detail_worker_lane(
    struct pgwt_execution_detail_result *out, uint32_t pid, int create)
{
    for (int i = 0; i < out->num_workers; i++)
        if (out->workers[i].pid == pid)
            return &out->workers[i];
    if (!create)
        return NULL;

    struct pgwt_exec_lane *tmp = realloc(
        out->workers, (out->num_workers + 1) * sizeof(*out->workers));
    if (!tmp) {
        out->failed = 1;
        return NULL;
    }
    out->workers = tmp;
    struct pgwt_exec_lane *lane = &out->workers[out->num_workers++];
    memset(lane, 0, sizeof(*lane));
    lane->pid = pid;
    return lane;
}

static int detail_lane_add(struct pgwt_exec_lane *lane,
                           const struct pgwt_trace_event *ev, int max_events)
{
    lane->total_events++;
    if (lane->num_events >= max_events)
        return 0;
    if (!lane->events) {
        lane->events = calloc(max_events, sizeof(*lane->events));
        if (!lane->events)
            return -1;
    }

    struct pgwt_exec_event *dst = &lane->events[lane->num_events++];
    dst->event_id = ev->old_event;
    dst->start_ns = ev->timestamp_ns >= ev->duration_ns
                  ? ev->timestamp_ns - ev->duration_ns : 0;
    dst->duration_ns = ev->duration_ns;
    dst->has_cpu = ev->cpu_ns != PGWT_CPU_NS_UNKNOWN;
    dst->cpu_ns = dst->has_cpu ? ev->cpu_ns : 0;
    if (ev->old_event == 0)
        snprintf(dst->name, sizeof(dst->name), "CPU*");
    else
        pgwt_event_full_name(ev->old_event, dst->name, sizeof(dst->name));
    return 0;
}

void pgwt_compute_execution_detail(
    const struct pgwt_trace_event *events, int count,
    uint32_t leader_pid, uint64_t start_ns, uint64_t end_ns,
    const struct pgwt_filter *f,
    const struct pgwt_backend_link *links, int n_links, int max_events_per_lane,
    struct pgwt_execution_detail_result *out)
{
    memset(out, 0, sizeof(*out));
    out->leader.pid = leader_pid;
    if (max_events_per_lane <= 0)
        return;

    uint64_t plan_start = 0, plan_qid = 0;
    uint64_t ready_plan_start = 0, ready_plan_end = 0, ready_plan_qid = 0;
    int plan_open = 0, plan_ready = 0, found_exec = 0, inside_exec = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (ev->pid == leader_pid) {
            if (ev->old_event == PGWT_MARKER_PLAN_START) {
                plan_start = ev->timestamp_ns;
                plan_qid = ev->query_id;
                plan_open = 1;
                continue;
            }
            if (ev->old_event == PGWT_MARKER_PLAN_END) {
                if (plan_open && ev->timestamp_ns >= plan_start) {
                    ready_plan_start = plan_start;
                    ready_plan_end = ev->timestamp_ns;
                    ready_plan_qid = plan_qid ? plan_qid : ev->query_id;
                    plan_ready = 1;
                    if (found_exec && !out->has_plan &&
                        ready_plan_end <= end_ns &&
                        (ready_plan_qid == 0 || out->query_id == 0 ||
                         ready_plan_qid == out->query_id)) {
                        out->plan_start_ns = ready_plan_start;
                        out->plan_end_ns = ready_plan_end;
                        out->has_plan = 1;
                    }
                }
                plan_open = 0;
                continue;
            }
            if (ev->old_event == PGWT_MARKER_EXEC_START) {
                if (ev->timestamp_ns == start_ns) {
                    found_exec = 1;
                    inside_exec = 1;
                    out->found_execution = 1;
                    out->query_id = ev->query_id;
                    if (plan_ready &&
                        (ready_plan_qid == 0 || out->query_id == 0 ||
                         ready_plan_qid == out->query_id)) {
                        out->plan_start_ns = ready_plan_start;
                        out->plan_end_ns = ready_plan_end;
                        out->has_plan = 1;
                    }
                } else if (inside_exec) {
                    inside_exec = 0;
                }
                /* A completed plan belongs to the next execution only. Scan
                 * context may contain earlier executions, so consume it even
                 * when this is not the requested start. */
                plan_ready = 0;
                continue;
            }
            if (ev->old_event == PGWT_MARKER_EXEC_END) {
                if (inside_exec)
                    inside_exec = 0;
                continue;
            }
        }

        if (PGWT_IS_MARKER(ev->old_event) ||
            (ev->flags & PGWT_EVENT_FLAG_SAMPLE) ||
            !inside_exec ||
            !interval_overlaps(ev->timestamp_ns, ev->duration_ns,
                               start_ns, end_ns))
            continue;

        struct pgwt_filter event_filter = f ? *f : (struct pgwt_filter){0};
        event_filter.pid = 0;
        if (!pgwt_filter_matches(&event_filter, ev))
            continue;

        struct pgwt_exec_lane *lane = NULL;
        if (ev->pid == leader_pid) {
            lane = &out->leader;
        } else if (detail_worker_leader(links, n_links, ev->pid) ==
                   (int)leader_pid) {
            lane = detail_worker_lane(out, ev->pid, 1);
        }
        if (lane && detail_lane_add(lane, ev, max_events_per_lane) != 0) {
            out->failed = 1;
            break;
        }
        if (out->failed)
            break;
    }

    out->total_events = out->leader.total_events;
    out->kept_events = out->leader.num_events;
    for (int i = 0; i < out->num_workers; i++) {
        out->total_events += out->workers[i].total_events;
        out->kept_events += out->workers[i].num_events;
    }
}

void pgwt_free_execution_detail(struct pgwt_execution_detail_result *out)
{
    if (!out) return;
    free(out->leader.events);
    for (int i = 0; i < out->num_workers; i++)
        free(out->workers[i].events);
    free(out->workers);
    memset(out, 0, sizeof(*out));
}

/* ── Per-query exec/plan lifecycle (Queries tab) ──────────── */

/* Grow (or create) the query-id table. 0 on success, -1 on allocation
 * failure. The pre-#275 table was a fixed 1024 slots whose probe loop
 * `while (used && query_id != qid) h = (h+1) & MASK;` never terminated once
 * the table held 1024 distinct query ids — a hang, not a wrong number. */
static int lifecycle_grow(struct pgwt_lifecycle_result *r)
{
    if (pgwt_test_alloc_fail("lifecycle_qid_grow"))
        return -1;
    int newcap = r->cap ? r->cap * 2 : 1024;
    struct pgwt_qid_lifecycle *ns =
        calloc((size_t)newcap, sizeof(*ns));
    if (!ns)
        return -1;
    uint32_t mask = (uint32_t)newcap - 1;
    for (int i = 0; i < r->cap; i++) {
        if (!r->slots[i].used)
            continue;
        uint32_t h = (uint32_t)((r->slots[i].query_id * 0x9e3779b9ULL) & mask);
        while (ns[h].used)
            h = (h + 1) & mask;
        ns[h] = r->slots[i];
    }
    free(r->slots);
    r->slots = ns;
    r->cap = newcap;
    return 0;
}

/* Slot for `qid`, inserting if absent. NULL only on allocation failure. */
static struct pgwt_qid_lifecycle *
lifecycle_slot(struct pgwt_lifecycle_result *r, uint64_t qid)
{
    if (r->cap == 0 || (r->n + 1) * 10 >= r->cap * 7) {
        if (lifecycle_grow(r) != 0)
            return NULL;
    }
    uint32_t mask = (uint32_t)r->cap - 1;
    uint32_t h = (uint32_t)((qid * 0x9e3779b9ULL) & mask);
    while (r->slots[h].used && r->slots[h].query_id != qid)
        h = (h + 1) & mask;
    if (!r->slots[h].used) {
        r->slots[h].used = 1;
        r->slots[h].query_id = qid;
        r->n++;
    }
    return &r->slots[h];
}

struct pgwt_qid_lifecycle *
pgwt_lifecycle_lookup(struct pgwt_lifecycle_result *r, uint64_t query_id)
{
    if (!r || r->cap == 0 || !r->slots)
        return NULL;
    uint32_t mask = (uint32_t)r->cap - 1;
    uint32_t h = (uint32_t)((query_id * 0x9e3779b9ULL) & mask);
    while (r->slots[h].used && r->slots[h].query_id != query_id)
        h = (h + 1) & mask;
    return r->slots[h].used ? &r->slots[h] : NULL;
}

void pgwt_lifecycle_free(struct pgwt_lifecycle_result *r)
{
    if (!r)
        return;
    for (int i = 0; i < r->cap; i++) {
        free(r->slots[i].exec_times);
        free(r->slots[i].plan_times);
    }
    free(r->slots);
    memset(r, 0, sizeof(*r));
}

/* Append one duration sample, keeping the written region contiguous. */
static void lifecycle_sample(double **times, int *nsamples, int *cap,
                             double ms)
{
    if (ms < 0 || *nsamples >= 10000)
        return;
    if (*nsamples >= *cap) {
        int nc = *cap ? *cap * 2 : 64;
        double *t = realloc(*times, (size_t)nc * sizeof(double));
        if (t) { *times = t; *cap = nc; }
    }
    if (*times && *nsamples < *cap)
        (*times)[(*nsamples)++] = ms;
}

void pgwt_compute_query_lifecycle(const struct pgwt_trace_event *events,
                                  int count,
                                  struct pgwt_lifecycle_result *out)
{
    memset(out, 0, sizeof(*out));
    if (!events || count <= 0)
        return;

    /* Per-pid open-marker state. #275: was `pid_st[512]` + linear probe +
     * `if (pi < 0) continue;`. Unbounded now; a pid gets a slot on its
     * first marker, exactly as before. */
    struct lc_pid_state {
        uint32_t pid;
        uint64_t exec_start_ns, plan_start_ns;
        uint64_t qid;
    };
    struct lc_pid_state *pid_st = NULL;
    int npids = 0, cap_pids = 0;
    struct pgwt_pid_index pid_ix;
    pgwt_pid_index_init(&pid_ix);

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        uint32_t m = ev->old_event;
        if (!PGWT_IS_MARKER(m)) continue;

        int pi = pgwt_pid_index_find(&pid_ix, ev->pid);
        if (pi < 0) {
            if (npids >= cap_pids) {
                int newcap = cap_pids ? cap_pids * 2 : 128;
                struct lc_pid_state *tmp =
                    pgwt_test_alloc_fail("lifecycle_pid_state") ? NULL
                    : cap_pids == 0
                      ? calloc((size_t)newcap, sizeof(*tmp))
                      : realloc(pid_st, (size_t)newcap * sizeof(*tmp));
                if (!tmp) goto oom;
                pid_st = tmp;
                cap_pids = newcap;
            }
            pi = npids;
            if (pgwt_pid_index_put(&pid_ix, ev->pid, pi) != 0)
                goto oom;
            npids++;
            memset(&pid_st[pi], 0, sizeof(pid_st[0]));
            pid_st[pi].pid = ev->pid;
        }

        if (m == PGWT_MARKER_EXEC_START) {
            pid_st[pi].exec_start_ns = ev->timestamp_ns;
            pid_st[pi].qid = ev->query_id;
        } else if (m == PGWT_MARKER_EXEC_END && pid_st[pi].exec_start_ns) {
            double ms = (ev->timestamp_ns - pid_st[pi].exec_start_ns) / 1e6;
            uint64_t qid = pid_st[pi].qid;
            pid_st[pi].exec_start_ns = 0;
            if (qid == 0) continue;
            struct pgwt_qid_lifecycle *lc = lifecycle_slot(out, qid);
            if (!lc) goto oom;
            lc->exec_count++;
            lc->exec_total_ms += ms;
            lifecycle_sample(&lc->exec_times, &lc->exec_nsamples,
                             &lc->exec_cap, ms);
        } else if (m == PGWT_MARKER_PLAN_START) {
            pid_st[pi].plan_start_ns = ev->timestamp_ns;
            pid_st[pi].qid = ev->query_id;
        } else if (m == PGWT_MARKER_PLAN_END && pid_st[pi].plan_start_ns) {
            double ms = (ev->timestamp_ns - pid_st[pi].plan_start_ns) / 1e6;
            uint64_t qid = pid_st[pi].qid;
            pid_st[pi].plan_start_ns = 0;
            if (qid == 0) continue;
            struct pgwt_qid_lifecycle *lc = lifecycle_slot(out, qid);
            if (!lc) goto oom;
            lc->plan_count++;
            lc->plan_total_ms += ms;
            lifecycle_sample(&lc->plan_times, &lc->plan_nsamples,
                             &lc->plan_cap, ms);
        }
    }

    free(pid_st);
    pgwt_pid_index_free(&pid_ix);
    return;

oom:
    /* Never a partial table: #275 is a short answer presented as complete. */
    free(pid_st);
    pgwt_pid_index_free(&pid_ix);
    pgwt_lifecycle_free(out);
    out->failed = 1;
}

/* ── Variants ────────────────────────────────────────────── */

/* One raw execution: events between EXEC_START and EXEC_END */
struct raw_exec {
    uint32_t events[128];       /* event_ids in order */
    uint64_t durations[128];    /* duration per event */
    int      len;
    uint64_t total_ns;          /* sum of durations */
    uint64_t query_id;
};

/* Compressed pattern for hashing */
struct compressed_pattern {
    uint32_t steps[PGWT_MAX_VARIANT_STEPS];
    int      is_loop[PGWT_MAX_VARIANT_STEPS];
    int      loop_len[PGWT_MAX_VARIANT_STEPS];
    int      num_steps;
};

/* Detect the longest repeating subsequence starting at pos.
 * Returns loop body length (0 if no loop). */
static int detect_loop(const uint32_t *events, int len, int pos)
{
    /* Try loop body lengths from 1 to len/2 */
    for (int body = 1; body <= (len - pos) / 2; body++) {
        int reps = 1;
        int j = pos + body;
        while (j + body <= len) {
            int match = 1;
            for (int k = 0; k < body; k++) {
                if (events[pos + k] != events[j + k]) { match = 0; break; }
            }
            if (!match) break;
            reps++;
            j += body;
        }
        if (reps >= 2) return body;  /* found a loop with >=2 repetitions */
    }
    return 0;
}

/* Compress a raw event sequence: collapse loops */
static void compress_exec(const struct raw_exec *raw, struct compressed_pattern *out)
{
    out->num_steps = 0;
    int i = 0;
    while (i < raw->len && out->num_steps < PGWT_MAX_VARIANT_STEPS) {
        int body = detect_loop(raw->events, raw->len, i);
        if (body > 0 && out->num_steps + body < PGWT_MAX_VARIANT_STEPS) {
            /* Mark first step of loop */
            out->steps[out->num_steps] = raw->events[i];
            out->is_loop[out->num_steps] = 1;
            out->loop_len[out->num_steps] = body;
            out->num_steps++;
            /* Add remaining loop body steps */
            for (int k = 1; k < body && out->num_steps < PGWT_MAX_VARIANT_STEPS; k++) {
                out->steps[out->num_steps] = raw->events[i + k];
                out->is_loop[out->num_steps] = 0;
                out->loop_len[out->num_steps] = 0;
                out->num_steps++;
            }
            /* Skip past all repetitions */
            int reps = 0;
            int j = i;
            while (j + body <= raw->len) {
                int match = 1;
                for (int k = 0; k < body; k++) {
                    if (raw->events[i + k] != raw->events[j + k]) { match = 0; break; }
                }
                if (!match) break;
                reps++;
                j += body;
            }
            i = j;
        } else {
            out->steps[out->num_steps] = raw->events[i];
            out->is_loop[out->num_steps] = 0;
            out->loop_len[out->num_steps] = 0;
            out->num_steps++;
            i++;
        }
    }
}

/* Hash a compressed pattern */
static uint64_t hash_pattern(const struct compressed_pattern *p)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < p->num_steps; i++) {
        h ^= p->steps[i];
        h *= 0x100000001b3ULL;
        if (p->is_loop[i]) {
            h ^= 0xDEADBEEF;
            h *= 0x100000001b3ULL;
        }
    }
    return h;
}

static int cmp_variant_time_desc(const void *a, const void *b)
{
    const struct pgwt_variant *va = a, *vb = b;
    if (vb->total_ns > va->total_ns) return 1;
    if (vb->total_ns < va->total_ns) return -1;
    return 0;
}

#define VARIANT_HT_SIZE 4096
#define VARIANT_HT_MASK (VARIANT_HT_SIZE - 1)

/* Per-variant p95 is computed from a bounded sample of execution times:
 * the FIRST PGWT_VARIANT_MAX_SAMPLES executions of that variant, in arrival
 * order. Beyond that the sample is closed and later executions are counted
 * (exec_count) but not sampled, so a truncated p95 is a percentile of a
 * prefix, not of the whole population -- prefix bias is a known limitation
 * (#271); pgwt_variant.p95_sample_n reports the sample size so a consumer
 * can tell a truncated p95 from a complete one. */
#define PGWT_VARIANT_MAX_SAMPLES 10000

struct variant_accum {
    uint64_t hash;
    int      used;
    struct compressed_pattern pattern;
    int      exec_count;
    uint64_t total_ns;
    uint64_t *exec_times;       /* for percentile calculation */
    int      exec_times_cap;
    /* Slots of exec_times ACTUALLY WRITTEN, incremented only on a successful
     * store. Never inferred from exec_count/cap (#271): that inference was
     * the bug -- it counted slots 10000..cap that the sampling guard never
     * wrote, so the sort and the p95 pick ran over indeterminate memory
     * (realloc does not zero). It also cannot survive a transient realloc
     * failure, which drops a sample and would leave a hole below the bound. */
    int      exec_times_n;
    double   loop_n_sum;        /* sum of loop iteration counts */
    int      loop_n_count;
    /* Track distinct query_ids (small set per variant) */
    uint64_t query_ids[16];
    int      num_query_ids;
    uint64_t top_query_id;
    int      top_query_count;
    /* Per-step duration accumulators */
    uint64_t step_total_ns[PGWT_MAX_VARIANT_STEPS];
    int      step_count[PGWT_MAX_VARIANT_STEPS];
};

static void variant_accum_add_qid(struct variant_accum *va, uint64_t qid)
{
    if (qid == 0) return;
    for (int i = 0; i < va->num_query_ids; i++) {
        if (va->query_ids[i] == qid) return;
    }
    if (va->num_query_ids < 16)
        va->query_ids[va->num_query_ids++] = qid;
}

void pgwt_compute_variants(const struct pgwt_trace_event *events, int count,
                            const struct pgwt_filter *f, int max_variants,
                            enum pgwt_variant_phase phase,
                            struct pgwt_variants_result *out)
{
    memset(out, 0, sizeof(*out));

    /* Select boundary markers based on phase */
    const uint32_t marker_start = (phase == PGWT_PHASE_PLAN)
        ? PGWT_MARKER_PLAN_START : PGWT_MARKER_EXEC_START;
    const uint32_t marker_end = (phase == PGWT_PHASE_PLAN)
        ? PGWT_MARKER_PLAN_END : PGWT_MARKER_EXEC_END;

    struct variant_accum *ht = calloc(VARIANT_HT_SIZE, sizeof(*ht));
    if (!ht) return;

    /* Phase 1: scan events, extract sequences per PID */
    struct pid_exec_state {
        uint32_t pid;
        int      active;    /* 1 = inside start..end markers */
        struct raw_exec exec;
    };
    /* #275: this was `struct pid_exec_state pids[512]` with a linear probe
     * and `if (num_pids >= MAX_PIDS) continue;` — the 513th distinct pid in
     * the window, and every one after it, was dropped with no flag, no log
     * and no error, so exec_count/avg/p95 came back quietly short under
     * ordinary connection churn. Now a pid-keyed hash index (pid_index.h)
     * over a growable state array, with NO bound: the only failure path is
     * a real allocation failure, which fails the request rather than
     * trimming the answer.
     *
     * State is created only for a pid that actually OPENS a phase
     * (marker_start). That is behaviour-identical to creating it on first
     * sight: every branch below is gated on ps->active, which a freshly
     * created state has at 0, so a pid with no state can do nothing. It
     * also keeps the footprint proportional to the pids that execute —
     * pid_exec_state embeds a ~1.5 KB raw_exec — instead of to every pid
     * that merely appears. */
    struct pid_exec_state *pids = NULL;
    int num_pids = 0, cap_pids = 0;
    struct pgwt_pid_index pid_ix;
    pgwt_pid_index_init(&pid_ix);
    int total_execs = 0;
    int alloc_failed = 0;

    for (int i = 0; i < count && !alloc_failed; i++) {
        const struct pgwt_trace_event *ev = &events[i];

        /* Find or create PID state */
        int pidx = pgwt_pid_index_find(&pid_ix, ev->pid);
        if (pidx < 0) {
            if (ev->old_event != marker_start)
                continue;
            if (num_pids >= cap_pids) {
                int newcap = cap_pids ? cap_pids * 2 : 64;
                /* calloc for the first block, realloc to grow: the state
                 * must start zeroed, and this keeps the common
                 * few-pids case to a single allocation. */
                struct pid_exec_state *tmp =
                    pgwt_test_alloc_fail("variants_pid_state") ? NULL
                    : cap_pids == 0
                      ? calloc((size_t)newcap, sizeof(*tmp))
                      : realloc(pids, (size_t)newcap * sizeof(*tmp));
                if (!tmp) { alloc_failed = 1; break; }
                pids = tmp;
                if (cap_pids)
                    memset(pids + cap_pids, 0,
                           (size_t)(newcap - cap_pids) * sizeof(*pids));
                cap_pids = newcap;
            }
            pidx = num_pids;
            if (pgwt_pid_index_put(&pid_ix, ev->pid, pidx) != 0) {
                alloc_failed = 1;
                break;
            }
            num_pids++;
            pids[pidx].pid = ev->pid;
        }

        struct pid_exec_state *ps = &pids[pidx];

        if (ev->old_event == marker_start) {
            /* Start a new phase */
            memset(&ps->exec, 0, sizeof(ps->exec));
            ps->active = 1;
            ps->exec.query_id = ev->query_id;
            continue;
        }

        if (ev->old_event == marker_end && ps->active) {
            /* End of execution — process it */
            ps->active = 0;
            total_execs++;

            struct raw_exec *re = &ps->exec;
            if (re->len == 0) {
                /* CPU-only execution (no waits) */
                re->events[0] = 0;  /* CPU */
                re->durations[0] = re->total_ns;
                re->len = 1;
            }

            /* Compress and hash */
            struct compressed_pattern cp;
            memset(&cp, 0, sizeof(cp));
            compress_exec(re, &cp);
            uint64_t h = hash_pattern(&cp);

            /* Count loop iterations in raw data */
            double loop_n = 0;
            if (re->len > 1) {
                /* Simple heuristic: len / compressed_len gives avg repetition */
                loop_n = cp.num_steps > 0 ? (double)re->len / cp.num_steps : 1;
            }

            /* Insert into hash table */
            uint32_t slot = (uint32_t)(h & VARIANT_HT_MASK);
            while (ht[slot].used && ht[slot].hash != h)
                slot = (slot + 1) & VARIANT_HT_MASK;

            struct variant_accum *va = &ht[slot];
            if (!va->used) {
                va->used = 1;
                va->hash = h;
                va->pattern = cp;
            }
            va->exec_count++;
            va->total_ns += re->total_ns;
            va->loop_n_sum += loop_n;
            va->loop_n_count++;
            variant_accum_add_qid(va, re->query_id);

            /* Sample this execution's duration for the p95, appending at
             * exec_times_n so the written region stays contiguous even if a
             * realloc fails for one execution and succeeds for the next. */
            if (va->exec_times_n < PGWT_VARIANT_MAX_SAMPLES) {
                if (va->exec_times_n >= va->exec_times_cap) {
                    int newcap = va->exec_times_cap ? va->exec_times_cap * 2 : 64;
                    if (newcap > PGWT_VARIANT_MAX_SAMPLES)
                        newcap = PGWT_VARIANT_MAX_SAMPLES;
                    uint64_t *tmp = realloc(va->exec_times, newcap * sizeof(uint64_t));
                    if (tmp) { va->exec_times = tmp; va->exec_times_cap = newcap; }
                }
                if (va->exec_times && va->exec_times_n < va->exec_times_cap)
                    va->exec_times[va->exec_times_n++] = re->total_ns;
            }

            /* Accumulate per-step durations from raw data */
            int si = 0;
            for (int j = 0; j < re->len && si < cp.num_steps; j++) {
                if (re->events[j] == cp.steps[si]) {
                    va->step_total_ns[si] += re->durations[j];
                    va->step_count[si]++;
                    /* Advance through pattern steps (handle loops) */
                    if (!cp.is_loop[si] || j + 1 >= re->len ||
                        re->events[j + 1] != cp.steps[si])
                        si++;
                    if (si >= cp.num_steps) si = cp.num_steps - 1;
                }
            }

            continue;
        }

        if (PGWT_IS_MARKER(ev->old_event) || PGWT_IS_MARKER(ev->new_event))
            continue;

        /* Regular event inside an execution */
        if (ps->active && ps->exec.len < 128) {
            if (!f || pgwt_filter_matches(f, ev)) {
                if (!pgwt_is_idle_event(ev->old_event)) {
                    ps->exec.events[ps->exec.len] = ev->old_event;
                    ps->exec.durations[ps->exec.len] = ev->duration_ns;
                    ps->exec.total_ns += ev->duration_ns;
                    ps->exec.len++;
                    if (ev->query_id) ps->exec.query_id = ev->query_id;
                }
            }
        }
    }

    /* An allocation failure in the pid map is the ONLY way out of phase 1
     * short of the end of the event array, and it fails the whole request.
     * It is never a silent trim: #275 is exactly a partial answer presented
     * as complete. */
    if (alloc_failed) {
        for (int i = 0; i < VARIANT_HT_SIZE; i++)
            free(ht[i].exec_times);
        free(ht);
        free(pids);
        pgwt_pid_index_free(&pid_ix);
        memset(out, 0, sizeof(*out));
        out->failed = 1;
        return;
    }

    /* Phase 2: collect variants, sort by total time */
    int nv = 0;
    for (int i = 0; i < VARIANT_HT_SIZE; i++)
        if (ht[i].used) nv++;

    struct pgwt_variant *variants = calloc(nv, sizeof(*variants));
    int vi = 0;
    for (int i = 0; i < VARIANT_HT_SIZE && vi < nv; i++) {
        if (!ht[i].used) continue;
        struct variant_accum *va = &ht[i];
        struct pgwt_variant *v = &variants[vi++];

        v->hash = va->hash;
        v->exec_count = va->exec_count;
        v->num_query_ids = va->num_query_ids;
        v->total_ns = va->total_ns;
        v->avg_ns = va->exec_count > 0 ? va->total_ns / va->exec_count : 0;
        v->avg_loop_n = va->loop_n_count > 0 ? va->loop_n_sum / va->loop_n_count : 1;
        v->num_steps = va->pattern.num_steps;

        /* Find most frequent query_id */
        v->top_query_id = va->num_query_ids > 0 ? va->query_ids[0] : 0;

        /* p95 over the sampled executions ONLY -- exec_times_n, never
         * exec_count (#271) and never the capacity. qsort, not the old
         * O(n^2) exchange sort (#269): the array holds no satellite data,
         * so the ascending permutation is unique and the picked value is
         * bit-identical to the exchange sort's for the same input. */
        v->p95_sample_n = va->exec_times_n;
        if (va->exec_times && va->exec_times_n > 0) {
            int n = va->exec_times_n;
            pgwt_sort_u64_asc(va->exec_times, n);
            v->p95_ns = va->exec_times[(int)(n * 0.95)];
        }

        /* Copy steps with names and timing */
        for (int s = 0; s < va->pattern.num_steps && s < PGWT_MAX_VARIANT_STEPS; s++) {
            v->steps[s].event_id = va->pattern.steps[s];
            v->steps[s].is_loop = va->pattern.is_loop[s];
            v->steps[s].loop_len = va->pattern.loop_len[s];
            if (va->pattern.steps[s] == 0)
                snprintf(v->steps[s].name, 64, "CPU*");
            else
                pgwt_event_full_name(va->pattern.steps[s],
                                     v->steps[s].name, sizeof(v->steps[s].name));
            v->step_avg_ns[s] = va->step_count[s] > 0
                ? va->step_total_ns[s] / va->step_count[s] : 0;
        }

        free(va->exec_times);
    }

    qsort(variants, vi, sizeof(variants[0]), cmp_variant_time_desc);

    int nr = vi < max_variants ? vi : max_variants;
    out->variants = variants;
    out->num_variants = nr;
    out->total_executions = total_execs;

    free(ht);
    free(pids);
    pgwt_pid_index_free(&pid_ix);
}
