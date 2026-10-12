/* cmdline.h — /proc/pid/cmdline parser for backend metadata */
#ifndef PGWT_CMDLINE_H
#define PGWT_CMDLINE_H

#include <sys/types.h>

enum pgwt_backend_type {
    PGWT_BT_CLIENT = 0,
    PGWT_BT_CHECKPOINTER,
    PGWT_BT_BG_WRITER,
    PGWT_BT_WAL_WRITER,
    PGWT_BT_AUTOVAC_LAUNCHER,
    PGWT_BT_AUTOVAC_WORKER,
    PGWT_BT_WAL_SENDER,
    PGWT_BT_WAL_RECEIVER,
    PGWT_BT_STARTUP,
    PGWT_BT_LOGICAL_LAUNCHER,
    PGWT_BT_LOGICAL_WORKER,
    PGWT_BT_ARCHIVER,
    PGWT_BT_LOGGER,
    PGWT_BT_PARALLEL_WORKER,
    PGWT_BT_IO_WORKER,
    PGWT_BT_BG_WORKER,
    PGWT_BT_UNKNOWN,
};

struct pgwt_metadata {
    enum pgwt_backend_type backend_type;
    char usename[64];
    char datname[64];
    char client_addr[48];
    pid_t leader_pid;
};

/* Parse /proc/<pid>/cmdline and fill metadata. Returns 0 on success. */
int pgwt_parse_cmdline(pid_t pid, struct pgwt_metadata *meta);

/* Single-read eligibility for a queued lifecycle target. 1 = a live child
 * of parent, 0 = positively retired/reaped or no longer that parent's child,
 * -1 = unavailable/invalid (never treat -1 as a true/live result). */
int pgwt_backend_target_live(pid_t pid, pid_t parent);

/* Human-readable name for a backend type. */
const char *pgwt_backend_type_name(enum pgwt_backend_type bt);

#endif /* PGWT_CMDLINE_H */
