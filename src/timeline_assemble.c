/* timeline_assemble.c — S0 stub of the timeline wait assembler (#1006).
 *
 * The prototypes in timeline_assemble.h are frozen; S1 replaces these
 * bodies. Every entry point refuses with -ENOSYS (or NULL) so nothing can
 * mistake the stub for an assembler that produced no waits.
 */
#include "timeline_assemble.h"

#include <errno.h>
#include <string.h>

struct pgwt_tl_assembler *pgwt_tl_asm_create(const struct pgwt_tl_asm_config *cfg)
{
    (void)cfg;
    errno = ENOSYS;
    return NULL;
}

void pgwt_tl_asm_destroy(struct pgwt_tl_assembler *a)
{
    (void)a;
}

int pgwt_tl_asm_feed(struct pgwt_tl_assembler *a, const void *rec, size_t size)
{
    (void)a; (void)rec; (void)size;
    return -ENOSYS;
}

int pgwt_tl_asm_flush_idle(struct pgwt_tl_assembler *a, uint64_t now_ns,
                           pgwt_tl_inflight_fn in_flight, void *ctx)
{
    (void)a; (void)now_ns; (void)in_flight; (void)ctx;
    return -ENOSYS;
}

int pgwt_tl_asm_finish(struct pgwt_tl_assembler *a, uint64_t stop_ns,
                       const struct pgwt_tl_inflight *inflight, size_t n_inflight)
{
    (void)a; (void)stop_ns; (void)inflight; (void)n_inflight;
    return -ENOSYS;
}

void pgwt_tl_asm_get_stats(const struct pgwt_tl_assembler *a,
                           struct pgwt_tl_asm_stats *out)
{
    (void)a;
    if (out)
        memset(out, 0, sizeof(*out));
}

int pgwt_tl_is_sleep_syscall(uint32_t sc_id)
{
    (void)sc_id;
    return -ENOSYS;
}
