#ifndef PGWT_PERCPU_TRANSPORT_H
#define PGWT_PERCPU_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pgwt_transport_wire.h"
#include "transport_budget.h"
#include "transport_diagnostics.h"
#include "core/pgwt_merge_adapter.h"

struct pgwt_daemon;
struct pgwt_percpu_transport;

/* Portable perf_event_attr values (Linux UAPI numbers). The Linux binding
 * copies these into the real attr; the host test pins the same policy. */
struct pgwt_transport_perf_attributes {
    uint32_t type, wakeup_events, wakeup_watermark;
    uint64_t config, sample_type, sample_period;
    bool watermark;
};
struct pgwt_transport_perf_attributes pgwt_transport_perf_attributes(size_t buffer_bytes);
/* The daemon's existing epoll timeout is 10 ms. NEW round requests, topology,
 * helper health, the open round's failure poll and the standalone core CHECK
 * run at most once per 10 ms slot, even with continuous watermark/control
 * readiness. Drain on every call. Keep slots anchored to the clock: early
 * wakes must not reset the deadline. The fence (after the round's E and L
 * boundaries) and completion are progress work: retried on EVERY loop pass.
 *
 * COMPLETION RULE. A round completes when (a) its E and L boundaries were
 * consumed, (b) every CPU's higher-round acknowledgement was consumed, and
 * (c) consumption reached the frozen target on every (CPU,destination), with
 * every skipped sequence number a counted discard and no failure counted.
 * An empty pass is NOT required: emptiness is not evidence (CONTRACT.md:128)
 * and a busy box may never produce one. With prompt helpers the round
 * completes in the pass that consumes the last acknowledgement.
 * A target not reached is a HOLD, not a fault, while its records can still
 * arrive. It is decided in a bounded number of passes even if no pass is
 * ever empty: the E/L producer positions are read once after every ack (the
 * cut); each pass reads up to 4096 records per ring and yields at its time
 * budget. Passes that stop early count toward the existing stall limit.
 * Unfinished reservations are never skipped. After the calculated backlog
 * allowance plus PGWT_TRANSPORT_CUT_STALL_PASSES the round fails closed with
 * VOID(unknown_evidence), including when time yields delay the cut.
 * Past the cut, a still-missing attempt must be a counted discard, else
 * VOID(sequence_gap). A failed attempt is diagnosed without any pass, from
 * the failure counters read after the acks and on every service slot.
 * DATA newer than an E/L cutoff can need two rounds. Kernel-buffer residence
 * is <=10 ms + one pass. Helpers, OS stalls and attachment proof need their
 * own bounds; there is no unconditional 70 ms/100 ms guarantee: the box
 * measures held age and event-to-release age, and the daemon reports both.
 * The daemon suspends with VOID(allocation_fail) if the 262144-entry queue
 * fills. Each main-loop iteration shares a 50 ms cooperative service budget,
 * with at most 10 ms for raw draining before completion/merge work. Callbacks
 * consume the yielding record; unread and unadmitted suffixes stay queued.
 * Shutdown drains and completes rounds for up to 256 passes/100 ms, then
 * VOIDs unknown_evidence and counts evidence still queued or unread.
 * An individual core feed or OS call is synchronous; these budgets apply
 * between those calls (issue 932 tracks the core's quadratic work).
 * Time is NEVER completeness evidence or permission to release DATA. */
#define PGWT_TRANSPORT_MIN_RING_RECORD 56U   /* 48-byte boundary + 8-byte ring header */
#define PGWT_TRANSPORT_CUT_STALL_PASSES 4096U
uint64_t pgwt_transport_service_interval_ns(void);
bool pgwt_transport_service_due(uint64_t now, uint64_t previous, bool serviced);

/* Callback dispatch keeps existing isolated lifecycle tests linkable without
 * transport I/O. Daemon init installs the production functions below. */
struct pgwt_transport_callbacks {
    int (*consume)(struct pgwt_daemon *, uint32_t dest, bool bounded);
    int (*record)(struct pgwt_daemon *, uint32_t dest,
                  const void *bytes, size_t size);
};

/* B owns these seams. Negative errno on failure, never silently succeed.
 * PGWT_TRANSPORT_HOST_TEST selects portable test I/O in scripts/check.sh;
 * B keeps Linux loader/reader dependencies out of that compilation.
 * prepare is idempotent (CLI and daemon init both call it), runs before
 * discovery/loading, and cleans up its own partial failure. open runs after
 * skeleton load and before producer attachment.
 * The nested epoll fd covers CPU, E and L; poll must be bounded and service
 * completion rounds/held data even when no fd was ready. */
int pgwt_percpu_transport_prepare(struct pgwt_daemon *d);
/* Called after skeleton open, before load: map sizing, helper identity and
 * rodata setup owned by B. helper_tgid is available after prepare(). */
int pgwt_percpu_transport_configure(struct pgwt_daemon *d);
uint32_t pgwt_percpu_transport_helper_tgid(const struct pgwt_daemon *d);
size_t pgwt_percpu_transport_cpu_count(const struct pgwt_daemon *);
uint32_t pgwt_percpu_transport_cpu_id(const struct pgwt_daemon *, size_t);
int pgwt_percpu_transport_open(struct pgwt_daemon *d);
int pgwt_percpu_transport_epoll_fd(const struct pgwt_daemon *d);
int pgwt_percpu_transport_poll(struct pgwt_daemon *d);
/* One deadline shared by every service call in this main-loop iteration. */
void pgwt_percpu_transport_begin_pass(struct pgwt_daemon *, uint64_t capture_deadline_ns);
void pgwt_transport_begin_pass(struct pgwt_percpu_transport *, uint64_t limit_ns);
bool pgwt_transport_pass_expired(struct pgwt_percpu_transport *);
/* Full counter checkpoint through the same injectable I/O as the reader.
 * Available after prepare/load, including before the buffers are opened. */
int pgwt_percpu_transport_snapshot(struct pgwt_daemon *, ma_snapshot *, uint64_t *now);
/* consume returns records consumed, or negative errno. E returns -EAGAIN
 * on a budget yield so the main loop immediately services pending work.
 * bounded is the existing event-loop budget request; callers must not drain
 * raw rings. */
int pgwt_percpu_transport_consume(struct pgwt_daemon *d, uint32_t dest,
                                 bool bounded);
int pgwt_percpu_transport_record(struct pgwt_daemon *d, uint32_t dest,
                                const void *bytes, size_t size);
void pgwt_percpu_transport_close(struct pgwt_daemon *d);
/* End schedstat samples and print reader/helper deltas before teardown. */
void pgwt_percpu_transport_finish_diagnostics(struct pgwt_daemon *d);

/* Portable reader/protocol. CPU IDs are sorted physical online CPU numbers;
 * source indices are their ordinal positions followed by E and L. All I/O,
 * including completion-delimited draining, is supplied by this interface.
 * Linux calls to exercise in the post-A1 box smoke:
 * prepare/topology: sysfs online CPU read, libbpf_num_possible_cpus,
 *   sched_getaffinity, pthread_create/sigmask/getschedparam, helper pin checks
 *   (sched_setaffinity/getaffinity/getcpu) and SYS_getppid;
 * configure/open: BPF map/program introspection and set_max_entries, uname,
 *   map FD lookup, perf_buffer__new_raw, ring_buffer__new, epoll_create1/ctl,
 *   buffer epoll FDs and bpf_program__attach;
 * control/attempts/counters: bpf_map_update_elem / bpf_map_lookup_elem;
 * now/drain/consume/wait: clock_gettime, perf_buffer__consume,
 *   ring_buffer__consume and epoll_wait (shutdown wait <=1 ms);
 * close: pthread signal/join/destroy, bpf_link__destroy, buffer frees, close.
 * Legacy lifecycle/trace effects use capture_config callbacks. Qualification
 * open/write/close belongs to the separate injectable binary sink callback. */
struct pgwt_transport_io {
    void *ctx;
    /* Optional daemon binding operations. Injected readers can replace the
     * entire kernel lifetime, including map/support checks and helper setup. */
    int (*prepare)(void *);
    int (*configure)(void *);
    int (*open)(void *);
    int (*epoll_fd)(void *);
    uint32_t (*helper_tgid)(void *);
    int (*consume)(void *, uint32_t dest, bool bounded);
    void (*close)(void *);
    uint64_t (*now)(void *);
    void (*budget)(void *, const struct pgwt_transport_budget *);
    int (*cpu_set_unchanged)(void *);
    int (*control)(void *, const struct pgwt_transport_ctl *);
    int (*wake)(void *, uint32_t cpu);
    /* cpu-major triples, CPU/E/L, across exactly the participating CPUs. */
    int (*attempts)(void *, uint64_t *, size_t);
    /* Same shape. failures: attempts whose record did not arrive (any
     * nonzero value is a loss). discards: attempts withdrawn by design
     * after a successful reservation, nothing lost. Per key:
     * attempts = delivered + failures + discards. */
    int (*failures)(void *, uint64_t *, size_t);
    int (*discards)(void *, uint64_t *, size_t);
    /* Shared-ring byte positions, [0]=E and [1]=L: how far producers have
     * reserved, and how far this reader has consumed. Reservations are
     * ordered, and an unfinished one stops the reader. */
    int (*positions)(void *, uint64_t produced[2], uint64_t consumed[2]);
    int (*counters)(void *, ma_snapshot *);
    /* One bounded pass over every destination. Returns 1 if the pass
     * consumed nothing, 0 if it consumed something, negative errno on error.
     * The result feeds a statistic and the shutdown loop only; it is never
     * a completion condition. */
    int (*drain)(void *);
    /* Shutdown only: allow a requested helper to run between bounded passes.
     * wait returns negative errno on failure; production waits at most 1 ms,
     * rounded down to fit the current pass and absolute finishing deadline. */
    int (*wait)(void *);
    bool (*waiting)(void *);
    size_t (*pending)(void *);
    /* Legacy side effects at wire consumption. Returns 1 when the record is
     * outside the covered population (no lifetime cookie): it was consumed
     * and accounted but is not merge input. */
    int (*received)(void *, const ma_envelope *, uint64_t);
    /* Return the latest post-publication snapshot through the last argument;
     * a later queued envelope must never replay a pre-callback observation. */
    int (*envelope)(void *, const ma_envelope *, uint64_t, ma_snapshot *);
    bool (*suspended)(void *);
    int (*fault)(void *, pgwt_merge_reason, uint64_t, const ma_snapshot *);
};
struct pgwt_wire_record {
    ma_envelope envelope;
    uint32_t cpu, dest, round;
    uint64_t seq;
};
/* Decode only: position is supplied by the consuming reader. Payload is
 * borrowed; all fields are read explicitly as little endian, unaligned. */
int pgwt_transport_decode(const uint32_t *cpus, size_t ncpu, uint32_t dest,
                          uint64_t position, const void *, size_t,
                          struct pgwt_wire_record *);
struct pgwt_percpu_transport *pgwt_transport_create(
    const uint32_t *cpus, size_t ncpu, size_t capacity,
    const struct pgwt_transport_io *);
int pgwt_transport_record(struct pgwt_percpu_transport *, uint32_t dest,
                           const void *, size_t);
/* libbpf stops a ring consume only on a negative callback result. The record
 * at the limit is fully accounted; -EAGAIN is an intentional budget yield. */
int pgwt_transport_ring_record(struct pgwt_percpu_transport *, uint32_t dest,
                               const void *, size_t, size_t *consumed, size_t limit);
int pgwt_transport_ring_result(int result, size_t consumed, size_t limit);
/* perf RAW framing includes u32 advertised size and kernel alignment padding. */
int pgwt_transport_perf(struct pgwt_percpu_transport *, uint32_t cpu,
                        const void *, size_t);
int pgwt_transport_lost(struct pgwt_percpu_transport *, uint32_t cpu, uint64_t);
int pgwt_transport_poll(struct pgwt_percpu_transport *, bool waiting);
int pgwt_transport_finish(struct pgwt_percpu_transport *);
void pgwt_transport_destroy(struct pgwt_percpu_transport *);
bool pgwt_transport_lost_total(const struct pgwt_percpu_transport *, uint64_t *);
uint64_t pgwt_transport_max_held_ns(const struct pgwt_percpu_transport *);
/* Measurements, not gates. Index 0/1/2 of the final_ arrays is CPU/E/L. */
struct pgwt_transport_stats {
    pgwt_pool_usage queue;
    pgwt_allocation_failure allocation;
    struct pgwt_distribution request_to_el, fence_to_ack, ack_to_release_end;
    struct pgwt_distribution release_replay, records_per_round;
    uint64_t rounds_requested, rounds_completed;
    uint64_t rounds_completed_nonempty; /* completed on a pass that consumed something */
    uint64_t holds;              /* completion checks with every ack and an unreached target */
    uint64_t cuts;               /* rounds that needed the producer-position cut */
    uint64_t terminal_discards;  /* attempts missing at a cut, all counted discards; summed
                                  * over rounds, so one trailing discard counts once per round
                                  * until a later record of its key arrives */
    uint64_t skipped;            /* skipped sequence numbers explained by discards */
    uint64_t uncovered;          /* cookie-less records consumed, not merge input */
    uint64_t records, queue_high, round_ns_total, round_ns_max;
    uint64_t finish_deadline_reached, finish_pass_limit_reached, unfinished_records;
    uint64_t unfinished_transport_records, unfinished_capture_records, unfinished_unread_records;
    /* Scheduling deferrals count yields (a suffix may be deferred again).
     * Terminal counters above count the actual remaining items once. */
    uint64_t drain_budget_yields, drain_slice_yields, release_budget_yields, reader_budget_yields;
    uint64_t ring_budget_yields, perf_budget_yields, hint_budget_yields;
    uint64_t consume_budget_yields, wait_budget_yields, ring_record_cap_yields, cut_pass_limit_reached;
    uint64_t cut_pass_limit_records; /* queued and unaccounted frozen-target items at the cap */
    uint64_t acks_at_completion; /* CPUs whose higher-round boundary was consumed, summed over completed rounds */
    uint64_t acks_before_fence;  /* accepted acks whose cutoff precedes their fence request */
    uint64_t final_attempts[3], final_delivered[3], final_failures[3], final_discards[3];
    bool final_read, final_reconciled;
};
struct pgwt_transport_stats pgwt_transport_stats(const struct pgwt_percpu_transport *);
size_t pgwt_transport_pending_records(const struct pgwt_percpu_transport *);
bool pgwt_transport_silent(const struct pgwt_percpu_transport *, uint64_t now, uint64_t interval);

#ifdef PGWT_TRANSPORT_HOST_TEST
/* Corpus-only cadence: initial source/proof state and explicit controls are
 * fixture premises, not a production completion proof. Schedules separately
 * exercise the actual fence. Both modes use record() and the SAME queued
 * release function; these hooks are absent from the Linux daemon. */
void pgwt_transport_replay_position(struct pgwt_percpu_transport *, size_t, uint64_t);
int pgwt_transport_replay_release(struct pgwt_percpu_transport *);
#endif

#endif
