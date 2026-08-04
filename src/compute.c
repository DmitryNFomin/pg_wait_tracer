/* compute.c — Server-side compute functions for pgwt-server
 *
 * Direct port of client/src/compute.rs. Works on raw pgwt_trace_event arrays.
 * All result structs use malloc'd arrays — caller frees with free(result->rows).
 */
#include "compute.h"
#include "summary_writer.h"
#include "summary_reader.h"
#include "wait_event.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

    for (int i = 0; i < count; i++) {
        struct pgwt_trace_event *ev = &events[i];
        struct tag_pid_state *st = tag_pid_get(ht, ev->pid);
        if (!st)
            continue;

        /* Markers drive the per-pid windows and are otherwise left alone. */
        if (PGWT_IS_MARKER(ev->old_event)) {
            uint64_t ts = ev->timestamp_ns;
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
                             const struct pgwt_filter *f, double wall_ms,
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

        double dur_ns = (double)ev->duration_ns;

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
                double m = (double)ev->cpu_ns;
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
                wait_gap_cpu_ns += (double)ev->cpu_ns;   /* observability only */
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
    /* Max rows: 1 (DB Time) + 1 (Off-CPU*) + NUM_CLASSES * (1 class + 5 sub) */
    int max_rows = 2 + PGWT_NUM_CLASSES * 6;
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
        if (strcasecmp(classes[c].name, "cpu") == 0)
            snprintf(rows[nr].name, sizeof(rows[nr].name), "CPU*");
        else
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%.31s", display);
        rows[nr].time_ms     = cls_ms;
        rows[nr].pct_db_time = pct;
        rows[nr].aas         = wall_ms > 0 ? cls_ms / wall_ms : 0;
        rows[nr].indent      = 1;
        nr++;

        /* T8: Off-CPU* is a sibling of CPU* (the measured off-CPU/runqueue-
         * unaccounted remainder of on-CPU gaps). Emitted only where measured
         * cpu_ns existed (v3 exact data) — sampled/v2 windows show CPU* alone
         * (Off-CPU is unavailable, not zero). It is part of DB Time. */
        if (strcasecmp(classes[c].name, "cpu") == 0 && has_measured_cpu) {
            double off_ms = offcpu_ns / 1e6;
            snprintf(rows[nr].name, sizeof(rows[nr].name), "Off-CPU*");
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

/* Histogram bucket upper boundaries in microseconds */
static const uint64_t hist_upper_us[HISTOGRAM_BUCKETS] = {
    1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 16384
};

static double hist_percentile(const uint64_t hist[HISTOGRAM_BUCKETS],
                              uint64_t total, double pct)
{
    if (total == 0) return 0;
    uint64_t threshold = (uint64_t)((double)total * pct);
    uint64_t cumulative = 0;
    for (int b = 0; b < HISTOGRAM_BUCKETS; b++) {
        cumulative += hist[b];
        if (cumulative >= threshold)
            return (double)hist_upper_us[b];
    }
    return (double)hist_upper_us[HISTOGRAM_BUCKETS - 1];
}

static int cmp_event_row_desc(const void *a, const void *b)
{
    double da = ((const struct pgwt_event_row *)a)->total_ms;
    double db = ((const struct pgwt_event_row *)b)->total_ms;
    return (db > da) - (db < da);
}

void pgwt_compute_top_events(const struct pgwt_trace_event *events, int count,
                             const struct pgwt_filter *f, double wall_ms,
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

        /* Idle-but-visible events (Client:ClientRead) appear in the list
         * but are excluded from DB Time. Their time is not part of the
         * DB-Time denominator, so a numeric %DB is meaningless for them
         * (it overshoots and makes the column sum past 100%). Their %DB is
         * marked with a sentinel below and rendered as "—". */
        if (!pgwt_is_idle_event(ev->old_event))
            db_time_ns += ev->duration_ns;

        /* Hash by event_id */
        uint32_t h = ev->old_event & EVENT_HT_MASK;
        while (ht[h].count > 0 && ht[h].event_id != ev->old_event)
            h = (h + 1) & EVENT_HT_MASK;

        if (ht[h].count == 0) {
            ht[h].event_id = ev->old_event;
            num_entries++;
        }
        ht[h].count++;
        ht[h].total_ns += ev->duration_ns;
        /* Latency stats from exact records only (FID-3). */
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
        r->max_us   = (double)ht[i].max_ns / 1000.0;
        r->p50_us   = hist_percentile(ht[i].hist, ht[i].exact_count, 0.50);
        r->p95_us   = hist_percentile(ht[i].hist, ht[i].exact_count, 0.95);
        r->p99_us   = hist_percentile(ht[i].hist, ht[i].exact_count, 0.99);
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
                               const struct pgwt_filter *f, double wall_ms,
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
        ht[h].total_ns += ev->duration_ns;

        if (ev->old_event == 0) {
            ht[h].cpu_ns += ev->duration_ns;
        } else {
            /* O(1) hash lookup for per-wait totals */
            int wslot = wait_ht_find_or_insert(ht[h].waits, ev->old_event);
            if (wslot >= 0)
                ht[h].waits[wslot].total_ns += ev->duration_ns;
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
    uint64_t total_ns;
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
                              const struct pgwt_filter *f, double wall_ms,
                              struct pgwt_queries_result *out)
{
    (void)wall_ms;
    memset(out, 0, sizeof(*out));

    struct query_accum *ht = calloc(QUERY_HT_SIZE, sizeof(*ht));
    int num_entries = 0;
    uint64_t db_time_ns = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
            continue;
        if (ev->flags & PGWT_EVENT_FLAG_IO_WORKER)
            continue;   /* T2: io_workers are structurally query-less */
        if (ev->query_id == 0)
            continue;

        db_time_ns += ev->duration_ns;

        uint32_t h = (uint32_t)(ev->query_id ^ (ev->query_id >> 32)) & QUERY_HT_MASK;
        while (ht[h].count > 0 && ht[h].query_id != ev->query_id)
            h = (h + 1) & QUERY_HT_MASK;

        if (ht[h].count == 0) {
            ht[h].query_id = ev->query_id;
            num_entries++;
        }
        ht[h].count++;
        ht[h].total_ns += ev->duration_ns;
        ht[h].class_ns[pgwt_wait_class_index(ev->old_event)] += ev->duration_ns;

        /* O(1) hash lookup for per-wait totals */
        if (ev->old_event != 0) {
            int wslot = wait_ht_find_or_insert(ht[h].waits, ev->old_event);
            if (wslot >= 0)
                ht[h].waits[wslot].total_ns += ev->duration_ns;
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
        r->avg_us   = ht[i].count > 0
                     ? (double)ht[i].total_ns / (double)ht[i].count / 1000.0
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

/* Client:ClientRead time recorded for a query in this summary record
 * (from its top_events). Used to subtract the idle-but-visible ClientRead
 * portion out of the lumped Client class_ns so it is not counted as DB
 * Time / AAS (load), while ClientRead still shows in event lists. */
static double summary_query_clientread_ns(const struct pgwt_summary_query *sq)
{
    for (int j = 0; j < sq->num_top_events; j++)
        if (sq->top_events[j].event_id == WEI(PG_WAIT_CLIENT, 0))
            return (double)sq->top_events[j].total_ns;
    return 0.0;
}

/* System-wide Client:ClientRead time recorded in this summary record
 * (from the per-event table). Same purpose as above for the no-filter path. */
static double summary_clientread_ns(const struct pgwt_summary_accum *rec)
{
    for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
        const struct pgwt_summary_event *se = &rec->events[e];
        if (se->event_id == WEI(PG_WAIT_CLIENT, 0))
            return (double)se->total_ns;
    }
    return 0.0;
}

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
            /* AAS is active load: exclude idle Client:ClientRead from the
             * lumped Client class_ns. */
            double cr_ns = summary_query_clientread_ns(sq);
            for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
                if (c == PGWT_CLASS_ACTIVITY) continue;
                double cls_ns = (double)sq->class_ns[c];
                if (c == PGWT_CLASS_CLIENT) cls_ns -= cr_ns;
                ctx->buckets[bi].class_aas[c] += cls_ns;
            }
            break;
        }
        return 0;
    }

    if (!ctx->has_class_filter && !ctx->has_event_filter) {
        /* AAS is active load: exclude idle Client:ClientRead. */
        double cr_ns = summary_clientread_ns(rec);
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            if (c == PGWT_CLASS_ACTIVITY) continue;
            double cls_ns = (double)rec->class_ns[c];
            if (c == PGWT_CLASS_CLIENT) cls_ns -= cr_ns;
            ctx->buckets[bi].class_aas[c] += cls_ns;
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

            /* Client:ClientRead is idle: route it to the idle bucket, not
             * DB Time, even though it is lumped into the Client class_ns. */
            double cr_ns = summary_query_clientread_ns(sq);
            for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
                double cls_ns = (double)sq->class_ns[c];
                if (c == PGWT_CLASS_CLIENT)
                    cls_ns -= cr_ns;
                if (c == PGWT_CLASS_ACTIVITY) {
                    ctx->idle_time_ns += (double)sq->class_ns[c];
                } else {
                    ctx->classes[c].total_ns += cls_ns;
                    ctx->db_time_ns += cls_ns;
                }
            }
            ctx->idle_time_ns += cr_ns;
            for (int j = 0; j < sq->num_top_events; j++) {
                uint32_t eid = sq->top_events[j].event_id;
                if (pgwt_is_idle_event(eid)) continue;
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
    if (f->class_name[0] == '\0' && f->event_id == 0) {
        /* Client:ClientRead is idle: subtract it out of the lumped Client
         * class_ns so it counts toward idle time, not DB Time. */
        double cr_ns = summary_clientread_ns(rec);
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            double cls_ns = (double)rec->class_ns[c];
            if (c == PGWT_CLASS_CLIENT)
                cls_ns -= cr_ns;
            if (c == PGWT_CLASS_ACTIVITY) {
                ctx->idle_time_ns += (double)rec->class_ns[c];
            } else {
                ctx->classes[c].total_ns += cls_ns;
                ctx->db_time_ns += cls_ns;
            }
        }
        ctx->idle_time_ns += cr_ns;
    }

    /* Per-event stats: iterate event entries */
    for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
        const struct pgwt_summary_event *se = &rec->events[e];
        if (se->event_id == 0 && se->count == 0) continue;
        if (!summary_event_matches_filter(f, se->event_id)) continue;
        if (pgwt_is_idle_event(se->event_id)) continue;

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
    int max_rows = 1 + PGWT_NUM_CLASSES * 6;
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
        row->max_us   = (double)ctx.ht[i].max_ns / 1000.0;
        row->p50_us   = hist_percentile(ctx.ht[i].hist, ctx.ht[i].exact_count, 0.50);
        row->p95_us   = hist_percentile(ctx.ht[i].hist, ctx.ht[i].exact_count, 0.95);
        row->p99_us   = hist_percentile(ctx.ht[i].hist, ctx.ht[i].exact_count, 0.99);
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
 * to timestamp. Two sessions overlap if their time intervals overlap on the
 * same event. We detect bursts by sorting events by start time and using a
 * sliding window.
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

static int cmp_burst_desc(const void *a, const void *b)
{
    int na = ((const struct pgwt_burst *)a)->num_sessions;
    int nb = ((const struct pgwt_burst *)b)->num_sessions;
    return (nb > na) - (nb < na);
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

    /* Build sorted list of active intervals */
    int cap = count < 100000 ? count : 100000;  /* limit for memory */
    struct active_entry *active = malloc(cap * sizeof(*active));
    int nactive = 0;

    for (int i = 0; i < count && nactive < cap; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
            continue;
        if (ev->old_event == 0) continue;  /* skip CPU for burst detection */
        if (ev->timestamp_ns < from_ns || ev->timestamp_ns > to_ns)
            continue;

        active[nactive].pid = ev->pid;
        active[nactive].event_id = ev->old_event;
        active[nactive].start_ns = ev->timestamp_ns - ev->duration_ns;
        active[nactive].end_ns = ev->timestamp_ns;
        nactive++;
    }

    qsort(active, nactive, sizeof(active[0]), cmp_active_by_start);

    /* Phase 1: Peak concurrency per AAS bucket.
     * For each bucket, count DISTINCT PIDs waiting on the same event. */
    for (int b = 0; b < num_buckets; b++) {
        uint64_t bstart = from_ns + (uint64_t)b * bucket_ns;
        uint64_t bend = bstart + bucket_ns;

        /* Track distinct PIDs per event using small hash sets */
        struct {
            uint32_t eid;
            uint32_t pids[128]; /* distinct PIDs for this event */
            int npids;
        } ev_pids[64];
        int nev = 0;

        for (int i = 0; i < nactive; i++) {
            if (active[i].start_ns >= bend) break;
            if (active[i].end_ns <= bstart) continue;

            /* Find or create entry for this event */
            int found = -1;
            for (int j = 0; j < nev; j++) {
                if (ev_pids[j].eid == active[i].event_id) {
                    found = j;
                    break;
                }
            }
            if (found < 0 && nev < 64) {
                found = nev;
                ev_pids[nev].eid = active[i].event_id;
                ev_pids[nev].npids = 0;
                nev++;
            }
            if (found < 0) continue;

            /* Add PID if not already present (distinct count) */
            int dup = 0;
            for (int k = 0; k < ev_pids[found].npids; k++) {
                if (ev_pids[found].pids[k] == active[i].pid) {
                    dup = 1;
                    break;
                }
            }
            if (!dup && ev_pids[found].npids < 128)
                ev_pids[found].pids[ev_pids[found].npids++] = active[i].pid;
        }

        /* Find peak (distinct PIDs, not event count) */
        for (int j = 0; j < nev; j++) {
            if (ev_pids[j].npids > out->peak_sessions[b]) {
                out->peak_sessions[b] = ev_pids[j].npids;
                out->peak_event[b] = ev_pids[j].eid;
            }
        }
    }

    /* Phase 2: Burst detection.
     * Sliding window: find groups of N+ sessions entering the same wait
     * event within burst_window_ns of each other. */
    int burst_cap = 256;
    struct pgwt_burst *bursts = calloc(burst_cap, sizeof(*bursts));
    int nbursts = 0;

    for (int i = 0; i < nactive && nbursts < burst_cap; i++) {
        uint32_t eid = active[i].event_id;
        uint64_t window_end = active[i].start_ns + burst_window_ns;

        /* Count sessions with same event starting within window */
        int burst_count = 0;
        uint32_t pids[64];
        int npids = 0;

        for (int j = i; j < nactive; j++) {
            if (active[j].start_ns > window_end) break;
            if (active[j].event_id == eid) {
                burst_count++;
                if (npids < 64) {
                    /* Avoid duplicate PIDs */
                    int dup = 0;
                    for (int k = 0; k < npids; k++)
                        if (pids[k] == active[j].pid) { dup = 1; break; }
                    if (!dup) pids[npids++] = active[j].pid;
                }
            }
        }

        if (npids >= burst_threshold) {
            struct pgwt_burst *b = &bursts[nbursts++];
            b->timestamp_ns = active[i].start_ns;
            b->event_id = eid;
            b->num_sessions = npids;
            pgwt_event_full_name(eid, b->event_name, sizeof(b->event_name));
            b->num_pids = npids < 64 ? npids : 64;
            memcpy(b->pids, pids, b->num_pids * sizeof(uint32_t));

            /* Skip past this burst to avoid duplicates */
            while (i + 1 < nactive &&
                   active[i + 1].start_ns <= window_end &&
                   active[i + 1].event_id == eid)
                i++;
        }
    }

    free(active);

    qsort(bursts, nbursts, sizeof(bursts[0]), cmp_burst_desc);

    out->bursts = bursts;
    out->num_bursts = nbursts;
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
    int active_row;
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
    (*states)[idx].active_row = -1;
    return idx;
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
            /* A second start does not manufacture an end for the first one:
             * the prior row remains explicitly in progress. */
            st->active_row = -1;
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
                st->active_row = execution_append(&rows, &n_rows, &cap_rows,
                                                  &row);
                if (st->active_row < 0) {
                    out->failed = 1;
                    break;
                }
            }
            /* A completed plan belongs to this next execution, including a
             * start outside the caller's output window. Never reuse it. */
            st->plan_ready = 0;
            continue;
        }
        if (marker == PGWT_MARKER_EXEC_END) {
            if (st->active_row >= 0 && st->active_row < n_rows &&
                ev->timestamp_ns >= rows[st->active_row].start_ns) {
                struct pgwt_execution *row = &rows[st->active_row];
                row->end_ns = ev->timestamp_ns;
                row->in_progress = 0;
                if (row->query_id == 0)
                    row->query_id = ev->query_id;
            }
            st->active_row = -1;
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
        if (event_matches && st->active_row >= 0 && st->active_row < n_rows) {
            rows[st->active_row].n_events++;
            rows[st->active_row].matches_event_filter = 1;
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
                int row_idx = states[li].active_row;
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

struct variant_accum {
    uint64_t hash;
    int      used;
    struct compressed_pattern pattern;
    int      exec_count;
    uint64_t total_ns;
    uint64_t *exec_times;       /* for percentile calculation */
    int      exec_times_cap;
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
    #define MAX_PIDS 512
    struct pid_exec_state *pids = calloc(MAX_PIDS, sizeof(*pids));
    int num_pids = 0;
    int total_execs = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];

        /* Find or create PID state */
        int pidx = -1;
        for (int j = 0; j < num_pids; j++) {
            if (pids[j].pid == ev->pid) { pidx = j; break; }
        }
        if (pidx < 0) {
            if (num_pids >= MAX_PIDS) continue;
            pidx = num_pids++;
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

            /* Track exec times for percentile */
            if (va->exec_count <= 10000) {
                if (va->exec_count > va->exec_times_cap) {
                    int newcap = va->exec_times_cap ? va->exec_times_cap * 2 : 64;
                    uint64_t *tmp = realloc(va->exec_times, newcap * sizeof(uint64_t));
                    if (tmp) { va->exec_times = tmp; va->exec_times_cap = newcap; }
                }
                if (va->exec_times && va->exec_count <= va->exec_times_cap)
                    va->exec_times[va->exec_count - 1] = re->total_ns;
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

        /* p95 */
        if (va->exec_times && va->exec_count > 0) {
            int n = va->exec_count < va->exec_times_cap ? va->exec_count : va->exec_times_cap;
            /* Simple sort for p95 */
            for (int a = 0; a < n - 1; a++)
                for (int b = a + 1; b < n; b++)
                    if (va->exec_times[a] > va->exec_times[b]) {
                        uint64_t tmp = va->exec_times[a];
                        va->exec_times[a] = va->exec_times[b];
                        va->exec_times[b] = tmp;
                    }
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
}
