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
/* Attach only for a confidently parsed major == 13. PG14.0-14.2 is deferred
 * to #830 (exact PG14/15/16 Timeout tables): the tracer's native PG14 id 1
 * and synthetic id 30 share "CheckpointWriteDelay" but have opposite
 * accounting (native counted, synthetic excluded). A failed parse leaving
 * major 0 is not applicable: no bridge is attached and
 * the checkpointer's 100 ms pg_usleep is COUNTED as DB Time. This visible
 * over-counting is the safe failure per idle_rule.h; silently under-counting
 * is not. No parse failure can make this predicate exclude time that the
 * native path would count. PG12 is outside the supported capture range. */
int pgwt_needs_ckpt_pacing_probe(int major, int minor);
enum pgwt_pacing_verdict pgwt_pacing_probe_verdict(uint64_t total_fires,
    uint64_t unclassified_asleep_observations, unsigned threshold);
struct pgwt_daemon;
struct pgwt_backend;
int pgwt_pacing_probe_attach(struct pgwt_daemon *d);
int pgwt_pacing_register_backend(struct pgwt_daemon *d, struct pgwt_backend *be);
void pgwt_pacing_probe_check(struct pgwt_daemon *d);
void pgwt_pacing_probe_finish(struct pgwt_daemon *d);
#endif
