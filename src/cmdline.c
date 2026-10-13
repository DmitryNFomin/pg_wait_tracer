/* cmdline.c — /proc/pid/cmdline parser for backend metadata */
#include "cmdline.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

static const char *bt_names[] = {
    [PGWT_BT_CLIENT]           = "client",
    [PGWT_BT_CHECKPOINTER]    = "checkpointer",
    [PGWT_BT_BG_WRITER]       = "bgwriter",
    [PGWT_BT_WAL_WRITER]      = "walwriter",
    [PGWT_BT_AUTOVAC_LAUNCHER]= "autovac_launcher",
    [PGWT_BT_AUTOVAC_WORKER]  = "autovac_worker",
    [PGWT_BT_WAL_SENDER]      = "walsender",
    [PGWT_BT_WAL_RECEIVER]    = "walreceiver",
    [PGWT_BT_STARTUP]         = "startup",
    [PGWT_BT_LOGICAL_LAUNCHER]= "logical_launcher",
    [PGWT_BT_LOGICAL_WORKER]  = "logical_worker",
    [PGWT_BT_ARCHIVER]        = "archiver",
    [PGWT_BT_LOGGER]          = "logger",
    [PGWT_BT_PARALLEL_WORKER] = "parallel_worker",
    [PGWT_BT_IO_WORKER]       = "io_worker",
    [PGWT_BT_BG_WORKER]      = "bg_worker",
    [PGWT_BT_UNKNOWN]         = "unknown",
};

const char *pgwt_backend_type_name(enum pgwt_backend_type bt)
{
    if (bt >= 0 && bt <= PGWT_BT_UNKNOWN)
        return bt_names[bt];
    return "unknown";
}

/* Extract a space-delimited field from p into dst. Returns pointer past field. */
static const char *extract_field(const char *p, char *dst, size_t dstsz)
{
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != ' ' && i < dstsz - 1)
        dst[i++] = *p++;
    dst[i] = '\0';
    return p;
}

/* Parse "user db host(port) state" from a client/walsender cmdline */
static void parse_connection_fields(const char *p, struct pgwt_metadata *meta)
{
    p = extract_field(p, meta->usename, sizeof(meta->usename));
    p = extract_field(p, meta->datname, sizeof(meta->datname));
    p = extract_field(p, meta->client_addr, sizeof(meta->client_addr));

    /* Strip port from "10.0.1.5(42312)" → "10.0.1.5" */
    char *paren = strchr(meta->client_addr, '(');
    if (paren) *paren = '\0';
}

int pgwt_parse_cmdline(pid_t pid, struct pgwt_metadata *meta)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

    memset(meta, 0, sizeof(*meta));
    meta->backend_type = PGWT_BT_UNKNOWN;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;

    buf[n] = '\0';
    /* cmdline uses \0 separators — replace with spaces */
    for (ssize_t i = 0; i < n - 1; i++)
        if (buf[i] == '\0') buf[i] = ' ';

    char *p = buf;

    /* Skip "postgres: " prefix */
    if (strncmp(p, "postgres: ", 10) != 0)
        return -1;
    p += 10;

    /* PG18+: skip optional cluster identifier "18/main: " */
    char *colon = strchr(p, ':');
    if (colon && colon[1] == ' ' && (colon - p) < 32) {
        /* Verify it looks like "digits/name: " (not a client session field) */
        char *slash = memchr(p, '/', colon - p);
        if (slash)
            p = colon + 2;
    }

    /* Identify backend type by keyword */
    if (strncmp(p, "checkpointer", 12) == 0) {
        meta->backend_type = PGWT_BT_CHECKPOINTER;
    } else if (strncmp(p, "background writer", 17) == 0) {
        meta->backend_type = PGWT_BT_BG_WRITER;
    } else if (strncmp(p, "walwriter", 9) == 0) {
        meta->backend_type = PGWT_BT_WAL_WRITER;
    } else if (strncmp(p, "autovacuum launcher", 19) == 0) {
        meta->backend_type = PGWT_BT_AUTOVAC_LAUNCHER;
    } else if (strncmp(p, "autovacuum worker", 17) == 0) {
        meta->backend_type = PGWT_BT_AUTOVAC_WORKER;
        extract_field(p + 18, meta->datname, sizeof(meta->datname));
    } else if (strncmp(p, "walsender", 9) == 0) {
        meta->backend_type = PGWT_BT_WAL_SENDER;
        parse_connection_fields(p + 10, meta);
    } else if (strncmp(p, "walreceiver", 11) == 0) {
        meta->backend_type = PGWT_BT_WAL_RECEIVER;
    } else if (strncmp(p, "startup", 7) == 0) {
        meta->backend_type = PGWT_BT_STARTUP;
    } else if (strncmp(p, "logical replication launcher", 28) == 0) {
        meta->backend_type = PGWT_BT_LOGICAL_LAUNCHER;
    } else if (strncmp(p, "logical replication worker", 25) == 0) {
        meta->backend_type = PGWT_BT_LOGICAL_WORKER;
    } else if (strncmp(p, "archiver", 8) == 0) {
        meta->backend_type = PGWT_BT_ARCHIVER;
    } else if (strncmp(p, "logger", 6) == 0) {
        meta->backend_type = PGWT_BT_LOGGER;
    } else if (strncmp(p, "parallel worker for PID ", 24) == 0) {
        meta->backend_type = PGWT_BT_PARALLEL_WORKER;
        meta->leader_pid = atoi(p + 24);
    } else if (strncmp(p, "io worker", 9) == 0) {
        meta->backend_type = PGWT_BT_IO_WORKER;
    } else if (strchr(p, '(') || strstr(p, "[local]")) {
        /* Client backend: "user db host(port) state" or "user db [local] state" */
        meta->backend_type = PGWT_BT_CLIENT;
        parse_connection_fields(p, meta);
    } else {
        /* Background worker (extension or custom): store name as app_name */
        meta->backend_type = PGWT_BT_BG_WORKER;
        /* Copy the worker description (e.g. "pg_wait_sampling collector") */
        size_t len = strlen(p);
        /* Trim trailing spaces (cmdline padding) */
        while (len > 0 && p[len - 1] == ' ') len--;
        if (len >= sizeof(meta->usename)) len = sizeof(meta->usename) - 1;
        memcpy(meta->usename, p, len);
        meta->usename[len] = '\0';
    }

    return 0;
}

int pgwt_backend_target_live(pid_t pid, pid_t parent)
{
    if (pid <= 0 || parent <= 0) { errno = EINVAL; return -1; }
    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *file = fopen(path, "r");
    if (!file)
        return errno == ENOENT || errno == ESRCH ? 0 : -1;
    if (!fgets(line, sizeof(line), file)) {
        int error = ferror(file) ? errno : EIO;
        fclose(file);
        errno = error;
        return -1;
    }
    fclose(file);
    char *end = strrchr(line, ')');
    char state;
    long ppid;
    if (!end || sscanf(end + 1, " %c %ld", &state, &ppid) != 2 ||
        ppid < 0 || ppid > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (ppid != parent || state == 'Z' || state == 'X' || state == 'x')
        return 0;
    if (!strchr("RSDTtWIP", state)) { errno = EINVAL; return -1; }
    return 1;
}
