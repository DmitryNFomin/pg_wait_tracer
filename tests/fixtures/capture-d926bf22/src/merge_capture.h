#ifndef PGWT_MERGE_CAPTURE_H
#define PGWT_MERGE_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include "core/pgwt_merge_adapter.h"
#include "transport_budget.h"

struct pgwt_daemon;
struct pgwt_merge_capture;

/* Identity/proof history is retained until capture teardown. No retirement or
 * reuse: the next distinct lifetime after this limit pauses exact capture
 * with VOID(allocation_fail). Query bindings have the same hard capacity. */
#define PGWT_CAPTURE_LIFETIME_CAPACITY 262144U

/* Userspace attachment facts, NOT a wire record. event is ma_attach_event;
 * known/true use MA_ALLOCATOR through MA_IDEMPOTENT_INIT, in that bit order.
 * A missing known bit is unknown, not evidence. No facts are supplied here. */
struct pgwt_attach_report {
    uint32_t pid;          /* target backend */
    uint32_t event;        /* MA_BEGIN/COMPLETE/INTERRUPTED/etc. */
    uint64_t cookie;       /* target lifetime; 0 means absent */
    uint32_t facts_known;  /* twelve MA_* fact bits whose values are known */
    uint32_t facts_true;   /* true subset of facts_known */
};
_Static_assert(sizeof(struct pgwt_attach_report) == 24, "attach report size");
_Static_assert(offsetof(struct pgwt_attach_report, pid) == 0, "attach pid");
_Static_assert(offsetof(struct pgwt_attach_report, event) == 4, "attach event");
_Static_assert(offsetof(struct pgwt_attach_report, cookie) == 8, "attach cookie");
_Static_assert(offsetof(struct pgwt_attach_report, facts_known) == 16, "attach known");
_Static_assert(offsetof(struct pgwt_attach_report, facts_true) == 20, "attach true");

/* A2 delivers reports through this single callback. B installs it at open. */
typedef int (*pgwt_attach_report_fn)(void *ctx,
                                    const struct pgwt_attach_report *report);

/* Full checkpoint: refresh all 15 components, including the 13 BPF-backed
 * components (0 and 2-13), logical INIT loss (1), daemon state-map failures
 * added to component 12, and writer rejection (14). Called at open, after
 * every CPU ack and reached target before accepting a round's boundaries,
 * on faults/LOST, at shutdown, and every 10 ms of transport silence. The sink
 * refreshes ONLY component 14 after each publication (pgwt_merge.h:84-86). */
int pgwt_merge_capture_snapshot(struct pgwt_daemon *, ma_snapshot *, uint64_t *);

/* Flat merge output: B's qualification dump is the first consumer; the
 * later capture writer can supply a second sink without changing daemon.c.
 * Callback borrows the output for this call and returns false on rejection. */
struct pgwt_merge_capture_sink {
    pgwt_merge_sink emit;
    void *ctx;
    /* Optional buffered consumer: finish an output batch after each core
     * input, even if it emitted TEXT without advancing the frontier. A
     * FRONTIER/VOID must already be written before emit returns success. */
    bool (*flush)(void *);
};

int pgwt_merge_capture_open(struct pgwt_daemon *d);
int pgwt_merge_capture_poll(struct pgwt_daemon *d);
/* Calls the SAME tick duties as the default handler, at the same drain point.
 * Full-only admission makes the lightweight accumulator branch inactive;
 * summary/escalation maintenance is idle without a writer/open window.
 * The live view contains only released DATA plus the legacy state-map open
 * intervals. Queued/held records can appear on a later tick; the tick reports
 * their count at the copy. It is not a certification claim: only the
 * qualification sink's FRONTIER certifies released DATA. Display timer has
 * already been read. */
int pgwt_merge_capture_tick(struct pgwt_daemon *d);
/* All queued/held envelopes, including boundaries/lifecycle/TEXT, counted
 * once across transport, adapter and core. This is a backlog gauge, not a
 * missing-duration estimate or a count of unconsumed kernel records. */
uint64_t pgwt_merge_capture_pending_records(const struct pgwt_daemon *d);
size_t pgwt_capture_pending_records(const struct pgwt_merge_capture *);
/* Final drain/held-data disposition before any capture state is destroyed. */
int pgwt_merge_capture_finish(struct pgwt_daemon *d);
int pgwt_merge_capture_record(struct pgwt_daemon *d, size_t source,
                             const void *bytes, size_t size);
int pgwt_merge_capture_attach(struct pgwt_daemon *d,
                             const struct pgwt_attach_report *report);
void pgwt_merge_capture_close(struct pgwt_daemon *d);

/* Portable production feed, also used by the injected reader tests. */
struct pgwt_capture_config {
    size_t cpu_count, capacity;
    pgwt_merge_observation baseline;
    struct pgwt_merge_capture_sink sink;
    void *ctx;
    /* Lifecycle side effects run once at wire consumption, before admission;
     * the attachment callback may synchronously report BEGIN/COMPLETE. */
    int (*lifecycle)(void *, const void *, size_t);
    int (*trace)(void *, const void *, size_t);
    /* Publication refresh ONLY: update writer_push_fail (component 14) and
     * detection time. Preserve components 0-13 and their known state from
     * the latest completed-round/silence checkpoint. Never read BPF here. */
    int (*observe)(void *, pgwt_merge_observation *);
    uint64_t (*now)(void *);
    void (*writer_failed)(void *);
    /* TEST ONLY (daemon: PGWT_TEST_EXACT_ATTACH_PREMISE=1). Assume the
     * attachment proof of every fork-seeded lifetime at FORK consumption.
     * Without unit A2 no proof exists and every such generation VOIDs. */
    bool test_attach_premise;
};
struct pgwt_merge_capture *pgwt_capture_create(const struct pgwt_capture_config *);
/* Returns 1 for a record outside the covered population (TRACE/lifecycle
 * with no lifetime cookie): its legacy side effect ran, but it must not be
 * passed to pgwt_capture_envelope. 0 otherwise; negative errno on failure. */
int pgwt_capture_received(struct pgwt_merge_capture *, const ma_envelope *, uint64_t);
int pgwt_capture_envelope(struct pgwt_merge_capture *, const ma_envelope *,
                           uint64_t, const ma_snapshot *);
void pgwt_capture_set_budget(struct pgwt_merge_capture *, const struct pgwt_transport_budget *);
int pgwt_capture_fault(struct pgwt_merge_capture *, pgwt_merge_reason,
                        uint64_t, const ma_snapshot *);
int pgwt_capture_allocation_fault(struct pgwt_merge_capture *, pgwt_allocation_failure,
                                   uint64_t, const ma_snapshot *);
pgwt_merge_diagnostics pgwt_capture_diagnostics(const struct pgwt_merge_capture *);
int pgwt_capture_attach(struct pgwt_merge_capture *, const struct pgwt_attach_report *,
                         uint64_t, const ma_snapshot *);
uint64_t pgwt_capture_max_held_ns(const struct pgwt_merge_capture *, uint64_t now);
/* Measurements, not gates. held: wire consumption to publication, per
 * published DATA/TEXT. event: semantic timestamp to publication, per
 * published DATA (includes kernel-buffer residence). Quantiles are bucket
 * upper edges, 100 us resolution, saturating at 2 s. */
struct pgwt_capture_stats {
    uint64_t released_data, released_text, frontiers, voids;
    uint64_t source_budget_yields, sweep_budget_yields, unfinished_records;
    uint64_t held_p50_ns, held_p99_ns, held_max_ns;
    uint64_t event_p50_ns, event_p99_ns, event_max_ns;
    /* The record with the largest event age: merge rank (0 FORK, 1 INIT,
     * 2 TRANS/MARKER, 3 EXIT), source index (CPUs, then E, L) and pid. */
    unsigned event_max_rank, event_max_source;
    uint64_t event_max_pid;
};
struct pgwt_capture_stats pgwt_capture_stats(const struct pgwt_merge_capture *);
bool pgwt_capture_waiting(const struct pgwt_merge_capture *);
ma_snapshot pgwt_capture_last_snapshot(const struct pgwt_merge_capture *);
pgwt_merge_report pgwt_capture_report(const struct pgwt_merge_capture *);
int pgwt_capture_finish(struct pgwt_merge_capture *, uint64_t, const ma_snapshot *);
void pgwt_capture_destroy(struct pgwt_merge_capture *);
/* Qualification evidence only, NOT a product capture format. Explicit LE,
 * no C padding. File header: magic "PGWTQB02" (8), version u32=2, cpu_count
 * u32, lifetime_capacity u32, TRACE bytes/record u32=88, then cpu_count u32
 * physical CPU IDs. Each capture appends its own header.
 * Record prefix: size u16 (including prefix), kind u8 (merge output enum;
 * 4=END), flags u8, source u32 (VOID: reason enum, others without source: 0).
 * DATA/TEXT: epoch,o,g,dependency_id (4*u64), then unchanged payload. DATA
 * flags: bit0 prior_interval_certified, bits1-2 rank, bit3 identity_known,
 * bit4 fork_seed_proved, bits5-6 dependency. N: q=0, has_q=false. t,pid are
 * in the unchanged TRACE/LIFECYCLE payload. DATA is 88 bytes for TRACE and
 * 64 for LIFECYCLE; TEXT is 312 bytes. FRONTIER/VOID: epoch,F,d (3*u64),
 * 15*u64 counters in merge-v1 order, flag0=known; 152 bytes.
 * END: max_held_ns,perf_lost_total (2*u64), status u32, reserved u32=0;
 * flag0=lost_known, flag1=epoch_invalid; 32 bytes.
 * DATA/TEXT append to preallocated memory. FRONTIER/VOID flush that release
 * batch in ONE write callback; no per-record stdio/fflush. A failed batch
 * is discarded, followed by the core's VOID(writer_push_fail) attempt.
 * A consumer must discard the uncertified suffix on VOID or a partial tail. */
#define PGWT_CAPTURE_DUMP_TRACE_BYTES 88U
struct pgwt_capture_dump;
typedef bool (*pgwt_capture_write_fn)(void *, const void *, size_t);
struct pgwt_capture_dump *pgwt_capture_dump_create(size_t records,
                                                  pgwt_capture_write_fn, void *);
bool pgwt_capture_dump_header(struct pgwt_capture_dump *, const uint32_t *, size_t);
bool pgwt_capture_dump(void *, const pgwt_merge_output *, pgwt_merge_observation *);
bool pgwt_capture_dump_flush(void *);
bool pgwt_capture_dump_end(struct pgwt_capture_dump *, uint64_t age,
                           bool lost_known, uint64_t lost, pgwt_merge_report);
void pgwt_capture_dump_destroy(struct pgwt_capture_dump *);

#ifdef PGWT_TRANSPORT_HOST_TEST
/* Frozen corpus supplies initial attached generations, dependency premises,
 * and explicit REARMs. These fixture seams are absent from the Linux daemon;
 * the ordinary create/received/envelope/sink paths are still exercised. */
struct pgwt_merge_capture *pgwt_capture_replay_create(
    const struct pgwt_capture_config *, const pgwt_merge_config *, const ma_table *);
pgwt_merge_status pgwt_capture_replay_control(struct pgwt_merge_capture *, const pgwt_merge_input *);
ma_table pgwt_capture_replay_table(struct pgwt_merge_capture *);
#endif

#endif
