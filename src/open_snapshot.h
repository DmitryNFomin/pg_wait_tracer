/* open_snapshot.h — the {"cmd":"open_intervals"} seam (#424, live half).
 *
 * THE PROBLEM. A window whose right edge is at/near "now" omits every wait
 * still in progress: an open interval has no closing record, so the trace the
 * server reads cannot contain it. The daemon knows the open state. This is how
 * it is asked, and the FROZEN shape of the answer — the live server half codes
 * against these field names and cannot renegotiate them mid-flight.
 *
 * WHAT IT IS NOT. The snapshot writes NOTHING to the trace. It does not call
 * the de-escalation/shutdown flush: that flush persists a synthetic close
 * WITHOUT advancing BPF state (src/escalation.c), so the eventual real close
 * would overlap time already credited. Only the per-pid arithmetic is shared,
 * through the pure builder in src/open_interval.h. The consumer keeps these
 * spans in a request-scoped overlay read by the TIME reducers only; nothing
 * here is an occurrence, and nothing here is persisted.
 *
 * Likewise it does not make a later REPLAY of the same trace correct — that
 * needs persisted open-interval markers (trace format v4). Nobody may claim
 * three-reader equality for an unclosed interval.
 *
 * DEGRADED IS A STATUS, NEVER A SHORT ANSWER. status/reason are what the UI
 * label is built from. When the snapshot is unavailable or refused, `pids` is
 * ABSENT — not an empty array, not zeros — so the consumer reports the
 * in-flight contribution as absent and labels the closed-record totals as
 * excluding in-flight time. An incomplete total is never advertised as
 * complete. */
#ifndef PGWT_OPEN_SNAPSHOT_H
#define PGWT_OPEN_SNAPSHOT_H

#include "pg_wait_tracer.h"
#include "cJSON.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PGWT_OPEN_INTERVALS_CMD  "open_intervals"

/* Reply cap, bytes. NOT 64 KB: MAX_BACKENDS is 1024 and a row of this shape
 * serialises to ~300-400 bytes, so a full house is ~400 KB. The cap exists so
 * an over-cap reply is REFUSED whole (status unavailable) rather than written
 * half-way and read as a partial answer; it is sized well above the worst
 * legitimate case, not at it. tests/test_open_interval.c measures the real
 * 1024-backend size against this number so the margin cannot silently rot. */
#define PGWT_OPEN_INTERVALS_REPLY_MAX  (1024u * 1024u)

/* OVERALL deadline for the client, milliseconds — not a per-I/O timeout. A
 * wedged daemon must not eat the paint budget, and a 3 s per-I/O timeout is
 * not a bound on a slow fragmented reply: the client has to track one deadline
 * across every poll/read and give up at it with status unavailable. */
#define PGWT_OPEN_INTERVALS_TIMEOUT_MS  500

enum pgwt_open_snapshot_status {
    PGWT_OPEN_SNAP_INCLUDED = 0,   /* pids present and trustworthy */
    PGWT_OPEN_SNAP_UNAVAILABLE,    /* the daemon could not answer (transient) */
    PGWT_OPEN_SNAP_REFUSED,        /* this tier has no watchpoint intervals */
};

static inline const char *
pgwt_open_snapshot_status_str(enum pgwt_open_snapshot_status s)
{
    switch (s) {
    case PGWT_OPEN_SNAP_INCLUDED:    return "included";
    case PGWT_OPEN_SNAP_REFUSED:     return "refused";
    case PGWT_OPEN_SNAP_UNAVAILABLE: break;
    }
    return "unavailable";
}

/* One live backend's open stretch. */
struct pgwt_open_row {
    uint32_t pid;
    uint64_t incarnation;     /* THIS backend incarnation's watchpoint attach
                               * instant (mono). A bare pid is not an identity:
                               * pid reuse is real. */
    uint64_t generation;      /* exact generation the interval belongs to */
    uint32_t event_id;        /* raw wait_event_info; 0 = on-CPU */
    uint32_t cat_flag;        /* 0 foreground, or PGWT_EVENT_FLAG_{IO_WORKER,
                               * MAINT,BACKGROUND} */
    uint32_t eff_event;       /* the daemon's own live classification */
    uint32_t flags;           /* PGWT_EVENT_FLAG_* as the builder set them */
    uint64_t open_start_ns;   /* BPF last_ts (mono) — the stretch's start */
    uint64_t duration_ns;     /* snapshot_ns - open_start_ns, always > 0 */
    uint64_t cpu_ns;          /* MEASURED CPU of this stretch, or
                               * PGWT_CPU_NS_UNKNOWN (serialised as ABSENT) */
    uint64_t cpu_ns_total;    /* the raw S3 inputs, so the consumer can redo */
    uint64_t on_cpu_ts;       /* the same arithmetic rather than re-derive it */
    uint64_t last_cpu_ns;
    uint64_t query_id;
    uint64_t cmd_open_ns;     /* command-open ns inside [open_start, snapshot] */
    bool     cmd_open;        /* INSTANTANEOUS gate — never classify on this
                               * alone; it misclassifies (the gate is already
                               * clear at the ClientRead that ends a waitless
                               * statement whose whole run was in-command) */
    bool     cmd_in_window;   /* the majority rule's verdict for the stretch */
    bool     cmd_by_markers;  /* true: markers decided it. false: fell back */
    bool     query_synth;     /* PG13 synthetic query id */
};

struct pgwt_open_snapshot {
    enum pgwt_open_snapshot_status status;
    char     reason[128];          /* specific enough to render as a label */
    bool     complete;             /* false ⇒ the answer is partial/degraded */
    uint64_t snapshot_ns;          /* mono instant the state map was read */
    uint64_t snapshot_wall_ns;     /* CLOCK_REALTIME at that instant */
    uint64_t now_mono_ns;          /* (now_mono, now_wall) anchor — the ONLY */
    uint64_t now_wall_ns;          /* sanctioned mono→wall conversion here */
    uint64_t committed_through_ns; /* mono: every closed record at or before
                                    * this is published to the trace. A gap
                                    * below it is "still open", not "closed but
                                    * not yet visible" */
    uint64_t daemon_generation;    /* exact generation (0 = none) */
    uint64_t daemon_start_wall_ns; /* + pid: clock identity across a restart */
    uint32_t daemon_pid;
    char     tier[16];             /* "escalated" | "sampled" | "lightweight" */
    bool     cpu_accounting;
    bool     cmd_gate_active;
    int      open_count;           /* rows found (== n_rows unless degraded) */
    int      n_rows;
    struct pgwt_open_row rows[MAX_BACKENDS];
};

static inline void pgwt_open_snapshot_set_reason(struct pgwt_open_snapshot *s,
                                                 const char *reason)
{
    snprintf(s->reason, sizeof(s->reason), "%s", reason ? reason : "");
}

/* Mono → wall through the snapshot's own anchor (never a per-file offset).
 * Returns 0 when the anchor is unusable or the result would go negative. */
static inline uint64_t
pgwt_open_snapshot_wall_of(const struct pgwt_open_snapshot *s, uint64_t mono)
{
    if (s->now_mono_ns == 0 || s->now_wall_ns == 0)
        return 0;
    int64_t off = (int64_t)s->now_wall_ns - (int64_t)s->now_mono_ns;
    int64_t w = (int64_t)mono + off;
    return w > 0 ? (uint64_t)w : 0;
}

static inline void pgwt_json_u64(cJSON *o, const char *k, uint64_t v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    cJSON_AddRawToObject(o, k, buf);
}

/* Serialise one snapshot. PURE: no daemon, no BPF, no socket — so the exact
 * wire shape is unit-testable, which is the only reason the field names can be
 * frozen honestly.
 *
 * `reply_max` is the byte cap (PGWT_OPEN_INTERVALS_REPLY_MAX in production; a
 * test passes a small value to reach the over-cap path). An over-cap reply is
 * replaced WHOLE by status unavailable + reason — never truncated, because a
 * truncated JSON reply read by a consumer is indistinguishable from a short
 * honest one until it fails to parse, and a partial answer that DOES parse is
 * worse still. */
static inline cJSON *pgwt_open_snapshot_to_json(
    const struct pgwt_open_snapshot *s, size_t reply_max)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;
    /* ok reports that the daemon ANSWERED; status reports whether the answer
     * is usable. A daemon too old to know the command replies
     * {"ok":false,"error":"unknown command"} — the consumer maps that to
     * unavailable itself. */
    cJSON_AddBoolToObject(root, "ok", s->status == PGWT_OPEN_SNAP_INCLUDED);
    cJSON_AddStringToObject(root, "cmd", PGWT_OPEN_INTERVALS_CMD);
    cJSON_AddStringToObject(root, "status",
                            pgwt_open_snapshot_status_str(s->status));
    cJSON_AddStringToObject(root, "reason", s->reason);
    cJSON_AddBoolToObject(root, "complete", s->complete);
    pgwt_json_u64(root, "snapshot_ns", s->snapshot_ns);
    pgwt_json_u64(root, "snapshot_wall_ns", s->snapshot_wall_ns);
    pgwt_json_u64(root, "now_mono_ns", s->now_mono_ns);
    pgwt_json_u64(root, "now_wall_ns", s->now_wall_ns);
    pgwt_json_u64(root, "committed_through_ns", s->committed_through_ns);
    pgwt_json_u64(root, "committed_through_wall_ns",
                  pgwt_open_snapshot_wall_of(s, s->committed_through_ns));
    pgwt_json_u64(root, "daemon_generation", s->daemon_generation);
    pgwt_json_u64(root, "daemon_start_wall_ns", s->daemon_start_wall_ns);
    pgwt_json_u64(root, "daemon_pid", s->daemon_pid);
    cJSON_AddStringToObject(root, "tier", s->tier);
    cJSON_AddBoolToObject(root, "cpu_accounting", s->cpu_accounting);
    cJSON_AddBoolToObject(root, "cmd_gate_active", s->cmd_gate_active);
    cJSON_AddNumberToObject(root, "open_count", s->open_count);

    /* pids is present ONLY for a trustworthy answer. Absent beats empty: an
     * empty array says "nothing is open", which is a measurement. */
    if (s->status == PGWT_OPEN_SNAP_INCLUDED) {
        cJSON *arr = cJSON_AddArrayToObject(root, "pids");
        if (!arr) {
            cJSON_Delete(root);
            return NULL;
        }
        for (int i = 0; i < s->n_rows; i++) {
            const struct pgwt_open_row *r = &s->rows[i];
            cJSON *o = cJSON_CreateObject();
            if (!o) {
                cJSON_Delete(root);
                return NULL;
            }
            cJSON_AddItemToArray(arr, o);
            pgwt_json_u64(o, "pid", r->pid);
            pgwt_json_u64(o, "incarnation", r->incarnation);
            pgwt_json_u64(o, "generation", r->generation);
            pgwt_json_u64(o, "event_id", r->event_id);
            pgwt_json_u64(o, "cat_flag", r->cat_flag);
            pgwt_json_u64(o, "eff_event", r->eff_event);
            pgwt_json_u64(o, "flags", r->flags);
            pgwt_json_u64(o, "open_start_ns", r->open_start_ns);
            pgwt_json_u64(o, "duration_ns", r->duration_ns);
            /* UNKNOWN is ABSENT, never 0: an unmeasured CPU stretch and a
             * measured-zero one are different facts (#318/#387). */
            if (r->cpu_ns != PGWT_CPU_NS_UNKNOWN)
                pgwt_json_u64(o, "cpu_ns", r->cpu_ns);
            pgwt_json_u64(o, "cpu_ns_total", r->cpu_ns_total);
            pgwt_json_u64(o, "on_cpu_ts", r->on_cpu_ts);
            pgwt_json_u64(o, "last_cpu_ns", r->last_cpu_ns);
            pgwt_json_u64(o, "query_id", r->query_id);
            pgwt_json_u64(o, "cmd_open_ns", r->cmd_open_ns);
            cJSON_AddBoolToObject(o, "cmd_open", r->cmd_open);
            cJSON_AddBoolToObject(o, "cmd_in_window", r->cmd_in_window);
            cJSON_AddBoolToObject(o, "cmd_by_markers", r->cmd_by_markers);
            cJSON_AddBoolToObject(o, "query_synth", r->query_synth);
        }
    }

    /* Measure the real wire bytes. cJSON gives no cheap size estimate, so the
     * check is on the printed string the sender would queue — the only number
     * that cannot be wrong by an accounting mistake. */
    char *probe = cJSON_PrintUnformatted(root);
    if (!probe) {
        cJSON_Delete(root);
        return NULL;
    }
    size_t len = strlen(probe);
    free(probe);
    if (len + 1 <= reply_max)
        return root;

    cJSON_Delete(root);
    struct pgwt_open_snapshot cut = *s;
    cut.status = PGWT_OPEN_SNAP_UNAVAILABLE;
    cut.complete = false;
    cut.n_rows = 0;
    snprintf(cut.reason, sizeof(cut.reason),
             "reply too large: %d open intervals serialise to %zu bytes, "
             "cap %zu", s->open_count, len + 1, reply_max);
    /* One level of recursion at most: the rows are gone, so the replacement
     * cannot itself be over-cap unless the cap is absurdly small, in which
     * case this returns NULL and the caller reports a send failure. */
    if (cut.status == s->status)
        return NULL;
    return pgwt_open_snapshot_to_json(&cut, reply_max);
}

#endif /* PGWT_OPEN_SNAPSHOT_H */
