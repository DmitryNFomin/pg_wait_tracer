/* effective_cores.c -- affinity intersected with hierarchical cgroup quota */
#define _GNU_SOURCE
#include "effective_cores.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define PGWT_CGROUP_TEXT_MAX 16384
#define PGWT_AFFINITY_START_CPUS 128
#define PGWT_AFFINITY_MAX_CPUS (1U << 20)
#define PGWT_CAPACITY_EPSILON 1e-9

enum cgroup_version {
    CGROUP_NONE = 0,
    CGROUP_V1,
    CGROUP_V2,
};

struct cgroup_location {
    enum cgroup_version version;
    char path[PATH_MAX];
};

static struct pgwt_effective_cores_result unknown_result(void)
{
    return (struct pgwt_effective_cores_result) {
        .cores = PGWT_EFFECTIVE_CORES_UNKNOWN,
        .source = PGWT_EFFECTIVE_CORES_SOURCE_UNKNOWN,
        .materially_changed = false,
    };
}

const char *pgwt_effective_cores_source_name(
    enum pgwt_effective_cores_source source)
{
    switch (source) {
    case PGWT_EFFECTIVE_CORES_AFFINITY: return "affinity";
    case PGWT_EFFECTIVE_CORES_QUOTA: return "quota";
    case PGWT_EFFECTIVE_CORES_OVERRIDE: return "override";
    case PGWT_EFFECTIVE_CORES_SOURCE_UNKNOWN:
    default:
        return "unknown";
    }
}

int pgwt_sched_affinity_count(pid_t target_pid, void *context,
                              size_t *cpu_count)
{
    (void)context;

    long configured = sysconf(_SC_NPROCESSORS_CONF);
    size_t ncpus = configured > PGWT_AFFINITY_START_CPUS
                 ? (size_t)configured : PGWT_AFFINITY_START_CPUS;

    while (ncpus <= PGWT_AFFINITY_MAX_CPUS) {
        size_t set_size = CPU_ALLOC_SIZE(ncpus);
        cpu_set_t *set = CPU_ALLOC(ncpus);
        if (!set)
            return -1;

        CPU_ZERO_S(set_size, set);
        if (sched_getaffinity(target_pid, set_size, set) == 0) {
            int count = CPU_COUNT_S(set_size, set);
            CPU_FREE(set);
            if (count <= 0)
                return -1;
            *cpu_count = (size_t)count;
            return 0;
        }

        int saved_errno = errno;
        CPU_FREE(set);
        if (saved_errno != EINVAL || ncpus > PGWT_AFFINITY_MAX_CPUS / 2)
            return -1;
        ncpus *= 2;
    }
    return -1;
}

static int rooted_path(char *dst, size_t dst_size, const char *root,
                       const char *absolute_path)
{
    if (!root || !root[0])
        root = "/";
    if (!absolute_path || absolute_path[0] != '/')
        return -1;

    size_t root_len = strlen(root);
    while (root_len > 1 && root[root_len - 1] == '/')
        root_len--;

    int n;
    if (root_len == 1 && root[0] == '/')
        n = snprintf(dst, dst_size, "%s", absolute_path);
    else
        n = snprintf(dst, dst_size, "%.*s%s", (int)root_len, root,
                     absolute_path);
    return n >= 0 && (size_t)n < dst_size ? 0 : -1;
}

static int read_text_file(const char *path, char *buf, size_t buf_size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    size_t used = 0;
    while (used + 1 < buf_size) {
        ssize_t n = read(fd, buf + used, buf_size - used - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        used += (size_t)n;
    }

    char extra;
    ssize_t extra_n = read(fd, &extra, 1);
    close(fd);
    if (extra_n != 0)
        return -1;

    buf[used] = '\0';
    return 0;
}

static bool valid_cgroup_path(const char *path)
{
    if (!path || path[0] != '/')
        return false;
    if (strcmp(path, "/") == 0)
        return true;

    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0 || (len == 1 && p[0] == '.') ||
            (len == 2 && p[0] == '.' && p[1] == '.'))
            return false;
        for (size_t i = 0; i < len; i++)
            if ((unsigned char)p[i] < 0x20)
                return false;
        if (!slash)
            break;
        p = slash + 1;
    }
    return true;
}

static bool controller_list_has_cpu(const char *controllers, size_t len)
{
    const char *p = controllers;
    const char *end = controllers + len;
    while (p < end) {
        const char *comma = memchr(p, ',', (size_t)(end - p));
        const char *token_end = comma ? comma : end;
        if (token_end - p == 3 && memcmp(p, "cpu", 3) == 0)
            return true;
        p = comma ? comma + 1 : end;
    }
    return false;
}

static int parse_cgroup_location(char *text, struct cgroup_location *location)
{
    bool have_v1_cpu = false;
    bool have_v2 = false;
    char v1_path[PATH_MAX] = "";
    char v2_path[PATH_MAX] = "";
    char *saveptr = NULL;

    for (char *line = strtok_r(text, "\n", &saveptr);
         line; line = strtok_r(NULL, "\n", &saveptr)) {
        if (!line[0])
            continue;
        char *first_colon = strchr(line, ':');
        char *second_colon = first_colon ? strchr(first_colon + 1, ':') : NULL;
        if (!first_colon || first_colon == line || !second_colon)
            return -1;

        for (char *p = line; p < first_colon; p++)
            if (!isdigit((unsigned char)*p))
                return -1;

        const char *controllers = first_colon + 1;
        size_t controllers_len = (size_t)(second_colon - controllers);
        const char *path = second_colon + 1;
        if (!valid_cgroup_path(path))
            return -1;

        bool unified = first_colon - line == 1 && line[0] == '0' &&
                       controllers_len == 0;
        if (unified) {
            if (have_v2 || snprintf(v2_path, sizeof(v2_path), "%s", path) >=
                           (int)sizeof(v2_path))
                return -1;
            have_v2 = true;
        } else if (controller_list_has_cpu(controllers, controllers_len)) {
            if (have_v1_cpu ||
                snprintf(v1_path, sizeof(v1_path), "%s", path) >=
                    (int)sizeof(v1_path))
                return -1;
            have_v1_cpu = true;
        }
    }

    /* A hybrid hierarchy uses its v1 CPU-controller entry. */
    if (have_v1_cpu) {
        location->version = CGROUP_V1;
        snprintf(location->path, sizeof(location->path), "%s", v1_path);
        return 0;
    }
    if (have_v2) {
        location->version = CGROUP_V2;
        snprintf(location->path, sizeof(location->path), "%s", v2_path);
        return 0;
    }
    return -1;
}

static int target_cgroup(const char *root, pid_t target_pid,
                         struct cgroup_location *location)
{
    char absolute[64];
    char path[PATH_MAX];
    char text[PGWT_CGROUP_TEXT_MAX];

    int n = snprintf(absolute, sizeof(absolute), "/proc/%d/cgroup",
                     target_pid);
    if (n < 0 || (size_t)n >= sizeof(absolute) ||
        rooted_path(path, sizeof(path), root, absolute) != 0 ||
        read_text_file(path, text, sizeof(text)) != 0)
        return -1;
    return parse_cgroup_location(text, location);
}

static int parse_positive_u64(const char *text, uint64_t *value)
{
    if (!text || !text[0] || text[0] == '-' || text[0] == '+')
        return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || end == text || parsed == 0)
        return -1;
    while (isspace((unsigned char)*end))
        end++;
    if (*end != '\0')
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_v2_cpu_max(const char *text, bool *finite, double *cores)
{
    char quota_text[64];
    char period_text[64];
    char extra;
    if (sscanf(text, " %63s %63s %c", quota_text, period_text, &extra) != 2)
        return -1;

    uint64_t period;
    if (parse_positive_u64(period_text, &period) != 0)
        return -1;

    if (strcmp(quota_text, "max") == 0) {
        *finite = false;
        *cores = 0.0;
        return 0;
    }

    uint64_t quota;
    if (parse_positive_u64(quota_text, &quota) != 0)
        return -1;
    *finite = true;
    *cores = (double)quota / (double)period;
    return isfinite(*cores) && *cores > 0.0 ? 0 : -1;
}

static int parse_v1_quota(const char *text, bool *finite, uint64_t *quota)
{
    errno = 0;
    char *end = NULL;
    long long parsed = strtoll(text, &end, 10);
    if (errno || end == text)
        return -1;
    while (isspace((unsigned char)*end))
        end++;
    if (*end != '\0')
        return -1;
    if (parsed == -1) {
        *finite = false;
        *quota = 0;
        return 0;
    }
    if (parsed <= 0)
        return -1;
    *finite = true;
    *quota = (uint64_t)parsed;
    return 0;
}

static int parse_cpu_list(const char *text, size_t *count)
{
    const char *p = text;
    uint64_t total = 0;
    uint64_t previous_end = 0;
    bool have_previous = false;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (!isdigit((unsigned char)*p))
        return -1;

    for (;;) {
        errno = 0;
        char *end = NULL;
        unsigned long long first = strtoull(p, &end, 10);
        if (errno || end == p)
            return -1;
        uint64_t last = (uint64_t)first;
        p = end;
        if (*p == '-') {
            p++;
            errno = 0;
            unsigned long long range_end = strtoull(p, &end, 10);
            if (errno || end == p || range_end < first)
                return -1;
            last = (uint64_t)range_end;
            p = end;
        }

        if (have_previous && first <= previous_end)
            return -1;
        if (last - first + 1 > UINT64_MAX - total)
            return -1;
        total += last - first + 1;
        previous_end = last;
        have_previous = true;

        if (*p == ',') {
            p++;
            if (!isdigit((unsigned char)*p))
                return -1;
            continue;
        }
        while (isspace((unsigned char)*p))
            p++;
        if (*p != '\0')
            return -1;
        break;
    }

    if (total == 0 || total > SIZE_MAX)
        return -1;
    *count = (size_t)total;
    return 0;
}

static int cgroup_file_path(char *dst, size_t dst_size, const char *root,
                            const char *mount, const char *cgroup_path,
                            const char *file)
{
    char absolute[PATH_MAX];
    int n;
    if (strcmp(cgroup_path, "/") == 0)
        n = snprintf(absolute, sizeof(absolute), "%s/%s", mount, file);
    else
        n = snprintf(absolute, sizeof(absolute), "%s%s/%s", mount,
                     cgroup_path, file);
    if (n < 0 || (size_t)n >= sizeof(absolute))
        return -1;
    return rooted_path(dst, dst_size, root, absolute);
}

static void parent_cgroup(char *path)
{
    if (strcmp(path, "/") == 0)
        return;
    char *slash = strrchr(path, '/');
    if (slash == path)
        path[1] = '\0';
    else
        *slash = '\0';
}

static int v2_limits(const char *root, const char *cgroup_path,
                     size_t *cpuset_count, bool *quota_finite,
                     double *quota_cores)
{
    const char *mount = "/sys/fs/cgroup";
    char path[PATH_MAX];
    char text[256];
    char current[PATH_MAX];

    if (cgroup_file_path(path, sizeof(path), root, mount, cgroup_path,
                         "cpuset.cpus.effective") != 0 ||
        read_text_file(path, text, sizeof(text)) != 0 ||
        parse_cpu_list(text, cpuset_count) != 0)
        return -1;

    snprintf(current, sizeof(current), "%s", cgroup_path);
    *quota_finite = false;
    *quota_cores = 0.0;
    for (;;) {
        bool finite;
        double cores;
        if (cgroup_file_path(path, sizeof(path), root, mount, current,
                             "cpu.max") != 0 ||
            read_text_file(path, text, sizeof(text)) != 0 ||
            parse_v2_cpu_max(text, &finite, &cores) != 0)
            return -1;
        if (finite && (!*quota_finite || cores < *quota_cores)) {
            *quota_finite = true;
            *quota_cores = cores;
        }
        if (strcmp(current, "/") == 0)
            break;
        parent_cgroup(current);
    }
    return 0;
}

static int find_v1_cpu_mount(const char *root, char *mount, size_t mount_size)
{
    static const char *candidates[] = {
        "/sys/fs/cgroup/cpu",
        "/sys/fs/cgroup/cpu,cpuacct",
        "/sys/fs/cgroup/cpuacct,cpu",
    };
    char path[PATH_MAX];
    struct stat st;

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (rooted_path(path, sizeof(path), root, candidates[i]) == 0 &&
            stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (snprintf(mount, mount_size, "%s", candidates[i]) >=
                (int)mount_size)
                return -1;
            return 0;
        }
    }
    return -1;
}

static int v1_limits(const char *root, const char *cgroup_path,
                     bool *quota_finite, double *quota_cores)
{
    char mount[64];
    char path[PATH_MAX];
    char quota_text[256];
    char period_text[256];
    char current[PATH_MAX];

    if (find_v1_cpu_mount(root, mount, sizeof(mount)) != 0)
        return -1;

    snprintf(current, sizeof(current), "%s", cgroup_path);
    *quota_finite = false;
    *quota_cores = 0.0;
    for (;;) {
        bool finite;
        uint64_t quota;
        uint64_t period;
        if (cgroup_file_path(path, sizeof(path), root, mount, current,
                             "cpu.cfs_quota_us") != 0 ||
            read_text_file(path, quota_text, sizeof(quota_text)) != 0 ||
            parse_v1_quota(quota_text, &finite, &quota) != 0 ||
            cgroup_file_path(path, sizeof(path), root, mount, current,
                             "cpu.cfs_period_us") != 0 ||
            read_text_file(path, period_text, sizeof(period_text)) != 0 ||
            parse_positive_u64(period_text, &period) != 0)
            return -1;

        if (finite) {
            double cores = (double)quota / (double)period;
            if (!isfinite(cores) || cores <= 0.0)
                return -1;
            if (!*quota_finite || cores < *quota_cores) {
                *quota_finite = true;
                *quota_cores = cores;
            }
        }
        if (strcmp(current, "/") == 0)
            break;
        parent_cgroup(current);
    }
    return 0;
}

struct pgwt_effective_cores_result
pgwt_effective_cores(pid_t target_pid,
                     const struct pgwt_effective_cores_options *options)
{
    struct pgwt_effective_cores_options defaults = {
        .filesystem_root = "/",
        .affinity_count = pgwt_sched_affinity_count,
        .affinity_context = NULL,
        .override_cores = 0.0,
    };
    if (options) {
        defaults = *options;
        if (!defaults.filesystem_root)
            defaults.filesystem_root = "/";
        if (!defaults.affinity_count)
            defaults.affinity_count = pgwt_sched_affinity_count;
    }

    if (defaults.override_cores != 0.0) {
        if (!isfinite(defaults.override_cores) || defaults.override_cores < 0.0)
            return unknown_result();
        return (struct pgwt_effective_cores_result) {
            .cores = defaults.override_cores,
            .source = PGWT_EFFECTIVE_CORES_OVERRIDE,
            .materially_changed = false,
        };
    }

    size_t affinity_count;
    if (defaults.affinity_count(target_pid, defaults.affinity_context,
                                &affinity_count) != 0 || affinity_count == 0)
        return unknown_result();

    struct cgroup_location location;
    if (target_cgroup(defaults.filesystem_root, target_pid, &location) != 0)
        return unknown_result();

    size_t allowed_count = affinity_count;
    bool quota_finite;
    double quota_cores;
    if (location.version == CGROUP_V2) {
        size_t cpuset_count;
        if (v2_limits(defaults.filesystem_root, location.path, &cpuset_count,
                      &quota_finite, &quota_cores) != 0)
            return unknown_result();
        if (cpuset_count < allowed_count)
            allowed_count = cpuset_count;
    } else if (location.version == CGROUP_V1) {
        if (v1_limits(defaults.filesystem_root, location.path, &quota_finite,
                      &quota_cores) != 0)
            return unknown_result();
    } else {
        return unknown_result();
    }

    double allowed_cores = (double)allowed_count;
    if (quota_finite && quota_cores <= allowed_cores) {
        return (struct pgwt_effective_cores_result) {
            .cores = quota_cores,
            .source = PGWT_EFFECTIVE_CORES_QUOTA,
            .materially_changed = false,
        };
    }
    return (struct pgwt_effective_cores_result) {
        .cores = allowed_cores,
        .source = PGWT_EFFECTIVE_CORES_AFFINITY,
        .materially_changed = false,
    };
}

void pgwt_effective_cores_resolver_init(
    struct pgwt_effective_cores_resolver *resolver,
    const struct pgwt_effective_cores_options *options)
{
    memset(resolver, 0, sizeof(*resolver));
    if (options)
        resolver->options = *options;
    resolver->current = unknown_result();
}

static bool capacity_changed(const struct pgwt_effective_cores_result *old,
                             const struct pgwt_effective_cores_result *new)
{
    bool old_known = old->source != PGWT_EFFECTIVE_CORES_SOURCE_UNKNOWN;
    bool new_known = new->source != PGWT_EFFECTIVE_CORES_SOURCE_UNKNOWN;
    if (old_known != new_known)
        return true;
    if (!old_known)
        return false;

    double old_abs = fabs(old->cores);
    double new_abs = fabs(new->cores);
    double scale = old_abs > new_abs ? old_abs : new_abs;
    if (scale < 1.0)
        scale = 1.0;
    return fabs(old->cores - new->cores) > PGWT_CAPACITY_EPSILON * scale;
}

struct pgwt_effective_cores_result
pgwt_effective_cores_refresh(struct pgwt_effective_cores_resolver *resolver,
                             pid_t target_pid)
{
    struct pgwt_effective_cores_result next =
        pgwt_effective_cores(target_pid, &resolver->options);
    if (resolver->has_previous)
        next.materially_changed = capacity_changed(&resolver->current, &next);
    resolver->current = next;
    resolver->has_previous = true;
    return next;
}
