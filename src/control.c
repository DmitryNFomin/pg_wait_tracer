/* control.c — Daemon control socket: unix socket + JSON-line protocol */
#define _GNU_SOURCE   /* accept4 */
#include "control.h"
#include "daemon.h"
#include "event_writer.h"
#include "provider.h"
#include "escalation.h"
#include "sampler.h"
#include "map_reader.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/epoll.h>

/* ── Helpers ──────────────────────────────────────────────── */

static uint64_t now_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Add uint64 as raw JSON number (avoids double precision loss) */
static void cjson_add_uint64(cJSON *obj, const char *name, uint64_t val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)val);
    cJSON_AddRawToObject(obj, name, buf);
}

/* Lowercase token for an escalation reason ("manual"|"anomaly"|...). Used so
 * the UI can distinguish WHY a full-fidelity window is open (e.g. annotate a
 * manual escalate differently from an anomaly-triggered one). */
static const char *esc_reason_str(int reason)
{
    switch (reason) {
    case PGWT_ESC_REASON_MANUAL:   return "manual";
    case PGWT_ESC_REASON_ANOMALY:  return "anomaly";
    case PGWT_ESC_REASON_EXPIRED:  return "expired";
    case PGWT_ESC_REASON_REQUEST:  return "request";
    case PGWT_ESC_REASON_SHUTDOWN: return "shutdown";
    case PGWT_ESC_REASON_BUDGET:   return "budget";
    default:                       return "unknown";
    }
}

/* The capture tier being recorded RIGHT NOW, honest for every mode (ESC-9):
 * tiered flips sampled/escalated; a fixed EXACT-fidelity provider (--mode full)
 * is always "escalated" (full fidelity); everything else is "sampled". Shared
 * by status and metrics so they can never disagree. */
static const char *daemon_tier_str(const struct pgwt_daemon *d)
{
    if (d->escalation.enabled)
        return d->escalation.active ? "escalated" : "sampled";
    if (d->provider && d->provider->fidelity == PGWT_FIDELITY_EXACT)
        return "escalated";   /* full mode: always full fidelity */
    return "sampled";
}

/* Count live tracked backends */
static int count_backends(const struct pgwt_daemon *d)
{
    int n = 0;
    for (int i = 0; i < d->backends.count; i++)
        if (d->backends.entries[i].is_alive)
            n++;
    return n;
}

/* ── Response builders ────────────────────────────────────── */

static cJSON *build_status(const struct pgwt_daemon *d)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", 1);
    /* Capture tier. --lightweight is reported as its own mode so the answer
     * is never a lie about what is being captured; otherwise report the
     * active provider name ("full" | "sampled"), with tiered distinguished. */
    const char *mode;
    if (d->lightweight_mode)
        mode = "lightweight";
    else if (d->mode == PGWT_MODE_TIERED)
        mode = "tiered";
    else if (d->provider)
        mode = d->provider->name;
    else
        mode = "full";
    cJSON_AddStringToObject(root, "mode", mode);
    cJSON_AddNumberToObject(root, "uptime_s",
                            (double)(now_mono_ns() - d->start_ts) / 1e9);
    cJSON_AddNumberToObject(root, "backends", count_backends(d));
    cJSON_AddNumberToObject(root, "pg_pid", d->postmaster_pid);
    cJSON_AddStringToObject(root, "version", PGWT_BUILD_VERSION);

    /* Current capture tier and escalation state (A4). "tier" is what is being
     * captured right now: in tiered mode it flips between "sampled" (always-on
     * baseline) and "escalated" (full-fidelity window open); for full/sampled
     * it mirrors the fixed provider fidelity. */
    cJSON_AddStringToObject(root, "tier", daemon_tier_str(d));
    cJSON_AddBoolToObject(root, "escalation_supported", d->escalation.enabled);
    cJSON_AddNumberToObject(root, "escalation_seconds_remaining",
                            pgwt_escalation_remaining_s(d));
    cJSON_AddNumberToObject(root, "escalation_budget_remaining_s",
                            pgwt_escalation_budget_remaining_s(d));
    /* ESC-6: report the budget mode truthfully. unlimited => remaining is the
     * -1 sentinel; this flag disambiguates it from a finite budget. */
    cJSON_AddBoolToObject(root, "escalation_budget_unlimited",
                          d->escalation.budget_unlimited);
    /* Reason of the currently-open window (so the UI can annotate manual vs
     * anomaly escalations distinctly). "none" while not escalated. */
    cJSON_AddStringToObject(root, "escalation_reason",
                            d->escalation.active
                                ? esc_reason_str(d->escalation.window_reason)
                                : "none");

    /* Sampler read health (SMP-1). A total read failure renders as an idle
     * database in the DATA — this out-of-band flag is how a client can tell
     * "idle" from "blind". healthy=true when no sampler runs (full mode). */
    bool sampler_healthy = true;
    char reason[160] = "";
    if (d->sampler && !d->sampler->health.healthy) {
        sampler_healthy = false;
        snprintf(reason, sizeof(reason),
                 "sampler reads failing: %s (%llu consecutive ticks, "
                 "%llu total)",
                 strerror(d->sampler->health.last_errno),
                 (unsigned long long)d->sampler->health.consec_failed_ticks,
                 (unsigned long long)d->sampler->health.failed_ticks_total);
    }
    cJSON_AddBoolToObject(root, "sampler_healthy", sampler_healthy);
    cJSON_AddStringToObject(root, "sampler_unhealthy_reason", reason);

    /* T8 measured-CPU capability (docs/ROADMAP_AND_STATUS.md), reported
     * as loudly here as in the startup log — never silent. "measured": the
     * exact tier reads task se.sum_exec_runtime deltas and the trace carries
     * real cpu_ns + Off-CPU*. "legacy": /proc schedstat was unavailable
     * (CONFIG_SCHED_INFO off), so CPU is inferred from wait gaps (includes
     * runqueue/throttle time) and cpu_ns carries the UNKNOWN sentinel. */
    cJSON_AddStringToObject(root, "cpu_accounting",
                            d->cpu_accounting ? "measured" : "legacy");
    return root;
}

static cJSON *build_metrics(const struct pgwt_daemon *d)
{
    const struct pgwt_counters *ctr = &d->counters;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddNumberToObject(root, "uptime_s",
                            (double)(now_mono_ns() - d->start_ts) / 1e9);

    /* Event path counters (incremented in event_stream.c / daemon.c) */
    cjson_add_uint64(root, "events_total", ctr->events_total);
    cJSON_AddNumberToObject(root, "events_per_sec", ctr->events_per_sec);
    cjson_add_uint64(root, "lifecycle_events_total",
                     ctr->lifecycle_events_total);

    /* Watchpoint attach failures (incremented in backend.c) */
    cjson_add_uint64(root, "wp_attach_failures_total",
                     ctr->wp_attach_failures_total);

    cJSON_AddNumberToObject(root, "backends_tracked", count_backends(d));

    /* Sampled tier counters (0 when not sampling) */
    cjson_add_uint64(root, "samples_total", ctr->samples_total);
    cJSON_AddNumberToObject(root, "samples_per_sec", ctr->samples_per_sec);
    cjson_add_uint64(root, "sample_read_faults_total",
                     ctr->sample_read_faults_total);
    cjson_add_uint64(root, "sampler_ticks_missed_total",
                     ctr->sampler_ticks_missed_total);

    /* Capture hardening (T4): any non-zero here is a loudly-logged problem.
     * state_map_full_total = BPF-side insert failures + userspace
     * preseed/seed failures (each one is a backend recording nothing or
     * losing query attribution — CAP-1). seen_query_ids_full_total = query
     * text capture lost for new ids (CAP-6). invalid_wait_reads_total =
     * garbage class-byte readings dropped (wrong offset backstop, CAP-2/5).
     * state_reseeds_total = frozen-stale entries reseeded by the per-tick
     * sweep (seed→arm race repair, one INFO line each). */
    cjson_add_uint64(root, "state_map_full_total",
                     ctr->state_map_full_total
                     + pgwt_read_bpf_fail_counter((struct pgwt_daemon *)d,
                                                  PGWT_BPF_FAIL_STATE_MAP));
    cjson_add_uint64(root, "seen_query_ids_full_total",
                     pgwt_read_bpf_fail_counter((struct pgwt_daemon *)d,
                                                PGWT_BPF_FAIL_SEEN_QIDS));
    cjson_add_uint64(root, "invalid_wait_reads_total",
                     ctr->invalid_wait_reads_total);
    cjson_add_uint64(root, "state_reseeds_total",
                     ctr->state_reseeds_total);
    cJSON_AddBoolToObject(root, "sampler_healthy",
                          !(d->sampler && !d->sampler->health.healthy));

    /* T2 decomposed-AAS observability (docs/AAS_SEMANTICS_DECISION.md):
     * io_worker_busy_pct is the utilization signal that replaces counting
     * io_workers in AAS (busy% over the last display interval; a sustained
     * high value means the io_worker pool is near saturation).
     * noncmd_cpu_samples_total counts client we==0 readings outside a
     * command — observed but deliberately not recorded as activity. */
    cJSON_AddNumberToObject(root, "io_worker_busy_pct",
                            ctr->io_worker_busy_pct);

    /* T8 measured-CPU counters (docs/ROADMAP_AND_STATUS.md). Lifetime
     * totals over exact-tier measured intervals; all 0 on a legacy daemon.
     * cpu_accounting mirrors status so a metrics scrape alone reveals the tier.
     * wait_gap_cpu_ns_total is the self-check (should stay ≈0 vs total wait). */
    cJSON_AddStringToObject(root, "cpu_accounting",
                            d->cpu_accounting ? "measured" : "legacy");
    cjson_add_uint64(root, "cpu_ns_total", ctr->cpu_ns_total);
    cjson_add_uint64(root, "offcpu_ns_total", ctr->offcpu_ns_total);
    cjson_add_uint64(root, "cpu_clamped_total", ctr->cpu_clamped_ns_total);
    cjson_add_uint64(root, "wait_gap_cpu_ns_total", ctr->wait_gap_cpu_ns_total);

    cjson_add_uint64(root, "io_worker_samples_total",
                     ctr->io_worker_samples_total);
    cjson_add_uint64(root, "io_worker_busy_total",
                     ctr->io_worker_busy_total);
    cjson_add_uint64(root, "noncmd_cpu_samples_total",
                     ctr->noncmd_cpu_samples_total);

    /* Provider self-metrics. ringbuf_drops_total is the full tier's BPF-side
     * event_ringbuf drop count (A2 wired this; A0 deliberately omitted it). */
    struct pgwt_metrics pm;
    memset(&pm, 0, sizeof(pm));
    if (d->provider && d->provider->self_metrics)
        d->provider->self_metrics((struct pgwt_daemon *)d, &pm);
    cjson_add_uint64(root, "ringbuf_drops_total", pm.ringbuf_drops_total);

    /* Trace writer stats (0 when recording is disabled) */
    cjson_add_uint64(root, "trace_events_written_total",
                     d->event_writer ? d->event_writer->total_events_written : 0);
    cjson_add_uint64(root, "trace_bytes_written_total",
                     d->event_writer ? d->event_writer->total_bytes_written : 0);

    /* Escalation accounting (A4). ESC-9: tier is derived the SAME way as
     * status — a fixed EXACT provider (--mode full) reports "escalated", never
     * a bogus "sampled". */
    cJSON_AddStringToObject(root, "tier", daemon_tier_str(d));
    cJSON_AddBoolToObject(root, "escalation_active", d->escalation.active);
    cJSON_AddNumberToObject(root, "escalation_seconds_remaining",
                            pgwt_escalation_remaining_s(d));
    cJSON_AddNumberToObject(root, "escalation_budget_remaining_s",
                            pgwt_escalation_budget_remaining_s(d));
    cJSON_AddBoolToObject(root, "escalation_budget_unlimited",
                          d->escalation.budget_unlimited);
    cjson_add_uint64(root, "escalation_windows_total",
                     d->escalation.windows_total);
    cjson_add_uint64(root, "escalation_denied_total",
                     d->escalation.denied_total);
    cjson_add_uint64(root, "escalation_budget_closed_total",
                     d->escalation.budget_closed_total);

    /* Anomaly-trigger accounting (A5). 0 when not in tiered mode. */
    cjson_add_uint64(root, "anomaly_fires_total", d->anomaly.fires_total);
    cjson_add_uint64(root, "anomaly_near_total", d->anomaly.near_total);
    cjson_add_uint64(root, "anomaly_dropped_budget_total",
                     d->anomaly.dropped_budget);
    cjson_add_uint64(root, "anomaly_dropped_cooldown_total",
                     d->anomaly.dropped_cooldown);
    cJSON_AddNumberToObject(root, "anomaly_baseline_aas",
                            d->anomaly.baseline_aas);
    cJSON_AddNumberToObject(root, "anomaly_mad_aas",
                            d->anomaly.mad_aas);

    return root;
}

static cJSON *build_error(const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", 0);
    cJSON_AddStringToObject(root, "error", msg);
    return root;
}

/* {"cmd":"escalate","duration_s":N,"reason":"..."} → grant a bounded,
 * budgeted full-fidelity window. Acks with the granted seconds, or denies
 * with a reason (over budget / not tiered / bad args). An escalate while a
 * window is already open EXTENDS its deadline (subject to budget). */
static cJSON *build_escalate(struct pgwt_daemon *d, cJSON *req)
{
    cJSON *dur = cJSON_GetObjectItem(req, "duration_s");
    int duration_s = cJSON_IsNumber(dur) ? (int)dur->valuedouble : 60;

    cJSON *root = cJSON_CreateObject();
    int granted = 0;
    const char *why = NULL;
    if (pgwt_escalate(d, duration_s, PGWT_ESC_REASON_MANUAL,
                      &granted, &why) != 0) {
        cJSON_AddBoolToObject(root, "ok", 0);
        cJSON_AddStringToObject(root, "error",
                                why ? why : "escalation denied");
        cJSON_AddNumberToObject(root, "budget_remaining_s",
                                pgwt_escalation_budget_remaining_s(d));
        return root;
    }
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddBoolToObject(root, "escalated", 1);
    cJSON_AddNumberToObject(root, "granted_s", granted);
    cJSON_AddNumberToObject(root, "seconds_remaining",
                            pgwt_escalation_remaining_s(d));
    cJSON_AddNumberToObject(root, "budget_remaining_s",
                            pgwt_escalation_budget_remaining_s(d));
    return root;
}

/* {"cmd":"deescalate"} → detach watchpoints now (idempotent). */
static cJSON *build_deescalate(struct pgwt_daemon *d)
{
    cJSON *root = cJSON_CreateObject();
    if (!d->escalation.enabled) {
        cJSON_AddBoolToObject(root, "ok", 0);
        cJSON_AddStringToObject(root, "error",
                                "escalation requires --mode tiered");
        return root;
    }
    pgwt_deescalate(d, PGWT_ESC_REASON_REQUEST);
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddBoolToObject(root, "escalated", 0);
    cJSON_AddNumberToObject(root, "budget_remaining_s",
                            pgwt_escalation_budget_remaining_s(d));
    return root;
}

/* ── Client handling ──────────────────────────────────────── */

static void client_drop(struct pgwt_control *c, struct pgwt_control_client *cl)
{
    if (cl->fd < 0)
        return;
    epoll_ctl(c->epoll_fd, EPOLL_CTL_DEL, cl->fd, NULL);
    close(cl->fd);
    cl->fd = -1;
    cl->len = 0;
}

/* Send one JSON object + '\n'. Returns 0 on success, -1 on failure
 * (caller drops the client). MSG_NOSIGNAL: a client that disconnects
 * mid-response must not SIGPIPE the daemon. */
static int client_send_json(struct pgwt_control_client *cl, cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str)
        return -1;

    size_t len = strlen(str);
    str[len] = '\n';  /* overwrite NUL — cJSON allocates len+1 */
    size_t off = 0;
    int rc = 0;
    while (off < len + 1) {
        ssize_t w = send(cl->fd, str + off, len + 1 - off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            /* EAGAIN on a <4KB response means the client stopped
             * reading — drop it rather than block the daemon. */
            rc = -1;
            break;
        }
        off += (size_t)w;
    }
    free(str);
    return rc;
}

/* Handle one complete request line. Returns 0 on success, -1 if the
 * client should be dropped (send failure). */
static int handle_request(struct pgwt_control *c,
                          struct pgwt_control_client *cl, const char *line)
{
    cJSON *req = cJSON_Parse(line);
    if (!req)
        return client_send_json(cl, build_error("invalid json"));

    cJSON *cmd = cJSON_GetObjectItem(req, "cmd");
    cJSON *resp;
    if (!cJSON_IsString(cmd) || !cmd->valuestring)
        resp = build_error("missing cmd");
    else if (strcmp(cmd->valuestring, "status") == 0)
        resp = build_status(c->d);
    else if (strcmp(cmd->valuestring, "metrics") == 0)
        resp = build_metrics(c->d);
    else if (strcmp(cmd->valuestring, "escalate") == 0)
        resp = build_escalate(c->d, req);
    else if (strcmp(cmd->valuestring, "deescalate") == 0)
        resp = build_deescalate(c->d);
    else
        resp = build_error("unknown command");

    cJSON_Delete(req);
    return client_send_json(cl, resp);
}

static void handle_client_readable(struct pgwt_control *c,
                                   struct pgwt_control_client *cl)
{
    for (;;) {
        if (cl->len >= sizeof(cl->buf) - 1) {
            /* No newline within buffer — protocol violation */
            client_send_json(cl, build_error("request too long"));
            client_drop(c, cl);
            return;
        }

        ssize_t r = read(cl->fd, cl->buf + cl->len,
                         sizeof(cl->buf) - 1 - cl->len);
        if (r == 0) {
            /* Client disconnected (possibly mid-request) — discard */
            client_drop(c, cl);
            return;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;  /* drained */
            client_drop(c, cl);
            return;
        }
        cl->len += (size_t)r;

        /* Process complete lines */
        char *nl;
        while ((nl = memchr(cl->buf, '\n', cl->len)) != NULL) {
            *nl = '\0';
            size_t line_len = (size_t)(nl - cl->buf) + 1;

            if (cl->buf[0] != '\0' &&
                handle_request(c, cl, cl->buf) != 0) {
                client_drop(c, cl);
                return;
            }

            memmove(cl->buf, cl->buf + line_len, cl->len - line_len);
            cl->len -= line_len;
        }
    }
}

static void handle_accept(struct pgwt_control *c)
{
    for (;;) {
        int fd = accept4(c->listen_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            return;  /* EAGAIN: done; other errors: nothing to do */
        }

        struct pgwt_control_client *cl = NULL;
        for (int i = 0; i < PGWT_CTRL_MAX_CLIENTS; i++) {
            if (c->clients[i].fd < 0) {
                cl = &c->clients[i];
                break;
            }
        }
        if (!cl) {
            /* All slots busy — refuse politely */
            const char *msg = "{\"ok\":false,\"error\":\"too many clients\"}\n";
            send(fd, msg, strlen(msg), MSG_NOSIGNAL);
            close(fd);
            continue;
        }

        cl->fd = fd;
        cl->len = 0;

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (epoll_ctl(c->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            close(fd);
            cl->fd = -1;
        }
    }
}

/* ── Public API ───────────────────────────────────────────── */

int pgwt_control_init(struct pgwt_control *c, struct pgwt_daemon *d,
                      int epoll_fd)
{
    memset(c, 0, sizeof(*c));
    c->listen_fd = -1;
    c->epoll_fd = epoll_fd;
    c->d = d;
    for (int i = 0; i < PGWT_CTRL_MAX_CLIENTS; i++)
        c->clients[i].fd = -1;

    int n = snprintf(c->sock_path, sizeof(c->sock_path), "%s/pgwt.sock",
                     d->trace_dir);
    struct sockaddr_un addr;
    if (n < 0 || (size_t)n >= sizeof(addr.sun_path)) {
        fprintf(stderr, "WARN: control socket path too long: %s/pgwt.sock\n",
                d->trace_dir);
        return -1;
    }

    /* ESC-10: a plain unlink() lets a second daemon steal a LIVE daemon's
     * control plane (and race for the same trace dir). Liveness-probe first:
     * try to connect to the existing socket. If a daemon answers, refuse
     * startup loudly; only unlink a socket that is stale (connect refused) or a
     * non-socket path. */
    {
        struct stat sb;
        if (lstat(c->sock_path, &sb) == 0) {
            if (S_ISSOCK(sb.st_mode)) {
                int probe = socket(AF_UNIX,
                                   SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (probe >= 0) {
                    struct sockaddr_un pa;
                    memset(&pa, 0, sizeof(pa));
                    pa.sun_family = AF_UNIX;
                    memcpy(pa.sun_path, c->sock_path, (size_t)n + 1);
                    int crc = connect(probe, (struct sockaddr *)&pa,
                                      sizeof(pa));
                    close(probe);
                    if (crc == 0) {
                        fprintf(stderr,
                                "FATAL: another pg_wait_tracer daemon is "
                                "already running on %s\n"
                                "  Refusing to steal its control socket. Stop "
                                "it first, or use a different --trace-dir.\n",
                                c->sock_path);
                        return -2;   /* live-daemon conflict: fatal (ESC-10) */
                    }
                }
            }
            /* Stale socket (or non-socket file) from a crashed daemon. */
            unlink(c->sock_path);
        }
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "WARN: control socket: socket(): %s\n",
                strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, c->sock_path, (size_t)n + 1);

    /* Mode 0600 from the moment of creation (no chmod window) */
    mode_t old_umask = umask(0177);
    int bind_rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old_umask);
    if (bind_rc != 0) {
        fprintf(stderr, "WARN: control socket: bind(%s): %s\n",
                c->sock_path, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 8) != 0) {
        fprintf(stderr, "WARN: control socket: listen(%s): %s\n",
                c->sock_path, strerror(errno));
        close(fd);
        unlink(c->sock_path);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        fprintf(stderr, "WARN: control socket: epoll_ctl: %s\n",
                strerror(errno));
        close(fd);
        unlink(c->sock_path);
        return -1;
    }

    c->listen_fd = fd;
    return 0;
}

int pgwt_control_handle_fd(struct pgwt_control *c, int fd)
{
    if (fd == c->listen_fd) {
        handle_accept(c);
        return 1;
    }
    for (int i = 0; i < PGWT_CTRL_MAX_CLIENTS; i++) {
        if (c->clients[i].fd == fd) {
            handle_client_readable(c, &c->clients[i]);
            return 1;
        }
    }
    return 0;
}

void pgwt_control_cleanup(struct pgwt_control *c)
{
    for (int i = 0; i < PGWT_CTRL_MAX_CLIENTS; i++)
        client_drop(c, &c->clients[i]);
    if (c->listen_fd >= 0) {
        epoll_ctl(c->epoll_fd, EPOLL_CTL_DEL, c->listen_fd, NULL);
        close(c->listen_fd);
        c->listen_fd = -1;
        unlink(c->sock_path);
    }
}
