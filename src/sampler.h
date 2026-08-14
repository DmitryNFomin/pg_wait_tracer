/* sampler.h — Sampled capture provider (Track A, D1)
 *
 * Pure-userspace ASH-style sampling: on each timer tick read every tracked
 * backend's PGPROC->wait_event_info (batched process_vm_readv for targets in
 * SHARED memory, per-pid pread for process-local ones — SMP-2), turn each
 * non-CPU reading into a SAMPLES record, and hand the batch to the trace
 * writer via pgwt_writer_push_samples(). No BPF in the per-sample hot path;
 * PostgreSQL executes zero extra instructions.
 *
 * Addresses come from the backend registry (pid -> resolved wait_event_info
 * address, the same field the watchpoint tier resolves). query_id per sample
 * is joined from the BPF state_map (the on_report_query_id uprobe maintains
 * pid->query_id) — never read from PG memory in the sample path. The join is
 * one batched map dump per tick (BPF_MAP_LOOKUP_BATCH) with a per-pid
 * lookup fallback on kernels without batch support (SMP-4).
 *
 * Batching soundness (SMP-2): PG's main shm is an inherited MAP_SHARED
 * anonymous mmap — same VA in every backend AND the same pages, so reading
 * many backends' fields through ONE reader pid is exact. That argument does
 * NOT hold for process-local (.data/.bss) addresses (e.g. the
 * my_wait_event_info dummy in aux processes without a PGPROC): those exist
 * at the same VA in every forked child but are PRIVATE pages — a batched
 * read through another pid SUCCEEDS and returns the reader's value,
 * silently misattributed. Only targets whose address the daemon verified to
 * be in a shared mapping (is_shared) are batched; the rest are read per-pid.
 *
 * Robustness (SMP-1): a TOTAL read failure (e.g. EPERM on process_vm_readv
 * + fallback) is indistinguishable from an idle database in the data — so
 * it must be loud out-of-band: first failure and persistent failure are
 * logged, and the control-socket `status` carries sampler_healthy + reason.
 */
#ifndef PGWT_SAMPLER_H
#define PGWT_SAMPLER_H

#include "pg_wait_tracer.h"
#include "cmdline.h"   /* enum pgwt_backend_type — drives the we==0 policy */

#include <stdint.h>
#include <sys/types.h>

struct pgwt_daemon;
struct pgwt_metrics;

/* A registry snapshot the sampler reads each tick. Decouples the sampler's
 * core logic (testable without the daemon) from the live backend table:
 * the daemon fills this from d->backends, but the unit test fills it from a
 * fake registry. */
struct pgwt_sample_target {
    pid_t    pid;
    uint64_t wait_event_addr; /* PGPROC->wait_event_info address */
    uint64_t query_id;        /* joined from the registry/state_map */
    int      is_shared;       /* 1 = addr verified in a MAP_SHARED mapping
                               * (batchable); anything else = per-pid read
                               * only (SMP-2) */
    /* T2 (docs/AAS_SEMANTICS_DECISION.md): the decomposed-AAS inputs.
     * backend_type decides the on-CPU (we==0) policy and the category flag
     * stamped on the sample; cmd_open is the pg_stat_activity
     * state='active' gate (BPF on_report_activity uprobe) that decides
     * whether a CLIENT backend's we==0 reading is a CPU sample. */
    enum pgwt_backend_type backend_type;
    int      cmd_open;
};

/* The two sampled-attribution sources use the same normalized shape.  The
 * tick source supplies query_id=0 while its command gate is closed. */
struct pgwt_sampled_attr_value {
    uint64_t query_id;
    int cmd_open;
};

enum pgwt_sampled_attr_source {
    PGWT_SAMPLED_ATTR_UPROBE = 0,
    PGWT_SAMPLED_ATTR_TICK,
    PGWT_SAMPLED_ATTR_UNATTRIBUTED,
    PGWT_SAMPLED_ATTR_DROP,
};

enum pgwt_sampled_attr_mismatch {
    PGWT_SAMPLED_ATTR_MISMATCH_CMD_OPEN = 1u << 0,
    PGWT_SAMPLED_ATTR_MISMATCH_QUERY_ID = 1u << 1,
};

/* Sampler read-health tracking (SMP-1). Pure state machine (unit-testable):
 * a tick where n_targets > 0 but NOTHING could be read is a failure tick.
 * The first failure logs immediately; persistent failure re-logs
 * periodically; recovery logs once and clears the flag. */
struct pgwt_sampler_health {
    int      healthy;              /* starts true (1) */
    uint64_t consec_failed_ticks;
    uint64_t failed_ticks_total;
    uint64_t last_log_ns;
    int      last_errno;
};

/* What the caller should log after pgwt_sampler_health_note(). */
enum pgwt_sampler_health_action {
    PGWT_SAMPLER_LOG_NONE = 0,
    PGWT_SAMPLER_LOG_DEGRADED,    /* first failure, or periodic re-log */
    PGWT_SAMPLER_LOG_RECOVERED,
};

struct pgwt_sampler {
    /* sample_period_ns is the NOMINAL tick interval (1e9 / sample_rate_hz).
     * Each written SAMPLES block carries the EFFECTIVE period for that tick
     * (measured inter-tick elapsed — SMP-3: missed/late ticks must not
     * deflate sample weight vs wall time). */
    uint64_t sample_period_ns;
    uint64_t last_tick_ns;        /* previous tick timestamp (0 = first) */

    /* Scratch buffers sized for MAX_BACKENDS, allocated once. */
    uint32_t     *read_vals;     /* MAX_BACKENDS — wait_event_info per target */
    uint8_t      *read_valid;    /* MAX_BACKENDS — 1 only after a successful read */
    struct pgwt_trace_event *samples; /* MAX_BACKENDS — encoded batch */

    /* Batched pid->query_id join scratch (SMP-4). qid_cap entries. */
    uint32_t *qid_keys;           /* state_map key dump */
    struct pgwt_pid_state *qid_vals; /* state_map value dump */
    int       qid_cap;
    int       qid_batch_supported; /* -1 unknown, 0 no (per-pid fallback), 1 yes */

    struct pgwt_sampler_health health;   /* SMP-1 */

    /* Per-tick read coverage. Stage 3 consumes this to decide whether a tick
     * has enough sampler coverage to advance its detector; this stage only
     * produces and exposes it. */
    uint32_t read_targets_last;
    uint32_t read_valid_last;
    uint32_t read_invalid_last;

    uint64_t samples_total;       /* cumulative SAMPLES records written */
    uint64_t read_faults_total;   /* per-pid pread fallbacks taken */
    uint64_t read_failures_total; /* targets with no successful read */
};

/* Provider hooks (see provider.h). Exposed for the vtable in sampler.c. */
int  pgwt_sampler_start(struct pgwt_daemon *d);
int  pgwt_sampler_stop(struct pgwt_daemon *d);
int  pgwt_sampler_poll(struct pgwt_daemon *d);
void pgwt_sampler_metrics(struct pgwt_daemon *d, struct pgwt_metrics *m);

/* ── Exposed for unit testing (no daemon, no BPF) ─────────────────────── */

/* Read each target's wait_event_info from process memory. Targets marked
 * is_shared are read in one batched process_vm_readv() sweep (per-pid pread
 * fallback for entries that fault); all other targets are read individually
 * via pread(/proc/<pid>/mem) — NEVER through another pid (SMP-2). Writes
 * results into out_vals[i] and sets out_valid[i] to 1 only when that target
 * was read successfully. A successful value of 0 is therefore distinct from
 * an unread target, whose value remains 0 and validity remains 0. read_faults
 * (may be NULL) is incremented once per pread fallback taken in the batched
 * sweep. Returns the number of entries successfully read; errno is left at
 * the last failing syscall's value when the return is short. */
int pgwt_sampler_read_targets(const struct pgwt_sample_target *targets, int n,
                              uint32_t *out_vals, uint8_t *out_valid,
                              uint64_t *read_faults);

/* Build the SAMPLES batch from targets + their freshly-read values.
 *
 * On-CPU (we==0) policy — the T2 decision (docs/AAS_SEMANTICS_DECISION.md):
 *   - CLIENT (and UNKNOWN/unclassified) backends: recorded as a first-class
 *     CPU sample (event id 0) ONLY while the command-open gate is set —
 *     the pg_stat_activity state='active' window. A we==0 reading with the
 *     gate closed is between-command churn: NOT recorded as activity, but
 *     counted in *noncmd_cpu_skipped (may be NULL) for observability.
 *   - background process types (checkpointer, bgwriter, walwriter,
 *     autovacuum workers, parallel workers, io_workers, ...): we==0 always
 *     records as CPU — their parked states are instrumented Activity-class
 *     waits, so on-CPU unambiguously means working.
 *
 * Every record is stamped with an in-memory category flag from the target's
 * backend_type (FLAG_IO_WORKER / FLAG_MAINT / FLAG_BACKGROUND; client and
 * parallel workers are the unflagged foreground). Flags are never persisted
 * — the anomaly engine and the live accumulator consume them (io_worker
 * records must not enter AAS/DB Time).
 *
 * A target whose valid[i] is 0 is skipped before any CPU/wait classification.
 * A valid value with an INVALID class byte (CAP-2/5: garbage from a wrong
 * offset) is skipped and counted in *invalid_reads (may be NULL) — garbage
 * must never be recorded as data. `tick_ts` is the sample timestamp stamped
 * on every record. Returns the number of sample records written into out. */
int pgwt_sampler_build_batch(const struct pgwt_sample_target *targets,
                             const uint32_t *vals, const uint8_t *valid,
                             int n, uint64_t tick_ts,
                             struct pgwt_trace_event *out,
                             uint64_t *invalid_reads,
                             uint64_t *noncmd_cpu_skipped);

/* Record one tick's target-level read coverage. Counts are clamped to a
 * coherent 0 <= valid <= targets range; each invalid target contributes once
 * to read_failures_total. Pure and exposed for the sampler unit harness. */
void pgwt_sampler_note_coverage(struct pgwt_sampler *s, int targets, int valid);

/* T2: category flag (PGWT_EVENT_FLAG_*) for a backend type, and the we==0
 * recording policy. Pure helpers shared by the sampler and the server-side
 * category tagging. */
uint32_t pgwt_backend_type_flag(enum pgwt_backend_type bt);
int      pgwt_cpu_sample_recordable(enum pgwt_backend_type bt, int cmd_open);

/* Stage 2 source gate.  A degraded/disabled layout selects the existing
 * uprobe value.  A validated layout selects the coherent tick value, and a
 * failed tick read selects DROP with zeroed outputs — never the previous
 * tick or the uprobe shadow. */
enum pgwt_sampled_attr_source pgwt_sampler_select_attr(
    int tick_source_enabled, int tick_read_ok,
    const struct pgwt_sampled_attr_value *tick,
    const struct pgwt_sampled_attr_value *uprobe,
    struct pgwt_sample_target *target);

/* Compare one coherent at-tick value with its uprobe-map shadow after each
 * source applies cmd_open ? query_id : 0.  Retained idle query ids therefore
 * agree at zero instead of producing false shadow mismatches. */
unsigned pgwt_sampled_attr_compare(
    const struct pgwt_sampled_attr_value *tick,
    const struct pgwt_sampled_attr_value *uprobe);

/* Load-bearing shadow proof: only an at-tick active backend participates,
 * and its st_query_id is compared with the state map's raw last_query_id.
 * cmd_open boundary disagreement remains visible in the total tuple counters
 * but does not turn equal active query ids into false active mismatches. */
int pgwt_sampled_attr_active_query_mismatch(
    const struct pgwt_sampled_attr_value *tick,
    const struct pgwt_sampled_attr_value *uprobe);

/* T2 (EL9 fix): read a backend's OWN authoritative command state, bypassing
 * the transition-edge state_map. The on_report_activity/on_report_query_id
 * uprobes fire only at command START; a command already in flight when the
 * state_map entry is created loses that single edge and cmd_open/query_id stay
 * 0 for the whole command (CPU* == 0, no attribution). Reading each tick is
 * race-free: cmd_open <- (debug_query_string != NULL); query_id <-
 * PgBackendStatus.st_query_id (only while in a command, when the offset is
 * known). Reads against the target's own pid (both are process-local globals).
 * The cmd_open and query_id outputs are untouched on read failure — the caller
 * keeps its edge-maintained fallback. dqs_addr==0 (symbol unresolved) is a
 * no-op. */
void pgwt_read_cmd_gate(pid_t pid, uint64_t dqs_addr, uint64_t be_entry_addr,
                        int st_query_id_off, int *cmd_open, uint64_t *query_id);

/* SMP-3: effective sample period for the tick at `now_ns`, given the
 * previous tick's timestamp (0 = first tick → nominal). Late/missed ticks
 * yield a longer period so total sample weight tracks wall time; the result
 * is clamped to [nominal, 60s] (a sampler can be late, never early). */
uint64_t pgwt_sampler_effective_period(uint64_t nominal_ns,
                                       uint64_t last_tick_ns,
                                       uint64_t now_ns);

/* SMP-1: note one tick's read outcome. n_targets == 0 ticks are neutral.
 * Returns the log action the caller must perform (the state machine is
 * pure; the logging side effect stays with the daemon). */
enum pgwt_sampler_health_action
pgwt_sampler_health_note(struct pgwt_sampler_health *h, int n_targets,
                         int n_read, int err_no, uint64_t now_ns);

/* SMP-4: pid -> {query_id, cmd_open} join index over a dumped state_map.
 * Entries are sorted in place by pid; lookup is a binary search. */
struct pgwt_qid_entry {
    uint32_t pid;
    uint64_t query_id;
    int      cmd_open;    /* command-open gate from the state_map (T2) */
};
void     pgwt_qid_index_sort(struct pgwt_qid_entry *entries, int n);
uint64_t pgwt_qid_index_lookup(const struct pgwt_qid_entry *entries, int n,
                               uint32_t pid);
/* Full-entry variant (query_id + cmd_open). NULL on miss. */
const struct pgwt_qid_entry *
pgwt_qid_index_get(const struct pgwt_qid_entry *entries, int n, uint32_t pid);

#endif /* PGWT_SAMPLER_H */
