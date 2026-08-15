/* sampler.c — Sampled capture provider (Track A, D1; hardened in T4)
 *
 * See sampler.h for the design. The core read/build logic
 * (pgwt_sampler_read_targets / pgwt_sampler_build_batch) and the pure
 * helpers (health state machine, effective period, qid join index) are free
 * of BPF and daemon dependencies so the unit test can exercise them against
 * a controlled target process. The provider hooks (start/stop/poll/metrics)
 * bridge to the live daemon: they build the per-tick target list from the
 * backend registry + BPF state_map and push the batch into the trace writer.
 *
 * The provider vtable itself lives at the bottom and is compiled into both
 * the daemon (with BPF) and never into pgwt-server. The BPF-touching parts
 * are guarded by !PGWT_SERVER so the unit test can link the BPF-free core.
 */
#define _GNU_SOURCE   /* process_vm_readv */
#include "sampler.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/uio.h>

/* ── BPF-free core (testable without a daemon) ────────────────────────── */

/* Read one target's value against its OWN pid via /proc/<pid>/mem.
 * Returns 1 on success (val written), 0 on failure (errno preserved). */
static int read_target_own_pid(const struct pgwt_sample_target *t,
                               uint32_t *val)
{
    if (t->pid <= 0 || t->wait_event_addr == 0) {
        errno = EINVAL;
        return 0;
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", t->pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    int ok = (pread(fd, val, sizeof(*val),
                    (off_t)t->wait_event_addr) == (ssize_t)sizeof(*val));
    close(fd);
    return ok;
}

/* Ground-truth command-gate + query_id read (T2, EL9 fix).
 *
 * on_report_activity / on_report_query_id maintain cmd_open and last_query_id
 * in the BPF state_map by TRANSITION EDGE only — they fire once, at command
 * start. A long command already in flight when the backend's state_map entry
 * is created misses that single edge, so the entry's cmd_open/query_id stay 0
 * for the ENTIRE command. This happens whenever the sampler starts tracking a
 * backend mid-query: the "run a compute query, THEN attach the tracer" case
 * (straddle at attach), or the sampled-tier new-backend seed race. Every
 * on-CPU (we==0) sample of that command is then dropped by the build_batch
 * gate — CPU* collapses to 0 — and the samples carry no query_id. The defect
 * is latent in sampled/tiered mode on ALL platforms and PG versions; it
 * surfaced first on EL9 because the manual repro ran a long straddling compute
 * query (short/repeated queries re-fire the edge after the entry exists, so
 * the bug hides).
 *
 * Reading the backend's OWN authoritative state is race-free:
 *   cmd_open <- (debug_query_string != NULL): the executing-a-command window,
 *               the same global the BPF query-text path reads. Resolved by
 *               symbol for every PG version — no PgBackendStatus offset guess.
 *               NULL between commands (verified: idle backend == NULL), so no
 *               between-command CPU is counted (docs/AAS_SEMANTICS_DECISION.md
 *               — the ~3x over-count the gate exists to prevent stays gone).
 *   query_id <- PgBackendStatus.st_query_id (MyBEEntry -> +off): the field the
 *               full-mode preseed already reads. Refreshed only while inside a
 *               command (st_query_id can hold a stale id once idle) and only
 *               when its offset is known (PG14+); PG13 keeps its B1 seed.
 *
 * Reads go against the target's OWN pid: both are process-LOCAL globals, so a
 * foreign-pid read would return the reader's copy (SMP-2). *cmd_open /
 * *query_id are left untouched on any read failure, so the caller keeps its
 * edge-maintained fallback — this path never fabricates. */
void pgwt_read_cmd_gate(pid_t pid, uint64_t dqs_addr, uint64_t be_entry_addr,
                        int st_query_id_off, int *cmd_open, uint64_t *query_id)
{
    if (dqs_addr == 0)
        return;   /* symbol unresolved — keep the edge-maintained fallback */

    uint64_t dqs = 0;
    struct iovec ld = { &dqs, sizeof(dqs) };
    struct iovec rd = { (void *)(uintptr_t)dqs_addr, sizeof(dqs) };
    if (process_vm_readv(pid, &ld, 1, &rd, 1, 0) != (ssize_t)sizeof(dqs))
        return;   /* read failed — do not disturb the fallback */

    int open_now = (dqs != 0) ? 1 : 0;
    if (cmd_open)
        *cmd_open = open_now;

    if (open_now && query_id && be_entry_addr && st_query_id_off > 0) {
        uint64_t be_ptr = 0;
        struct iovec lb = { &be_ptr, sizeof(be_ptr) };
        struct iovec rb = { (void *)(uintptr_t)be_entry_addr, sizeof(be_ptr) };
        if (process_vm_readv(pid, &lb, 1, &rb, 1, 0) == (ssize_t)sizeof(be_ptr)
            && be_ptr) {
            uint64_t qid = 0;
            struct iovec lq = { &qid, sizeof(qid) };
            struct iovec rq = { (void *)(uintptr_t)(be_ptr + st_query_id_off),
                                sizeof(qid) };
            if (process_vm_readv(pid, &lq, 1, &rq, 1, 0) == (ssize_t)sizeof(qid))
                *query_id = qid;
        }
    }
}

int pgwt_sampler_read_targets(const struct pgwt_sample_target *targets, int n,
                              uint32_t *out_vals, uint8_t *out_valid,
                              uint64_t *read_faults)
{
    if (n <= 0 || !targets || !out_vals || !out_valid)
        return 0;

    memset(out_vals, 0, (size_t)n * sizeof(*out_vals));
    memset(out_valid, 0, (size_t)n * sizeof(*out_valid));

    struct iovec *liov = calloc(n, sizeof(*liov));
    struct iovec *riov = calloc(n, sizeof(*riov));
    int *idx = calloc(n, sizeof(*idx));   /* batch position -> target index */
    if (!liov || !riov || !idx) {
        free(liov);
        free(riov);
        free(idx);
        return 0;
    }

    int got = 0;
    int nb = 0;   /* number of batchable (shared-memory) targets */
    for (int i = 0; i < n; i++) {
        /* SMP-2: only targets whose address is verified to live in a
         * MAP_SHARED mapping may be read through another pid. A private
         * address mapped at the same VA in every child reads SUCCESSFULLY
         * through a foreign pid and returns the READER's value —
         * misattributed, silently. Everything else goes per-pid below. */
        if (targets[i].is_shared != 1)
            continue;
        liov[nb].iov_base = &out_vals[i];
        liov[nb].iov_len  = sizeof(uint32_t);
        riov[nb].iov_base = (void *)(uintptr_t)targets[i].wait_event_addr;
        riov[nb].iov_len  = sizeof(uint32_t);
        idx[nb] = i;
        nb++;
    }

    /* Shared-memory batch: the addresses are backed by the SAME pages in
     * every backend, so one process_vm_readv through a single reader pid
     * resolves them all in one syscall. process_vm_readv reads iovecs in
     * order and stops at the first faulting entry; we sweep — read as far
     * as possible, per-pid pread the single faulting entry, then RESUME the
     * combined read for the remainder. One outlier costs one pread, not N. */
    int base = 0;   /* first not-yet-read batch position */
    while (base < nb) {
        pid_t reader_pid = targets[idx[base]].pid;
        int rem = nb - base;
        ssize_t r = process_vm_readv(reader_pid, liov + base, rem,
                                     riov + base, rem, 0);
        int done = (r > 0) ? (int)(r / sizeof(uint32_t)) : 0;
        for (int j = 0; j < done; j++)
            out_valid[idx[base + j]] = 1;
        got  += done;
        base += done;
        if (base >= nb)
            break;

        /* batch entry `base` faulted under reader_pid (or the reader itself
         * is gone). Read it against its own pid. */
        uint32_t val = 0;
        if (read_target_own_pid(&targets[idx[base]], &val)) {
            out_vals[idx[base]] = val;
            out_valid[idx[base]] = 1;
            got++;
        }
        if (read_faults)
            (*read_faults)++;
        base++;   /* move past the entry we just handled (or skipped) */
    }

    /* Per-pid reads for everything not proven shared (SMP-2). */
    for (int i = 0; i < n; i++) {
        if (targets[i].is_shared == 1)
            continue;
        uint32_t val = 0;
        if (read_target_own_pid(&targets[i], &val)) {
            out_vals[i] = val;
            out_valid[i] = 1;
            got++;
        }
    }

    free(liov);
    free(riov);
    free(idx);
    return got;
}

/* T2 category flag for a backend type (docs/AAS_SEMANTICS_DECISION.md).
 * Foreground (client, parallel worker) carries no flag; autovacuum workers
 * are maintenance; io_workers are their own excluded-from-AAS class; every
 * other aux process is background. UNKNOWN is treated as foreground/client
 * (conservative: its CPU stays command-gated, its waits count as before). */
uint32_t pgwt_backend_type_flag(enum pgwt_backend_type bt)
{
    switch (bt) {
    case PGWT_BT_CLIENT:
    case PGWT_BT_PARALLEL_WORKER:
    case PGWT_BT_UNKNOWN:
        return 0;                              /* foreground */
    case PGWT_BT_AUTOVAC_WORKER:
        return PGWT_EVENT_FLAG_MAINT;          /* maintenance */
    case PGWT_BT_IO_WORKER:
        return PGWT_EVENT_FLAG_IO_WORKER;      /* excluded from AAS/DB Time */
    default:
        return PGWT_EVENT_FLAG_BACKGROUND;     /* checkpointer, bgwriter, … */
    }
}

/* T2 on-CPU (we==0) recording policy. Client backends (and UNKNOWN, which
 * cannot prove otherwise) record CPU only inside a command — the
 * pg_stat_activity state='active' window — because ungated we==0 counting
 * measured ~3x true activity on chatty OLTP (between-command bookkeeping
 * scales with transaction rate, not with work). Background types' parked
 * states are instrumented Activity-class waits, so we==0 always means
 * working; parallel workers exist only inside a query. */
int pgwt_cpu_sample_recordable(enum pgwt_backend_type bt, int cmd_open)
{
    switch (bt) {
    case PGWT_BT_CLIENT:
    case PGWT_BT_UNKNOWN:
        return cmd_open ? 1 : 0;
    case PGWT_BT_LOGGER:
        return 0;   /* not a PG-work process; never sampled as active */
    default:
        return 1;   /* background, maintenance, parallel worker, io_worker */
    }
}

enum pgwt_sampled_attr_source pgwt_sampler_select_attr(
    int tick_source_enabled, int tick_read_ok,
    const struct pgwt_sampled_attr_value *tick,
    const struct pgwt_sampled_attr_value *uprobe,
    struct pgwt_sample_target *target)
{
    if (!target)
        return PGWT_SAMPLED_ATTR_DROP;

    target->query_id = 0;
    target->cmd_open = 0;
    if (!tick_source_enabled) {
        if (uprobe) {
            target->query_id = uprobe->query_id;
            target->cmd_open = uprobe->cmd_open;
        }
        return PGWT_SAMPLED_ATTR_UPROBE;
    }
    if (!tick_read_ok || !tick) {
        /* Only command-gated CLIENT/UNKNOWN targets must fail closed: stale
         * cmd_open could admit false CPU.  Query-less types remain recordable
         * without attribution, preserving their wait/CPU coverage. */
        if (target->backend_type == PGWT_BT_CLIENT ||
            target->backend_type == PGWT_BT_UNKNOWN)
            return PGWT_SAMPLED_ATTR_DROP;
        return PGWT_SAMPLED_ATTR_UNATTRIBUTED;
    }

    target->query_id = tick->query_id;
    target->cmd_open = tick->cmd_open;
    return PGWT_SAMPLED_ATTR_TICK;
}

unsigned pgwt_sampled_attr_compare(
    const struct pgwt_sampled_attr_value *tick,
    const struct pgwt_sampled_attr_value *uprobe)
{
    if (!tick || !uprobe)
        return PGWT_SAMPLED_ATTR_MISMATCH_CMD_OPEN |
               PGWT_SAMPLED_ATTR_MISMATCH_QUERY_ID;
    unsigned mismatch = 0;
    if (!!tick->cmd_open != !!uprobe->cmd_open)
        mismatch |= PGWT_SAMPLED_ATTR_MISMATCH_CMD_OPEN;
    /* Compare EFFECTIVE attribution.  PgBackendStatus and the state map both
     * retain the last query id after a command closes; sampled attribution
     * deliberately exposes neither one while idle. */
    uint64_t tick_query_id = tick->cmd_open ? tick->query_id : 0;
    uint64_t uprobe_query_id = uprobe->cmd_open ? uprobe->query_id : 0;
    if (tick_query_id != uprobe_query_id)
        mismatch |= PGWT_SAMPLED_ATTR_MISMATCH_QUERY_ID;
    return mismatch;
}

int pgwt_sampled_attr_active_query_mismatch(
    const struct pgwt_sampled_attr_value *tick,
    const struct pgwt_sampled_attr_value *uprobe)
{
    return tick && uprobe && tick->cmd_open &&
           tick->query_id != uprobe->query_id;
}

int pgwt_sampler_build_batch(const struct pgwt_sample_target *targets,
                             const uint32_t *vals, const uint8_t *valid,
                             int n, uint64_t tick_ts,
                             struct pgwt_trace_event *out,
                             uint64_t *invalid_reads,
                             uint64_t *noncmd_cpu_skipped)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        /* A failed read leaves vals[i] == 0, but that is not an on-CPU
         * observation. Exclude it before the we==0 policy or any other
         * classification so read failures cannot fabricate active demand. */
        if (!valid || !valid[i])
            continue;

        uint32_t we = vals[i];

        /* event 0 == on CPU. T2 (AAS-1): a session on CPU IS an active
         * session — record it as a first-class CPU sample, gated by the
         * per-type policy above. (The old sampler skipped all we==0 and no
         * coverage-derived CPU existed anywhere: a pure CPU storm produced
         * AAS ~ 0 and the anomaly engine was blind to it.) */
        if (we == 0) {
            if (!pgwt_cpu_sample_recordable(targets[i].backend_type,
                                            targets[i].cmd_open)) {
                if (noncmd_cpu_skipped)
                    (*noncmd_cpu_skipped)++;
                continue;
            }
        } else if (pgwt_classify_wei(we) == PGWT_WEI_GARBAGE) {
            /* CAP-2/5 backstop (all PG versions, every tick): a value with
             * an unknown class byte is garbage from a wrong offset/address —
             * it must NEVER be recorded as data. Count it so the daemon
             * screams. */
            if (invalid_reads)
                (*invalid_reads)++;
            continue;
        }

        struct pgwt_trace_event *e = &out[count++];
        e->timestamp_ns = tick_ts;
        e->pid          = (uint32_t)targets[i].pid;
        e->old_event    = 0;       /* samples carry no previous state */
        e->new_event    = we;      /* the sampled wait event (0 = CPU) */
        /* In-memory category tag (never persisted — the SAMPLES layout has
         * no flags column; offline consumers re-derive the category from
         * backends.jsonl). The SAMPLE flag itself is set by the reader. */
        e->flags        = pgwt_backend_type_flag(targets[i].backend_type);
        e->duration_ns  = 0;       /* samples carry no duration */
        e->query_id     = targets[i].query_id;
    }
    return count;
}

void pgwt_sampler_note_coverage(struct pgwt_sampler *s, int targets, int valid)
{
    if (!s)
        return;
    if (targets < 0)
        targets = 0;
    if (valid < 0)
        valid = 0;
    if (valid > targets)
        valid = targets;

    s->read_targets_last = (uint32_t)targets;
    s->read_valid_last = (uint32_t)valid;
    s->read_invalid_last = (uint32_t)(targets - valid);
    s->read_failures_total += s->read_invalid_last;
}

uint64_t pgwt_sampler_effective_period(uint64_t nominal_ns,
                                       uint64_t last_tick_ns,
                                       uint64_t now_ns)
{
    if (last_tick_ns == 0 || now_ns <= last_tick_ns)
        return nominal_ns;
    uint64_t elapsed = now_ns - last_tick_ns;
    if (elapsed < nominal_ns)
        return nominal_ns;   /* the timer never fires early; jitter only */
    if (elapsed > 60ULL * 1000000000ULL)
        return 60ULL * 1000000000ULL;   /* sanity clamp on absurd stalls */
    return elapsed;
}

enum pgwt_sampler_health_action
pgwt_sampler_health_note(struct pgwt_sampler_health *h, int n_targets,
                         int n_read, int err_no, uint64_t now_ns)
{
    /* Re-log period while persistently failing: once a minute. */
    const uint64_t RELOG_NS = 60ULL * 1000000000ULL;

    if (n_targets <= 0)
        return PGWT_SAMPLER_LOG_NONE;   /* nothing to read — neutral tick */

    if (n_read > 0) {
        int was_unhealthy = !h->healthy;
        h->healthy = 1;
        h->consec_failed_ticks = 0;
        return was_unhealthy ? PGWT_SAMPLER_LOG_RECOVERED
                             : PGWT_SAMPLER_LOG_NONE;
    }

    /* Total failure: N targets, zero readable. Indistinguishable from an
     * idle database in the data — must be loud out-of-band (SMP-1). */
    h->consec_failed_ticks++;
    h->failed_ticks_total++;
    h->last_errno = err_no;
    h->healthy = 0;
    if (h->consec_failed_ticks == 1 || now_ns - h->last_log_ns >= RELOG_NS) {
        h->last_log_ns = now_ns;
        return PGWT_SAMPLER_LOG_DEGRADED;
    }
    return PGWT_SAMPLER_LOG_NONE;
}

/* ── SMP-4: pid -> query_id join index ────────────────────────────────── */

static int qid_entry_cmp(const void *a, const void *b)
{
    uint32_t pa = ((const struct pgwt_qid_entry *)a)->pid;
    uint32_t pb = ((const struct pgwt_qid_entry *)b)->pid;
    return (pa > pb) - (pa < pb);
}

void pgwt_qid_index_sort(struct pgwt_qid_entry *entries, int n)
{
    qsort(entries, (size_t)n, sizeof(*entries), qid_entry_cmp);
}

const struct pgwt_qid_entry *
pgwt_qid_index_get(const struct pgwt_qid_entry *entries, int n, uint32_t pid)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (entries[mid].pid == pid)
            return &entries[mid];
        if (entries[mid].pid < pid)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

uint64_t pgwt_qid_index_lookup(const struct pgwt_qid_entry *entries, int n,
                               uint32_t pid)
{
    const struct pgwt_qid_entry *e = pgwt_qid_index_get(entries, n, pid);
    return e ? e->query_id : 0;
}

bool pgwt_exact_attr_shadow_comparable(const struct pgwt_exact_attr *edge,
                                       uint64_t generation)
{
    return edge && edge->query_generation == generation &&
           edge->cmd_generation == generation;
}

/* ── Daemon-side provider hooks (need the BPF skeleton) ───────────────── */

#ifndef PGWT_SERVER

#include "daemon.h"
#include "backend.h"
#include "event_writer.h"
#include "discovery.h"
#include "map_reader.h"
#include "wait_event.h"

#include <time.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Read pid -> {query_id, cmd_open} from the BPF exact_attr_map, maintained by
 * the on_report_query_id / on_report_activity uprobes. Per-pid fallback for
 * kernels without BPF_MAP_LOOKUP_BATCH (SMP-4). */
static bool lookup_pid_state(struct pgwt_daemon *d, pid_t pid,
                             uint64_t generation, bool generation_valid,
                             uint64_t *qid, int *cmd_open)
{
    *qid = 0;
    *cmd_open = 0;
    if (!d->skel)
        return false;
    int fd = bpf_map__fd(d->skel->maps.exact_attr_map);
    if (fd < 0)
        return false;
    uint32_t key = (uint32_t)pid;
    struct pgwt_exact_attr st;
    if (bpf_map_lookup_elem(fd, &key, &st) == 0) {
        *qid = st.query_id;
        *cmd_open = st.cmd_open;
        return generation_valid &&
               pgwt_exact_attr_shadow_comparable(&st, generation);
    }
    return false;
}

/* SMP-4: dump the whole exact_attr_map in a few BPF_MAP_LOOKUP_BATCH
 * syscalls and build a sorted pid->query_id index, instead of one
 * bpf_map_lookup_elem syscall per backend per tick (10k syscalls/s at
 * 1000 backends × 10 Hz). Returns the number of index entries, or -1 when
 * batch lookup is unsupported (caller falls back to per-pid lookups). */
static int dump_qid_index(struct pgwt_daemon *d, struct pgwt_sampler *s,
                          struct pgwt_qid_entry *out, uint64_t generation,
                          bool generation_valid)
{
    if (!d->skel || s->qid_batch_supported == 0)
        return -1;
    int fd = bpf_map__fd(d->skel->maps.exact_attr_map);
    if (fd < 0)
        return -1;

    uint32_t in_batch = 0, out_batch = 0;
    void *in = NULL;
    int total = 0;
    while (total < s->qid_cap) {
        uint32_t count = (uint32_t)(s->qid_cap - total);
        int err = bpf_map_lookup_batch(fd, in, &out_batch,
                                       s->qid_keys + total,
                                       s->qid_vals + total, &count, NULL);
        if (err == 0 || err == -ENOENT) {
            total += (int)count;
            if (err == -ENOENT)
                break;   /* map exhausted */
            in_batch = out_batch;
            in = &in_batch;
            continue;
        }
        if (total == 0) {
            /* First call failed outright: kernel lacks batch support
             * (EINVAL/ENOTSUPP...). Remember and fall back per-pid. */
            s->qid_batch_supported = 0;
            if (d->verbose)
                fprintf(stderr, "INFO: BPF_MAP_LOOKUP_BATCH unavailable "
                        "(%s) — using per-pid query_id lookups\n",
                        strerror(-err));
            return -1;
        }
        break;   /* partial dump: use what we have */
    }
    s->qid_batch_supported = 1;

    for (int i = 0; i < total; i++) {
        out[i].pid = s->qid_keys[i];
        out[i].query_id = s->qid_vals[i].query_id;
        out[i].cmd_open = s->qid_vals[i].cmd_open;
        out[i].shadow_valid = generation_valid &&
            pgwt_exact_attr_shadow_comparable(&s->qid_vals[i], generation);
    }
    pgwt_qid_index_sort(out, total);
    return total;
}

/* Seed an empty state_map entry for a sampled-mode backend. The
 * on_report_query_id uprobe only UPDATES existing entries (it never creates
 * them), so without a seed the sampler would never see a query_id — there is
 * no on_watchpoint in sampled mode to create the entry. Idempotent
 * (BPF_NOEXIST). CAP-1: a full map here means this backend's samples lose
 * query attribution — counted + logged loudly, never silent. */
static void seed_state_entry(struct pgwt_daemon *d, pid_t pid, uint64_t addr)
{
    if (!d->skel)
        return;
    int fd = bpf_map__fd(d->skel->maps.state_map);
    if (fd < 0)
        return;
    uint32_t key = (uint32_t)pid;
    struct pgwt_pid_state st = {
        .last_event = 0,
        .last_ts = mono_ns(),
        .last_query_id = 0,
        .wait_event_addr = addr,
    };
    int rc = bpf_map_update_elem(fd, &key, &st, BPF_NOEXIST);
    if (rc != 0 && rc != -EEXIST && errno != EEXIST) {
        d->counters.state_map_full_total++;
        if (!d->state_map_full_logged) {
            d->state_map_full_logged = true;
            fprintf(stderr,
                    "ERROR: BPF state_map is FULL — cannot seed PID %d (and "
                    "any further backend).\n"
                    "  Affected backends' samples lose query attribution. "
                    "state_map_full_total on the control socket counts every "
                    "affected backend.\n",
                    pid);
        }
    }
}

int pgwt_sampler_start(struct pgwt_daemon *d)
{
    struct pgwt_sampler *s = calloc(1, sizeof(*s));
    if (!s) {
        fprintf(stderr, "FATAL: cannot allocate sampler state\n");
        return -1;
    }

    int hz = d->sample_rate_hz > 0 ? d->sample_rate_hz : 10;
    s->sample_period_ns = 1000000000ULL / (uint64_t)hz;
    s->health.healthy = 1;
    s->qid_batch_supported = -1;

    /* qid dump buffers sized to the loaded exact_attr_map capacity.
     * shrunk via PGWT_STATE_MAP_ENTRIES in test builds). */
    s->qid_cap = MAX_BACKENDS;
    if (d->skel) {
        uint32_t me = bpf_map__max_entries(d->skel->maps.exact_attr_map);
        if (me > 0 && (int)me < s->qid_cap)
            s->qid_cap = (int)me;
    }

    s->read_vals = calloc(MAX_BACKENDS, sizeof(*s->read_vals));
    s->read_valid = calloc(MAX_BACKENDS, sizeof(*s->read_valid));
    s->samples   = calloc(MAX_BACKENDS, sizeof(*s->samples));
    s->qid_keys  = calloc(s->qid_cap, sizeof(*s->qid_keys));
    s->qid_vals  = calloc(s->qid_cap, sizeof(*s->qid_vals));
    if (!s->read_vals || !s->read_valid || !s->samples || !s->qid_keys
        || !s->qid_vals) {
        free(s->read_vals);
        free(s->read_valid);
        free(s->samples);
        free(s->qid_keys);
        free(s->qid_vals);
        free(s);
        fprintf(stderr, "FATAL: cannot allocate sampler buffers\n");
        return -1;
    }

    d->sampler = s;
    if (d->verbose)
        fprintf(stderr, "INFO: sampler started: %d Hz (period %llu ns)\n",
                hz, (unsigned long long)s->sample_period_ns);
    return 0;
}

int pgwt_sampler_stop(struct pgwt_daemon *d)
{
    struct pgwt_sampler *s = d->sampler;
    if (!s)
        return 0;
    free(s->read_vals);
    free(s->read_valid);
    free(s->samples);
    free(s->qid_keys);
    free(s->qid_vals);
    free(s);
    d->sampler = NULL;
    return 0;
}

/* Fold one sample batch into the live in-process accumulator so the running
 * --view output reflects sampled-mode data in real time.
 *
 * In full/lightweight mode the event_ringbuf consumer (pgwt_handle_trace_event)
 * is what populates d->event_accum, which the timer copies into d->accum for
 * display. Sampled mode has no ringbuf consumer — the sampler is the data
 * source — so without this step the live view stays empty ("(no data yet)")
 * even though samples are correctly captured and written to the trace file.
 *
 * Each sample is an ASH point observation worth one sample period of wall time;
 * we weight it by period_ns exactly as the offline reader/server do
 * (event_reader.c sets FLAG_SAMPLE; server.c normalizes old_event/duration_ns).
 * The build_batch records keep their on-disk shape (new_event set, old/duration
 * zero) so the trace-file format is unchanged; we read new_event here and apply
 * the per-sample weight locally. */
static void pgwt_sampler_accumulate(struct pgwt_daemon *d,
                                    const struct pgwt_trace_event *samples,
                                    int count, uint64_t period_ns)
{
    struct pgwt_accumulator *acc = d->event_accum;
    if (!acc)
        return;

    /* ESC-3: while escalated, the exact ringbuf ALSO feeds d->event_accum for
     * every pid with a live watchpoint. Folding the sampler's estimate in on
     * top double-counts the live --view (measured ~1.9x). Gate the sampler's
     * contribution for exactly those covered pids — the live-path analogue of
     * the offline marker-based exact-wins merge. Freshly-forked backends not
     * yet watchpointed (wp_fd < 0) still have the sampler as their only source,
     * so they keep accumulating. The on-disk SAMPLES are untouched; the offline
     * merge de-dupes them via the escalation markers (FID-1). */
    bool escalated = d->escalation.active;

    for (int i = 0; i < count; i++) {
        uint32_t we  = samples[i].new_event;   /* sampled wait event (0 = CPU) */
        uint64_t dur = period_ns;              /* ASH weight per sample */
        int io_worker = (samples[i].flags & PGWT_EVENT_FLAG_IO_WORKER) != 0;

        if (PGWT_IS_MARKER(we))
            continue;

        if (escalated) {
            struct pgwt_backend *be =
                pgwt_find_backend(&d->backends, (pid_t)samples[i].pid);
            if (be && be->wp_fd >= 0)
                continue;   /* exact ringbuf owns this pid's live accumulation */
        }

        /* Per-PID accumulation */
        struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(acc, samples[i].pid);
        if (pa) {
            struct pgwt_event_stats *es = pgwt_get_or_create_event(pa, we);
            if (es) {
                es->count++;
                es->total_ns += dur;
                if (dur < es->min_ns) es->min_ns = dur;
                if (dur > es->max_ns) es->max_ns = dur;
                uint32_t bucket = pgwt_duration_to_bucket(dur);
                if (bucket < HISTOGRAM_BUCKETS)
                    es->histogram[bucket]++;
            }
            /* io_worker time never enters DB Time / AAS (T2): the event
             * stats above keep it VISIBLE, the load accounting skips it. */
            if (!io_worker) {
                if (we == 0) {
                    pa->cpu_time_ns += dur;   /* first-class CPU sample */
                    pa->db_time_ns  += dur;
                } else if (!pgwt_is_idle_event(we)) {
                    pa->wait_time_ns += dur;
                    pa->db_time_ns   += dur;
                }
            }
        }

        /* System-wide accumulation */
        struct pgwt_event_stats *se = pgwt_get_or_create_system_event(acc, we);
        if (se) {
            se->count++;
            se->total_ns += dur;
            if (dur < se->min_ns) se->min_ns = dur;
            if (dur > se->max_ns) se->max_ns = dur;
            uint32_t bucket = pgwt_duration_to_bucket(dur);
            if (bucket < HISTOGRAM_BUCKETS)
                se->histogram[bucket]++;
        }

        /* Time model by class (io_worker time excluded from DB Time) */
        if (!io_worker)
            pgwt_update_time_model(&acc->tm, we, dur);

        /* Query attribution (io_workers are structurally query-less) */
        if (samples[i].query_id != 0 && !io_worker) {
            struct pgwt_query_event_stats *qe =
                pgwt_get_or_create_query_event(acc, samples[i].query_id, we);
            if (qe) {
                qe->count++;
                qe->total_ns += dur;
                if (dur < qe->min_ns) qe->min_ns = dur;
                if (dur > qe->max_ns) qe->max_ns = dur;
            }
        }
    }
}

/* One sampling tick. Build the target list from the live registry, read all
 * wait_event_info (shared-memory batch + per-pid fallbacks), encode wait
 * readings AND policy-gated on-CPU readings (T2: we==0 is a first-class CPU
 * sample), and push a SAMPLES block. Called from the daemon timer. */
int pgwt_sampler_poll(struct pgwt_daemon *d)
{
    struct pgwt_sampler *s = d->sampler;
    if (!s)
        return 0;

    uint64_t tick_ts = mono_ns();
    /* SMP-3: weight this tick by the MEASURED inter-tick elapsed time, not
     * the nominal period — when the daemon stalls (attach storms, load) and
     * timer expirations coalesce, the nominal weight would silently deflate
     * AAS/DB-Time exactly when it matters. The SAMPLES block header carries
     * the per-block period, so the on-disk format is unchanged. */
    uint64_t period_ns = pgwt_sampler_effective_period(s->sample_period_ns,
                                                       s->last_tick_ns,
                                                       tick_ts);
    s->last_tick_ns = tick_ts;

    const bool tick_source_enabled =
        pgwt_pgbs_sampled_attr_enabled(&d->backend_status_layout);
    uint32_t attr_edge_mask = d->exact_probes.core.attached_mask &
                              PGWT_EXACT_PROBE_ATTR_MASK;
    const bool attr_edges_attached = attr_edge_mask != 0;
    const bool attr_shadow_pair_attached =
        attr_edge_mask == PGWT_EXACT_PROBE_ATTR_MASK;

    struct pgwt_exact_config exact_cfg = {0};
    uint32_t exact_cfg_key = 0;
    bool exact_cfg_valid = attr_edges_attached &&
        bpf_map_lookup_elem(bpf_map__fd(d->skel->maps.exact_config_map),
                            &exact_cfg_key, &exact_cfg) == 0;

    /* SMP-4: dump the legacy edge source only when those links are actually
     * attached (PG13/degraded baseline, or a live exact window). Validated
     * PG14-18 sampled operation performs no pointless map dump and has no
     * shadow traps: Stage 2's coherent tick source is authoritative. */
    struct pgwt_qid_entry qidx_buf[MAX_BACKENDS];
    int qidx_n = attr_edges_attached
        ? dump_qid_index(d, s, qidx_buf, exact_cfg.generation,
                         exact_cfg_valid)
        : 0;

    /* Reuse a stack target array sized to the live count; MAX_BACKENDS cap. */
    static struct pgwt_sample_target targets[MAX_BACKENDS];
    static uint8_t attr_valid[MAX_BACKENDS];
    int n = 0;
    int attr_errno = 0;
    for (int i = 0; i < d->backends.count && n < MAX_BACKENDS; i++) {
        struct pgwt_backend *be = &d->backends.entries[i];
        if (!be->is_alive || be->pid <= 0)
            continue;
        /* In sampled mode no bootstrap watchpoint resolves wp_addr, so a
         * freshly-forked backend arrives with wp_addr == 0. Lazily resolve
         * it via the version-appropriate path (PG17+ my_wait_event_info
         * global; PG<17 MyProc + offset — same global VA in every backend,
         * the deref gives that backend's own PGPROC slot). Skip this tick if
         * the backend hasn't set the pointer yet.
         *
         * While the resolved address is NOT in shared memory, keep
         * re-resolving every tick: on PG17+ the pointer is non-zero from the
         * first instruction (static initializer → the process-local dummy
         * local_my_wait_event_info) — a tick landing in the fork→InitProcess
         * window would otherwise cache the dummy FOREVER and that backend's
         * waits would never be sampled (the fork→attach race's sampled-tier
         * sibling). Genuinely-local addresses (aux processes without a
         * PGPROC, e.g. syslogger) stay non-shared and are simply re-resolved
         * to the same value — a cheap /proc read per tick for a handful of
         * processes. Once the address lands in shm it is cached for good. */
        if (be->wp_addr == 0 || be->wp_addr_shared == 0) {
            uint64_t addr = pgwt_resolve_backend_wait_addr(d, be->pid);
            if (addr == 0 && be->wp_addr == 0)
                continue;
            if (addr != 0 && addr != be->wp_addr) {
                be->wp_addr = addr;
                be->wp_addr_shared = -1;
                /* Seed state for the PG13/degraded attribution fallback and
                 * future exact watchpoint enrollment (idempotent). */
                seed_state_entry(d, be->pid, be->wp_addr);
            }
        }
        /* T2: classify the process ONCE it is far enough through init to
         * have set its title (wp_addr resolved implies init done). The
         * sampled-tier fork path never parsed cmdline at all, so every
         * forked backend read as type 0 == client — io_workers, autovacuum
         * and parallel workers were unclassifiable. */
        if (!be->meta_parsed) {
            if (pgwt_parse_cmdline(be->pid, &be->meta) == 0) {
                be->meta_parsed = true;
                if (d->backend_meta)
                    pgwt_bm_write(d->backend_meta, be->pid, &be->meta);
            }
        }
        /* SMP-2: classify the address — only verified-shared addresses may
         * be batched through a foreign pid. */
        if (be->wp_addr_shared < 0)
            be->wp_addr_shared =
                (pgwt_addr_is_shared(be->pid, be->wp_addr) == 1) ? 1 : 0;
        targets[n].pid             = be->pid;
        targets[n].wait_event_addr = be->wp_addr;
        targets[n].is_shared       = be->wp_addr_shared;
        targets[n].backend_type    = be->meta_parsed ? be->meta.backend_type
                                                     : PGWT_BT_UNKNOWN;
        /* The syslogger is not a PostgreSQL backend: it has no MyBEEntry or
         * PgBackendStatus slot, and its on-CPU samples are already excluded
         * by policy.  Keep its irrelevant zero attribution on the legacy
         * source instead of manufacturing a permanent coverage hole. */
        const bool target_tick_enabled = tick_source_enabled &&
            targets[n].backend_type != PGWT_BT_LOGGER;
        struct pgwt_sampled_attr_value uprobe_attr = {0};
        bool uprobe_shadow_valid = false;
        if (qidx_n >= 0) {
            const struct pgwt_qid_entry *qe =
                pgwt_qid_index_get(qidx_buf, qidx_n, (uint32_t)be->pid);
            if (qe) {
                uprobe_attr.query_id = qe->query_id;
                uprobe_attr.cmd_open = qe->cmd_open;
                uprobe_shadow_valid = qe->shadow_valid;
            }
        } else {
            uprobe_shadow_valid = lookup_pid_state(
                d, be->pid, exact_cfg.generation, exact_cfg_valid,
                &uprobe_attr.query_id, &uprobe_attr.cmd_open);
        }

        struct pgwt_pgbs_sampled_attr tick_raw;
        struct pgwt_sampled_attr_value tick_attr = {0};
        int tick_ok = 0;
        if (target_tick_enabled) {
            errno = 0;
            tick_ok = pgwt_pgbs_read_sampled_attr(
                be->pid, d->my_be_entry_addr, &d->backend_status_layout,
                &tick_raw) == 0;
            if (tick_ok) {
                tick_attr.query_id = tick_raw.query_id;
                tick_attr.cmd_open = tick_raw.cmd_open;
                if (attr_shadow_pair_attached && uprobe_shadow_valid) {
                    unsigned mismatch = pgwt_sampled_attr_compare(
                        &tick_attr, &uprobe_attr);
                    d->counters.sampled_attr_shadow_total++;
                    if (tick_attr.cmd_open)
                        d->counters.sampled_attr_shadow_active_total++;
                    if (pgwt_sampled_attr_active_query_mismatch(
                            &tick_attr, &uprobe_attr))
                        d->counters
                            .sampled_attr_shadow_active_mismatch_total++;
                    if (mismatch) {
                        d->counters.sampled_attr_shadow_mismatch_total++;
                        if (mismatch & PGWT_SAMPLED_ATTR_MISMATCH_CMD_OPEN)
                            d->counters
                                .sampled_attr_shadow_cmd_open_mismatch_total++;
                        if (mismatch & PGWT_SAMPLED_ATTR_MISMATCH_QUERY_ID)
                            d->counters
                                .sampled_attr_shadow_query_id_mismatch_total++;
                        if (getenv("PGWT_DEBUG_SAMPLED_ATTR_SHADOW"))
                            fprintf(stderr,
                                "SAMPLED-ATTR-SHADOW-MISMATCH: pid=%d "
                                "active=%d fields=%s%s "
                                "uprobe=(last_query_id=0x%llx,cmd_open=%d,"
                                "effective_query_id=0x%llx) "
                                "tick=(st_query_id=0x%llx,cmd_open=%d,"
                                "state=%u,effective_query_id=0x%llx)\n",
                                be->pid,
                                tick_attr.cmd_open,
                                mismatch &
                                    PGWT_SAMPLED_ATTR_MISMATCH_CMD_OPEN
                                    ? "cmd_open" : "",
                                mismatch &
                                    PGWT_SAMPLED_ATTR_MISMATCH_QUERY_ID
                                    ? (mismatch &
                                       PGWT_SAMPLED_ATTR_MISMATCH_CMD_OPEN
                                       ? ",query_id" : "query_id") : "",
                                (unsigned long long)uprobe_attr.query_id,
                                uprobe_attr.cmd_open,
                                (unsigned long long)(uprobe_attr.cmd_open
                                    ? uprobe_attr.query_id : 0),
                                (unsigned long long)tick_attr.query_id,
                                tick_attr.cmd_open, tick_raw.state,
                                (unsigned long long)(tick_attr.cmd_open
                                    ? tick_attr.query_id : 0));
                    }
                }
            } else {
                d->counters.sampled_attr_tick_read_failures_total++;
                if (!attr_errno)
                    attr_errno = errno ? errno : EAGAIN;
                if (getenv("PGWT_DEBUG_SAMPLED_ATTR_SHADOW"))
                    fprintf(stderr,
                            "SAMPLED-ATTR-TICK-INVALID: pid=%d error=%s\n",
                            be->pid, strerror(errno ? errno : EAGAIN));
            }
        }

        enum pgwt_sampled_attr_source source = pgwt_sampler_select_attr(
            target_tick_enabled, tick_ok,
            &tick_attr, &uprobe_attr, &targets[n]);
        attr_valid[n] = source != PGWT_SAMPLED_ATTR_DROP;
        n++;
    }
    if (n == 0) {
        pgwt_sampler_note_coverage(s, 0, 0);
        return 0;
    }

    uint64_t faults_before = s->read_faults_total;
    errno = 0;
    int got = pgwt_sampler_read_targets(targets, n, s->read_vals,
                                        s->read_valid,
                                        &s->read_faults_total);
    int read_errno = errno;
    d->counters.sample_read_faults_total +=
        (s->read_faults_total - faults_before);

    /* A validated-layout tick read is part of the target observation.  If it
     * failed, invalidate the target even when wait_event_info succeeded: the
     * batch must neither reuse stale command state nor admit a CPU sample.
     * The existing anomaly coverage path then sees the same incomplete tick. */
    for (int i = 0; i < n; i++) {
        if (!attr_valid[i] && s->read_valid[i]) {
            s->read_valid[i] = 0;
            got--;
        }
    }
    if (got == 0 && attr_errno)
        read_errno = attr_errno;
    pgwt_sampler_note_coverage(s, n, got);

    /* Debug-only evidence for the anomaly CPU-coverage gate. Keep each failed
     * target attributable: aggregate coverage alone cannot distinguish a
     * missed storm backend from an unrelated background process disappearing
     * during the registry/read race. */
    if (getenv("PGWT_DEBUG_ANOMALY_CPU_TICK")) {
        for (int i = 0; i < n; i++) {
            if (!s->read_valid[i])
                fprintf(stderr,
                        "ANOMALY-CPU-INVALID: pid=%d backend_type=%s\n",
                        targets[i].pid,
                        pgwt_backend_type_name(targets[i].backend_type));
        }
    }

    /* SMP-1: a tick where NOTHING could be read looks exactly like an idle
     * database in the data — it must be loud out-of-band. */
    switch (pgwt_sampler_health_note(&s->health, n, got, read_errno, tick_ts)) {
    case PGWT_SAMPLER_LOG_DEGRADED:
        fprintf(stderr,
                "ERROR: sampler read failure: 0 of %d backends readable "
                "(last error: %s; %llu consecutive failed ticks).\n"
                "  Sampled data is NOT being captured — an idle-looking view "
                "now means 'blind', not 'idle'. status.sampler_healthy=false "
                "until reads recover.\n",
                n, strerror(s->health.last_errno),
                (unsigned long long)s->health.consec_failed_ticks);
        break;
    case PGWT_SAMPLER_LOG_RECOVERED:
        fprintf(stderr, "INFO: sampler reads recovered (%llu failed ticks "
                "total)\n",
                (unsigned long long)s->health.failed_ticks_total);
        break;
    default:
        break;
    }

    /* io_worker utilization (T2): io_workers are EXCLUDED from AAS/DB Time
     * (their busy time is a shadow copy of the requesting backends'
     * AioIoCompletion waits — docs/AAS_SEMANTICS_DECISION.md) but their
     * busy fraction is a genuine capacity signal. busy = any reading that
     * is not an instrumented idle wait (IoWorkerMain) — i.e. on-CPU or a
     * real wait like IO:DataFileRead. */
    for (int i = 0; i < n; i++) {
        if (!s->read_valid[i]
            || targets[i].backend_type != PGWT_BT_IO_WORKER)
            continue;
        d->counters.io_worker_samples_total++;
        uint32_t we = s->read_vals[i];
        if (we == 0 || !pgwt_is_idle_event(we))
            d->counters.io_worker_busy_total++;
    }

    /* T2 command-gate ground truth (EL9 fix). The state_map cmd_open the
     * targets carry is transition-edge maintained: it is correct for a backend
     * whose entry existed before its command began, but stuck at 0 for a
     * command already in flight when the entry was created (attach straddle /
     * new-backend seed race). That would drop the command's every on-CPU
     * sample. Verify against the backend's OWN debug_query_string for exactly
     * the samples at risk — an on-CPU (we==0) reading from a command-gated type
     * (client/unknown) whose edge-gate is closed. This is the buggy set only;
     * the common steady state (edge already open, or not on CPU) does zero
     * extra reads. When ground truth says "in a command", also refresh the
     * query_id the edge-uprobe likewise missed. */
    if (!tick_source_enabled && d->debug_query_string_addr) {
        for (int i = 0; i < n; i++) {
            if (!s->read_valid[i] || s->read_vals[i] != 0
                || targets[i].cmd_open)
                continue;   /* not on-CPU, or the edge already caught it */
            if (targets[i].backend_type != PGWT_BT_CLIENT
                && targets[i].backend_type != PGWT_BT_UNKNOWN)
                continue;   /* only client/unknown CPU is command-gated */
            int cg = 0;
            uint64_t qg = targets[i].query_id;
            pgwt_read_cmd_gate(targets[i].pid, d->debug_query_string_addr,
                               d->my_be_entry_addr, d->st_query_id_offset,
                               &cg, &qg);
            if (cg) {
                targets[i].cmd_open = 1;
                targets[i].query_id = qg;
                d->counters.cmd_gate_recovered_total++;
            }
        }
    }

    uint64_t invalid_before = d->counters.invalid_wait_reads_total;
    int count = pgwt_sampler_build_batch(targets, s->read_vals, s->read_valid,
                                         n, tick_ts, s->samples,
                                         &d->counters.invalid_wait_reads_total,
                                         &d->counters.noncmd_cpu_samples_total);
    if (d->counters.invalid_wait_reads_total != invalid_before
        && !d->invalid_wait_reads_logged) {
        d->invalid_wait_reads_logged = true;
        fprintf(stderr,
                "ERROR: sampler read wait_event_info value(s) with an invalid "
                "wait-event class byte — the resolved address/offset is wrong "
                "for this PostgreSQL build.\n"
                "  Garbage readings are dropped, never recorded "
                "(invalid_wait_reads_total counts them).\n");
    }

    /* Fold the batch into the live accumulator so the running --view reflects
     * sampled data in real time (there is no ringbuf consumer in this tier). */
    pgwt_sampler_accumulate(d, s->samples, count, period_ns);

    /* Anomaly-trigger rules (A5) run on the live sample batch every tick — in
     * tiered mode they may AUTO-escalate to full fidelity. Evaluated even when
     * count == 0 (an all-idle / all-CPU tick is a legitimate low-AAS reading
     * that the rolling baseline must learn from). No-op outside tiered mode. */
    pgwt_anomaly_tick(d, s->samples, count);

    if (count == 0)
        return 0;

    if (d->event_writer)
        pgwt_writer_push_samples(d->event_writer, s->samples, count,
                                 period_ns);

    s->samples_total += (uint64_t)count;
    d->counters.samples_total += (uint64_t)count;
    return 0;
}

void pgwt_sampler_metrics(struct pgwt_daemon *d, struct pgwt_metrics *m)
{
    if (d->sampler) {
        m->samples_total      = d->sampler->samples_total;
        m->sample_read_faults = d->sampler->read_faults_total;
        m->sample_read_failures_total = d->sampler->read_failures_total;
        m->sample_read_targets = d->sampler->read_targets_last;
        m->sample_read_valid = d->sampler->read_valid_last;
        m->sample_read_invalid = d->sampler->read_invalid_last;
    }
}

/* ── Provider vtables ─────────────────────────────────────────────────── */

const struct pgwt_capture_provider pgwt_provider_sampled = {
    .name         = "sampled",
    .fidelity     = PGWT_FIDELITY_SAMPLED,
    .start        = pgwt_sampler_start,
    .stop         = pgwt_sampler_stop,
    .poll         = pgwt_sampler_poll,
    .self_metrics = pgwt_sampler_metrics,
};

#endif /* !PGWT_SERVER */
