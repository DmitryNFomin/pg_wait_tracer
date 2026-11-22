/* CPU scheduling-group identity, independent of quota/capacity discovery. */
#ifndef PGWT_CPU_GROUP_H
#define PGWT_CPU_GROUP_H

#include <stdbool.h>
#include <sys/types.h>

#define PGWT_CPU_GROUP_PATH_MAX 4096

enum pgwt_cpu_group_relation {
    PGWT_CPU_GROUP_UNKNOWN = 0,
    PGWT_CPU_GROUP_SAME,
    PGWT_CPU_GROUP_DIFFERENT,
};

struct pgwt_cpu_group {
    int version;                 /* 1 or 2; zero when unknown */
    bool hybrid;                 /* v1 cpu entry takes precedence over v2 */
    char path[PGWT_CPU_GROUP_PATH_MAX];
    char reason[512];            /* failing input + reason; empty on success */
};

/* Deterministic over fixture trees: no host paths, environment, global state,
 * logging or process changes. proc_root contains <pid>/cgroup; cgroup_root is
 * the unified mount. Production passes /proc and /sys/fs/cgroup. A v1 cpu
 * entry wins on hybrid systems because that hierarchy owns the CPU controller.
 * For v2, find the nearest ancestor whose parent enables cpu; otherwise root.
 * Missing/unreadable/malformed evidence is UNKNOWN, never a guessed match. */
struct pgwt_cpu_group pgwt_cpu_group_read(const char *proc_root,
                                        const char *cgroup_root, pid_t pid);
enum pgwt_cpu_group_relation pgwt_cpu_group_compare(
    const char *proc_root, const char *cgroup_root, pid_t daemon, pid_t postmaster,
    struct pgwt_cpu_group *daemon_group, struct pgwt_cpu_group *postgres_group);

/* Human-readable hierarchy selection, including the reason on hybrids. */
const char *pgwt_cpu_group_source(const struct pgwt_cpu_group *group);

#endif
