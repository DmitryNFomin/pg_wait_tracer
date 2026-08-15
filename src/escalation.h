/* escalation.h — Tiered-mode escalation engine (Track A, D5)
 *
 * In --mode tiered the sampled provider runs always-on; full-fidelity
 * hardware-watchpoint capture is escalated for BOUNDED, BUDGETED windows on
 * demand (manually via the control socket in A4; on anomaly in A5). This
 * module owns the escalation lifecycle:
 *
 *   - escalate(): begin a generation, atomically attach the exact query and
 *     activity probe bundle, coherently preseed command/query state, synthesize
 *     command starts for straddlers, arm watchpoints across the existing
 *     backend registry, and publish a START marker last.
 *   - bounded windows: every escalation carries a hard deadline (timerfd on
 *     the daemon epoll loop); on expiry watchpoints are detached even if the
 *     requester vanished.
 *   - budget: a rolling-hour accounting of consumed full-fidelity seconds.
 *     A manual escalate that would exceed --escalation-budget is DENIED with a
 *     reason; anomaly triggers (A5) will consult the same budget silently.
 *
 * The sampler keeps running through an escalation window (stopping it would
 * risk gaps if escalation crashes); A3's exact-wins merge de-dupes the
 * overlap at read time.
 *
 * Fork/exit tracepoints keep the registry current in every tier. On validated
 * PG14-18 the query/activity bundle is detached while sampled and is owned by
 * the exact generation; PG13/degraded attribution links remain pinned. A
 * backend forked mid-escalation is generation-seeded before its watchpoint is
 * armed via the normal fork->bootstrap->init path.
 */
#ifndef PGWT_ESCALATION_H
#define PGWT_ESCALATION_H

#include <stdbool.h>
#include <stdint.h>

struct pgwt_daemon;

/* Rolling-hour budget ledger. Each closed window appends one segment; the
 * accounting drops segments older than the window when computing consumed
 * seconds, giving a true rolling-hour figure without a per-second ring. */
#define PGWT_ESC_LEDGER_MAX 256

struct pgwt_esc_segment {
    uint64_t start_ns;   /* monotonic window open */
    uint64_t end_ns;     /* monotonic window close (0 = still open) */
};

struct pgwt_escalation {
    bool      enabled;            /* true only in --mode tiered */
    bool      active;             /* currently escalated (watchpoints armed) */
    bool      admitting;          /* new backends may join the exact window */

    uint64_t  generation;         /* current exact generation (0 = none) */
    uint64_t  next_generation;    /* monotonically increasing, never reused */
    uint64_t  attach_boundary_ns; /* generation boundary before link attach */

    int       timer_fd;          /* deadline timerfd on the daemon epoll; -1 */
    uint64_t  window_deadline_ns; /* monotonic deadline of the active window */
    uint64_t  window_start_ns;    /* monotonic open time of the active window */
    int       window_reason;      /* enum pgwt_escalation_reason of active window */

    /* Budget: full-fidelity seconds allowed per rolling hour. */
    uint64_t  budget_ns;          /* --escalation-budget converted to ns (0 = deny-all) */
    bool      budget_unlimited;   /* --escalation-budget unlimited/-1: no cap (ESC-6) */
    uint64_t  rolling_window_ns;  /* the "per hour" window (3600s) */

    /* Ledger of closed (and the one open) segments for rolling accounting. */
    struct pgwt_esc_segment ledger[PGWT_ESC_LEDGER_MAX];
    int       ledger_count;

    /* Lifetime stats (control-socket metrics). */
    uint64_t  windows_total;      /* escalation windows opened */
    uint64_t  denied_total;       /* manual escalates denied (over budget) */
    uint64_t  budget_closed_total;/* windows closed mid-flight by the budget clamp (ESC-1) */
};

/* Initialize the escalation engine. budget_s is --escalation-budget (full
 * seconds per rolling hour). Creates the deadline timerfd and registers it
 * with the daemon epoll. enabled is set only for PGWT_MODE_TIERED. Returns 0
 * on success, -1 on a fatal setup error. */
int pgwt_escalation_init(struct pgwt_daemon *d, int budget_s);

/* Free the deadline timerfd. If a window is open, it is closed (watchpoints
 * detached, END marker written with the SHUTDOWN reason). */
void pgwt_escalation_cleanup(struct pgwt_daemon *d);

/* Request an escalation for duration_s seconds with the given reason. On
 * success *granted_s receives the actual granted window length and the engine
 * is escalated; returns 0. On denial returns -1 and *why (if non-NULL) points
 * to a static reason string. If already escalated the deadline is EXTENDED to
 * max(current, now+duration) when budget allows. Budget is enforced for every
 * caller (manual and anomaly). */
int pgwt_escalate(struct pgwt_daemon *d, int duration_s, int reason,
                  int *granted_s, const char **why);

/* Detach now (idempotent). reason is recorded in the END marker. */
void pgwt_deescalate(struct pgwt_daemon *d, int reason);

/* T8: flush every OPEN exact interval (wait or on-CPU) as a final transition
 * carrying its measured cpu_ns. Called at daemon shutdown so a command
 * straddling capture end (esp. a pure-CPU command in --mode full, which fires
 * no wait boundary) records its on-CPU stretch instead of vanishing. */
void pgwt_flush_open_intervals(struct pgwt_daemon *d);

/* Called when the deadline timerfd fires: closes the window if expired. */
void pgwt_escalation_on_timer(struct pgwt_daemon *d);

/* Mid-window budget enforcement (ESC-1): if an active window's rolling-hour
 * consumption has reached the budget, de-escalate now. Cheap no-op when not
 * escalated / unlimited. Called from the daemon's periodic timer. */
void pgwt_escalation_check_budget(struct pgwt_daemon *d);

/* True if fd belongs to the escalation deadline timer (drives dispatch). */
bool pgwt_escalation_is_timer_fd(const struct pgwt_daemon *d, int fd);

/* Seconds remaining in the active window (0 if not escalated). */
double pgwt_escalation_remaining_s(const struct pgwt_daemon *d);

/* Full-fidelity seconds still available in the current rolling hour. */
double pgwt_escalation_budget_remaining_s(const struct pgwt_daemon *d);

#endif /* PGWT_ESCALATION_H */
