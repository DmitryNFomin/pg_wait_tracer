/* provider_full.c — Full (watchpoint) capture provider (Track A, A.0)
 *
 * Wraps the original hardware-watchpoint + BPF path behind the provider
 * interface with NO behavioral change. The watchpoint attach/detach
 * lifecycle stays in backend.c (driven by the fork/exit tracepoints and
 * pgwt_scan_existing_backends); this provider owns the runtime drain of the
 * event_ringbuf and the full-tier self-metrics.
 *
 * start/stop are no-ops: the watchpoints are armed during pgwt_daemon_init
 * (scan_existing_backends) and as backends fork, exactly as before. poll()
 * is the event_ringbuf consume that daemon.c used to call inline.
 */
#include "provider.h"
#include "daemon.h"
#include "ringbuf_metrics.h"

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

static int full_start(struct pgwt_daemon *d)
{
    (void)d;
    return 0;  /* watchpoints armed in daemon_init / on fork — unchanged */
}

static int full_stop(struct pgwt_daemon *d)
{
    (void)d;
    return 0;  /* watchpoints closed in pgwt_close_all_backends — unchanged */
}

static int full_poll(struct pgwt_daemon *d)
{
    if (d->event_rb)
        ring_buffer__consume(d->event_rb);
    return 0;
}

/* Read aggregate event-ring drops and per-site failures across all rings.
 * Both maps are cumulative per-CPU arrays; sum every possible CPU. */
static void full_metrics(struct pgwt_daemon *d, struct pgwt_metrics *m)
{
    if (!d->skel)
        return;
    pgwt_read_ringbuf_metrics(m, bpf_map__fd(d->skel->maps.ringbuf_drops),
                             bpf_map__fd(d->skel->maps.ringbuf_site_drops),
                             libbpf_num_possible_cpus(), bpf_map_lookup_elem);
}

const struct pgwt_capture_provider pgwt_provider_full = {
    .name         = "full",
    .fidelity     = PGWT_FIDELITY_EXACT,
    .start        = full_start,
    .stop         = full_stop,
    .poll         = full_poll,
    .self_metrics = full_metrics,
};
