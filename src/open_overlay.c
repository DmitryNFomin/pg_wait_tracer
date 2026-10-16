/* open_overlay.c — request-scoped open-interval overlay (#424, live half).
 * See src/open_overlay.h for the design, the wire format and the integration
 * seam. Everything below is either pure (parse/build/arithmetic) or the one
 * bounded socket round-trip; nothing here touches the common raw event array.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
/* The daemon half's shared builder. pgwt_open_stretch_cpu_ns() is the ONE
 * open-stretch CPU arithmetic, used by the de-escalation flush, the shutdown
 * flush and this consumer — a second copy here is exactly the drift the
 * header was created to remove. */
#include "open_interval.h"
#include "open_overlay.h"

/* ── small helpers ─────────────────────────────────────────── */

static void set_reason(char *dst, size_t cap, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

static void set_reason(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
}

static long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* wall = mono + offset, refusing a result that would underflow past zero. */
static int mono_to_wall(uint64_t mono, int64_t offset, uint64_t *out)
{
    int64_t w = (int64_t)mono + offset;
    if (w <= 0)
        return -1;
    *out = (uint64_t)w;
    return 0;
}

const char *pgwt_overlay_status_name(enum pgwt_overlay_status st)
{
    switch (st) {
    case PGWT_OVERLAY_INCLUDED:    return "included";
    case PGWT_OVERLAY_REFUSED:     return "refused";
    case PGWT_OVERLAY_UNAVAILABLE: break;
    }
    return "unavailable";
}

/* ── JSON field readers ────────────────────────────────────── */

/* A u64 arrives either as a JSON number or as a decimal string. Wall-clock
 * nanoseconds are ~1.8e18 and a double carries 53 bits, so a number loses up
 * to ~256 ns there; strings are exact and preferred. Rejects negatives,
 * non-integers, empty/garbage strings and anything past UINT64_MAX. */
static int json_u64(const cJSON *obj, const char *key, uint64_t *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it)
        return -1;
    if (cJSON_IsNumber(it)) {
        double d = it->valuedouble;
        if (!(d >= 0.0) || d >= 18446744073709551616.0)
            return -1;
        *out = (uint64_t)d;
        return 0;
    }
    if (cJSON_IsString(it) && it->valuestring) {
        const char *s = it->valuestring;
        while (*s == ' ')
            s++;
        if (*s < '0' || *s > '9')
            return -1;
        errno = 0;
        char *end = NULL;
        unsigned long long v = strtoull(s, &end, 10);
        if (errno == ERANGE || !end || *end != '\0')
            return -1;
        *out = (uint64_t)v;
        return 0;
    }
    return -1;
}

static int json_u32(const cJSON *obj, const char *key, uint32_t *out)
{
    uint64_t v;
    if (json_u64(obj, key, &v) != 0 || v > 0xFFFFFFFFull)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int json_u16(const cJSON *obj, const char *key, uint16_t *out)
{
    uint64_t v;
    if (json_u64(obj, key, &v) != 0 || v > 0xFFFFull)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

/* The daemon emits the gate flags as JSON booleans (cJSON_AddBoolToObject).
 * A 0/1 number is accepted too, so a producer that switches representation
 * does not take the pane down — but a string, null or absence is a missing
 * field, not a false. */
static int json_bool(const cJSON *obj, const char *key, uint8_t *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it)
        return -1;
    if (cJSON_IsBool(it)) {
        *out = cJSON_IsTrue(it) ? 1 : 0;
        return 0;
    }
    if (cJSON_IsNumber(it) && (it->valuedouble == 0.0 || it->valuedouble == 1.0)) {
        *out = it->valuedouble != 0.0;
        return 0;
    }
    return -1;
}

/* A required, NON-EMPTY string. An empty string is treated as missing: the
 * producer refuses a row whose wait category it could not resolve rather than
 * shipping "", and a consumer that accepted "" would turn that contract back
 * into a guess. */
static int json_str(const cJSON *obj, const char *key, char *out, size_t cap)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(it) || !it->valuestring || !it->valuestring[0])
        return -1;
    snprintf(out, cap, "%s", it->valuestring);
    return 0;
}

/* The command classification for an open stretch [t0, t1].
 *
 * THREE STEPS, and two of them are RULES rather than consequences of the
 * majority rule — do not re-derive this from first principles. The daemon
 * half's first draft had only the middle step and differed from the real
 * pgwt_live_cmd_gate_classify in 59 of 96 gate states; with the two
 * short-circuits and the clamp it differs in 0 of 96.
 *
 *   1. no sweep state for this pid  → the emission-time gate `cmd_open`,
 *      exactly as the closed-record path does;
 *   2. the pid has no CMD markers at all → IN-COMMAND (the server's seen_cmd
 *      rule: an unmarked pid's CPU stays CPU);
 *   3. otherwise the majority rule over banked + the still-open run, CLAMPED
 *      to the stretch — banked may predate t0, because state_map's last_ts
 *      can run ahead of the last drained record, and the unclamped sum then
 *      exceeds the stretch it is supposed to describe.
 *
 * `cmd_sweep_open`, never `cmd_open`: the first is the marker sweep's flag,
 * the second BPF's instantaneous uprobe gate, and they disagree exactly in
 * the #98 case the sweep exists for. PROVISIONAL either way — the majority
 * can flip when the interval finally closes. */
/* Step 3's quantity on its own: command-open ns inside [t0, t1]. This is the
 * daemon's `cmd_open_ns`, and the parser cross-checks the two — which is also
 * what makes the CLAMP observable. The clamp cannot change the majority
 * verdict (min(open, wall) >= wall/2 whenever open >= wall/2), so a test that
 * only looked at the verdict could never see it removed. Here it can. */
static uint64_t overlay_cmd_open_ns(const struct pgwt_open_interval *it,
                                    uint64_t t0, uint64_t t1)
{
    if (!it->cmd_gate_present || !it->cmd_seen)
        return 0;
    uint64_t wall = t1 > t0 ? t1 - t0 : 0;
    uint64_t open_ns = it->cmd_banked_ns;
    if (it->cmd_sweep_open) {
        uint64_t a = it->cmd_anchor_ns > t0 ? it->cmd_anchor_ns : t0;
        if (t1 > a)
            open_ns += t1 - a;
    }
    if (open_ns > wall)
        open_ns = wall;
    return open_ns;
}

static int overlay_in_command(const struct pgwt_open_interval *it,
                              uint64_t t0, uint64_t t1)
{
    if (!it->cmd_gate_present)
        return it->cmd_open != 0;
    if (!it->cmd_seen)
        return 1;

    uint64_t wall = t1 > t0 ? t1 - t0 : 0;
    if (t1 == t0)
        return it->cmd_sweep_open != 0;
    return overlay_cmd_open_ns(it, t0, t1) * 2 >= wall;
}

/* ── lifecycle ─────────────────────────────────────────────── */

void pgwt_overlay_init(struct pgwt_overlay *ov)
{
    if (!ov)
        return;
    memset(ov, 0, sizeof(*ov));
    /* Fail-closed: an overlay nobody filled in reports ABSENCE, never an
     * "included" total of zero. */
    ov->status = PGWT_OVERLAY_UNAVAILABLE;
    set_reason(ov->reason, sizeof(ov->reason), "overlay not requested");
}

void pgwt_overlay_free(struct pgwt_overlay *ov)
{
    if (!ov)
        return;
    free(ov->spans);
    ov->spans = NULL;
    ov->count = 0;
}

void pgwt_overlay_refuse(struct pgwt_overlay *ov, const char *reason)
{
    if (!ov)
        return;
    pgwt_overlay_free(ov);
    ov->status = PGWT_OVERLAY_REFUSED;
    set_reason(ov->reason, sizeof(ov->reason), "%s", reason ? reason : "refused");
}

void pgwt_overlay_unavailable(struct pgwt_overlay *ov, const char *reason)
{
    if (!ov)
        return;
    pgwt_overlay_free(ov);
    ov->status = PGWT_OVERLAY_UNAVAILABLE;
    set_reason(ov->reason, sizeof(ov->reason), "%s",
               reason ? reason : "in-flight right edge unavailable");
}

void pgwt_open_snapshot_free(struct pgwt_open_snapshot *snap)
{
    if (!snap)
        return;
    free(snap->items);
    snap->items = NULL;
    snap->count = 0;
}

static void snap_fail(struct pgwt_open_snapshot *snap,
                      enum pgwt_overlay_status st, const char *reason)
{
    pgwt_open_snapshot_free(snap);
    snap->status = st;
    set_reason(snap->reason, sizeof(snap->reason), "%s", reason);
}

/* ── route / tier gate ─────────────────────────────────────── */

int pgwt_overlay_wanted(const struct pgwt_overlay_request *req,
                        char *reason, size_t reason_size)
{
    const char *why = NULL;

    if (!req)
        why = "no request";
    else if (req->route == PGWT_OVERLAY_ROUTE_SUMMARY)
        /* Summary v8 credits open waits per sealed second already; an overlay
         * here would double count. Contract REVISION 2 §4c. */
        why = "summary route already credits open waits per sealed second";
    else if (req->tier == PGWT_OVERLAY_TIER_SAMPLED)
        why = "sampled tier has no watchpoint intervals";
    else if (req->tier == PGWT_OVERLAY_TIER_LIGHTWEIGHT)
        why = "lightweight tier has no watchpoint intervals";
    else if (req->to_wall_ns <= req->from_wall_ns)
        why = "empty window";

    if (why) {
        if (reason && reason_size)
            set_reason(reason, reason_size, "%s", why);
        return 0;
    }
    if (reason && reason_size)
        reason[0] = '\0';
    return 1;
}

/* ── parse + validate ──────────────────────────────────────── */

/* A reply bigger than this is not a 1024-backend truth, it is a bug or an
 * attack; refuse rather than allocate for it. */
#define OVERLAY_MAX_ENTRIES 8192

int pgwt_overlay_parse(const char *json, struct pgwt_open_snapshot *snap)
{
    if (!snap)
        return -1;
    memset(snap, 0, sizeof(*snap));
    snap->status = PGWT_OVERLAY_UNAVAILABLE;
    set_reason(snap->reason, sizeof(snap->reason), "no daemon reply");

    if (!json || !*json) {
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, "empty daemon reply");
        return -1;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, "unparseable daemon reply");
        return -1;
    }

    const cJSON *rsn = cJSON_GetObjectItemCaseSensitive(root, "reason");
    const char *rsn_s = (cJSON_IsString(rsn) && rsn->valuestring && rsn->valuestring[0])
                      ? rsn->valuestring : NULL;

    /* A daemon too old to know the command answers {"ok":false,"error":…}.
     * That is UNAVAILABLE (transient, a version skew), not REFUSED — refused
     * is reserved for "this tier cannot have open intervals", which the
     * daemon reports through `status`. */
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(err) && err->valuestring) {
        char buf[192];
        set_reason(buf, sizeof(buf), "daemon could not answer: %s", err->valuestring);
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, buf);
        cJSON_Delete(root);
        return -1;
    }

    /* STATUS FIRST, and then the producer's own invariant:
     *      pids present  ==  status "included"  ==  complete true
     * There is deliberately no fourth combination. A reply that breaks the
     * invariant is a producer bug, and the fail-safe answer to a producer bug
     * is the degraded one — never the optimistic one. */
    char status[32] = "";
    if (json_str(root, "status", status, sizeof(status)) != 0) {
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                  "daemon reply missing mandatory field status");
        cJSON_Delete(root);
        return -1;
    }
    const cJSON *complete = cJSON_GetObjectItemCaseSensitive(root, "complete");
    if (!cJSON_IsBool(complete)) {
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                  "daemon reply missing mandatory field complete");
        cJSON_Delete(root);
        return -1;
    }
    if (strcmp(status, "included") != 0) {
        char buf[192];
        int refused = strcmp(status, "refused") == 0;
        set_reason(buf, sizeof(buf), "daemon reported %s: %s", status,
                   rsn_s ? rsn_s : "no reason given");
        snap_fail(snap, refused ? PGWT_OVERLAY_REFUSED : PGWT_OVERLAY_UNAVAILABLE, buf);
        cJSON_Delete(root);
        return -1;
    }
    if (!cJSON_IsTrue(complete)) {
        char buf[192];
        set_reason(buf, sizeof(buf), "daemon reported an incomplete snapshot: %s",
                   rsn_s ? rsn_s : "no reason given");
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, buf);
        cJSON_Delete(root);
        return -1;
    }
    snap->complete = 1;

    static const char *const top_u64[] = {
        "snapshot_ns", "now_mono_ns", "now_wall_ns",
        "committed_through_ns", "daemon_generation",
        "snapshot_wall_ns", "committed_through_wall_ns"
    };
    uint64_t tv[7];
    for (int i = 0; i < 7; i++) {
        if (json_u64(root, top_u64[i], &tv[i]) != 0) {
            char buf[192];
            set_reason(buf, sizeof(buf),
                       "daemon reply missing/invalid mandatory field %s", top_u64[i]);
            snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, buf);
            cJSON_Delete(root);
            return -1;
        }
    }
    snap->snapshot_ns               = tv[0];
    snap->now_mono_ns               = tv[1];
    snap->now_wall_ns               = tv[2];
    snap->committed_through_ns      = tv[3];
    snap->daemon_generation         = tv[4];
    snap->snapshot_wall_ns          = tv[5];
    snap->committed_through_wall_ns = tv[6];
    (void)json_u64(root, "daemon_start_wall_ns", &snap->daemon_start_wall_ns);
    (void)json_u32(root, "daemon_pid", &snap->daemon_pid);
    (void)json_str(root, "tier", snap->tier, sizeof(snap->tier));

    if (snap->snapshot_ns == 0 || snap->now_mono_ns == 0 || snap->now_wall_ns == 0) {
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                  "daemon reply has a zero clock field");
        cJSON_Delete(root);
        return -1;
    }
    snap->anchor_offset_ns = (int64_t)snap->now_wall_ns - (int64_t)snap->now_mono_ns;

    /* The wall fields and the anchor are two expressions of one clock model,
     * computed by the producer and by us. They must agree exactly: a daemon
     * whose wall stamps do not sit on its own anchor has no coherent mapping,
     * and every span we would place on the timeline from it is arbitrary. */
    uint64_t want_snap_wall = 0, want_committed_wall = 0;
    if (mono_to_wall(snap->snapshot_ns, snap->anchor_offset_ns, &want_snap_wall) != 0 ||
        mono_to_wall(snap->committed_through_ns, snap->anchor_offset_ns,
                     &want_committed_wall) != 0) {
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                  "daemon clock anchor does not map its own snapshot");
        cJSON_Delete(root);
        return -1;
    }
    if (want_snap_wall != snap->snapshot_wall_ns ||
        want_committed_wall != snap->committed_through_wall_ns) {
        char buf[192];
        set_reason(buf, sizeof(buf),
                   "daemon wall stamps disagree with its own anchor "
                   "(snapshot_wall_ns %llu, anchored %llu)",
                   (unsigned long long)snap->snapshot_wall_ns,
                   (unsigned long long)want_snap_wall);
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, buf);
        cJSON_Delete(root);
        return -1;
    }

    /* cpu_accounting decides whether a CPU figure exists at all, so it is
     * mandatory: defaulting it either way would turn "not measured" into a
     * measured number or the reverse. */
    uint8_t cpu_acct = 0, gate_active = 0;
    if (json_bool(root, "cpu_accounting", &cpu_acct) != 0) {
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                  "daemon reply missing mandatory field cpu_accounting");
        cJSON_Delete(root);
        return -1;
    }
    snap->cpu_accounting = cpu_acct;
    if (json_bool(root, "cmd_gate_active", &gate_active) == 0)
        snap->cmd_gate_active = gate_active;

    const cJSON *pids = cJSON_GetObjectItemCaseSensitive(root, "pids");
    if (!cJSON_IsArray(pids)) {
        /* status said "included", so by the producer's own invariant the array
         * must be here. Absent is NOT "nothing open": an empty ARRAY is that
         * measurement, and treating a missing array as one would be exactly
         * the silent zero this module exists to prevent. */
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                  "daemon reply says included but carries no pids array");
        cJSON_Delete(root);
        return -1;
    }

    int n = cJSON_GetArraySize(pids);
    /* open_count is the producer's own row counter, written before the array
     * was serialised. Comparing it with what actually arrived catches a reply
     * that lost rows on the way out and still parses — the two numbers come
     * from different places, which is the only reason the check can fail. */
    uint64_t open_count = 0;
    if (json_u64(root, "open_count", &open_count) == 0 && open_count != (uint64_t)n) {
        char buf[192];
        set_reason(buf, sizeof(buf),
                   "daemon reply carries %d open intervals but reports open_count %llu",
                   n, (unsigned long long)open_count);
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, buf);
        cJSON_Delete(root);
        return -1;
    }
    if (n < 0 || n > OVERLAY_MAX_ENTRIES) {
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                  "daemon reply reports an implausible number of open intervals");
        cJSON_Delete(root);
        return -1;
    }
    if (n > 0) {
        snap->items = calloc((size_t)n, sizeof(*snap->items));
        if (!snap->items) {
            snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, "out of memory");
            cJSON_Delete(root);
            return -1;
        }
    }

    int idx = 0;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, pids) {
        struct pgwt_open_interval *it = &snap->items[idx];
        const char *bad = NULL;

        if (!cJSON_IsObject(e))                              bad = "entry";
        else if (json_u32(e, "pid", &it->pid) != 0)          bad = "pid";
        else if (json_u64(e, "incarnation", &it->incarnation) != 0) bad = "incarnation";
        else if (json_u64(e, "generation", &it->generation) != 0) bad = "generation";
        else if (json_u32(e, "event_id", &it->event_id) != 0) bad = "event_id";
        /* cat_flag is the PROCESS type (io_worker/maint/background), which is
         * what the AAS exclusion turns on; `category` is the WAIT CLASS name.
         * Two different facts that both used to be called "category". */
        else if (json_u32(e, "cat_flag", &it->cat_flag) != 0) bad = "cat_flag";
        else if (json_str(e, "category", it->category, sizeof(it->category)) != 0)
            bad = "category (absent or empty)";
        else if (json_u32(e, "category_id", &it->category_id) != 0) bad = "category_id";
        else if (json_u64(e, "open_start_ns", &it->open_start_ns) != 0) bad = "open_start_ns";
        else if (json_u64(e, "cpu_ns_total", &it->cpu_ns_total) != 0) bad = "cpu_ns_total";
        else if (json_u64(e, "on_cpu_ts", &it->on_cpu_ts) != 0) bad = "on_cpu_ts";
        else if (json_u64(e, "last_cpu_ns", &it->last_cpu_ns) != 0) bad = "last_cpu_ns";
        else if (json_bool(e, "cmd_open", &it->cmd_open) != 0) bad = "cmd_open";
        else if (json_bool(e, "cmd_sweep_open", &it->cmd_sweep_open) != 0)
            bad = "cmd_sweep_open";
        else if (json_bool(e, "cmd_seen", &it->cmd_seen) != 0) bad = "cmd_seen";
        else if (json_bool(e, "cmd_gate_present", &it->cmd_gate_present) != 0)
            bad = "cmd_gate_present";
        else if (json_bool(e, "cmd_in_window", &it->cmd_in_window) != 0)
            bad = "cmd_in_window";
        else if (json_u64(e, "cmd_banked_ns", &it->cmd_banked_ns) != 0) bad = "cmd_banked_ns";
        else if (json_u64(e, "cmd_anchor_ns", &it->cmd_anchor_ns) != 0) bad = "cmd_anchor_ns";
        else if (json_u64(e, "query_id", &it->query_id) != 0) bad = "query_id";
        else if (json_u16(e, "query_quality", &it->query_quality) != 0) bad = "query_quality";
        else if (it->pid == 0)                               bad = "pid (zero)";
        else if (it->open_start_ns == 0)                     bad = "open_start_ns (zero)";
        else if (it->open_start_ns > snap->snapshot_ns)
            bad = "open_start_ns (after the snapshot)";
        else if (PGWT_IS_MARKER(it->event_id) || it->event_id == PGWT_EVENT_EXIT)
            bad = "event_id (a marker is never an open wait)";
        else if (it->on_cpu_ts != 0 && it->on_cpu_ts > snap->snapshot_ns)
            bad = "on_cpu_ts (after the snapshot)";

        if (!bad) {
            /* CPU. The arithmetic is B's shared helper (src/open_interval.h),
             * the SAME function the de-escalation and shutdown flushes use —
             * a second copy here is the one place the two producers could
             * drift, invisibly, with both answers looking plausible. The
             * helper is deliberately NOT clamped to the stretch; clamping is
             * the consumer's step and happens in the builder. */
            it->cpu_ns = pgwt_open_stretch_cpu_ns(it->cpu_ns_total, it->on_cpu_ts,
                                                  it->last_cpu_ns, snap->snapshot_ns,
                                                  snap->cpu_accounting != 0);
            uint64_t sent = 0;
            int has_sent = json_u64(e, "cpu_ns", &sent) == 0;
            if (!snap->cpu_accounting && has_sent)
                bad = "cpu_ns (present although the daemon measures no CPU)";
            else if (snap->cpu_accounting && has_sent && sent != it->cpu_ns)
                /* Cross-producer drift check: two processes, one formula. If
                 * these differ, one of the two computed the stretch at a
                 * different instant or with different inputs, and crediting
                 * either would be a guess. */
                bad = "cpu_ns (disagrees with the shared open-stretch arithmetic)";
            else if (snap->cpu_accounting && !has_sent)
                /* Absence stays absence — never a measured zero. */
                it->cpu_ns = PGWT_CPU_NS_UNKNOWN;
        }

        if (!bad) {
            /* The command classification, reduced here and cross-checked
             * against the daemon's own figures from the real classifier.
             * They are two implementations of one documented rule; a
             * disagreement is the 59-of-96 defect class, so it refuses.
             * BOTH quantities are compared: the verdict, and the open-ns the
             * verdict came from — a verdict alone cannot see the clamp. */
            uint64_t sent_open_ns = 0;
            it->in_command = overlay_in_command(it, it->open_start_ns,
                                                snap->snapshot_ns) ? 1 : 0;
            if (json_u64(e, "cmd_open_ns", &sent_open_ns) != 0)
                bad = "cmd_open_ns";
            else if (it->cmd_gate_present &&
                     sent_open_ns != overlay_cmd_open_ns(it, it->open_start_ns,
                                                         snap->snapshot_ns))
                bad = "cmd_open_ns (disagrees with our own gate reduction)";
            else if (it->in_command != it->cmd_in_window)
                bad = "cmd_in_window (the daemon's verdict and ours disagree)";
        }

        if (bad) {
            /* One bad entry fails the WHOLE reply. A partially parsed snapshot
             * would be a silently short total, which §5 forbids. */
            char buf[192];
            set_reason(buf, sizeof(buf),
                       "daemon reply entry %d has a missing/invalid field: %s",
                       idx, bad);
            snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, buf);
            cJSON_Delete(root);
            return -1;
        }
        idx++;
    }
    snap->count = (size_t)idx;
    snap->status = PGWT_OVERLAY_INCLUDED;
    snap->reason[0] = '\0';
    cJSON_Delete(root);
    return 0;
}

/* ── transport: one bounded round-trip ─────────────────────── */

static int wait_fd(int fd, short events, long deadline)
{
    for (;;) {
        long left = deadline - mono_ms();
        if (left <= 0)
            return -2;                 /* overall deadline hit */
        struct pollfd p = { .fd = fd, .events = events, .revents = 0 };
        int r = poll(&p, 1, (int)(left > 1000 ? 1000 : left));
        if (r > 0)
            return 0;
        if (r < 0 && errno != EINTR)
            return -1;
    }
}

int pgwt_overlay_fetch(const char *trace_dir, int timeout_ms,
                       struct pgwt_open_snapshot *snap)
{
    if (!snap)
        return -1;
    memset(snap, 0, sizeof(*snap));
    snap->status = PGWT_OVERLAY_UNAVAILABLE;
    set_reason(snap->reason, sizeof(snap->reason), "no daemon reply");

    if (!trace_dir) {
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, "no trace directory");
        return -1;
    }
    if (timeout_ms <= 0)
        timeout_ms = PGWT_OVERLAY_TIMEOUT_MS;
    /* ONE deadline for connect + send + read. The existing 3 s control-socket
     * timeout is per-I/O, so a daemon that answers one byte at a time can
     * outlive it indefinitely; this cannot be extended by fragmentation. */
    const long deadline = mono_ms() + timeout_ms;

    char sock_path[600];
    snprintf(sock_path, sizeof(sock_path), "%s/pgwt.sock", trace_dir);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                  "daemon control socket path too long");
        return -1;
    }
    strcpy(addr.sun_path, sock_path);

    struct stat st;
    if (stat(sock_path, &st) != 0) {
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                  "daemon control socket absent (daemon not running)");
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, "cannot create a control socket");
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (errno == EINPROGRESS) {
            int w = wait_fd(fd, POLLOUT, deadline);
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            if (w != 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr) {
                close(fd);
                snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                          w == -2 ? "daemon control socket timed out while connecting"
                                  : "daemon control socket refused the connection");
                return -1;
            }
        } else {
            int e = errno;
            close(fd);
            snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                      e == ECONNREFUSED
                          ? "daemon control socket is stale (daemon not running)"
                          : "cannot connect to the daemon control socket");
            return -1;
        }
    }

    static const char req[] = "{\"cmd\":\"open_intervals\"}\n";
    size_t sent = 0;
    while (sent < sizeof(req) - 1) {
#ifdef MSG_NOSIGNAL
        ssize_t w = send(fd, req + sent, sizeof(req) - 1 - sent, MSG_NOSIGNAL);
#else
        ssize_t w = send(fd, req + sent, sizeof(req) - 1 - sent, 0);
#endif
        if (w > 0) {
            sent += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (wait_fd(fd, POLLOUT, deadline) != 0) {
                close(fd);
                snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                          "daemon control socket timed out while sending the request");
                return -1;
            }
            continue;
        }
        close(fd);
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                  "daemon control socket write failed");
        return -1;
    }

    size_t cap = 64u * 1024u, off = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fd);
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, "out of memory");
        return -1;
    }

    const char *fail = NULL;
    for (;;) {
        if (off + 1 >= cap) {
            if (cap >= PGWT_OVERLAY_MAX_REPLY) {
                /* Truncation surfaces as a degraded status, never as a
                 * partial answer (contract §6). */
                fail = "daemon reply exceeded the 4 MB cap (truncated)";
                break;
            }
            size_t ncap = cap * 2;
            if (ncap > PGWT_OVERLAY_MAX_REPLY)
                ncap = PGWT_OVERLAY_MAX_REPLY;
            char *nb = realloc(buf, ncap);
            if (!nb) {
                fail = "out of memory while reading the daemon reply";
                break;
            }
            buf = nb;
            cap = ncap;
        }
        ssize_t r = read(fd, buf + off, cap - 1 - off);
        if (r > 0) {
            off += (size_t)r;
            if (memchr(buf, '\n', off))
                break;
            continue;
        }
        if (r == 0) {
            fail = "daemon closed the control socket mid-reply (truncated)";
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            int w = wait_fd(fd, POLLIN, deadline);
            if (w == -2) {
                fail = "daemon did not answer within the overlay deadline";
                break;
            }
            if (w != 0) {
                fail = "daemon control socket poll failed";
                break;
            }
            continue;
        }
        fail = "daemon control socket read failed";
        break;
    }
    close(fd);

    if (fail) {
        free(buf);
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE, fail);
        return -1;
    }

    buf[off] = '\0';
    char *nl = memchr(buf, '\n', off);
    if (!nl) {
        free(buf);
        snap_fail(snap, PGWT_OVERLAY_UNAVAILABLE,
                  "daemon reply had no line terminator (truncated)");
        return -1;
    }
    *nl = '\0';

    int rc = pgwt_overlay_parse(buf, snap);
    free(buf);
    return rc;
}

/* ── the per-pid high-water table over admitted records ────── */

struct hw_slot {
    uint32_t pid;
    int      used;
    uint64_t max_wall_ns;   /* latest admitted non-marker record for this pid */
    int      seen;
};

struct hw_table {
    struct hw_slot *slots;
    size_t mask;
};

static int hw_init(struct hw_table *t, size_t n_pids)
{
    size_t want = 16;
    while (want < n_pids * 4 + 16)
        want <<= 1;
    t->slots = calloc(want, sizeof(*t->slots));
    if (!t->slots)
        return -1;
    t->mask = want - 1;
    return 0;
}

static struct hw_slot *hw_find(struct hw_table *t, uint32_t pid, int insert)
{
    size_t h = ((size_t)pid * 2654435761u) & t->mask;
    for (size_t probe = 0; probe <= t->mask; probe++) {
        struct hw_slot *s = &t->slots[(h + probe) & t->mask];
        if (s->used && s->pid == pid)
            return s;
        if (!s->used) {
            if (!insert)
                return NULL;
            s->used = 1;
            s->pid = pid;
            return s;
        }
    }
    return NULL;
}

/* ── build ─────────────────────────────────────────────────── */

static uint64_t prorate(uint64_t value, uint64_t num, uint64_t den)
{
    if (den == 0 || num >= den)
        return value;
#if defined(__SIZEOF_INT128__)
    return (uint64_t)(((unsigned __int128)value * num) / den);
#else
    long double v = (long double)value * (long double)num / (long double)den;
    return (uint64_t)v;
#endif
}

int pgwt_overlay_build(struct pgwt_overlay *ov,
                       const struct pgwt_open_snapshot *snap,
                       const struct pgwt_overlay_request *req,
                       const struct pgwt_overlay_records *rec)
{
    if (!ov)
        return -1;
    pgwt_overlay_free(ov);
    ov->snapshot_wall_ns = 0;
    ov->committed_through_wall_ns = 0;
    ov->daemon_generation = 0;

    char why[192];
    if (!pgwt_overlay_wanted(req, why, sizeof(why))) {
        /* Enforced here too, not only at the call site: a perfectly good
         * snapshot handed to a summary-route or sampled-tier request is still
         * refused. The double-count guard must not depend on the caller
         * remembering to ask wanted() first. */
        pgwt_overlay_refuse(ov, why);
        return -1;
    }
    if (!snap) {
        pgwt_overlay_unavailable(ov, "no daemon snapshot");
        return -1;
    }
    if (snap->status != PGWT_OVERLAY_INCLUDED) {
        if (snap->status == PGWT_OVERLAY_REFUSED)
            pgwt_overlay_refuse(ov, snap->reason);
        else
            pgwt_overlay_unavailable(ov, snap->reason[0] ? snap->reason
                                                         : "daemon snapshot unusable");
        return -1;
    }
    if (req->filters_active && !req->accept) {
        /* A filtered window with unfiltered in-flight time added to it would
         * inflate precisely the view the user narrowed. Refuse visibly. */
        pgwt_overlay_unavailable(ov,
            "the request is filtered but no span filter was supplied");
        return -1;
    }
    if (!rec || (rec->n > 0 && !rec->events)) {
        /* Without the admitted records the precedence rule cannot run, and a
         * span that a real closing record already covers would double count.
         * A gate that cannot see must refuse. */
        pgwt_overlay_unavailable(ov, "admitted record view missing");
        return -1;
    }
    if (req->expect_generation && req->expect_generation != snap->daemon_generation) {
        pgwt_overlay_unavailable(ov,
            "daemon generation changed since the trace was opened (restart)");
        return -1;
    }

    uint64_t snapshot_wall = 0, committed_wall = 0;
    if (mono_to_wall(snap->snapshot_ns, snap->anchor_offset_ns, &snapshot_wall) != 0 ||
        mono_to_wall(snap->committed_through_ns, snap->anchor_offset_ns,
                     &committed_wall) != 0) {
        pgwt_overlay_unavailable(ov, "daemon clock anchor is inconsistent");
        return -1;
    }
    ov->snapshot_wall_ns = snapshot_wall;
    ov->committed_through_wall_ns = committed_wall;
    ov->daemon_generation = snap->daemon_generation;

    /* The window starts at or after the observation: the daemon's snapshot
     * says nothing about it. An empty overlay here would read as "nothing was
     * in flight" when the truth is "we did not look". Stale snapshot, reused
     * snapshot, clock skew — all of them land here, and all of them are
     * absence, not zero. */
    if (req->from_wall_ns >= snapshot_wall) {
        pgwt_overlay_unavailable(ov,
            "the daemon snapshot predates the window (stale snapshot or clock skew)");
        return -1;
    }

    /* Never credit past the observation: the right edge is the window's `to`
     * or the snapshot, whichever is earlier (contract §4b). */
    uint64_t end_cap = req->to_wall_ns < snapshot_wall ? req->to_wall_ns : snapshot_wall;

    if (snap->count == 0) {
        ov->status = PGWT_OVERLAY_INCLUDED;
        ov->reason[0] = '\0';
        return 0;
    }

    struct hw_table hw = { 0 };
    if (hw_init(&hw, snap->count) != 0) {
        pgwt_overlay_unavailable(ov, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < snap->count; i++)
        (void)hw_find(&hw, snap->items[i].pid, 1);

    /* One pass over the admitted array — the same array the time reducers are
     * about to read. Markers and point samples are not interval closes; an
     * EXIT record is (we have no duration for it, so fail closed and drop). */
    for (size_t i = 0; i < rec->n; i++) {
        const struct pgwt_trace_event *ev = &rec->events[i];
        struct hw_slot *s = hw_find(&hw, ev->pid, 0);
        if (!s)
            continue;
        if (PGWT_IS_MARKER(ev->new_event) || PGWT_IS_MARKER(ev->old_event))
            continue;
        if (ev->flags & PGWT_EVENT_FLAG_SAMPLE)
            continue;
        uint64_t w = rec->wall_of ? rec->wall_of(ev, rec->user)
                                  : (uint64_t)((int64_t)ev->timestamp_ns + rec->offset_ns);
        s->seen = 1;
        if (w > s->max_wall_ns)
            s->max_wall_ns = w;
    }

    struct pgwt_overlay_span *spans = calloc(snap->count, sizeof(*spans));
    if (!spans) {
        free(hw.slots);
        pgwt_overlay_unavailable(ov, "out of memory");
        return -1;
    }
    size_t n = 0;

    for (size_t i = 0; i < snap->count; i++) {
        const struct pgwt_open_interval *it = &snap->items[i];

        /* io_workers are excluded from AAS/DB Time everywhere else; they must
         * not re-enter through the overlay. */
        if (it->cat_flag & PGWT_EVENT_FLAG_IO_WORKER)
            continue;

        uint64_t s_from, s_to = snapshot_wall;
        if (mono_to_wall(it->open_start_ns, snap->anchor_offset_ns, &s_from) != 0)
            continue;
        if (s_to <= s_from)
            continue;               /* zero-length stretch — nothing open */

        uint64_t start = s_from > req->from_wall_ns ? s_from : req->from_wall_ns;
        uint64_t end   = end_cap;
        if (end <= start)
            continue;               /* a historical window lands here: no span,
                                     * with no "near now" test anywhere */

        /* The uniform precedence rule (contract REVISION 2 §4): any admitted
         * record for this pid past the open start IS the close of the same
         * interval — and with widened admission it is now admitted. */
        struct hw_slot *s = hw_find(&hw, it->pid, 0);
        if (s && s->seen && s->max_wall_ns > s_from)
            continue;

        /* committed_through is what tells us a gap is "still open" rather than
         * "closed but not yet published". If the closed prefix does not reach
         * this interval's start, the window's total would be short by an
         * unknown amount, so the whole overlay is reported absent rather than
         * quietly partial. */
        if (snap->committed_through_ns < it->open_start_ns) {
            free(spans);
            free(hw.slots);
            pgwt_overlay_unavailable(ov,
                "closed-record prefix is not published through the open interval start");
            return -1;
        }

        struct pgwt_overlay_span *sp = &spans[n];
        sp->pid           = it->pid;
        sp->incarnation   = it->incarnation;
        sp->event_id      = it->event_id;
        sp->cat_flag      = it->cat_flag;
        sp->category_id   = it->category_id;
        memcpy(sp->category, it->category, sizeof(sp->category));
        sp->query_id      = it->query_id;
        sp->query_quality = it->query_quality;
        sp->from_wall_ns  = start;
        sp->to_wall_ns    = end;
        sp->wall_ns       = end - start;
        sp->stretch_from_wall_ns = s_from;
        sp->stretch_to_wall_ns   = s_to;
        /* Reduced and cross-checked against the daemon's verdict at parse
         * time; PROVISIONAL, because the majority can flip at the close. */
        sp->in_command    = it->in_command;

        /* The shared helper's figure is deliberately unclamped (the flush has
         * never clamped, and changing that would rewrite records the trace
         * has carried since T8). Clamping is the CONSUMER's step, so it is
         * here: prorate the stretch's CPU onto the clipped span, then cap it
         * at the span's own wall — one backend cannot burn more CPU than the
         * wall time it occupied. */
        uint64_t cpu_full = it->cpu_ns;
        if (cpu_full == PGWT_CPU_NS_UNKNOWN) {
            sp->cpu_ns = PGWT_CPU_NS_UNKNOWN;
        } else {
            uint64_t c = prorate(cpu_full, sp->wall_ns, s_to - s_from);
            sp->cpu_ns = c > sp->wall_ns ? sp->wall_ns : c;
        }

        /* The request's own filter predicate, applied to the fully built span
         * so it can test event, category, query and pid alike. */
        if (req->accept && !req->accept(sp, req->accept_user))
            continue;

        /* Two live intervals for one pid must never overlap in time: that
         * would be one backend counted twice. PID reuse makes this reachable
         * (two incarnations), so it is checked, not assumed. */
        for (size_t j = 0; j < n; j++) {
            if (spans[j].pid != sp->pid)
                continue;
            if (spans[j].from_wall_ns < sp->to_wall_ns &&
                sp->from_wall_ns < spans[j].to_wall_ns) {
                free(spans);
                free(hw.slots);
                pgwt_overlay_unavailable(ov,
                    "daemon reported overlapping open intervals for one pid");
                return -1;
            }
        }
        n++;
    }

    free(hw.slots);
    ov->spans = spans;
    ov->count = n;
    ov->status = PGWT_OVERLAY_INCLUDED;
    ov->reason[0] = '\0';
    return 0;
}

/* ── the loader-level entry point ──────────────────────────── */

int pgwt_overlay_attach(struct pgwt_overlay *ov, const char *trace_dir,
                        int timeout_ms,
                        const struct pgwt_overlay_request *req,
                        const struct pgwt_overlay_records *rec)
{
    if (!ov)
        return -1;
    pgwt_overlay_init(ov);

    char why[192];
    if (!pgwt_overlay_wanted(req, why, sizeof(why))) {
        /* Refuse BEFORE any I/O: a summary-route or non-exact-tier request
         * must not pay for a wedged daemon, and must never reach a code path
         * where a reply could be credited. */
        pgwt_overlay_refuse(ov, why);
        return -1;
    }

    struct pgwt_open_snapshot snap;
    pgwt_overlay_fetch(trace_dir, timeout_ms, &snap);
    int rc = pgwt_overlay_build(ov, &snap, req, rec);
    pgwt_open_snapshot_free(&snap);
    return rc;
}

/* ── span arithmetic for the time reducers ─────────────────── */

uint64_t pgwt_overlay_span_overlap_ns(const struct pgwt_overlay_span *s,
                                      uint64_t from_wall_ns, uint64_t to_wall_ns)
{
    if (!s || to_wall_ns <= from_wall_ns)
        return 0;
    uint64_t a = s->from_wall_ns > from_wall_ns ? s->from_wall_ns : from_wall_ns;
    uint64_t b = s->to_wall_ns   < to_wall_ns   ? s->to_wall_ns   : to_wall_ns;
    return b > a ? b - a : 0;
}

uint64_t pgwt_overlay_span_cpu_ns(const struct pgwt_overlay_span *s,
                                  uint64_t from_wall_ns, uint64_t to_wall_ns)
{
    if (!s)
        return 0;
    if (s->cpu_ns == PGWT_CPU_NS_UNKNOWN)
        return PGWT_CPU_NS_UNKNOWN;
    uint64_t ov = pgwt_overlay_span_overlap_ns(s, from_wall_ns, to_wall_ns);
    if (ov == 0)
        return 0;
    /* No clamp needed here: the span's own cpu_ns was already clamped to its
     * wall by the builder, so a sub-window share of it cannot exceed that
     * sub-window. A second clamp would be unreachable code — and unreachable
     * code is indistinguishable from a guard that does not work. */
    return prorate(s->cpu_ns, ov, s->wall_ns);
}

uint64_t pgwt_overlay_total_wall_ns(const struct pgwt_overlay *ov)
{
    uint64_t t = 0;
    if (!ov || ov->status != PGWT_OVERLAY_INCLUDED)
        return 0;
    for (size_t i = 0; i < ov->count; i++)
        t += ov->spans[i].wall_ns;
    return t;
}

void pgwt_overlay_add_json(const struct pgwt_overlay *ov, struct cJSON *root)
{
    if (!root)
        return;
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return;
    if (!ov || ov->status != PGWT_OVERLAY_INCLUDED) {
        cJSON_AddStringToObject(o, "status",
                                pgwt_overlay_status_name(ov ? ov->status
                                                            : PGWT_OVERLAY_UNAVAILABLE));
        cJSON_AddStringToObject(o, "reason",
                                (ov && ov->reason[0]) ? ov->reason
                                                      : "in-flight right edge unavailable");
        /* The closed-record totals are still served — explicitly marked as
         * excluding in-flight time. NO numeric in-flight field is emitted:
         * absent, never a zero that reads as "nothing was in flight". */
        cJSON_AddBoolToObject(o, "excludes_in_flight", 1);
    } else {
        cJSON_AddStringToObject(o, "status", "included");
        cJSON_AddNumberToObject(o, "snapshot_wall_ns", (double)ov->snapshot_wall_ns);
        cJSON_AddNumberToObject(o, "pids", (double)ov->count);
        cJSON_AddNumberToObject(o, "in_flight_ns",
                                (double)pgwt_overlay_total_wall_ns(ov));
        cJSON_AddBoolToObject(o, "excludes_in_flight", 0);
    }
    cJSON_AddItemToObject((cJSON *)root, "open_intervals", o);
}
