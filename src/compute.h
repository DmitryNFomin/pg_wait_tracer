/* compute.h — Server-side compute functions for pgwt-server */
#ifndef PGWT_COMPUTE_H
#define PGWT_COMPUTE_H

#include "pg_wait_tracer.h"
#include <stdint.h>

/* Sentinel for %DB of idle-but-visible events (e.g. Client:ClientRead).
 * %DB = "share of DB Time" and applies only to non-idle (DB-Time) events;
 * idle events have time but no meaningful share, so their pct_db is set to
 * this sentinel and rendered as "—" (em-dash) / null (JSON).
 *
 * #435: used by pgwt_event_row.pct_db (Events) AND pgwt_tm_row.pct_db_time
 * (Overview). Overview's idle rows carried a numeric 0.0, which renders as a
 * meaningful "0.0%" bar -- a reader sums a column of percentages and the Idle
 * row contributes a measured-looking zero to it. NOT APPLICABLE and ZERO are
 * different facts, and the whole point of this sentinel is that the wire can
 * tell them apart. Any new %DB field joins this convention or says why. */
#define PGWT_PCT_DB_IDLE (-1.0)
#define PGWT_PCT_DB_IS_IDLE(p) ((p) < 0.0)

/* ── Wait classes (same order as Rust WaitClass enum) ──────── */

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

static const char *pgwt_class_names[PGWT_NUM_CLASSES] = {
    "cpu", "io", "lock", "lwlock", "ipc", "client",
    "timeout", "bufferpin", "activity", "extension", "unknown"
};

/* Display names matching PostgreSQL conventions */
static const char *pgwt_class_display[PGWT_NUM_CLASSES] = {
    "CPU", "IO", "Lock", "LWLock", "IPC", "Client",
    "Timeout", "BufferPin", "Activity", "Extension", "Unknown"
};

/* Map wait_event_info → class index (0–10) */
int pgwt_wait_class_index(uint32_t event_id);

/* ── T2: decomposed AAS categories (docs/AAS_SEMANTICS_DECISION.md) ─────
 * Every non-idle record falls in exactly one category:
 *   fg planning / execution / command — client-backend (and parallel-
 *     worker) work, split by the PLAN/EXEC marker windows; "command" is
 *     in-command time outside both (parse, bind, commit/abort);
 *   maintenance — autovacuum workers;
 *   background — checkpointer / bgwriter / walwriter / other aux;
 *   io_worker — PG18 io_method=worker processes: EXCLUDED from AAS and
 *     DB Time (their busy time shadow-copies the requesting backends'
 *     AioIoCompletion waits) but tracked for the busy%% utilization metric.
 * Idle records (Activity class, ClientRead, non-command CPU) are category
 * -1: excluded-but-visible, exactly as before. */
#define PGWT_NUM_CATS 6
#define PGWT_CAT_FG_PLAN   0
#define PGWT_CAT_FG_EXEC   1
#define PGWT_CAT_FG_CMD    2
#define PGWT_CAT_MAINT     3
#define PGWT_CAT_BG        4
#define PGWT_CAT_IO_WORKER 5

static const char *pgwt_cat_names[PGWT_NUM_CATS] = {
    "planning", "execution", "command", "maintenance", "background",
    "io_worker"
};

/* pid → category-flag table (PGWT_EVENT_FLAG_IO_WORKER/MAINT/BACKGROUND or
 * 0 for foreground), sorted by pid, built from backends.jsonl metadata. */
struct pgwt_pid_cat {
    uint32_t pid;
    uint32_t flag;
};

/* Annotation pass (T2) over a merged, time-ordered event array:
 *   - stamps each record's flags with its pid's category flag;
 *   - sweeps the PLAN/EXEC/CMD markers per pid and stamps FLAG_PLAN /
 *     FLAG_EXEC / FLAG_CMD_OPEN on the records inside those windows;
 *   - rewrites exact we==0 intervals that are MAJORITY-outside a command
 *     window to PGWT_WEI_NONCMD_CPU (idle post-command time, per the
 *     decision table). Only for pids that HAVE CMD markers in the window —
 *     legacy traces and background processes keep all we==0 as CPU;
 *   - samples: foreground samples with a query_id are tagged FLAG_EXEC
 *     (the cheap sampled-tier phase attribution; plan/exec sub-windows
 *     need the exact tier's markers);
 *   - #128 (query_attr.h): a foreground non-session-idle record that closed with
 *     query_id 0 is re-attributed to the id its command reported later
 *     (or earlier) — FLAG_QUERY_BACKFILL — else FLAG_QUERY_UNATTRIB, so
 *     top_queries reports it as unattributed_ms instead of dropping it.
 *     Live consumers defer closed foreground records until an id or command
 *     boundary resolves them by the same rules; foreground open stretches
 *     use a known command id or await their closing record.
 *   - #596: non-idle background/maintenance records with query_id 0 are
 *     flagged QUERY_UNATTRIB directly, never backfilled. IO_WORKER records
 *     remain outside the query model. "Non-idle" is the LOAD rule, so a
 *     checkpointer in Timeout:CheckpointWriteDelay is neither attributed nor
 *     unattributed. The raw/reader tag pass (compute.c) applies this rule.
 *     Since #641, the live accumulator (map_reader.c) applies it to closed
 *     records, open stretches and samples: the unattributed label is final,
 *     so they never wait for an id. Both paths drop unknown categories
 *     rather than let them borrow an id. Offline replay (event_reader.c,
 *     pgwt_replay_events) files a query row only for a nonzero query_id,
 *     dropping query_id-0 time entirely, including the #128 foreground
 *     unattributed bucket. The summary writer (summary_writer.c,
 *     summary_qattr_step) defers query-less background/maintenance records
 *     with no category arm or load-rule exclusion: pacing is over-counted
 *     into summary_query_unattributed_ns_total when resolved without an id,
 *     and a later observed id or cmd_qid at a boundary can still backfill
 *     this time onto a real statement.
 * cats may be NULL/empty (no metadata: everything is foreground).
 *
 * #363 — RETURN VALUE. 1 when every record in the array was tagged, 0 when
 * the pass refused (working-allocation failure, or more distinct pids than
 * the per-pid table can hold). This array IS the oracle every raw estimator
 * reads (docs/AAS_SEMANTICS_DECISION.md), so a pass that completed only
 * partway is a CONFIDENTLY WRONG answer, not a degraded one: the records it
 * did not reach keep old_event 0 instead of PGWT_WEI_NONCMD_CPU (counted as
 * CPU, which the contract excludes) and keep their emission-time query_id.
 * On 0 the array may already be partly rewritten and MUST be discarded, not
 * consumed — there is no "how far did it get" to recover. An empty array
 * (count <= 0) is a complete success, not a refusal.
 *
 * warn_unused_result on purpose: the `void` form this replaces is exactly
 * how the failure stayed invisible, so dropping the result is now a
 * compile-time warning rather than a silent wrong number. */
int pgwt_tag_events(struct pgwt_trace_event *events, int count,
                    const struct pgwt_pid_cat *cats, int n_cats)
    __attribute__((warn_unused_result));

/* #424: the same pass, told where the window's right edge is.
 *
 * WHY IT EXISTS. A record admitted by OVERLAP closes after `to`, so the
 * CMD_END that would have closed its command lies past the window and is
 * never loaded. The in-command MAJORITY RULE then sees a full duration with
 * an apparently still-open command and can mark the record in-command (or,
 * symmetrically, out of it) on the strength of time that is not in the
 * window at all. That is not a labelling nicety: the decision rewrites a
 * we==0 record's old_event to PGWT_WEI_NONCMD_CPU, which the idle rules
 * exclude from DB Time — so getting it wrong MOVES THE DB TIME HEADLINE,
 * not just a tag. With `to_ns` the majority is taken over the record's
 * IN-WINDOW portion, which is the part the window is reporting on.
 *
 * `to_ns == 0` means "no right edge" and is EXACTLY the old behaviour;
 * pgwt_tag_events() is that call, kept so the dozen existing callers and
 * tests are untouched and so the unbounded path stays bit-identical.
 *
 * Still provisional for a right-open record, and deliberately not claimed
 * otherwise: its query-id attribution (the #128 backward pass) can change
 * once the rest of its command is visible, and so can its measured CPU. The
 * WAIT WALL is stable; the attribution is not. */
int pgwt_tag_events_window(struct pgwt_trace_event *events, int count,
                           const struct pgwt_pid_cat *cats, int n_cats,
                           uint64_t to_ns)
    __attribute__((warn_unused_result));

/* #586: one pid's query id as it stood AT THE WINDOW'S LEFT EDGE.
 *
 * The mirror of `to_ns` above, and it exists for the mirror-image reason.
 * A record straddling `from` arrives whole -- its close is inside the
 * window, so it is a real occurrence of this window -- but the
 * EXEC_START/PLAN_START that reported its command's query id can sit before
 * `from` and is therefore NOT in the array. #128's rule 2 ("the last id
 * reported EARLIER in the same command", `cmd_qid`) is built by the forward
 * pass over the loaded array alone, so it has no evidence and the record
 * falls to rule 3, the UNATTRIBUTED bucket. No per-query filter can reach
 * that bucket (pgwt_filter.query_id == 0 means "no filter"), so the
 * drill-down LOSES the record rather than mis-filing it: 183 records summed
 * over 22 query filters against 184 unfiltered, one Lock:relation of
 * 2435.069 ms.
 *
 * A seed is the ONE thing the loader can supply from outside the array, and
 * it is deliberately the only thing this changes. `cmd_qid` is read in
 * exactly one place (fwd[], rule 2's fallback), so a seed can move a record
 * BETWEEN queries and can do nothing else: no record is added, no duration
 * is changed, `seen_cmd` stays untouched so the in-command majority rule and
 * the PGWT_WEI_NONCMD_CPU relabel behind DB Time are not reachable from here.
 * Both halves of that are pinned as numbers in tests/test_left_edge_attrib.c
 * sections 3 and 4.
 *
 * `pgwt_tag_events_window()` is this call with no seeds, so every existing
 * caller and the unbounded path stay bit-identical. A seed for a pid that
 * reports an id inside the window is harmless by construction: the forward
 * pass overwrites cmd_qid at the first id it meets, and rule 1 (an id
 * reported LATER) already takes precedence over rule 2. */
struct pgwt_qid_seed {
    uint32_t pid;
    uint64_t query_id;      /* nonzero: a 0 seed is the absence of a seed */
};

int pgwt_tag_events_window_seeded(struct pgwt_trace_event *events, int count,
                                  const struct pgwt_pid_cat *cats, int n_cats,
                                  uint64_t to_ns,
                                  const struct pgwt_qid_seed *seeds,
                                  int n_seeds)
    __attribute__((warn_unused_result));

/* The forward pass's MARKER rule for cmd_qid, in one place so the loader's
 * left-edge lookback (src/server.c) decides a seed with the same rule the
 * in-array forward pass uses rather than with a transcription of it:
 *
 *   CMD_START      — a new command: forget the previous statement's id. (The
 *                    id BPF resolved AT CMD_START is the PREVIOUS statement's,
 *                    which is why this is a reset and not an observation.)
 *   ESCALATE_*     — pack a reason, not an id: ignored.
 *   any other with  a nonzero query_id — the command's reported id.
 *
 * Not CMD_END: a run straddling CMD_END still closes after it, so cmd_qid
 * survives a command's close (query_attr.h). */
void pgwt_qid_marker_step(uint32_t marker, uint64_t query_id,
                          uint64_t *cmd_qid);

/* Category of a tagged record: 0..PGWT_NUM_CATS-1, or -1 for idle. */
int pgwt_event_category(const struct pgwt_trace_event *ev);

/* ── Fidelity (trace format v2 — D3) ───────────────────────── */

/* The fidelity of the data covering a queried time window.
 *
 *   EXACT   — every covered range has TRANSITIONS (full-fidelity) blocks.
 *   SAMPLED — every covered range has only SAMPLES blocks (point
 *             observations); estimators use ASH math (each sample worth
 *             one sample_period_ns).
 *   MIXED   — the window spans both. Per the exact-wins merge rule,
 *             transition data is authoritative inside transition-covered
 *             ranges; samples only contribute outside them.
 *   NONE    — no data at all in the window.
 *
 * The integer ordering is meaningful for merging window fidelities
 * (see pgwt_fidelity_merge): NONE < EXACT < SAMPLED < MIXED. */
enum pgwt_fidelity {
    PGWT_FIDELITY_NONE    = 0,
    PGWT_FIDELITY_EXACT   = 1,
    PGWT_FIDELITY_SAMPLED = 2,
    PGWT_FIDELITY_MIXED   = 3,
};

/* The fidelity a view requires to produce a meaningful result.
 *   SAMPLED-capable views (time_model, system/session/query events, AAS,
 *     active/sessions) work from samples via ASH math.
 *   EXACT-required views (histogram/heatmap, transitions, fingerprints,
 *     lock chains, interference, variants, concurrency) need real
 *     intervals/order and report "unavailable" over a sampled-only window. */
enum pgwt_required_fidelity {
    PGWT_REQ_SAMPLED = 0,   /* samples are enough */
    PGWT_REQ_EXACT   = 1,   /* needs full-fidelity transitions */
};

/* Derive the overall fidelity of a loaded window from what block types
 * actually contributed records after the exact-wins merge. */
static inline enum pgwt_fidelity
pgwt_fidelity_of(int has_transitions, int has_samples)
{
    if (has_transitions && has_samples) return PGWT_FIDELITY_MIXED;
    if (has_transitions)                return PGWT_FIDELITY_EXACT;
    if (has_samples)                    return PGWT_FIDELITY_SAMPLED;
    return PGWT_FIDELITY_NONE;
}

/* Lowercase token used in JSON responses ("exact"|"sampled"|"mixed"|"none"). */
static inline const char *pgwt_fidelity_str(enum pgwt_fidelity fid)
{
    switch (fid) {
    case PGWT_FIDELITY_EXACT:   return "exact";
    case PGWT_FIDELITY_SAMPLED: return "sampled";
    case PGWT_FIDELITY_MIXED:   return "mixed";
    default:                    return "none";
    }
}

/* True when a view requiring `req` fidelity cannot be produced from a
 * window of the given `fid`. EXACT-required views are unavailable over a
 * window that contributed no transition data (SAMPLED or NONE). */
static inline int
pgwt_fidelity_unavailable(enum pgwt_required_fidelity req, enum pgwt_fidelity fid)
{
    if (req != PGWT_REQ_EXACT)
        return 0;
    return (fid != PGWT_FIDELITY_EXACT && fid != PGWT_FIDELITY_MIXED);
}

/* Canonical message for an EXACT-required view over a sampled-only window. */
#define PGWT_UNAVAILABLE_MSG "requires full-fidelity data"
#define PGWT_UNAVAILABLE_CODE "full_fidelity_required"
#define PGWT_UNAVAILABLE_HINT "capture an exact window (escalate) and retry"

/* ── Filters ──────────────────────────────────────────────── */

struct pgwt_filter {
    char     class_name[32];   /* "" = no filter */
    uint32_t event_id;         /* 0 = CPU; meaningful when has_event_filter */
    int      has_event_filter; /* explicit event filter, including CPU */
    uint32_t pid;              /* 0 = no filter */
    uint64_t query_id;         /* 0 = no filter */
};

int pgwt_filter_matches(const struct pgwt_filter *f,
                        const struct pgwt_trace_event *ev);

/* ── AAS Buckets ──────────────────────────────────────────── */

struct pgwt_aas_bucket {
    uint64_t start_ns;
    double   class_aas[PGWT_NUM_CLASSES];
    /* T2: the same AAS decomposed by category. io_worker records appear
     * ONLY here (their own slot) — never in class_aas or max_aas. */
    double   cat_aas[PGWT_NUM_CATS];
    /* T8: measured off-CPU AAS — the portion of on-CPU (class CPU) gaps that
     * was NOT actually on a CPU (runqueue/throttle/unaccounted). class_aas[CPU]
     * holds the measured on-CPU share; the two sum to the old full-gap CPU AAS,
     * so the total is unchanged. Only nonzero where measured cpu_ns exists (v3
     * exact data); sampled/v2 leave it 0 and CPU stays the full gap. */
    double   offcpu_aas;
};

#define AAS_MAX_EVENT_SERIES 16

struct pgwt_aas_event_series {
    uint32_t event_id;
    char     name[64];     /* "IO:DataFileRead" */
    double   total_aas;    /* sum across all buckets, for sorting */
};

struct pgwt_aas_result {
    struct pgwt_aas_bucket *buckets;   /* malloc'd, caller frees */
    int      num_buckets;
    uint64_t bucket_ns;
    double   max_aas;

    /* Per-event breakdown (populated when detail_events=1) */
    int      num_event_series;   /* 0 = class mode (default) */
    struct pgwt_aas_event_series event_series[AAS_MAX_EVENT_SERIES];
    double  *event_aas;          /* malloc'd [num_buckets * num_event_series] */
};

void pgwt_compute_aas(const struct pgwt_trace_event *events, int count,
                      const struct pgwt_filter *f,
                      uint64_t from_ns, uint64_t to_ns, int num_buckets,
                      int detail_events, int max_series,
                      struct pgwt_aas_result *out);

/* ── Time Model ───────────────────────────────────────────── */

struct pgwt_tm_row {
    char   name[64];
    double time_ms;
    double pct_db_time;
    double aas;
    int    indent;       /* 0=top, 1=class, 2=sub-event */
};

struct pgwt_tm_result {
    struct pgwt_tm_row *rows;   /* malloc'd, caller frees */
    int    num_rows;
    double db_time_ms;
    double idle_time_ms;
    double aas;
    double wall_ms;
    /* T2 category decomposition (raw path only; 0 on the summary path).
     * cat_ms[PGWT_CAT_IO_WORKER] is io_worker BUSY ms — outside DB Time. */
    double cat_ms[PGWT_NUM_CATS];
    double io_worker_busy_pct;  /* busy ms / (io_worker pids × wall) × 100 */
    /* T8 measured CPU. When has_measured_cpu is set (v3 exact data present),
     * the CPU* row is the measured on-CPU time and offcpu_ms is its off-CPU /
     * runqueue-unaccounted sibling (both inside DB Time); when unset the CPU*
     * row is the legacy full-gap inference and there is NO Off-CPU* row (the
     * quantity is unavailable, not zero). cpu_clamped_ms and wait_gap_cpu_ms
     * are the self-checks: CPU that exceeded a gap's wall (clock skew) and CPU
     * measured during wait-labeled gaps (should be ≈0 — a sleeping task burns
     * no CPU). */
    double offcpu_ms;
    double cpu_ms;              /* measured CPU (== the CPU* row) */
    int    has_measured_cpu;
    double cpu_clamped_ms;
    double wait_gap_cpu_ms;
    /* The Idle row's named children must account for its total exactly. A
     * POSITIVE remainder is expected and labelled ("Other idle (parked
     * background loops)": the hidden Activity share, plus anything the
     * summary path's bounded per-event tables could not hold). A NEGATIVE
     * one -- children exceeding
     * the parent -- cannot happen by construction on any of the three paths,
     * so if it ever does it means the writer and the reader have diverged
     * about what counts as idle. Surfaced here rather than clamped away,
     * because that divergence is exactly the thing worth hearing about.
     * Always 0 in a healthy window; asserted 0 by tests/test_idle_*.c. */
    double idle_children_excess_ms;
};


/* Window clipping (issue: windowed aggregates counted straddlers in full).
 * `from_ns`/`to_ns` bound the REQUESTED window; each event contributes only
 * min(end, to) - max(start, from), exactly as pgwt_compute_aas already does,
 * so Overview/Top-N and the AAS chart answer the same question over the same
 * events. 0 means "unbounded on that side": a whole-capture request (the
 * server forwards req->from_ns / req->to_ns verbatim, both 0 when absent)
 * therefore gets bit-identical pre-fix numbers. Latency columns (avg/max/
 * percentiles) are deliberately NOT clipped — a wait's latency is a property
 * of the wait, not of the window; only the time-in-window columns are. */
void pgwt_compute_time_model(const struct pgwt_trace_event *events, int count,
                             const struct pgwt_filter *f,
                             uint64_t from_ns, uint64_t to_ns, double wall_ms,
                             struct pgwt_tm_result *out);

/* ── Top Events ───────────────────────────────────────────── */

struct pgwt_event_row {
    uint32_t event_id;
    char     name[64];
    uint64_t count;
    double   total_ms;
    /* Latency statistics (avg/max/percentiles) are computed from EXACT
     * (transition) records only — samples carry no real durations, so
     * feeding their normalized sample_period_ns durations into these
     * columns fabricates percentiles (FID-3). exact_count == 0 means the
     * row is sampled-only and the latency columns must be gated (rendered
     * as null / "—"); count and total_ms stay valid via ASH math. */
    uint64_t exact_count;
    double   avg_us;
    /* 0 when the row has counts/total (hence a real avg) but NO latency
     * distribution behind them: the per-query summary records carry no
     * histogram and no max. max_us and p50/p95/p99_us are then meaningless
     * and the server emits null for all four (#103 review) — never a
     * percentile answered from an empty histogram. */
    int      has_latency_dist;
    double   max_us;
    double   p50_us;
    double   p95_us;
    double   p99_us;
    /* Set when the percentile landed in the histogram's OPEN-ENDED top
     * bucket (>16.384 ms): the p*_us value is then only a lower bound and
     * the UI must render it as ">= 16.4ms", never as an exact number
     * (issue #103 — P50=P95=P99=16.4ms printed next to Avg 1.8s). Not
     * derivable client-side: buckets 14 and 15 share the same edge. */
    int      p50_overflow;
    int      p95_overflow;
    int      p99_overflow;
    double   pct_db;
    double   aas;
};

struct pgwt_events_result {
    struct pgwt_event_row *rows;   /* malloc'd, caller frees */
    int    num_rows;
    double db_time_ms;
    /* #553: the CPU* row's measured-CPU contract, stated on the RESULT so a
     * consumer can tell the two cases apart instead of guessing:
     *
     *   has_measured_cpu != 0 — AT LEAST ONE in-window CPU gap carried a
     *       measurement. The CPU* row is then Σ over gaps of (measured
     *       cpu_ns, split by overlap and clamped to its wall) for the
     *       measured ones and (gap wall) for any UNKNOWN ones, so on MIXED
     *       data the row is part measurement and part inference — not
     *       "measured on-CPU time" flat (tests/test_window_clip.c 11k pins
     *       4 ms measured + 6 ms inferred with this flag set). offcpu_ms is
     *       the rest of the MEASURED gaps' wall only: runqueue / throttle
     *       time, the same quantity pgwt_compute_time_model reports as
     *       Off-CPU*. DB Time still holds BOTH, so the rows' %DB sums to
     *       100% − offcpu_ms/db_time_ms, not to 100%.
     *
     *       NOT the same predicate as pgwt_tm_result.has_measured_cpu, which
     *       is also raised by measured spin inside a WAIT and whose Off-CPU*
     *       row is additionally gated on a non-zero CPU class total. The two
     *       therefore disagree in two corners (#575); this one is the
     *       narrower, and src/server.c's dump_summary names both.
     *   has_measured_cpu == 0 — no in-window CPU gap carried a measurement
     *       (sampled / v2 / no CPU capability, or no CPU gap at all). CPU* is
     *       then the unsplit gap wall by gap-inference and offcpu_ms is 0 —
     *       a refusal to split, never a claim that off-CPU time was zero.
     *
     * pgwt_compute_top_events_from_summaries leaves both at 0: the per-second
     * summary's events[] table carries wall for event 0 (summary_writer.c's
     * `se->total_ns += tns`), so on that routing CPU* is the unsplit total and
     * says so. The two paths therefore describe the SAME total split two
     * ways, which is the identity pinned in tests/test_agg_raw_crosscheck.c
     * section 6f: raw CPU* + raw offcpu_ms == summary CPU*.
     *
     * cpu_clamped_ms is the same self-check pgwt_compute_time_model reports
     * under that name: the measured CPU that exceeded its own clipped gap
     * wall (sum_exec_runtime is only current at a tick or a context switch,
     * so a sub-ms burst can be attributed past its interval). It is OUTSIDE
     * the rows and outside db_time_ms — a diagnostic, not time. Without it a
     * skewed measurement was silently absorbed on this response while
     * time_model counted it.
     *
     * Same name, same per-record arithmetic; not guaranteed to be the same
     * NUMBER on every window. time_model folds a SECOND term into its
     * counter (a negative DB − CPU* − Σwaits residual, compute.c's
     * `if (resid < 0)`), which this per-event table has no analogue for. The
     * two agree where that residual is non-negative, which is what
     * tests/test_window_clip.c 11e asserts on a single-gap window. */
    double offcpu_ms;
    double cpu_clamped_ms;
    int    has_measured_cpu;
};

void pgwt_compute_top_events(const struct pgwt_trace_event *events, int count,
                             const struct pgwt_filter *f,
                             uint64_t from_ns, uint64_t to_ns, double wall_ms,
                             struct pgwt_events_result *out);

/* ── Top Sessions ─────────────────────────────────────────── */

struct pgwt_session_row {
    uint32_t pid;
    double   db_time_ms;
    /* #562: the MEASURED on-CPU share of this session's DB Time, not the
     * on-CPU gaps' wall. The gap between two waits is "on a core" only for
     * the part sum_exec_runtime actually advanced; the rest is runqueue
     * time, reported as offcpu_ms below. Where no in-window gap of this
     * response carried a measurement (sampled / v2 / no CPU capability, and
     * the summary routing) this is the unsplit gap WALL by gap-inference,
     * and pgwt_sessions_result.has_measured_cpu says which of the two it
     * is. */
    double   cpu_pct;
    /* #562: Σ(this session's wait intervals) / DB Time -- the live
     * semantics. It used to be `100 - cpu_pct`, which is ALGEBRAICALLY the
     * same number only while cpu_pct carried the gap wall (DB Time is
     * exactly gaps + waits). Once cpu_pct became the measurement it stopped
     * being the wait share, so this had to move with it. */
    double   wait_pct;
    /* #562: the runqueue / throttle remainder of this session's MEASURED
     * gaps -- their in-window wall minus the measurement, the same quantity
     * pgwt_compute_time_model reports as "CPU (waiting for a core)". It is
     * INSIDE db_time_ms but on neither percentage above, so cpu + wait +
     * offcpu accounts for the row; without it the time the CPU column no
     * longer claims would simply be missing. 0 where has_measured_cpu is 0
     * -- a refusal to split, never a claim that off-CPU time was zero. */
    double   offcpu_ms;
    char     top_wait[64];
    uint32_t top_wait_id;
};

struct pgwt_sessions_result {
    struct pgwt_session_row *rows;   /* malloc'd, caller frees */
    int num_rows;
    /* #327: the per-pid table is unbounded now, so the only early exit is a
     * real allocation failure. When it happens the caller MUST report an
     * error — a session list that is short but looks complete is the defect
     * this field exists to prevent. Set by both the raw and the summary
     * path. */
    int failed;
    /* #562: the same contract pgwt_events_result carries for Top Events
     * (#553/PR #559), field for field rather than a parallel one.
     *
     *   has_measured_cpu != 0 — at least one in-window on-CPU GAP in this
     *       response carried a measurement, so every row's cpu_pct is
     *       measured on-CPU time (part inference on mixed data, where some
     *       gaps are UNKNOWN) and offcpu_ms is the measured gaps' runqueue
     *       remainder. NOT the same predicate as pgwt_tm_result's, whose
     *       scope is the whole window rather than one response; it IS the
     *       narrow one #575 settled on.
     *   has_measured_cpu == 0 — no in-window gap carried a measurement, so
     *       cpu_pct is the unsplit gap wall and every offcpu_ms is 0.
     *
     * pgwt_compute_top_sessions_from_summaries leaves it 0 ON PURPOSE: the
     * per-second record's struct pgwt_summary_session stores `cpu_ns += tns`
     * — WALL (src/summary_writer.c) — and the format has no measured column,
     * so that routing cannot report a measurement and says so. That is a
     * REAL divergence at the 120 s routing threshold, not a comparability
     * artefact, pinned as an identity (raw cpu + raw offcpu == summary cpu)
     * in tests/test_window_clip.c section 13d.
     *
     * cpu_clamped_ms is the skew self-check, same name and meaning as
     * pgwt_tm_result's: measured CPU that exceeded its own clipped gap wall
     * (sum_exec_runtime is only current at a tick or a context switch, so a
     * sub-ms burst can be attributed past its interval). Clamped OUT of the
     * row — it is not time that happened — and counted here, so a skewed
     * measurement is visible instead of silently absorbed. Outside every
     * row and outside db_time_ms. */
    double offcpu_ms;          /* Σ of the rows' offcpu_ms, for the response */
    double cpu_clamped_ms;
    int    has_measured_cpu;
};

void pgwt_compute_top_sessions(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f,
                               uint64_t from_ns, uint64_t to_ns, double wall_ms,
                               struct pgwt_sessions_result *out);

/* ── Top Queries ──────────────────────────────────────────── */

struct pgwt_query_row {
    uint64_t query_id;
    uint64_t count;
    double   total_ms;
    /* #441: `count` counts non-idle wait AND CPU intervals (not pure waits);
     * `avg_us` is the mean FULL duration of the EXACT ones among them, so it
     * is an "average interval", never an average wait and never
     * DB-Time/count. `exact_count` is the denominator that average was
     * actually taken over: 0 means there is no full-duration evidence in this
     * row at all (sampled-only, or the per-second summary path, which carries
     * no full durations) and the latency column is UNAVAILABLE -- emitted as
     * JSON null, exactly as pgwt_event_row.exact_count already gates the
     * Events tab's latency columns. Reading `avg_us` without checking
     * `exact_count` is the defect this field exists to prevent. */
    uint64_t exact_count;
    double   avg_us;
    double   pct_db;
    char     top_wait[64];
    uint32_t top_wait_id;
    double   class_ms[PGWT_NUM_CLASSES];  /* per-class time breakdown */
    /* Per-event breakdown (populated from raw events path) */
    int      num_events;
    uint32_t event_ids[16];
    double   event_ms[16];
};

struct pgwt_queries_result {
    struct pgwt_query_row *rows;   /* malloc'd, caller frees */
    int    num_rows;
    double db_time_ms;
    /* #128/#596: non-idle time without a query (foreground commands that
     * never reported an id, plus background/maintenance work) — reported
     * here instead of being dropped — and how much row time was
     * attributed to an id reported after the interval closed
     * (PGWT_EVENT_FLAG_QUERY_BACKFILL). Raw path only: the summary fast
     * path leaves both 0 (its per-query table has no unattributed bucket;
     * the server marks the response unattributed_available: false). */
    double   unattributed_ms;
    uint64_t unattributed_count;
    double   backfilled_ms;
    /* #327: see pgwt_sessions_result.failed. server.c's handle_top_queries
     * already emits code "compute_failed" when the lifecycle scan fails;
     * this joins that condition rather than adding a second arm. Set by
     * both the raw and the summary path. */
    int      failed;
};

void pgwt_compute_top_queries(const struct pgwt_trace_event *events, int count,
                              const struct pgwt_filter *f,
                              uint64_t from_ns, uint64_t to_ns, double wall_ms,
                              struct pgwt_queries_result *out);

/* ── Heatmap (latency distribution over time) ─────────────── */

struct pgwt_heatmap_result {
    uint64_t *grid;        /* malloc'd [num_buckets * HISTOGRAM_BUCKETS] */
    int      num_buckets;  /* time buckets */
    uint64_t bucket_ns;    /* time bucket width */
    uint64_t *times;       /* malloc'd [num_buckets] start timestamps */
    uint64_t max_count;    /* max cell value (for color scaling) */
    uint64_t total_events; /* total events in heatmap */
};

void pgwt_compute_heatmap(const struct pgwt_trace_event *events, int count,
                          const struct pgwt_filter *f,
                          uint64_t from_ns, uint64_t to_ns, int num_buckets,
                          struct pgwt_heatmap_result *out);

/* ── Summary-based compute (streaming visitor, 1-second records) ── */

void pgwt_compute_aas_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, int num_buckets,
    struct pgwt_aas_result *out);

void pgwt_compute_time_model_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_tm_result *out);

void pgwt_compute_top_events_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_events_result *out);

void pgwt_compute_top_sessions_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_sessions_result *out);

void pgwt_compute_top_queries_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_queries_result *out);

void pgwt_compute_heatmap_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, int num_buckets,
    struct pgwt_heatmap_result *out);

/* ── Transitions ──────────────────────────────────────────── */

struct pgwt_transition_row {
    uint32_t from_event;
    uint32_t to_event;
    char     from_name[64];
    char     to_name[64];
    uint64_t count;
    double   total_ns;       /* total time spent in from_event before this transition */
};

struct pgwt_transitions_result {
    struct pgwt_transition_row *rows;  /* malloc'd, caller frees */
    int    num_rows;
    int    total_rows;                 /* distinct links before max_rows cap */
    uint64_t total_transitions;
    /* #461: see pgwt_sessions_result.failed. The pair table is unbounded
     * now (it was 4096 slots with a probe loop that never terminated once
     * full), so the only early exit is a real allocation failure -- and it
     * withholds the WHOLE graph. A transitions graph quietly missing edges
     * is indistinguishable from a workload that never made those
     * transitions, so a trim is never an option here. When this is set every
     * other field is zeroed; the caller must report, not render. */
    int    failed;
};

/* #436: THE transition population, in one place.
 *
 * A record is in it iff it is an exact (non-sample) record, not admitted by
 * right-edge overlap, passes the uniform filter (markers, impossible
 * durations, class/event/pid/query), and both of its states are visible and
 * real. Returns 1 for in, 0 for out.
 *
 * Exported because the graph's NODE totals and its EDGE counts are two
 * readouts of the same population and used to be selected by two different
 * predicates: pgwt_compute_transitions applied the filter, while the node
 * pass in server.c's handle_transitions went through
 * pgwt_block_agg_node_counts(), which does not take a filter at all. Under a
 * class or event drill the node bars therefore summed records whose edges
 * the same response had excluded -- nodes with no incident edge, and node
 * totals larger than the sum of the edges leaving them, in one picture.
 * Whatever this predicate says, both sides now say the same thing, because
 * there is only one of it. */
int pgwt_transition_record_counts(const struct pgwt_filter *f,
                                  const struct pgwt_trace_event *ev);

/* Compute top state transitions (old_event → new_event).
 * Returns pairs sorted by count descending, limited to max_rows. */
void pgwt_compute_transitions(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f, int max_rows,
                               struct pgwt_transitions_result *out);

/* ── Fingerprint ─────────────────────────────────────────── */

struct pgwt_fingerprint_row {
    uint64_t query_id;
    double   class_pct[PGWT_NUM_CLASSES];  /* % of time per wait class */
    uint64_t total_transitions;
    uint32_t top_from;       /* most frequent transition source */
    uint32_t top_to;         /* most frequent transition target */
    char     signature[128]; /* compact text fingerprint */
};

struct pgwt_fingerprint_result {
    struct pgwt_fingerprint_row *rows;  /* malloc'd, caller frees */
    int    num_rows;
    /* #327: see pgwt_sessions_result.failed. */
    int    failed;
};

/* Compute per-query wait fingerprints: class distribution + top transition. */
void pgwt_compute_fingerprints(const struct pgwt_trace_event *events, int count,
                                const struct pgwt_filter *f,
                                struct pgwt_fingerprint_result *out);

/* ── Concurrency / Burst Detection ────────────────────────── */

/* pids[] is a DISPLAY SAMPLE, not the burst (#276): the first
 * PGWT_BURST_PID_SAMPLE distinct pids in entry order. num_sessions is the
 * EXACT distinct-pid count, so num_pids < num_sessions is the sample bound
 * being visible. Before #276 num_sessions was itself capped at 64, so the
 * count and the list agreed by being equally short, with nothing saying so. */
#define PGWT_BURST_PID_SAMPLE 64

struct pgwt_burst {
    uint64_t timestamp_ns;    /* when the burst started */
    uint32_t event_id;        /* which wait event */
    char     event_name[64];
    int      num_sessions;    /* exact distinct sessions that entered together */
    uint32_t pids[PGWT_BURST_PID_SAMPLE];   /* display sample, see above */
    int      num_pids;        /* <= num_sessions; smaller means sampled */
};

struct pgwt_concurrency_result {
    /* Per-bucket peak concurrency (same layout as AAS buckets). No bound:
     * every event of every bucket of the window is examined (#276). */
    int     *peak_sessions;   /* malloc'd [num_buckets] — max simultaneous waiters */
    uint32_t *peak_event;     /* malloc'd [num_buckets] — event with max concurrency */
    int      num_buckets;
    uint64_t bucket_ns;

    /* Bursts: the LARGEST burst in each bucket that had one, sorted by
     * num_sessions descending (ties by time, then event id — a total order,
     * because qsort is not stable). Selecting by size ALONE would cluster
     * every marker wherever the ties fall; per bucket keeps the returned set
     * representative of the whole window, which is what a time-bucketed view
     * needs (#276). */
    struct pgwt_burst *bursts;  /* malloc'd, caller frees */
    int    num_bursts;          /* buckets that had a burst: <= num_buckets */
    int    bursts_total;        /* burst ONSETS detected in the window */

    /* Allocation failure: the answer is ABSENT, not short. The caller must
     * report it (code "compute_failed") and never serialise the partial
     * result — a short answer that looks complete is #276 itself. */
    int    failed;
};

/* Detect concurrency peaks and burst events.
 * burst_window_ns: time window for burst detection (e.g. 10ms = 10000000).
 * burst_threshold: minimum sessions to count as burst (e.g. 4). */
void pgwt_compute_concurrency(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f,
                               uint64_t from_ns, uint64_t to_ns,
                               int num_buckets,
                               uint64_t burst_window_ns, int burst_threshold,
                               struct pgwt_concurrency_result *out);

/* ── Lock Chains ──────────────────────────────────────────── */

struct pgwt_lock_chain_link {
    uint32_t waiter_pid;
    /* #497: NOT a verified holder. The pid whose on-CPU interval overlapped
     * the wait the most -- an overlapping candidate, blocker unverified. */
    uint32_t blocker_pid;
    uint32_t lock_event;      /* Lock:transactionid, Lock:tuple, etc. */
    char     lock_name[64];
    uint64_t wait_ns;         /* how long the waiter waited */
    uint64_t timestamp_ns;    /* when the wait started */
};

struct pgwt_lock_chains_result {
    struct pgwt_lock_chain_link *links;  /* malloc'd, caller frees */
    int    num_links;
};

/* Detect lock chains: find sessions waiting on Lock events and pair each with
 * the pid whose on-CPU interval overlapped that wait the most.
 *
 * #497 -- WHAT THIS IS NOT. There is no lock tag and no ownership edge here,
 * and the candidate set is on-CPU intervals ONLY, so a holder that is itself
 * waiting, idle in transaction, or a prepared transaction is not merely less
 * likely to be picked: it can never be picked. The result is an OVERLAPPING
 * CANDIDATE with the blocker UNVERIFIED, which is what every caller must say
 * out loud (pgwt-server ships blocker_basis / blocker_note per response).
 * Returns waiter→candidate links sorted by wait_ns descending. */
void pgwt_compute_lock_chains(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f, int max_links,
                               struct pgwt_lock_chains_result *out);

/* ── Interference Scoring ────────────────────────────────── */

struct pgwt_interference_row {
    uint32_t pid_a;
    uint32_t pid_b;
    double   score;        /* 0-1: how much A and B contend on same events */
    uint32_t top_event;    /* most contended event */
    char     top_event_name[64];
    uint64_t overlap_ns;   /* total time both were waiting on same event */
};

struct pgwt_interference_result {
    struct pgwt_interference_row *rows;  /* malloc'd, caller frees */
    int    num_rows;
    int    failed;          /* allocation failure: rows and count are cleared */
};

/* Score cross-session interference: find PID pairs that frequently wait
 * on the same event at the same time. High-scoring pairs are "noisy neighbors". */
void pgwt_compute_interference(const struct pgwt_trace_event *events, int count,
                                const struct pgwt_filter *f, int max_rows,
                                struct pgwt_interference_result *out);

/* ── Execution waterfall / scatter data (U3/B6) ────────────────── */

/* Minimal backend relationship needed by the execution compute.  The server
 * derives this from backends.jsonl and passes it sorted by pid; keeping it
 * separate from display metadata makes the marker/event walk independently
 * testable. */
struct pgwt_backend_link {
    uint32_t pid;
    uint32_t leader_pid;
};

struct pgwt_execution {
    uint32_t pid;
    uint64_t query_id;
    uint64_t start_ns;
    uint64_t end_ns;          /* 0 while the execution is in progress */
    uint64_t plan_start_ns;
    uint64_t plan_end_ns;
    int      has_plan;
    int      in_progress;
    /* #222 review: this row never reached a real EXEC_END -- it was
     * closed at its pid's next CMD_END instead (an ERROR, a cancel,
     * statement_timeout, a client disconnect, or a lost EXEC_END marker).
     * end_ns is still a real wall-clock timestamp (when the command
     * actually closed), not fabricated from a window bound, but it is
     * DEDUCED from the idle transition, not measured at the query's own
     * completion -- meaningful only when in_progress is false. */
    int      end_inferred;
    int      started_before_window;
    int      matches_event_filter;
    int      n_events;
    int      n_workers;
};

struct pgwt_executions_result {
    struct pgwt_execution *rows;  /* malloc'd, caller frees */
    int num_rows;
    int failed;                   /* allocation failure; rows are unusable */
};

/* Extract every execution overlapping [from_ns,to_ns], including an execution
 * whose EXEC_START precedes the window, pair it with a later EXEC_END from the
 * same PID when present, and retain unpaired starts explicitly.
 * PLAN_START/END immediately preceding an execution are associated by PID and
 * query_id. Rows are not filtered or truncated here; callers can serve tables
 * and scatter plots from the same complete, honest extraction.
 *
 * "Unpaired" is bounded, not unbounded: a row still open when its pid's next
 * CMD_END fires (the command closed without a matching EXEC_END -- an
 * ERROR, a cancel, statement_timeout, a lost EXEC_END) is closed there with
 * a real measured duration, never left in_progress for the rest of the
 * capture (#222 review). Only a row with no CMD_END at all before the
 * window's own end -- the pid is still mid-statement right now -- stays
 * in_progress with duration_ms:null. */
void pgwt_compute_executions(const struct pgwt_trace_event *events, int count,
                             uint64_t from_ns, uint64_t to_ns,
                             const struct pgwt_filter *f,
                             const struct pgwt_backend_link *links, int n_links,
                             struct pgwt_executions_result *out);

struct pgwt_exec_event {
    uint32_t event_id;
    char     name[64];
    uint64_t start_ns;        /* #496: CLIPPED to the execution window — the
                               * slice start, which never precedes it */
    uint64_t duration_ns;     /* #496: the slice INSIDE the execution window,
                               * not the event's whole wall time; cpu_ns is
                               * prorated onto the same slice. Both are set in
                               * detail_lane_add() in compute.c */
    uint64_t raw_duration_ns; /* #529: the record's OWN wall time, unclipped.
                               * duration_ns is what a client SUMS (it cannot
                               * exceed the execution's duration); this is
                               * what a client DISPLAYS for one bar. Before
                               * this existed the Waterfall tooltip answered
                               * "how long was this wait?" with the clipped
                               * slice -- a 4 ms wait that opened 2 ms before
                               * EXEC_START read 2 ms -- and the true figure
                               * was on no field and no wire key, so no
                               * client could have told. Serialised as
                               * `raw_dur_ns` (src/server.c
                               * serialize_exec_lane); equal to duration_ns
                               * for every event the boundary does not cut. */
    uint64_t cpu_ns;
    int      has_cpu;
};

struct pgwt_exec_lane {
    uint32_t pid;
    struct pgwt_exec_event *events; /* malloc'd, caller frees */
    int num_events;                 /* retained events */
    int total_events;               /* before per-lane cap */
};

struct pgwt_execution_detail_result {
    struct pgwt_exec_lane leader;
    struct pgwt_exec_lane *workers; /* malloc'd, caller frees each events */
    int num_workers;
    uint64_t query_id;
    uint64_t plan_start_ns;
    uint64_t plan_end_ns;
    int has_plan;
    int total_events;
    int kept_events;
    int found_execution;          /* exact leader EXEC_START matched start_ns */
    int failed;                   /* allocation failure; payload is unusable */
};

/* Build one marker-identified execution waterfall. Only exact transition
 * records are eligible; sampled observations never acquire fabricated
 * interval durations. Events are selected by interval overlap; since #496
 * their start_ns/duration_ns are CLIPPED to the execution window, and since
 * #529 raw_duration_ns carries the record's own wall time beside it. (The
 * previous sentence here still read "keep their raw starts/durations", which
 * #496 had already made false.) */
void pgwt_compute_execution_detail(
    const struct pgwt_trace_event *events, int count,
    uint32_t leader_pid, uint64_t start_ns, uint64_t end_ns,
    const struct pgwt_filter *f,
    const struct pgwt_backend_link *links, int n_links, int max_events_per_lane,
    struct pgwt_execution_detail_result *out);

void pgwt_free_execution_detail(struct pgwt_execution_detail_result *out);

/* ── Variants (per-query execution flow patterns) ─────────── */

/* A step in a compressed flow pattern */
struct pgwt_variant_step {
    uint32_t event_id;
    char     name[64];
    int      is_loop;          /* 1 if this step starts a loop */
    int      loop_len;         /* number of events in the loop body */
};

#define PGWT_MAX_VARIANT_STEPS 32

struct pgwt_variant {
    struct pgwt_variant_step steps[PGWT_MAX_VARIANT_STEPS];
    int      num_steps;
    uint64_t hash;             /* hash of compressed pattern */
    int      exec_count;       /* number of executions matching this pattern */
    int      num_query_ids;    /* distinct query_ids */
    uint64_t total_ns;         /* total wall time across all executions */
    uint64_t avg_ns;           /* average execution time */
    uint64_t p95_ns;           /* p95 execution time, over p95_sample_n samples */
    int      p95_sample_n;     /* executions actually sampled for p95 (#271):
                                * min(exec_count, PGWT_VARIANT_MAX_SAMPLES),
                                * less any sample dropped by a failed realloc.
                                * p95_sample_n < exec_count means the p95 is a
                                * percentile of the FIRST p95_sample_n
                                * executions, not of all of them. */
    double   avg_loop_n;       /* average loop iteration count */
    uint64_t top_query_id;     /* most frequent query_id */
    /* Per-step timing (avg duration per step across executions) */
    uint64_t step_avg_ns[PGWT_MAX_VARIANT_STEPS];
};

/* ── Per-query execution/planning lifecycle (Queries tab) ─────────
 *
 * Extracted from handle_top_queries (#275). It used to carry its own
 * `pid_st[512]` with a linear probe and `if (pi < 0) continue;`, which
 * dropped the 513th distinct pid of the window and every one after it with
 * no flag anywhere, and its own fixed 1024-slot query-id table whose probe
 * loop had no bound (a window with 1024 distinct query ids spun forever).
 * Both are now unbounded hash tables. It lives here so a unit test can call
 * the real code rather than a copy of it.
 */
struct pgwt_qid_lifecycle {
    uint64_t query_id;
    int      used;
    int      exec_count, plan_count;
    double   exec_total_ms, plan_total_ms;
    double  *exec_times, *plan_times;
    int      exec_nsamples, plan_nsamples;  /* entries actually written */
    int      exec_cap, plan_cap;
};

struct pgwt_lifecycle_result {
    struct pgwt_qid_lifecycle *slots;   /* open-addressed, `cap` entries */
    int      cap;                       /* power of two, 0 when unused */
    int      n;                         /* occupied slots */
    int      failed;                    /* allocation failure: the caller
                                           MUST error, never serialise a
                                           short-but-complete-looking answer */
};

/* Scan one loaded window's markers into per-query exec/plan totals and
 * duration samples. Never partial: on allocation failure it sets
 * out->failed and produces nothing. */
void pgwt_compute_query_lifecycle(const struct pgwt_trace_event *events,
                                  int count,
                                  struct pgwt_lifecycle_result *out);

/* The entry for `query_id`, or NULL. Non-const: the caller sorts the
 * duration samples in place to pick percentiles. */
struct pgwt_qid_lifecycle *
pgwt_lifecycle_lookup(struct pgwt_lifecycle_result *r, uint64_t query_id);

void pgwt_lifecycle_free(struct pgwt_lifecycle_result *r);

struct pgwt_variants_result {
    struct pgwt_variant *variants;  /* malloc'd, caller frees */
    int    num_variants;
    int    total_executions;        /* total EXEC_START/END pairs found */
    /* #275: the per-pid state map is unbounded, so the only way phase 1 can
     * stop early is a real allocation failure. When it does, the caller
     * MUST report an error — a short variant list that looks complete is
     * the defect this field exists to prevent. */
    int    failed;
};

/* Phase selectors for variant extraction */
enum pgwt_variant_phase {
    PGWT_PHASE_EXEC = 0,   /* EXEC_START → EXEC_END */
    PGWT_PHASE_PLAN = 1,   /* PLAN_START → PLAN_END */
};

/* Extract per-query flow variants from trace events.
 * phase selects which markers to use as case boundaries.
 * Loop-compresses patterns, groups identical ones, ranks by total time. */
void pgwt_compute_variants(const struct pgwt_trace_event *events, int count,
                            const struct pgwt_filter *f, int max_variants,
                            enum pgwt_variant_phase phase,
                            struct pgwt_variants_result *out);

#endif /* PGWT_COMPUTE_H */
