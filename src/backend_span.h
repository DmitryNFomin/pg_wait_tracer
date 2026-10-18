/* backend_span.h — the BACKEND ACTIVE SPAN reducer (#455).
 *
 * WHY THIS EXISTS
 * ---------------
 * Queries' top row read `DB Time 720.4 s / 59.8%` beside `Execs 91,
 * Avg Exec 33 us`. The 714 s `Lock:relation` behind that DB Time is taken in
 * PARSE ANALYSIS — before `query__execute__start` — so every latency column
 * in the response is structurally blind to it: `Avg Exec` averages the
 * ninety-one executions that DID enter the executor, on a DIFFERENT backend.
 * The reader is left to reconcile "720 seconds of database time" with
 * "33 microseconds per execution" and the only available arithmetic,
 * DB Time / Execs = 7.9 s, is a number NOBODY EXPERIENCED: `Execs` counts
 * completed EXEC pairs and the waiting backend has none, so that quotient
 * divides one session's wait by ninety-one other sessions' executions.
 *
 * THE ONE FORBIDDEN OPERATION
 * ---------------------------
 * A span's duration comes EXCLUSIVELY from observed timestamps. Nothing in
 * this file, and nothing downstream of it, may derive a duration by
 * combining DB Time, Execs or Avg Exec. There is no accumulated-time input
 * to the reducer at all — `pgwt_span_reduce` sees a record's
 * `timestamp_ns`/`duration_ns` and the lifecycle markers, and that is the
 * whole of its arithmetic. tests/test_backend_span.c mutation 1 replaces a
 * span duration with DB Time / Execs and requires the timestamp oracle to
 * go red.
 *
 * A phase split of DB Time was considered and REJECTED as the fix: nested
 * execution clears `exec_open` while an outer execution is still open
 * (compute.c:400,506), exact intervals take their phase flags from
 * processing-time state (compute.c:581) so an interval crossing a phase
 * boundary is not partitioned, and the residual includes bind and commit —
 * "before execution" would overstate what was actually measured.
 *
 * WHAT A SPAN IS
 * --------------
 * One backend's ACTIVITY interval: the command-open window delimited by the
 * CMD_START / CMD_END markers (`pgstat_report_activity` state transitions),
 * for one pid, carrying whatever executions, waits and query associations
 * were observed inside it. It is NOT a statement: the trace has no command
 * instance id and no portal id (src/pg_wait_tracer.h:192), and the query id
 * BPF resolves AT CMD_START is the PREVIOUS statement's (compute.c:492-504
 * deliberately ignores it), so this file never uses it either. The identity
 * below is derived from backend identity plus the OBSERVED CMD_START
 * timestamp and claims to identify an ACTIVITY SPAN, never a statement.
 *
 * TWO INDEPENDENT AXES, NEVER COLLAPSED
 * -------------------------------------
 *   measurement = measured | partial | unmeasured   — what the markers support
 *   completion  = closed   | in_flight | unknown    — what the backend did
 *
 * They are orthogonal on purpose. A span can be `closed` and `partial` (a
 * CMD_END observed with no CMD_START: the left window edge), `in_flight` and
 * `measured`-impossible (an unobserved end is never "measured"), or
 * `unmeasured` and `unknown` (no CMD markers at all). Missing markers
 * produce `unmeasured`, which renders as "Not measured" — never a blank,
 * never a zero, and never a completed duration.
 *
 * `completed_duration_ns` EXISTS ONLY when BOTH boundaries were observed.
 * `pgwt_span_completed_duration_ns` returns 0 and the caller must serialize
 * JSON `null`; substituting the window end for an unobserved CMD_END is the
 * specific defect mutation 3 exists to catch.
 */
#ifndef PGWT_BACKEND_SPAN_H
#define PGWT_BACKEND_SPAN_H

#include <stdint.h>
#include "pg_wait_tracer.h"
#include "compute.h"

/* How well the markers delimit the span. */
enum pgwt_span_measurement {
    PGWT_SPAN_UNMEASURED = 0,  /* no CMD marker for this pid: nothing delimits it */
    PGWT_SPAN_PARTIAL    = 1,  /* one boundary observed, or a known gap inside */
    PGWT_SPAN_MEASURED   = 2,  /* both boundaries observed, no gap inside */
};

/* What the backend's activity did, independent of how well we measured it. */
enum pgwt_span_completion {
    PGWT_SPAN_UNKNOWN   = 0,   /* no CMD markers: we cannot say */
    PGWT_SPAN_IN_FLIGHT = 1,   /* open at the end of the processed stream */
    PGWT_SPAN_CLOSED    = 2,   /* a CMD_END was observed. The ACTIVITY-STATE
                                * transition closed — NOT a claim that the
                                * statement succeeded. */
};

#define PGWT_SPAN_MAX_QIDS 8   /* query associations retained per span */

/* One query id associated with a span, and the evidence that associated it.
 * `n_records` counts attributed wait records observed inside the span;
 * `n_exec_starts` counts EXEC_START markers carrying this id. A span may
 * legitimately have n_records > 0 and n_exec_starts == 0 — that IS the #455
 * backend, and requiring the latter is mutation 2. */
struct pgwt_span_qid {
    uint64_t query_id;
    uint32_t n_records;
    uint32_t n_exec_starts;
};

struct pgwt_backend_span {
    uint32_t pid;

    /* Identity: backend identity + the OBSERVED CMD_START timestamp.
     * `identity_ns` is 0 exactly when `start_observed` is 0, in which case
     * the span is identified by pid + `first_evidence_ns` and the UI must
     * say "Start not captured". Not a statement identifier. */
    uint64_t identity_ns;
    uint8_t  start_observed;
    uint8_t  end_observed;
    uint64_t start_ns;          /* meaningful iff start_observed */
    uint64_t end_ns;            /* meaningful iff end_observed */

    /* Observed evidence boundaries: the earliest interval START and latest
     * interval CLOSE of any record attributed to this span. Both are record
     * timestamps. The window edges never appear here. */
    uint8_t  has_evidence;
    uint64_t first_evidence_ns;
    uint64_t last_evidence_ns;

    uint8_t  measurement;       /* enum pgwt_span_measurement */
    uint8_t  completion;        /* enum pgwt_span_completion */
    uint8_t  is_worker;         /* pid has a leader in the link table */
    uint32_t leader_pid;        /* 0 when this pid is a leader (or unlinked) */

    /* Evidence quality inside the span. `gap_inside` forces PARTIAL even
     * with both boundaries observed: a known gap is NEVER bridged into a
     * fully measured span. */
    uint8_t  gap_inside;
    uint8_t  sampled_inside;    /* a SAMPLE record fell inside: sampled tier */
    uint8_t  escalation_inside; /* an escalation boundary fell inside */
    uint8_t  malformed;         /* CMD_START without CMD_END, or the reverse */

    /* Contained execution records. ZERO is a first-class value. */
    uint32_t n_exec_starts;
    uint32_t n_exec_ends;
    uint32_t n_plan_starts;
    uint32_t n_records;         /* non-marker records observed inside */

    /* Extended protocol / multi-statement disclosure. `client_pause_ns` is
     * the summed Client-class wait inside the span — the signal that one
     * span holds several executions AND client think time, which is why the
     * span is kept WHOLE and its duration is NOT additive across query
     * rows. */
    uint64_t client_pause_ns;
    uint8_t  multi_exec;        /* n_exec_starts > 1 */
    uint8_t  multi_qid;         /* more than one query id associated */

    uint32_t n_qids;
    uint32_t n_qids_dropped;    /* associations past PGWT_SPAN_MAX_QIDS */
    struct pgwt_span_qid qids[PGWT_SPAN_MAX_QIDS];

    /* Static string, never freed: WHY this span is partial/unknown. NULL
     * when measurement == MEASURED and completion == CLOSED. */
    const char *reason;
};

/* A known gap the caller supplies (coverage hole, tier change, rotation).
 * A span overlapping one is PARTIAL with `reason`, never MEASURED. */
struct pgwt_span_gap {
    uint64_t from_ns;
    uint64_t to_ns;
    const char *reason;
};

struct pgwt_span_params {
    uint64_t window_from_ns;    /* 0 = unbounded; used only for reporting */
    uint64_t window_to_ns;      /* 0 = unbounded; NEVER used as a boundary */
    uint32_t pid;               /* 0 = every pid. A pid SCOPES THE WHOLE
                                 * pass: records for other pids are not
                                 * reduced at all, so the per-query row
                                 * index is pid-scoped too — the same rule
                                 * DB Time already follows under a drill. */
    uint64_t query_id;          /* 0 = every association (detail scope only) */
    int      unattributed_only; /* select only spans with NO association */
    int      max_selected;      /* bounded detail output; <=0 = default */
    const struct pgwt_backend_link *links;
    int      n_links;
    const struct pgwt_span_gap *gaps;
    int      n_gaps;
};

/* Per-query-id row summary: the span the Queries row shows beside DB Time.
 * `best_inflight` wins when present, else `best_closed` (longest FULLY
 * MEASURED closed span), else `best_other` (so a marker-less query reads
 * "Not measured" rather than blank). Exactly one of the three is chosen by
 * pgwt_span_row_pick(). */
struct pgwt_span_qid_slot {
    uint8_t  used;              /* hash-slot occupancy; query_id 0 is never
                                 * indexed (span_assoc refuses it), but an
                                 * occupancy flag is still the only thing a
                                 * rehash may key on */
    uint64_t query_id;
    uint8_t  has_inflight, has_closed, has_other;
    struct pgwt_backend_span best_inflight;
    struct pgwt_backend_span best_closed;
    struct pgwt_backend_span best_other;
    /* Workers are reported SEPARATELY and never added to leader wall time. */
    uint32_t n_worker_spans;
    uint64_t worker_observed_max_ns;
    uint8_t  has_worker;
    uint32_t n_leader_spans;
};

struct pgwt_span_result {
    /* Bounded detail listing, ordered: in-flight first, then by observed
     * elapsed descending, tie-broken by (pid, identity_ns) ascending. */
    struct pgwt_backend_span *selected;
    int num_selected;
    int total_spans;            /* spans the pass produced, every scope */
    int scoped_spans;           /* spans matching the request's scope */
    int truncated;              /* scoped_spans > num_selected */

    /* Records that fell OUTSIDE any observed command window for a pid that
     * does have CMD markers — time between commands. Counted rather than
     * turned into spans, so an idle backend does not manufacture one span
     * per client round trip; reported so the count is never silently
     * dropped. */
    int n_outside_records;

    struct pgwt_span_qid_slot *qid_slots;
    int num_qid_slots;

    int n_closed, n_inflight, n_unmeasured, n_partial;
    int n_worker_spans;

    int failed;                 /* refused: NOTHING here may be read */
    const char *refusal_reason; /* static string, set iff failed */
};

/* Single pass over an already-ordered event array. Refuses (out->failed) on
 * any allocation failure and on an array it cannot trust (per-pid records
 * out of timestamp order), because a reducer that cannot see all of its
 * input must never answer. */
void pgwt_span_reduce(const struct pgwt_trace_event *events, int count,
                      const struct pgwt_span_params *params,
                      struct pgwt_span_result *out);

void pgwt_span_free(struct pgwt_span_result *out);

/* Observed elapsed: last observed boundary minus first observed boundary,
 * from TIMESTAMPS ONLY. Returns 0 (and writes 0) when the span has no two
 * distinct observed boundaries — the caller serializes null, never 0 as a
 * measured value. */
int pgwt_span_observed_elapsed_ns(const struct pgwt_backend_span *s,
                                  uint64_t *out_ns);

/* Completed duration: end_ns - start_ns, and ONLY when BOTH boundaries were
 * observed. Returns 0 otherwise — the window end is never substituted. */
int pgwt_span_completed_duration_ns(const struct pgwt_backend_span *s,
                                    uint64_t *out_ns);

const char *pgwt_span_measurement_name(int m);
const char *pgwt_span_completion_name(int c);

/* "In flight" / "Closed" / "Not measured" — the row's one-word kind. */
const char *pgwt_span_kind_name(const struct pgwt_backend_span *s);

/* Derived span identity as a stable string: "<pid>:<identity_ns>" when the
 * start was observed, "<pid>:~<first_evidence_ns>" when it was not, and
 * "<pid>:?" when there is no observation at all. Returns `buf`. */
char *pgwt_span_identity(const struct pgwt_backend_span *s,
                         char *buf, size_t len);

/* Row pick for the Queries row. Returns NULL when the slot holds nothing. */
const struct pgwt_backend_span *
pgwt_span_row_pick(const struct pgwt_span_qid_slot *slot);

const struct pgwt_span_qid_slot *
pgwt_span_qid_lookup(const struct pgwt_span_result *r, uint64_t query_id);

/* The selected span whose OBSERVED extent contains `ts` (narrowest first),
 * or NULL. Never "the nearest": the execution-detail panel asks which span
 * CONTAINS this execution, and a span near it is a different answer. */
const struct pgwt_backend_span *
pgwt_span_containing(const struct pgwt_span_result *r, uint32_t pid,
                     uint64_t ts);

#endif /* PGWT_BACKEND_SPAN_H */
