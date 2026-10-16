/* open_overlay.h — request-scoped OPEN-INTERVAL OVERLAY (#424, live half)
 *
 * A window whose right edge is at/near "now" omits every wait still in
 * progress: an open interval has no closing record, so the trace the server
 * reads simply does not contain it. The daemon knows the open state. This
 * module is the SERVER-SIDE client of {"cmd":"open_intervals"}: it asks,
 * validates the answer, and turns it into a small array of time-only spans.
 *
 * THE DESIGN (contract REVISION 3 §A). The spans live HERE, in a separate,
 * request-scoped overlay — they are NOT injected into the common raw event
 * array and they carry no "synthetic" flag that ~20 consumers would each have
 * to honour. Only the shared TIME reducers (AAS, time_model, sessions,
 * queries-time and the deliberately named span consumers) are given the
 * overlay. The occurrence and lifecycle streams (counts, histograms, max_ns,
 * percentiles, transitions, scatter, executions, fingerprints, timeline
 * record counts) never receive it, so they cannot inflate — safe by
 * construction rather than by auditing every consumer.
 *
 * WHAT IS STABLE AND WHAT IS NOT (contract REVISION 3 §C). A span's WAIT WALL
 * (to_wall_ns - from_wall_ns) is stable: when the interval finally closes, the
 * clipped real record covers exactly the same stretch. Its CPU, its command
 * classification and its query attribution are PROVISIONAL — an open CPU
 * stretch can read 10 CPU-seconds before closure and 5 after, and the command
 * majority can flip (src/compute.c:486-496). Do not assert post-close equality
 * for anything but the wait wall.
 *
 * ROUTE AND TIER. RAW route only: summary v8 already credits open waits per
 * sealed second, so an overlay on the summary route would double count. EXACT
 * tier only: the sampled and lightweight tiers have no watchpoint intervals to
 * report. Both are refused with an explicit reason, and the refusal is
 * enforced in pgwt_overlay_build() even if a valid snapshot is handed in.
 *
 * ABSENCE, NEVER A SILENT ZERO (contract REVISION 3 §E). If the daemon is
 * missing, slow, truncated, inconsistent or reports complete:false, the
 * overlay status is `unavailable`/`refused` with a reason, the in-flight
 * contribution is ABSENT (no numeric field at all — not zero), and the closed
 * record totals are still served, explicitly marked as excluding in-flight
 * time. pgwt_overlay_add_json() is the single place that renders that, so no
 * handler can accidentally advertise an incomplete total as complete.
 *
 * ──────────────────────────────────────────────────────────────────────────
 * THE WIRE FORMAT this module parses. The producer is `src/open_snapshot.h`
 * (daemon half); the field names and TYPES below are that file's, read off its
 * serialiser rather than off a description of it.
 *
 *   {
 *     "ok": <bool>, "cmd": "open_intervals",
 *     "status":  "included" | "unavailable" | "refused",   \ the invariant:
 *     "complete": <bool>,                                  | pids present ==
 *     "reason":  <string>,                                 / included == complete
 *     "snapshot_ns":               <u64 number, CLOCK_MONOTONIC: the observation>,
 *     "snapshot_wall_ns":          <u64 STRING>,
 *     "now_mono_ns":               <u64 number>,   \ the anchor pair — the ONLY
 *     "now_wall_ns":               <u64 STRING>,   / sanctioned mono→wall here
 *     "committed_through_ns":      <u64 number>,
 *     "committed_through_wall_ns": <u64 STRING>,
 *     "daemon_generation":         <u64 number>,
 *     "daemon_start_wall_ns":      <u64 STRING>,  "daemon_pid": <u32 number>,
 *     "tier":  "escalated" | "sampled" | "lightweight",
 *     "cpu_accounting": <bool>, "cmd_gate_active": <bool>,
 *     "open_count": <int>,
 *     "pids": [ {                       // present ONLY when status=included
 *         "pid": <u32>, "incarnation": <u64>, "generation": <u64>,
 *         "event_id": <u32>, "cat_flag": <u32 PGWT_EVENT_FLAG_{IO_WORKER,MAINT,
 *                                         BACKGROUND}, 0 = foreground>,
 *         "category": <string, pgwt_class_name(event_id) — never empty>,
 *         "category_id": <u32 WE_CLASS(event_id)>,
 *         "open_start_ns": <u64>, "duration_ns": <u64>,
 *         "cpu_ns": <u64, ABSENT when not measured — never 0>,
 *         "cpu_ns_total": <u64>, "on_cpu_ts": <u64>, "last_cpu_ns": <u64>,
 *         "query_id": <u64>, "query_quality": <u16>,
 *         "cmd_open_ns": <u64>, "cmd_banked_ns": <u64>, "cmd_anchor_ns": <u64>,
 *         "cmd_open": <bool>, "cmd_sweep_open": <bool>, "cmd_seen": <bool>,
 *         "cmd_gate_present": <bool>, "cmd_in_window": <bool>,
 *         "cmd_by_markers": <bool>, "query_synth": <bool>
 *     }, ... ]
 *   }
 *
 * Every *_ns value is accepted as a JSON number OR a decimal string, whichever
 * the daemon sends. Wall-clock ns MUST be strings: at ~1.76e18 a double's ulp
 * is 256 ns, so a number-typed wall stamp is already wrong at the parser.
 * Monotonic fields are numbers today and exact only below 2^53 ns (~104 days
 * of uptime, which a long-lived box reaches); this parser does not assume
 * numeric, so the producer can move them to strings without a client change.
 *
 * `cmd_open` is BPF's INSTANTANEOUS uprobe gate; `cmd_sweep_open` is the
 * marker sweep's flag, and they disagree exactly in the #98 case. The
 * banked/anchor arithmetic therefore uses cmd_sweep_open, and cmd_open is only
 * the fallback for a pid with no sweep state. See overlay_in_command() in the
 * .c for the three-step reduction — two of whose steps are RULES, not
 * derivations; B's first draft omitted them and differed from the real
 * classifier in 59 of 96 gate states.
 *
 * ──────────────────────────────────────────────────────────────────────────
 * HOW A HANDLER USES IT (the integration seam — src/server.c owns the calls):
 *
 *   struct pgwt_overlay ov;  pgwt_overlay_init(&ov);          // fail-closed
 *   struct pgwt_overlay_request r = { from_wall, to_wall, route, tier, 0 };
 *   if (pgwt_overlay_wanted(&r, NULL, 0)) {
 *       struct pgwt_open_snapshot snap;
 *       pgwt_overlay_fetch(trace_dir, PGWT_OVERLAY_TIMEOUT_MS, &snap);
 *       struct pgwt_overlay_records rec = { evs, n_evs, canon_off, NULL, NULL };
 *       pgwt_overlay_build(&ov, &snap, &r, &rec);
 *       pgwt_open_snapshot_free(&snap);
 *   } else {
 *       pgwt_overlay_refuse(&ov, "<the reason wanted() gave>");
 *   }
 *   ... time reducers consume pgwt_overlay_spans(&ov) ...
 *   pgwt_overlay_add_json(&ov, root);
 *   pgwt_overlay_free(&ov);
 *
 * pgwt_overlay_build() takes the ADMITTED RECORD ARRAY itself — the very array
 * the reducers are about to consume — rather than a count the caller feeds in
 * separately. That is deliberate: the precedence rule (drop a pid's span when
 * any admitted record for that pid closes past the open start) cannot be
 * defeated by a forgotten call site, because there is no second feed to
 * forget.
 */
#ifndef PGWT_OPEN_OVERLAY_H
#define PGWT_OPEN_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

#include "pg_wait_tracer.h"

struct cJSON;

/* A wedged daemon must not eat the 3000 ms paint budget. This is an OVERALL
 * deadline across connect/send/read, not a per-I/O timeout: a daemon that
 * drip-feeds one byte at a time cannot extend it (contract REVISION 2 §6). */
#define PGWT_OVERLAY_TIMEOUT_MS     500

/* MAX_BACKENDS is 1024 and each entry is ~200 bytes of JSON, so the reply is
 * ~200 KB at full occupancy: 64 KB is not enough (contract REVISION 3 §D).
 * The reader grows its buffer as the reply arrives and refuses — as a
 * truncation, never as a partial answer — only past this cap. */
#define PGWT_OVERLAY_MAX_REPLY      (4u * 1024u * 1024u)

enum pgwt_overlay_status {
    PGWT_OVERLAY_INCLUDED = 0,    /* spans are present and complete */
    PGWT_OVERLAY_UNAVAILABLE,     /* the daemon could not be asked or answered badly */
    PGWT_OVERLAY_REFUSED          /* this route/tier must not be overlaid */
};

enum pgwt_overlay_route { PGWT_OVERLAY_ROUTE_RAW = 0, PGWT_OVERLAY_ROUTE_SUMMARY };

enum pgwt_overlay_tier {
    PGWT_OVERLAY_TIER_EXACT = 0,
    PGWT_OVERLAY_TIER_SAMPLED,
    PGWT_OVERLAY_TIER_LIGHTWEIGHT
};

/* One open interval exactly as the daemon reported it (MONOTONIC domain). */
struct pgwt_open_interval {
    uint32_t pid;
    uint64_t incarnation;
    uint64_t generation;
    uint32_t event_id;
    uint32_t cat_flag;            /* process type: 0 foreground, or the
                                   * IO_WORKER / MAINT / BACKGROUND flag */
    uint32_t category_id;         /* wait class id */
    char     category[16];        /* wait class name, never empty */
    uint64_t open_start_ns;
    uint64_t cpu_ns_total;
    uint64_t on_cpu_ts;
    uint64_t last_cpu_ns;
    uint64_t cpu_ns;              /* the daemon's own figure, or
                                   * PGWT_CPU_NS_UNKNOWN when absent */
    uint64_t cmd_banked_ns;
    uint64_t cmd_anchor_ns;
    uint64_t query_id;
    uint16_t query_quality;
    uint8_t  cmd_open;            /* instantaneous BPF gate — never classify
                                   * on this alone */
    uint8_t  cmd_sweep_open;      /* the marker sweep's flag — this is the one
                                   * the banked/anchor arithmetic uses */
    uint8_t  cmd_seen;
    uint8_t  cmd_gate_present;
    uint8_t  cmd_in_window;       /* the DAEMON's verdict, cross-checked
                                   * against our own reduction */
    uint8_t  in_command;          /* our reduction; == cmd_in_window or the
                                   * reply is refused */
};

/* A validated daemon reply. status is ALWAYS set, including on every error
 * path, so a caller that ignores the return value still cannot read an
 * uninitialised verdict. */
struct pgwt_open_snapshot {
    enum pgwt_overlay_status status;
    char     reason[192];
    uint64_t snapshot_ns;            /* monotonic */
    uint64_t snapshot_wall_ns;       /* the daemon's own, verified == anchored */
    uint64_t now_mono_ns;
    uint64_t now_wall_ns;
    int64_t  anchor_offset_ns;       /* now_wall_ns - now_mono_ns; wall = mono + offset */
    uint64_t committed_through_ns;   /* monotonic */
    uint64_t committed_through_wall_ns;
    uint64_t daemon_generation;
    uint64_t daemon_start_wall_ns;   /* + pid: clock identity across a restart */
    uint32_t daemon_pid;
    char     tier[16];
    int      cpu_accounting;
    int      cmd_gate_active;
    int      complete;
    size_t   count;
    struct pgwt_open_interval *items;
};

/* One synthesized span, WALL domain, already clipped to the window and to the
 * snapshot. wall_ns is stable; cpu_ns / in_command / query_id are provisional. */
struct pgwt_overlay_span {
    uint32_t pid;
    uint64_t incarnation;
    uint32_t event_id;
    uint32_t cat_flag;
    uint32_t category_id;
    char     category[16];           /* the daemon's resolved wait class */
    uint64_t query_id;
    uint16_t query_quality;
    int      in_command;             /* majority rule over this span — PROVISIONAL */
    uint64_t from_wall_ns;           /* max(open_start, window from) */
    uint64_t to_wall_ns;             /* min(window to, snapshot) — never beyond */
    uint64_t wall_ns;                /* to_wall_ns - from_wall_ns, always > 0 */
    uint64_t cpu_ns;                 /* prorated; PGWT_CPU_NS_UNKNOWN when unmeasured */
    uint64_t stretch_from_wall_ns;   /* the unclipped open stretch, for provenance */
    uint64_t stretch_to_wall_ns;
};

struct pgwt_overlay {
    enum pgwt_overlay_status status;
    char     reason[192];
    uint64_t snapshot_wall_ns;
    uint64_t committed_through_wall_ns;
    uint64_t daemon_generation;
    struct pgwt_overlay_span *spans;
    size_t   count;
};

struct pgwt_overlay_request {
    uint64_t from_wall_ns;
    uint64_t to_wall_ns;
    enum pgwt_overlay_route route;
    enum pgwt_overlay_tier  tier;
    uint64_t expect_generation;      /* 0 = accept any daemon generation */

    /* FILTERS. A filtered window must not receive unfiltered in-flight time:
     * that would inflate exactly the view a user narrowed down. So the two
     * fields below are a contract, not a convenience — when filters_active is
     * set, accept() is MANDATORY and pgwt_overlay_build() refuses without it.
     * Forgetting to wire the filter in therefore degrades the pane visibly
     * instead of quietly adding time that the filter excluded. */
    int   filters_active;
    int (*accept)(const struct pgwt_overlay_span *span, void *user);
    void *accept_user;
};

/* The admitted record array — the one the time reducers are about to read.
 * wall_of() is for multi-file windows whose segments carry different canonical
 * offsets; when it is NULL, offset_ns is applied to every record. */
struct pgwt_overlay_records {
    const struct pgwt_trace_event *events;
    size_t   n;
    int64_t  offset_ns;
    uint64_t (*wall_of)(const struct pgwt_trace_event *ev, void *user);
    void    *user;
};

/* Fail-closed: status = UNAVAILABLE, reason "overlay not requested". A caller
 * that forgets everything else still renders an absent in-flight total. */
void pgwt_overlay_init(struct pgwt_overlay *ov);
void pgwt_overlay_free(struct pgwt_overlay *ov);

/* Returns 1 when this request may carry an overlay at all (raw route, exact
 * tier, positive window). Otherwise 0, with the reason written to reason[] when
 * reason is non-NULL. Call it before touching the socket. */
int  pgwt_overlay_wanted(const struct pgwt_overlay_request *req,
                         char *reason, size_t reason_size);

void pgwt_overlay_refuse(struct pgwt_overlay *ov, const char *reason);
void pgwt_overlay_unavailable(struct pgwt_overlay *ov, const char *reason);

/* Ask the daemon at {trace_dir}/pgwt.sock. Always fills snap (status + reason);
 * returns 0 when snap->status == INCLUDED, -1 otherwise. timeout_ms is the
 * OVERALL deadline. */
int  pgwt_overlay_fetch(const char *trace_dir, int timeout_ms,
                        struct pgwt_open_snapshot *snap);

/* Parse + validate one reply. Pure; no I/O. Always fills snap. */
int  pgwt_overlay_parse(const char *json, struct pgwt_open_snapshot *snap);
void pgwt_open_snapshot_free(struct pgwt_open_snapshot *snap);

/* Build the request-scoped overlay. Pure. ov must have been through
 * pgwt_overlay_init() first (build frees whatever spans it holds). Enforces,
 * in this order:
 *   - route/tier refusal (even when snap is a perfectly good INCLUDED reply),
 *   - snapshot status and daemon generation,
 *   - the window must actually be covered by the observation: a window that
 *     starts at or after the snapshot is UNKNOWN territory, not empty, and is
 *     refused rather than returned as an included overlay with no spans. (The
 *     other side — a `to` past the snapshot — is clipped, and the response's
 *     snapshot_wall_ns is what tells a consumer where knowledge ends.)
 *   - a filtered request with no accept() callback,
 *   - committed_through coverage of every span's open start,
 *   - the uniform precedence rule: drop a pid's span when ANY admitted
 *     non-marker record for that pid has ts > open_start — that record IS the
 *     close of the same interval. This is what makes a historical window
 *     produce an empty overlay with no "near now" heuristic anywhere,
 *   - the clip: span = [max(open_start, from), min(to, snapshot)].
 * Always sets ov->status. Returns 0 when ov->status == INCLUDED. */
int  pgwt_overlay_build(struct pgwt_overlay *ov,
                        const struct pgwt_open_snapshot *snap,
                        const struct pgwt_overlay_request *req,
                        const struct pgwt_overlay_records *rec);

/* THE LOADER-LEVEL ENTRY POINT — wanted → fetch → build, in one call.
 *
 * Attach the overlay ONCE where the raw window is loaded, and let every
 * raw-route consumer inherit it. Attaching per endpoint is the trap: it would
 * correct `time_model` while Sessions and Top Events — which take the same raw
 * route below 120 s — stayed silently short, i.e. the headline number right
 * and the drill-downs wrong, which is the shape of the bug being repaired.
 *
 * The route/tier refusal is evaluated BEFORE any I/O, so a summary-route or
 * non-exact-tier request never opens the socket; a wedged daemon cannot cost
 * those requests a millisecond. Always sets ov->status (ov is initialised
 * here); returns 0 only when the result is INCLUDED. */
int pgwt_overlay_attach(struct pgwt_overlay *ov, const char *trace_dir,
                        int timeout_ms,
                        const struct pgwt_overlay_request *req,
                        const struct pgwt_overlay_records *rec);

/* ns of this span inside [from, to) — for AAS buckets and any sub-window. */
uint64_t pgwt_overlay_span_overlap_ns(const struct pgwt_overlay_span *s,
                                      uint64_t from_wall_ns, uint64_t to_wall_ns);
/* Measured CPU of this span inside [from, to), prorated across the span.
 * PGWT_CPU_NS_UNKNOWN propagates — absence is never rendered as zero. */
uint64_t pgwt_overlay_span_cpu_ns(const struct pgwt_overlay_span *s,
                                  uint64_t from_wall_ns, uint64_t to_wall_ns);

/* Total in-flight wall ns the overlay contributes; 0 spans means 0. Only
 * meaningful when status == INCLUDED — the JSON emitter below is the one that
 * decides whether it may be shown at all. */
uint64_t pgwt_overlay_total_wall_ns(const struct pgwt_overlay *ov);

const char *pgwt_overlay_status_name(enum pgwt_overlay_status st);

/* Render the pane's status object onto a response:
 *   included    → {"status":"included","snapshot_wall_ns":N,"pids":K,
 *                  "in_flight_ns":T}
 *   otherwise   → {"status":"unavailable"|"refused","reason":"…",
 *                  "excludes_in_flight":true}  — with NO numeric in-flight
 *                  field present at all. Absent, never zero. */
void pgwt_overlay_add_json(const struct pgwt_overlay *ov, struct cJSON *root);

#endif /* PGWT_OPEN_OVERLAY_H */
