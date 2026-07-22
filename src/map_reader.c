/* map_reader.c — Read BPF state_map for open intervals, accumulator helpers
 *
 * The accumulator core (get-or-create helpers, time model, histogram
 * bucketing) is pure and BPF-free; the map-reading functions need libbpf
 * and the skeleton. Guarded by !PGWT_SERVER — same pattern as sampler.c /
 * anomaly.c — so unit tests (e.g. test_replay_fidelity) can link the pure
 * core on a box without bpftool. */
#include "map_reader.h"
#include "wait_event.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>

#ifndef PGWT_SERVER
#include "daemon.h"
#include "discovery.h"   /* pgwt_read_sched_cpu_ns (T8 live measured CPU) */
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

/* Must match bpf_ktime_get_ns() clock source (CLOCK_MONOTONIC). */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif /* !PGWT_SERVER */

void pgwt_accum_init(struct pgwt_accumulator *acc)
{
    memset(acc, 0, sizeof(*acc));
}

struct pgwt_pid_accum *pgwt_find_pid_accum(struct pgwt_accumulator *acc, uint32_t pid)
{
    for (int i = 0; i < acc->num_pids; i++) {
        if (acc->pids[i].pid == pid && acc->pids[i].active)
            return &acc->pids[i];
    }
    return NULL;
}

struct pgwt_pid_accum *pgwt_get_or_create_pid(struct pgwt_accumulator *acc, uint32_t pid)
{
    struct pgwt_pid_accum *pa = pgwt_find_pid_accum(acc, pid);
    if (pa) return pa;

    if (acc->num_pids >= MAX_BACKENDS)
        return NULL;
    pa = &acc->pids[acc->num_pids++];
    memset(pa, 0, sizeof(*pa));
    pa->pid = pid;
    pa->active = true;
    return pa;
}

struct pgwt_event_stats *pgwt_get_or_create_event(struct pgwt_pid_accum *pa, uint32_t we)
{
    for (int i = 0; i < pa->num_events; i++) {
        if (pa->events[i].wait_event == we)
            return &pa->events[i];
    }
    if (pa->num_events >= MAX_EVENTS_PER_PID)
        return NULL;
    struct pgwt_event_stats *es = &pa->events[pa->num_events++];
    memset(es, 0, sizeof(*es));
    es->wait_event = we;
    es->min_ns = UINT64_MAX;
    return es;
}

struct pgwt_event_stats *pgwt_find_system_event(struct pgwt_accumulator *acc,
                                                 uint32_t wait_event)
{
    for (int i = 0; i < acc->num_system_events; i++) {
        if (acc->system_events[i].wait_event == wait_event)
            return &acc->system_events[i];
    }
    return NULL;
}

struct pgwt_event_stats *pgwt_get_or_create_system_event(struct pgwt_accumulator *acc,
                                                          uint32_t we)
{
    struct pgwt_event_stats *se = pgwt_find_system_event(acc, we);
    if (se) return se;

    if (acc->num_system_events >= 4096)
        return NULL;
    se = &acc->system_events[acc->num_system_events++];
    memset(se, 0, sizeof(*se));
    se->wait_event = we;
    se->min_ns = UINT64_MAX;
    return se;
}

struct pgwt_query_event_stats *pgwt_get_or_create_query_event(
    struct pgwt_accumulator *acc, uint64_t query_id, uint32_t we)
{
    for (int i = 0; i < acc->num_query_events; i++) {
        if (acc->query_events[i].query_id == query_id &&
            acc->query_events[i].wait_event == we)
            return &acc->query_events[i];
    }
    if (acc->num_query_events >= MAX_QUERY_EVENTS)
        return NULL;
    struct pgwt_query_event_stats *qe = &acc->query_events[acc->num_query_events++];
    memset(qe, 0, sizeof(*qe));
    qe->query_id = query_id;
    qe->wait_event = we;
    qe->min_ns = UINT64_MAX;
    return qe;
}

void pgwt_update_time_model(struct pgwt_time_model *tm, uint32_t event,
                             uint64_t duration_ns)
{
    if (pgwt_is_idle_event(event)) {
        tm->activity_time_ns += duration_ns;
    } else if (event == 0) {
        tm->cpu_time_ns += duration_ns;
        tm->db_time_ns += duration_ns;
    } else {
        int cls = WE_CLASS(event);
        switch (cls) {
        case PG_WAIT_IO:        tm->io_time_ns += duration_ns; break;
        case PG_WAIT_LWLOCK:    tm->lwlock_time_ns += duration_ns; break;
        case PG_WAIT_LOCK:      tm->lock_time_ns += duration_ns; break;
        case PG_WAIT_BUFFERPIN: tm->bufferpin_time_ns += duration_ns; break;
        case PG_WAIT_CLIENT:    tm->client_time_ns += duration_ns; break;
        case PG_WAIT_IPC:       tm->ipc_time_ns += duration_ns; break;
        case PG_WAIT_TIMEOUT:   tm->timeout_time_ns += duration_ns; break;
        case PG_WAIT_EXTENSION: tm->extension_time_ns += duration_ns; break;
        case PG_WAIT_ACTIVITY:  tm->activity_time_ns += duration_ns; break;
        }
        tm->db_time_ns += duration_ns;
    }
}

uint32_t pgwt_duration_to_bucket(uint64_t ns)
{
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

#ifndef PGWT_SERVER
void pgwt_read_state_map(struct pgwt_daemon *d)
{
    int state_fd = bpf_map__fd(d->skel->maps.state_map);
    uint32_t skey = 0, snext;
    struct pgwt_pid_state sval;
    uint64_t now = now_ns();

    while (bpf_map_get_next_key(state_fd, &skey, &snext) == 0) {
        if (bpf_map_lookup_elem(state_fd, &snext, &sval) == 0) {
            /* T2: entries without a live watchpoint are query-id/cmd-gate
             * seeds — their last_event/last_ts are NOT interval state.
             * Counting them fabricated an ever-growing open "CPU" interval
             * per seeded backend (the live-view sibling of study defect 2).
             * Only reached in tiered mode during escalation, where every
             * ATTACHED backend has wp_live = 1 from the preseed. */
            if (!sval.wp_live) {
                skey = snext;
                continue;
            }
            uint64_t open_ns = now - sval.last_ts;
            uint32_t we = sval.last_event;

            /* Store current state for active sessions view */
            struct pgwt_pid_accum *pa_cur = pgwt_get_or_create_pid(&d->accum, snext);
            if (pa_cur) {
                pa_cur->current_event = we;
                pa_cur->current_wait_ns = open_ns;
            }

            /* Skip the open interval if d->accum already has closed data for
             * this (PID, event) — EXCEPT for on-CPU (we==0), see below.
             *
             * The guard exists to avoid double-counting a WAIT that the live
             * open-interval read and a same-window closed record could both
             * represent; wait accounting has shipped on it and is asserted by
             * test_deterministic, so it is preserved unchanged.
             *
             * On-CPU (we==0) MUST bypass it. The open [last_ts, now] stretch is
             * the backend's current, not-yet-closed on-CPU run; last_ts is
             * stamped by BPF on every transition, and event_accum holds only
             * CLOSED transitions, so the two are disjoint and the open on-CPU
             * segment is purely additive. With the guard applied to we==0, a
             * compute backend caught via FORK (client backends pass through
             * startup ClientRead → a brief on-CPU stretch → the query, leaving a
             * CLOSED we==0 segment) had its entire ongoing on-CPU LOOP
             * suppressed — the pinned query read ~0 CPU live, while a
             * scan-caught backend (no prior we==0) read the full amount. The
             * measured-CPU feature (S3) depends on this open on-CPU read, so
             * we==0 is always accounted. (The analogous ongoing-repeated-WAIT
             * under-count is tracked in docs/ROADMAP_AND_STATUS.md.) */
            bool has_closed_data = false;
            if (we != 0 && pa_cur) {
                for (int i = 0; i < pa_cur->num_events; i++) {
                    if (pa_cur->events[i].wait_event == we &&
                        pa_cur->events[i].count > 0) {
                        has_closed_data = true;
                        break;
                    }
                }
            }

            if (open_ns > 0 && !has_closed_data) {
                /* T8 symptom #3: for an on-CPU (we==0) in-progress interval,
                 * the CPU quantity is the MEASURED schedstat delta since the
                 * seeded base, not the full wall gap — a backend preempted or
                 * runqueue-stalled shows only the CPU it actually burned, and
                 * DB Time carries the wall gap (the off-CPU remainder is the
                 * DB-Time-minus-CPU unaccounted time). Display-time only: no
                 * interim trace record is written. Legacy (cpu_accounting off)
                 * keeps the full gap as CPU, byte-identical to before. */
                uint64_t cpu_open = open_ns;
                if (we == 0 && d->cpu_accounting) {
                    /* S3: exact on-CPU ns for the open interval = cpu_ns_total +
                     * current stretch (on_cpu_ts != 0 iff running), minus the
                     * seeded base. `now` is CLOCK_MONOTONIC == bpf_ktime. */
                    uint64_t exact = sval.cpu_ns_total;
                    if (sval.on_cpu_ts && now >= sval.on_cpu_ts)
                        exact += now - sval.on_cpu_ts;
                    if (exact >= sval.last_cpu_ns) {
                        uint64_t m = exact - sval.last_cpu_ns;
                        cpu_open = m < open_ns ? m : open_ns;   /* clamp to wall */
                    }
                }

                /* Per-PID accumulation */
                struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(&d->accum, snext);
                if (pa) {
                    /* CPU pseudo-event stats reflect measured CPU; waits use
                     * wall (they carry no CPU). */
                    uint64_t stat_ns = (we == 0) ? cpu_open : open_ns;
                    struct pgwt_event_stats *es = pgwt_get_or_create_event(pa, we);
                    if (es) {
                        es->count += 1;
                        es->total_ns += stat_ns;
                        if (stat_ns < es->min_ns) es->min_ns = stat_ns;
                        if (stat_ns > es->max_ns) es->max_ns = stat_ns;
                    }
                    if (we == 0) {
                        pa->cpu_time_ns += cpu_open;
                    } else if (!pgwt_is_idle_event(we)) {
                        pa->wait_time_ns += open_ns;
                    }
                    if (!pgwt_is_idle_event(we))
                        pa->db_time_ns += open_ns;   /* DB Time = wall gap */
                }

                /* System-wide accumulation */
                struct pgwt_event_stats *se = pgwt_get_or_create_system_event(&d->accum, we);
                if (se) {
                    uint64_t stat_ns = (we == 0) ? cpu_open : open_ns;
                    se->count += 1;
                    se->total_ns += stat_ns;
                    if (stat_ns < se->min_ns) se->min_ns = stat_ns;
                    if (stat_ns > se->max_ns) se->max_ns = stat_ns;
                }

                /* Time model by class: on-CPU splits into measured CPU + the
                 * off-CPU remainder (both inside DB Time). */
                if (we == 0) {
                    d->accum.tm.cpu_time_ns += cpu_open;
                    d->accum.tm.db_time_ns += open_ns;
                } else {
                    pgwt_update_time_model(&d->accum.tm, we, open_ns);
                }

                /* Query-level open interval */
                if (sval.last_query_id != 0) {
                    uint64_t stat_ns = (we == 0) ? cpu_open : open_ns;
                    struct pgwt_query_event_stats *qe =
                        pgwt_get_or_create_query_event(&d->accum, sval.last_query_id, we);
                    if (qe) {
                        qe->count += 1;
                        qe->total_ns += stat_ns;
                        if (stat_ns < qe->min_ns) qe->min_ns = stat_ns;
                        if (stat_ns > qe->max_ns) qe->max_ns = stat_ns;
                    }
                }
            }
        }
        skey = snext;
    }
}

/* Read BPF accum_map (PERCPU lightweight mode) and merge into event_accum.
 * Sums values across all CPUs, then deletes entries so next interval starts fresh. */
void pgwt_read_accum_map(struct pgwt_daemon *d)
{
    int accum_fd = bpf_map__fd(d->skel->maps.accum_map);
    struct pgwt_accumulator *acc = d->event_accum;
    int num_cpus = libbpf_num_possible_cpus();
    if (num_cpus <= 0) num_cpus = 1;

    /* Per-CPU value buffer (VLA, bounded by MAX_CPUS) */
    struct pgwt_accum_val pcpu_vals[num_cpus > MAX_CPUS ? MAX_CPUS : num_cpus];

    uint32_t key = 0, next_key = 0;

    while (bpf_map_get_next_key(accum_fd, &key, &next_key) == 0) {
        if (bpf_map_lookup_elem(accum_fd, &next_key, pcpu_vals) == 0) {
            uint32_t we = next_key;
            uint64_t dur = 0, cnt = 0;

            /* Sum across CPUs */
            int nc = num_cpus > MAX_CPUS ? MAX_CPUS : num_cpus;
            for (int i = 0; i < nc; i++) {
                dur += pcpu_vals[i].total_ns;
                cnt += pcpu_vals[i].count;
            }

            /* Time model by class */
            pgwt_update_time_model(&acc->tm, we, dur);

            /* System-wide event stats */
            struct pgwt_event_stats *se = pgwt_get_or_create_system_event(acc, we);
            if (se) {
                se->count += cnt;
                se->total_ns += dur;
            }

            /* Delete after reading */
            bpf_map_delete_elem(accum_fd, &next_key);
        }
        key = next_key;
    }
}


/* Sum one BPF fail_counters slot across CPUs (CAP-1/CAP-6). The BPF programs
 * bump these when a map insert fails (state_map / seen_query_ids full); the
 * control socket surfaces them so a full map is never silent. */
uint64_t pgwt_read_bpf_fail_counter(struct pgwt_daemon *d, uint32_t slot)
{
    if (!d->skel || slot >= PGWT_BPF_FAIL_MAX)
        return 0;
    int fd = bpf_map__fd(d->skel->maps.fail_counters);
    if (fd < 0)
        return 0;

    int num_cpus = libbpf_num_possible_cpus();
    if (num_cpus <= 0)
        num_cpus = 1;
    uint64_t per_cpu[num_cpus];
    if (bpf_map_lookup_elem(fd, &slot, per_cpu) != 0)
        return 0;

    uint64_t total = 0;
    for (int i = 0; i < num_cpus; i++)
        total += per_cpu[i];
    return total;
}
#endif /* !PGWT_SERVER */
