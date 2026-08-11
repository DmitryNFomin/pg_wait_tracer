/* provider.h — Capture provider interface (Track A, A.0 contract)
 *
 * One interface, multiple implementations, one trace schema. A provider
 * owns a capture *tier*: how wait events are observed and turned into
 * trace blocks. The daemon's lifecycle/registry machinery (fork/exit BPF
 * tracepoints, the query_id uprobe, the backend table) is shared; the
 * provider plugs into start/stop/poll and reports its own metrics.
 *
 *   full     — hardware watchpoints + BPF (EXACT fidelity): every
 *              wait_event_info transition becomes a TRANSITIONS record.
 *              This is the original, default behavior.
 *   sampled  — pure userspace process_vm_readv() at a fixed rate
 *              (SAMPLED fidelity): each tick is one point observation per
 *              tracked backend, written as SAMPLES records. No BPF in the
 *              per-sample hot path, ~zero cost on PostgreSQL.
 *   coop*    — cooperative (PG extension) — interface frozen here, filled
 *              in by the extension track (A6).
 *
 * The provider does NOT own backend discovery: in every mode the daemon
 * keeps the fork/exit tracepoints + query_id uprobe running to maintain
 * the registry (cheap, not in the sampler's hot path). A provider's
 * start() arms its capture mechanism; stop() disarms it; poll() drains
 * whatever it has captured into the trace writer.
 */
#ifndef PGWT_PROVIDER_H
#define PGWT_PROVIDER_H

#include <stdint.h>

struct pgwt_daemon;
struct pgwt_metrics;

/* Capture fidelity — what a tier's records mean. SAMPLED records are point
 * observations (worth one sample_period_ns each); EXACT records are
 * transition intervals with real durations. A3's compute layer keys view
 * availability off this. */
enum pgwt_fidelity {
    PGWT_FIDELITY_SAMPLED = 0,
    PGWT_FIDELITY_EXACT   = 1,
};

/* Provider-reported self-metrics, surfaced via the control socket. Filled
 * by self_metrics(); the control layer copies the live fields it wants.
 * Kept separate from pgwt_counters so a provider can report tier-specific
 * numbers without bloating the shared counter struct. */
struct pgwt_metrics {
    uint64_t samples_total;       /* SAMPLES records written (sampled tier) */
    double   samples_per_sec;     /* recent sample rate */
    uint64_t sample_read_faults;  /* process_vm_readv partial/EFAULT fallbacks */
    uint64_t sample_read_failures_total; /* targets with no successful read */
    uint32_t sample_read_targets; /* targets attempted on the latest tick */
    uint32_t sample_read_valid;   /* successful target reads on latest tick */
    uint32_t sample_read_invalid; /* failed target reads on latest tick */
    uint64_t ringbuf_drops_total; /* event_ringbuf drops (full tier, BPF-side) */
};

/* Capture provider vtable. All function pointers operate on the daemon;
 * a provider stores its private state inside struct pgwt_daemon (sampler
 * state, BPF skeleton, etc.) — there is exactly one daemon per process.
 * Any pointer may be NULL (treated as a no-op success) except poll() for
 * a provider that produces data. */
struct pgwt_capture_provider {
    const char *name;                 /* "full" | "sampled" | "coop" */
    enum pgwt_fidelity fidelity;

    /* Arm capture. Called once after pgwt_daemon_init() wiring is done.
     * Returns 0 on success, -1 on fatal error. */
    int  (*start)(struct pgwt_daemon *d);

    /* Disarm capture (detach watchpoints, stop the sampler). Idempotent. */
    int  (*stop)(struct pgwt_daemon *d);

    /* Drain captured data into the trace writer. Called from the event
     * loop: for the full tier this is the ringbuf consume; for the
     * sampled tier this is the per-tick read+encode. Returns 0 on success. */
    int  (*poll)(struct pgwt_daemon *d);

    /* Fill provider-specific metrics. May be NULL. */
    void (*self_metrics)(struct pgwt_daemon *d, struct pgwt_metrics *m);
};

/* Daemon capture mode (set from --mode, default full). */
enum pgwt_mode {
    PGWT_MODE_FULL = 0,    /* watchpoints only — today's default */
    PGWT_MODE_SAMPLED,     /* userspace sampler only, no watchpoints */
    PGWT_MODE_TIERED,      /* sampler always on; escalation lands in A4 */
    PGWT_MODE_COOP,        /* cooperative (PG extension) — A6 interface freeze,
                              implemented in the extension track. The stub
                              recognizes the mode but refuses activation with
                              a clean "not available in this build" message. */
};

/* The two real providers (A2). coop (A6) is an interface-frozen stub; its
 * vtable lives in provider_coop.h. */
extern const struct pgwt_capture_provider pgwt_provider_full;
extern const struct pgwt_capture_provider pgwt_provider_sampled;

#endif /* PGWT_PROVIDER_H */
