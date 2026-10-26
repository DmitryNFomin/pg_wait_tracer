/* perf_event.h — perf_event_open wrapper for hardware watchpoints */
#ifndef PGWT_PERF_EVENT_H
#define PGWT_PERF_EVENT_H

#include <sys/types.h>
#include <stdint.h>

/* Open a hardware write-watchpoint on addr for pid, attach bpf_prog_fd,
 * and ENABLE it immediately.
 * Returns perf_event fd (>= 0) on success, -1 on error (errno set). */
int pgwt_open_watchpoint(pid_t pid, uint64_t addr, int bpf_prog_fd);

/* Same, but the watchpoint is created DISABLED (CAP-8): the caller preseeds
 * the BPF state_map with the backend's current wait state FIRST, then calls
 * pgwt_watchpoint_enable(). Opening enabled before the preseed left a window
 * where a transition fired against an unseeded/stale state entry, mis-timing
 * the opening interval (re-exercised on every escalation). */
int pgwt_open_watchpoint_disabled(pid_t pid, uint64_t addr, int bpf_prog_fd);

/* Enable a watchpoint opened with pgwt_open_watchpoint_disabled().
 * Returns 0 on success, -1 on error (errno set). */
int pgwt_watchpoint_enable(int perf_fd);

/* Disable without closing. De-escalation first disables every watchpoint,
 * establishing a finite event boundary, then drains and flushes before the
 * fds are finally detached. */
int pgwt_watchpoint_disable(int perf_fd);

/* Close a watchpoint fd (auto-detaches the hardware breakpoint). */
void pgwt_close_watchpoint(int perf_fd);

/* #390: read this watchpoint's CUMULATIVE time_enabled/time_running (ns,
 * since the perf_event was created — PERF_EVENT_IOC_RESET is never called).
 * SET_BPF leaves no ring buffer and nothing ever sees PERF_RECORD_THROTTLE,
 * so a time_running shortfall against time_enabled is the only available
 * signal that the kernel rate-limited this watchpoint and it stopped
 * firing for part of that span. Returns 0 and fills both outputs on
 * success; -1 (both outputs zeroed) on a bad fd or an unexpected read
 * size. Caller tracks the previous cumulative pair to derive a per-tick
 * delta; this function does not. */
int pgwt_watchpoint_read_times(int perf_fd, uint64_t *time_enabled,
                               uint64_t *time_running);

#endif /* PGWT_PERF_EVENT_H */
