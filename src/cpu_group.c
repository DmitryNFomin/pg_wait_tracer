#define _GNU_SOURCE
#include "cpu_group.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void unknown(struct pgwt_cpu_group *group, const char *fmt, ...)
{
    group->version = 0;
    va_list args;
    va_start(args, fmt);
    vsnprintf(group->reason, sizeof(group->reason), fmt, args);
    va_end(args);
}

static int read_text(const char *path, char *text, size_t size,
                     struct pgwt_cpu_group *group)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        unknown(group, "cannot read %s: %s", path, strerror(errno));
        return 0;
    }
    size_t used = 0;
    for (;;) {
        char extra;
        ssize_t n = used == size - 1 ? read(fd, &extra, 1)
                                    : read(fd, text + used, size - 1 - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            unknown(group, "cannot read %s: %s", path, strerror(errno));
            close(fd);
            return 0;
        }
        if (!n)
            break;
        if (used == size - 1) {
            unknown(group, "cannot parse %s: input too long", path);
            close(fd);
            return 0;
        }
        if (memchr(text + used, '\0', (size_t)n)) {
            unknown(group, "cannot parse %s: embedded NUL", path);
            close(fd);
            return 0;
        }
        used += (size_t)n;
    }
    close(fd);
    text[used] = '\0';
    return 1;
}

static int valid_path(const char *path)
{
    if (*path != '/' || strlen(path) >= PGWT_CPU_GROUP_PATH_MAX ||
        (strlen(path) >= 10 && !strcmp(path + strlen(path) - 10, " (deleted)")))
        return 0;
    if (!strcmp(path, "/"))
        return 1;
    for (const char *p = path + 1; ; ) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (!n || (n == 1 && *p == '.') || (n == 2 && !memcmp(p, "..", 2)))
            return 0;
        for (size_t i = 0; i < n; ++i)
            if ((unsigned char)p[i] < 32 || (unsigned char)p[i] == 127)
                return 0;
        if (!slash)
            return 1;
        p = slash + 1;
    }
}

static int has_cpu(const char *list, const char *delim)
{
    for (const char *p = list; *p; ) {
        p += strspn(p, delim);
        size_t n = strcspn(p, delim);
        if (n == 3 && !memcmp(p, "cpu", 3))
            return 1;
        p += n;
    }
    return 0;
}

static void parent(char *path)
{
    char *slash = strrchr(path, '/');
    if (slash == path)
        path[1] = '\0';
    else
        *slash = '\0';
}

struct pgwt_cpu_group pgwt_cpu_group_read(const char *proc_root,
                                        const char *cgroup_root, pid_t pid)
{
    struct pgwt_cpu_group group = {0};
    char file[PGWT_CPU_GROUP_PATH_MAX], text[16384];
    char v1[PGWT_CPU_GROUP_PATH_MAX] = "", v2[PGWT_CPU_GROUP_PATH_MAX] = "";
    if (!proc_root || !cgroup_root || pid <= 0) {
        unknown(&group, "invalid proc/cgroup root or PID %ld", (long)pid);
        return group;
    }
    int n = snprintf(file, sizeof(file), "%s/%ld/cgroup", proc_root, (long)pid);
    if (n < 0 || (size_t)n >= sizeof(file)) {
        unknown(&group, "proc root too long for PID %ld", (long)pid);
        return group;
    }
    if (!read_text(file, text, sizeof(text), &group))
        return group;
    char *p = text;
    unsigned line_number = 0;
    while (*p) {
        ++line_number;
        char *line = p;
        char *end = strchr(p, '\n');
        if (end) { *end = '\0'; p = end + 1; }
        else p += strlen(p);
        char *first = strchr(line, ':');
        char *second = first ? strchr(first + 1, ':') : NULL;
        if (!first || first == line || !second || !valid_path(second + 1))
            goto malformed;
        for (char *id = line; id < first; ++id)
            if (!isdigit((unsigned char)*id))
                goto malformed;
        *first = *second = '\0';
        const char *controllers = first + 1, *path = second + 1;
        if (!strcmp(line, "0") && !*controllers) {
            if (*v2) goto malformed;
            strcpy(v2, path);
        } else {
            if (!strcmp(line, "0") || !*controllers || *controllers == ',' ||
                controllers[strlen(controllers) - 1] == ',' || strstr(controllers, ",,"))
                goto malformed;
            for (const char *c = controllers; *c; ++c)
                if (!isalnum((unsigned char)*c) && !strchr("_,=-", *c))
                    goto malformed;
            if (has_cpu(controllers, ",")) {
                if (*v1) goto malformed;
                strcpy(v1, path);
            }
        }
        continue;
malformed:
        unknown(&group, "cannot parse %s: malformed line %u", file, line_number);
        return group;
    }
    if (*v1) {
        group.version = 1;
        group.hybrid = *v2 != '\0';
        strcpy(group.path, v1);
        return group;
    }
    if (!*v2) {
        unknown(&group, "cannot parse %s: no CPU hierarchy", file);
        return group;
    }
    strcpy(group.path, v2);
    n = snprintf(file, sizeof(file), "%s%s", cgroup_root, group.path);
    struct stat st;
    if (n < 0 || (size_t)n >= sizeof(file)) {
        unknown(&group, "cgroup root/path too long: %s", group.path);
        return group;
    }
    if (stat(file, &st) != 0) {
        unknown(&group, "cannot read cgroup directory %s: %s", file, strerror(errno));
        return group;
    }
    if (!S_ISDIR(st.st_mode)) {
        unknown(&group, "cannot read cgroup directory %s: not a directory", file);
        return group;
    }
    while (strcmp(group.path, "/")) {
        char ancestor[PGWT_CPU_GROUP_PATH_MAX];
        strcpy(ancestor, group.path);
        parent(ancestor);
        n = snprintf(file, sizeof(file), "%s%s/cgroup.subtree_control",
                     cgroup_root, !strcmp(ancestor, "/") ? "" : ancestor);
        if (n < 0 || (size_t)n >= sizeof(file)) {
            unknown(&group, "cgroup root/path too long: %s", ancestor);
            return group;
        }
        if (!read_text(file, text, sizeof(text), &group))
            return group;
        /* Kernel controller names are whitespace-separated identifiers. */
        for (const char *c = text; *c; ++c)
            if (!isalnum((unsigned char)*c) && *c != '_' &&
                !isspace((unsigned char)*c)) {
                unknown(&group, "cannot parse %s: malformed controller list", file);
                return group;
            }
        if (has_cpu(text, " \t\n\r"))
            break;
        strcpy(group.path, ancestor);
    }
    group.version = 2;
    return group;
}

enum pgwt_cpu_group_relation pgwt_cpu_group_compare(
    const char *proc_root, const char *cgroup_root, pid_t daemon, pid_t postmaster,
    struct pgwt_cpu_group *daemon_group, struct pgwt_cpu_group *postgres_group)
{
    *daemon_group = pgwt_cpu_group_read(proc_root, cgroup_root, daemon);
    *postgres_group = pgwt_cpu_group_read(proc_root, cgroup_root, postmaster);
    if (!daemon_group->version || !postgres_group->version)
        return PGWT_CPU_GROUP_UNKNOWN;
    return daemon_group->version == postgres_group->version &&
           !strcmp(daemon_group->path, postgres_group->path)
         ? PGWT_CPU_GROUP_SAME : PGWT_CPU_GROUP_DIFFERENT;
}

const char *pgwt_cpu_group_source(const struct pgwt_cpu_group *group)
{
    if (group->hybrid)
        return "v1 (hybrid: the v1 cpu hierarchy owns the CPU controller)";
    return group->version == 1 ? "v1" : group->version == 2 ? "v2" : "unknown";
}
