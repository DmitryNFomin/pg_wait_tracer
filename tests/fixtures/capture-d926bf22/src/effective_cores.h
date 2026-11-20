/* effective_cores.h -- target PostgreSQL effective CPU-capacity discovery */
#ifndef PGWT_EFFECTIVE_CORES_H
#define PGWT_EFFECTIVE_CORES_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define PGWT_EFFECTIVE_CORES_UNKNOWN (-1.0)

enum pgwt_effective_cores_source {
    PGWT_EFFECTIVE_CORES_AFFINITY = 0,
    PGWT_EFFECTIVE_CORES_QUOTA,
    PGWT_EFFECTIVE_CORES_OVERRIDE,
    PGWT_EFFECTIVE_CORES_SOURCE_UNKNOWN,
};

struct pgwt_effective_cores_result {
    double cores;   /* logical-core-equivalents, or UNKNOWN sentinel */
    enum pgwt_effective_cores_source source;
    bool materially_changed;
};

/* Return the number of online logical CPUs allowed to target_pid. */
typedef int (*pgwt_affinity_count_fn)(pid_t target_pid, void *context,
                                      size_t *cpu_count);

struct pgwt_effective_cores_options {
    /* Prefix for absolute proc/cgroup paths. NULL means "/". Tests point
     * this at a fixture tree and never access the host cgroup filesystem. */
    const char *filesystem_root;

    /* NULL selects the production sched_getaffinity implementation. */
    pgwt_affinity_count_fn affinity_count;
    void *affinity_context;

    /* 0 means discover. A positive finite value wins over all discovery. */
    double override_cores;
};

struct pgwt_effective_cores_resolver {
    struct pgwt_effective_cores_options options;
    struct pgwt_effective_cores_result current;
    bool has_previous;
};

/* One-shot resolution. materially_changed is always false. */
struct pgwt_effective_cores_result
pgwt_effective_cores(pid_t target_pid,
                     const struct pgwt_effective_cores_options *options);

/* Stateful wrapper for cheap periodic refresh. The first result is not a
 * change; subsequent known/unknown transitions or numerical changes are. */
void pgwt_effective_cores_resolver_init(
    struct pgwt_effective_cores_resolver *resolver,
    const struct pgwt_effective_cores_options *options);

struct pgwt_effective_cores_result
pgwt_effective_cores_refresh(struct pgwt_effective_cores_resolver *resolver,
                             pid_t target_pid);

int pgwt_sched_affinity_count(pid_t target_pid, void *context,
                              size_t *cpu_count);

const char *pgwt_effective_cores_source_name(
    enum pgwt_effective_cores_source source);

#endif /* PGWT_EFFECTIVE_CORES_H */
