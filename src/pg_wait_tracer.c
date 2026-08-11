/* pg_wait_tracer.c — Main entry point: argument parsing, discovery, startup */
#include "daemon.h"
#include "discovery.h"
#include "replay.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Target (auto-detect if omitted for single instance):\n"
        "  -p, --pid <PID>       Postmaster PID\n"
        "  -D, --pgdata <DIR>    PGDATA directory (reads postmaster.pid)\n"
        "\n"
        "Output control:\n"
        "  -V, --view <VIEW>     time_model (default), system_event, session_event,\n"
        "                        histogram, query_event, active\n"
        "  -S, --sort <MODE>     Sort for active view: wait_time (default), db_time,\n"
        "                        pid, event\n"
        "  -f, --format <FMT>    tui | text (default: auto-detect TTY)\n"
        "  -i, --interval <SEC>  Refresh interval in seconds (default: 5)\n"
        "  -d, --duration <SEC>  Run for N seconds then exit (default: unlimited)\n"
        "  -n, --count <N>       Print N intervals then exit\n"
        "  -w, --window <W1,W2,W3>  Time windows, e.g. 5s,1m,5m (first = interval)\n"
        "\n"
        "Filters:\n"
        "  -e, --event <NAME>    Event filter (histogram: required; query_event: by event)\n"
        "  -P, --pid-filter <PID> Show detail for specific backend (session_event)\n"
        "  -Q, --query-id <ID>   Filter query_event to one query\n"
        "\n"
        "Recording:\n"
        "  -T, --trace-dir <DIR>      Write raw trace files to DIR\n"
        "  -R, --trace-retention <H>  Keep trace files for H hours (default: 24)\n"
        "      --retention-gb <G>     Also cap total trace-dir size at G GiB\n"
        "                             (oldest archives deleted first; fractional\n"
        "                             values allowed; default: no size cap)\n"
        "      --trace-group <GROUP>  Group for trace file access (default: dba)\n"
        "\n"
        "Replay (offline analysis — no root, no PostgreSQL needed):\n"
        "      --replay               Replay trace files instead of live tracing\n"
        "      --from <TIME>          Start time: ISO 8601, relative (1h, 30m), or 'now'\n"
        "      --to <TIME>            End time (same formats as --from)\n"
        "\n"
        "Daemon:\n"
        "      --daemon               Run as daemon (reconnect on PG restart)\n"
        "\n"
        "Capture tier:\n"
        "      --mode <MODE>          tiered (default) | sampled | full | coop\n"
        "                             tiered: low-overhead always-on sampler; escalate\n"
        "                                     to full fidelity for bounded, budgeted\n"
        "                                     windows (on demand or on anomaly). The\n"
        "                                     default — safe to leave running 24/7.\n"
        "                             sampled: userspace ASH-style sampling, ~zero PG cost.\n"
        "                             full: hardware watchpoints, exact transitions\n"
        "                                   (always-on, 6-30%% overhead).\n"
        "                             coop: cooperative (PG extension) — not available in\n"
        "                                   this build; ships in the extension track.\n"
        "      --sample-rate <HZ>     Sampling rate for sampled/tiered (1-1000, default 10)\n"
        "      --escalation-budget <S>  Tiered: full-fidelity seconds allowed per\n"
        "                             rolling hour (default 300). 0 = escalation\n"
        "                             DISABLED (deny all); 'unlimited' = no cap.\n"
        "                             Escalate via the control socket:\n"
        "                             {\"cmd\":\"escalate\",\"duration_s\":N}.\n"
        "\n"
        "Anomaly triggers (tiered mode — auto-escalate on the sampled stream):\n"
        "      --anomaly-aas-factor <K>   Fire when AAS > K*rolling_baseline (default 3.0)\n"
        "      --anomaly-aas-ticks <N>    ...sustained for N consecutive ticks (default 3)\n"
        "      --anomaly-lock-fraction <F>  Fire when Lock-class share of active\n"
        "                             samples > F (default 0.30), sustained N ticks\n"
        "      --anomaly-lock-min-aas <A>  ...AND lock-class AAS >= A (default 2.0), so a\n"
        "                             single backend's routine lock wait can't escalate\n"
        "      --anomaly-cooldown-s <S>   Min seconds between auto-escalations (default 120)\n"
        "      --anomaly-window-s <S>     Full-fidelity window length per auto-trigger (default 60)\n"
        "      --anomaly-cpu-capacity <C> Override target effective logical CPU cores\n"
        "                             (default: affinity/cgroup discovery)\n"
        "\n"
        "Performance tuning:\n"
        "      --lightweight          BPF accumulator only (no per-event ringbuf).\n"
        "                             Reduces overhead ~40%%, loses histogram/session/query views\n"
        "      --skip-query-id        Skip query_id reads in BPF (saves ~1%% overhead).\n"
        "                             Disables query_event view\n"
        "\n"
        "Other:\n"
        "  -q, --quiet           Suppress view output (daemon mode: record only)\n"
        "  -v, --verbose         Verbose output to stderr\n"
        "  -h, --help            Show this help\n"
        "\n"
        "Examples:\n"
        "  sudo %s                                           # auto-detect instance\n"
        "  sudo %s --pid 12345 --count 1                     # one-shot\n"
        "  sudo %s --view system_event --count 5             # 5 intervals\n"
        "  sudo %s --view histogram --event IO:DataFileRead\n"
        "  sudo %s --window 5s,1m,5m --count 3                 # time windows\n"
        "  sudo %s --count 10 | cat                          # text mode (piped)\n"
        "\n"
        "  # Daemon (reconnects on PG restart):\n"
        "  sudo %s --daemon -T /tmp/traces\n"
        "  sudo %s --daemon --pgdata /var/lib/pgsql/18/data\n"
        "\n"
        "  # Replay (no root needed):\n"
        "  %s --replay -T /tmp/traces --view time_model\n"
        "  %s --replay -T /tmp/traces --from 1h --view system_event\n"
        "  %s --replay -T /tmp/traces --from '2025-02-25T14:00:00' --to '2025-02-25T15:00:00'\n",
        prog, prog, prog, prog, prog, prog, prog,
        prog, prog,
        prog, prog, prog);
}

static enum pgwt_view parse_view(const char *s)
{
    if (strcmp(s, "time_model") == 0)     return PGWT_VIEW_TIME_MODEL;
    if (strcmp(s, "system_event") == 0)   return PGWT_VIEW_SYSTEM_EVENT;
    if (strcmp(s, "session_event") == 0)  return PGWT_VIEW_SESSION_EVENT;
    if (strcmp(s, "histogram") == 0)      return PGWT_VIEW_HISTOGRAM;
    if (strcmp(s, "query_event") == 0)    return PGWT_VIEW_QUERY_EVENT;
    if (strcmp(s, "active") == 0)        return PGWT_VIEW_ACTIVE;
    fprintf(stderr, "ERROR: unknown view '%s'\n", s);
    exit(1);
}

static enum pgwt_sort_mode parse_sort(const char *s)
{
    if (strcmp(s, "wait_time") == 0)  return PGWT_SORT_WAIT_TIME;
    if (strcmp(s, "db_time") == 0)    return PGWT_SORT_DB_TIME;
    if (strcmp(s, "pid") == 0)        return PGWT_SORT_PID;
    if (strcmp(s, "event") == 0)      return PGWT_SORT_EVENT;
    fprintf(stderr, "ERROR: unknown sort mode '%s' (use: wait_time, db_time, pid, event)\n", s);
    exit(1);
}

static int parse_windows(const char *s, int *windows, int *num_windows)
{
    char buf[128];
    int n = 0;

    snprintf(buf, sizeof(buf), "%s", s);

    char *tok = strtok(buf, ",");
    while (tok && n < PGWT_MAX_WINDOWS) {
        char *end;
        long val = strtol(tok, &end, 10);
        if (val <= 0 || end == tok) {
            fprintf(stderr, "ERROR: invalid window value '%s'\n", tok);
            return -1;
        }

        int secs;
        switch (*end) {
        case 's': case '\0': secs = (int)val; break;
        case 'm':            secs = (int)val * 60; break;
        case 'h':            secs = (int)val * 3600; break;
        default:
            fprintf(stderr, "ERROR: invalid window suffix '%c' (use s, m, h)\n", *end);
            return -1;
        }

        if (n > 0 && secs <= windows[n - 1]) {
            fprintf(stderr, "FATAL: windows must be in increasing order (%ds <= %ds)\n",
                    secs, windows[n - 1]);
            return -1;
        }

        windows[n++] = secs;
        tok = strtok(NULL, ",");
    }

    if (n == 0) {
        fprintf(stderr, "ERROR: --window requires at least one value\n");
        return -1;
    }

    *num_windows = n;
    return 0;
}

static enum pgwt_format parse_format(const char *s)
{
    if (strcmp(s, "tui") == 0)   return PGWT_FMT_TUI;
    if (strcmp(s, "text") == 0)  return PGWT_FMT_TEXT;
    if (strcmp(s, "json") == 0)  return PGWT_FMT_JSON;
    if (strcmp(s, "csv") == 0)   return PGWT_FMT_CSV;
    fprintf(stderr, "ERROR: unknown format '%s' (use: tui, text, json, csv)\n", s);
    exit(1);
}

static enum pgwt_mode parse_mode(const char *s)
{
    if (strcmp(s, "full") == 0)     return PGWT_MODE_FULL;
    if (strcmp(s, "sampled") == 0)  return PGWT_MODE_SAMPLED;
    if (strcmp(s, "tiered") == 0)   return PGWT_MODE_TIERED;
    if (strcmp(s, "coop") == 0)     return PGWT_MODE_COOP;
    fprintf(stderr, "ERROR: unknown mode '%s' (use: full, sampled, tiered, coop)\n", s);
    exit(1);
}

/* Long-only option values (no short form) */
#define OPT_REPLAY 256
#define OPT_FROM   257
#define OPT_TO     258
#define OPT_DAEMON       259
#define OPT_TRACE_GROUP  260
#define OPT_LIGHTWEIGHT  261
#define OPT_SKIP_QID     262
#define OPT_SKIP_USDT    263
#define OPT_MODE         264
#define OPT_SAMPLE_RATE  265
#define OPT_ESC_BUDGET   266
#define OPT_ANOM_AAS_FACTOR 267
#define OPT_ANOM_AAS_TICKS  268
#define OPT_ANOM_LOCK_FRAC  269
#define OPT_ANOM_COOLDOWN   270
#define OPT_ANOM_WINDOW     271
#define OPT_RETENTION_GB    272
#define OPT_ANOM_LOCK_MIN_AAS 273
#define OPT_ANOM_CPU_CAPACITY 274

static struct option long_opts[] = {
    {"pid",        required_argument, NULL, 'p'},
    {"pgdata",     required_argument, NULL, 'D'},
    {"interval",   required_argument, NULL, 'i'},
    {"duration",   required_argument, NULL, 'd'},
    {"view",       required_argument, NULL, 'V'},
    {"format",     required_argument, NULL, 'f'},
    {"count",      required_argument, NULL, 'n'},
    {"window",     required_argument, NULL, 'w'},
    {"event",      required_argument, NULL, 'e'},
    {"pid-filter", required_argument, NULL, 'P'},
    {"query-id",   required_argument, NULL, 'Q'},
    {"sort",            required_argument, NULL, 'S'},
    {"trace-dir",       required_argument, NULL, 'T'},
    {"trace-retention", required_argument, NULL, 'R'},
    {"retention-gb",    required_argument, NULL, OPT_RETENTION_GB},
    {"replay",          no_argument,       NULL, OPT_REPLAY},
    {"from",            required_argument, NULL, OPT_FROM},
    {"to",              required_argument, NULL, OPT_TO},
    {"daemon",          no_argument,       NULL, OPT_DAEMON},
    {"trace-group",     required_argument, NULL, OPT_TRACE_GROUP},
    {"lightweight",     no_argument,       NULL, OPT_LIGHTWEIGHT},
    {"skip-query-id",   no_argument,       NULL, OPT_SKIP_QID},
    {"skip-usdt",       no_argument,       NULL, OPT_SKIP_USDT},
    {"mode",            required_argument, NULL, OPT_MODE},
    {"sample-rate",     required_argument, NULL, OPT_SAMPLE_RATE},
    {"escalation-budget", required_argument, NULL, OPT_ESC_BUDGET},
    {"anomaly-aas-factor",  required_argument, NULL, OPT_ANOM_AAS_FACTOR},
    {"anomaly-aas-ticks",   required_argument, NULL, OPT_ANOM_AAS_TICKS},
    {"anomaly-lock-fraction", required_argument, NULL, OPT_ANOM_LOCK_FRAC},
    {"anomaly-lock-min-aas", required_argument, NULL, OPT_ANOM_LOCK_MIN_AAS},
    {"anomaly-cooldown-s",  required_argument, NULL, OPT_ANOM_COOLDOWN},
    {"anomaly-window-s",    required_argument, NULL, OPT_ANOM_WINDOW},
    {"anomaly-cpu-capacity", required_argument, NULL, OPT_ANOM_CPU_CAPACITY},
    {"quiet",           no_argument,       NULL, 'q'},
    {"verbose",         no_argument,       NULL, 'v'},
    {"help",            no_argument,       NULL, 'h'},
    {NULL, 0, NULL, 0},
};

int main(int argc, char **argv)
{
    /* Heap-allocate: pgwt_daemon is ~27 MB due to accumulator arrays */
    struct pgwt_daemon *d = calloc(1, sizeof(*d));
    if (!d) {
        fprintf(stderr, "FATAL: cannot allocate daemon state\n");
        return 1;
    }
    d->epoll_fd  = -1;
    d->timer_fd  = -1;
    d->sample_timer_fd = -1;
    d->signal_fd = -1;
    d->interval  = 5;
    d->view      = PGWT_VIEW_TIME_MODEL;
    d->mode      = PGWT_MODE_TIERED; /* default: low-overhead always-on sampler
                                        with on-demand/anomaly full-fidelity
                                        escalation. Force exact watchpoints with
                                        --mode full. */
    d->sample_rate_hz = 10;          /* default sampled rate (D1) */
    d->escalation_budget_s = 300;    /* tiered: full-fidelity s / rolling hour (D5) */
    /* Anomaly triggers (A5): negative sentinels = "use the built-in default"
     * (pgwt_anomaly_init derives them); only an explicit flag overrides. */
    d->anomaly_aas_factor    = -1.0;
    d->anomaly_aas_ticks     = -1;
    d->anomaly_lock_fraction = -1.0;
    d->anomaly_lock_min_aas  = -1.0;
    d->anomaly_cooldown_s    = -1;

    pid_t pm_pid = 0;
    const char *pgdata = NULL;
    bool format_set = false;
    bool view_set = false;
    bool replay_mode = false;
    bool daemon_mode = false;
    const char *from_str = NULL;
    const char *to_str = NULL;
    int opt;

    while ((opt = getopt_long(argc, argv, "p:D:i:d:V:f:n:w:e:P:Q:S:T:R:qvh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': pm_pid = atoi(optarg); break;
        case 'D': pgdata = optarg; break;
        case 'i': d->interval = atoi(optarg); break;
        case 'd': d->duration = atoi(optarg); break;
        case 'V': d->view = parse_view(optarg); view_set = true; break;
        case 'f': d->format = parse_format(optarg); format_set = true; break;
        case 'n': d->count = atoi(optarg); break;
        case 'w':
            if (parse_windows(optarg, d->windows, &d->num_windows) != 0) {
                free(d);
                return 1;
            }
            break;
        case 'e': d->event_filter = optarg; break;
        case 'P': d->pid_filter = atoi(optarg); break;
        case 'Q': d->query_id_filter = strtoull(optarg, NULL, 10); break;
        case 'S': d->sort_mode = parse_sort(optarg); break;
        case 'T': d->trace_dir = optarg; break;
        case 'R': d->trace_retention = atoi(optarg); break;
        case OPT_RETENTION_GB: d->trace_retention_gb = atof(optarg); break;
        case OPT_REPLAY: replay_mode = true; break;
        case OPT_FROM:   from_str = optarg; break;
        case OPT_TO:     to_str = optarg; break;
        case OPT_DAEMON: daemon_mode = true; break;
        case OPT_TRACE_GROUP: d->trace_group = optarg; break;
        case OPT_LIGHTWEIGHT: d->lightweight_mode = 1; break;
        case OPT_SKIP_QID:    d->skip_query_id = 1; break;
        case OPT_SKIP_USDT:   d->skip_usdt = 1; break;
        case OPT_MODE:        d->mode = parse_mode(optarg); break;
        case OPT_SAMPLE_RATE: d->sample_rate_hz = atoi(optarg); break;
        case OPT_ESC_BUDGET:
            /* ESC-6: "unlimited" (or a negative value) = no cap; 0 = deny-all;
             * >0 = seconds/rolling-hour. Stored as -1 for unlimited. */
            if (strcasecmp(optarg, "unlimited") == 0)
                d->escalation_budget_s = -1;
            else
                d->escalation_budget_s = atoi(optarg);
            break;
        case OPT_ANOM_AAS_FACTOR: d->anomaly_aas_factor = atof(optarg); break;
        case OPT_ANOM_AAS_TICKS:  d->anomaly_aas_ticks = atoi(optarg); break;
        case OPT_ANOM_LOCK_FRAC:  d->anomaly_lock_fraction = atof(optarg); break;
        case OPT_ANOM_LOCK_MIN_AAS: d->anomaly_lock_min_aas = atof(optarg); break;
        case OPT_ANOM_COOLDOWN:   d->anomaly_cooldown_s = atoi(optarg); break;
        case OPT_ANOM_WINDOW:     d->anomaly_window_s = atoi(optarg); break;
        case OPT_ANOM_CPU_CAPACITY: {
            char *end = NULL;
            errno = 0;
            double cores = strtod(optarg, &end);
            if (errno || end == optarg || *end != '\0' ||
                !isfinite(cores) || cores <= 0.0) {
                fprintf(stderr, "FATAL: --anomaly-cpu-capacity must be a "
                        "finite value > 0 (got '%s')\n", optarg);
                free(d);
                return 1;
            }
            d->anomaly_cpu_capacity = cores;
            break;
        }
        case 'q': d->quiet = true; break;
        case 'v': d->verbose = true; break;
        case 'h': usage(argv[0]); free(d); return 0;
        default:  usage(argv[0]); free(d); return 1;
        }
    }

    /* Default trace group to "dba" when trace recording is enabled */
    if (d->trace_dir && !d->trace_group)
        d->trace_group = "dba";

    /* TTY auto-detect: tui for terminal, text for pipe */
    if (!format_set)
        d->format = isatty(STDOUT_FILENO) ? PGWT_FMT_TUI : PGWT_FMT_TEXT;

    /* Replay mode: bypass all BPF/PostgreSQL discovery */
    if (replay_mode) {
        d->format = PGWT_FMT_TEXT;  /* replay always text mode */
        int rc = pgwt_run_replay(d, from_str, to_str);
        free(d);
        return rc;
    }

    /* Validate options (before PG discovery) */
    if (d->interval < 1) {
        fprintf(stderr, "FATAL: interval must be >= 1 second\n");
        free(d);
        return 1;
    }
    if (d->num_windows > 0 && d->windows[0] != d->interval) {
        fprintf(stderr, "FATAL: first window (%ds) must equal --interval (%ds)\n",
                d->windows[0], d->interval);
        free(d);
        return 1;
    }
    if (daemon_mode && (d->count > 0 || d->duration > 0)) {
        fprintf(stderr, "FATAL: --daemon cannot be used with --count or --duration\n");
        free(d);
        return 1;
    }

    /* Validate view-specific options */
    if (d->view == PGWT_VIEW_HISTOGRAM && (!d->event_filter || !d->event_filter[0])) {
        fprintf(stderr, "FATAL: histogram view requires --event <NAME>\n");
        free(d);
        return 1;
    }

    /* Validate: --lightweight cannot be used with views that need per-event data */
    if (d->lightweight_mode &&
        (d->view == PGWT_VIEW_HISTOGRAM ||
         d->view == PGWT_VIEW_SESSION_EVENT ||
         d->view == PGWT_VIEW_QUERY_EVENT)) {
        fprintf(stderr, "ERROR: --lightweight is incompatible with %s view "
                "(requires per-event data)\n",
                d->view == PGWT_VIEW_HISTOGRAM ? "histogram" :
                d->view == PGWT_VIEW_SESSION_EVENT ? "session_event" : "query_event");
        free(d);
        return 1;
    }

    /* Validate: --skip-query-id cannot be used with query_event view */
    if (d->skip_query_id && d->view == PGWT_VIEW_QUERY_EVENT) {
        fprintf(stderr, "ERROR: --skip-query-id is incompatible with query_event view\n");
        free(d);
        return 1;
    }

    /* Validate: --sample-rate range (only meaningful for sampled/tiered) */
    if (d->sample_rate_hz < 1 || d->sample_rate_hz > 1000) {
        fprintf(stderr, "FATAL: --sample-rate must be 1..1000 Hz (got %d)\n",
                d->sample_rate_hz);
        free(d);
        return 1;
    }

    /* Validate: --escalation-budget (tiered only). ESC-6 semantics: >0 =
     * seconds/rolling-hour; 0 = deny-all (escalation disabled); -1 =
     * "unlimited" (parsed above). Anything below -1 is a typo. */
    if (d->escalation_budget_s < -1) {
        fprintf(stderr, "FATAL: --escalation-budget must be >= 0, or "
                "'unlimited' (got %d)\n", d->escalation_budget_s);
        free(d);
        return 1;
    }

    /* Validate: --anomaly-* (tiered only; ignored otherwise). Negative =
     * "unset, use default", so only reject out-of-range explicit values. */
    if (d->anomaly_aas_factor == 0.0) {
        fprintf(stderr, "FATAL: --anomaly-aas-factor must be > 0\n");
        free(d);
        return 1;
    }
    if (d->anomaly_aas_ticks == 0) {
        fprintf(stderr, "FATAL: --anomaly-aas-ticks must be >= 1\n");
        free(d);
        return 1;
    }
    if (d->anomaly_lock_fraction > 1.0) {
        fprintf(stderr, "FATAL: --anomaly-lock-fraction must be in (0, 1]\n");
        free(d);
        return 1;
    }
    if (d->anomaly_lock_min_aas == 0.0) {
        fprintf(stderr, "FATAL: --anomaly-lock-min-aas must be > 0 "
                "(use --anomaly-lock-fraction alone to disable the floor)\n");
        free(d);
        return 1;
    }

    /* Validate: any non-full mode is incompatible with --lightweight
     * (lightweight is a watchpoint-only BPF-accumulator mode). */
    if (d->mode != PGWT_MODE_FULL && d->lightweight_mode) {
        const char *mname = d->mode == PGWT_MODE_SAMPLED ? "sampled"
                          : d->mode == PGWT_MODE_TIERED  ? "tiered"
                          : "coop";
        fprintf(stderr, "ERROR: --mode %s is incompatible with --lightweight\n",
                mname);
        free(d);
        return 1;
    }

    /* Tiered (A4) is the default mode: sampler always-on; full-fidelity
     * escalation on demand via the control socket, bounded by
     * --escalation-budget per rolling hour. Use --mode full for always-on
     * exact watchpoint tracing. */
    if (d->mode == PGWT_MODE_TIERED) {
        char budstr[32];
        if (d->escalation_budget_s < 0)
            snprintf(budstr, sizeof(budstr), "unlimited");
        else if (d->escalation_budget_s == 0)
            snprintf(budstr, sizeof(budstr), "0 — escalation DISABLED");
        else
            snprintf(budstr, sizeof(budstr), "%ds/hour",
                     d->escalation_budget_s);
        fprintf(stderr, "INFO: --mode tiered (default): sampler always-on; "
                "on-demand full-fidelity escalation (budget %s). "
                "Use --mode full for always-on exact tracing.\n", budstr);
    }

    /* Check root */
    if (geteuid() != 0) {
        fprintf(stderr, "FATAL: must run as root (hardware watchpoints require CAP_SYS_ADMIN)\n");
        free(d);
        return 1;
    }

    d->daemon_mode = daemon_mode;

    /* Daemon mode defaults to quiet unless user explicitly set --view */
    if (daemon_mode && !view_set)
        d->quiet = true;

    if (d->verbose) {
        if (d->lightweight_mode)
            fprintf(stderr, "INFO: lightweight BPF mode (no ringbuf)\n");
        if (d->skip_query_id)
            fprintf(stderr, "INFO: skipping query_id reads in BPF\n");
    }

    /* Set up discovery state: pgdata, pre-set PID, or auto-discover.
     * pgwt_discover() uses: d->pgdata (if set) > d->postmaster_pid (if set) > auto. */
    if (pgdata)
        snprintf(d->pgdata, sizeof(d->pgdata), "%s", pgdata);
    if (pm_pid > 0)
        d->postmaster_pid = pm_pid;

    /* In daemon mode with --pid but no --pgdata, infer PGDATA for restart detection */
    if (daemon_mode && pm_pid > 0 && !pgdata) {
        char inferred[512];
        if (pgwt_infer_pgdata(pm_pid, inferred, sizeof(inferred)) == 0) {
            snprintf(d->pgdata, sizeof(d->pgdata), "%s", inferred);
            if (d->verbose)
                fprintf(stderr, "INFO: inferred PGDATA: %s\n", d->pgdata);
        } else {
            fprintf(stderr, "WARN: cannot infer PGDATA from PID %d — "
                    "restart detection may not work\n", pm_pid);
        }
    }

    /* First discovery */
    if (pgwt_discover(d) != 0) {
        if (!pgdata && pm_pid == 0)
            fprintf(stderr, "\nUse --pid <PID> or --pgdata <DIR>\n");
        free(d);
        return 1;
    }
    /* pg_binary_saved is set by pgwt_discover via strdup (heap-allocated) */

    /* ── Daemon mode: supervision loop ─────────────────────── */
    if (daemon_mode) {
        int rc = 0;

        /* Block signals for sigtimedwait in wait phase.
         * pgwt_daemon_init() will also call sigprocmask (idempotent). */
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigprocmask(SIG_BLOCK, &mask, NULL);

        for (;;) {
            if (pgwt_daemon_init(d) != 0) {
                rc = 1;
                break;
            }

            pgwt_daemon_run(d);
            pgwt_daemon_cleanup(d);

            if (d->exit_reason != PGWT_EXIT_PG_DEAD)
                break;

            /* Wait for PG restart */
            fprintf(stderr, "pg_wait_tracer: waiting for PostgreSQL to restart...\n");
            bool found = false;
            while (!found) {
                struct timespec ts = { .tv_sec = 5 };
                int sig = sigtimedwait(&mask, NULL, &ts);
                if (sig > 0) {
                    fprintf(stderr, "\npg_wait_tracer: shutting down\n");
                    goto done;
                }
                /* sigtimedwait returns -1/EAGAIN on timeout — try discover */

                /* Clear pre-set PID so discover uses pgdata or auto */
                d->postmaster_pid = 0;

                if (pgwt_discover(d) == 0) {
                    /* pg_binary_saved updated inside pgwt_discover */
                    fprintf(stderr, "pg_wait_tracer: PostgreSQL restarted (PID %d), "
                            "re-attaching\n", d->postmaster_pid);
                    found = true;
                }
            }

            /* Reset per-cycle state */
            d->tick = 0;
            d->exit_reason = PGWT_EXIT_NORMAL;
        }

done:
        free(d);
        return rc;
    }

    /* ── Single-shot mode ──────────────────────────────────── */
    if (pgwt_daemon_init(d) != 0) {
        free(d);
        return 1;
    }

    pgwt_daemon_run(d);
    pgwt_daemon_cleanup(d);
    free(d);
    return 0;
}
