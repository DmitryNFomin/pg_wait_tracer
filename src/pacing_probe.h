/* BPF-free version parsing and pacing-probe liveness verdict. Refs #804. */
#ifndef PGWT_PACING_PROBE_H
#define PGWT_PACING_PROBE_H
#include <stdint.h>
/* Five independent 1-second observations avoid treating a single attach or
 * scheduler boundary as inertness, while refusing within a spread checkpoint.
 * Observations need not be consecutive; any matched fire proves capability
 * for the entire daemon run. */
#define PGWT_PACING_INERT_THRESHOLD 5U
enum pgwt_pacing_verdict { PGWT_PACING_OK, PGWT_PACING_INERT };
void pgwt_parse_pg_version(const char *line, int *major, int *minor);
/* Unknown minor (-1) on 14, including beta/rc forms, is NOT applicable.
 * PG12 is outside the supported capture range; do not attach there either. */
int pgwt_needs_ckpt_pacing_probe(int major, int minor);
enum pgwt_pacing_verdict pgwt_pacing_probe_verdict(uint64_t total_fires,
    uint64_t unclassified_asleep_observations, unsigned threshold);
struct pgwt_daemon;
struct pgwt_backend;
int pgwt_pacing_probe_attach(struct pgwt_daemon *d);
int pgwt_pacing_register_backend(struct pgwt_daemon *d, struct pgwt_backend *be);
void pgwt_pacing_probe_check(struct pgwt_daemon *d);
#endif
