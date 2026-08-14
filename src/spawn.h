/* spawn.h — fork/execvp child-process helper (CAP-7)
 *
 * Replacement for popen(): the daemon runs as root, and popen() interpolates
 * caller-derived strings (postgres binary path, pg_bindir, pg_user) into a
 * shell command line — a path or user name containing shell metacharacters
 * breaks quoting (at best) or injects commands (at worst). These helpers run
 * the child directly via fork+execvp with an argv array: no shell, nothing
 * to quote.
 *
 * pgwt_proc_open() mirrors popen(cmd, "r") with "2>/dev/null":
 *   - child's stdout is piped to p->out (read with stdio as before)
 *   - child's stdin and stderr are /dev/null
 *   - pgwt_proc_close() waits and returns the raw wait(2) status
 *     (0 == clean exit 0, like pclose), or -1 on error.
 */
#ifndef PGWT_SPAWN_H
#define PGWT_SPAWN_H

#include <stdio.h>
#include <sys/types.h>

struct pgwt_proc {
    FILE *out;   /* child's stdout (read end) */
    FILE *in;    /* child's stdin (write end), NULL for pgwt_proc_open() */
    pid_t pid;
};

/* Spawn argv[0] (PATH-resolved, like the shell popen used) with argv as its
 * argument vector (NULL-terminated). Returns 0 on success (p->out readable),
 * -1 on failure. exec failure in the child surfaces as exit status 127. */
int pgwt_proc_open(struct pgwt_proc *p, char *const argv[]);

/* Bidirectional variant used for a long-lived command session.  stdout is
 * exposed through p->out as above; writes to p->in feed the child's stdin.
 * Closing the process closes stdin first, so an interactive child receives
 * EOF and can retire its external session before it is reaped. */
int pgwt_proc_open_rw(struct pgwt_proc *p, char *const argv[]);

/* Close p->out, reap the child. Returns the raw wait status (0 for a clean
 * exit-0), or -1 on error. Safe to call after reading only part of the
 * output (the child gets EPIPE/SIGPIPE, which is reported in the status —
 * callers that close early must ignore the status, as with pclose). */
int pgwt_proc_close(struct pgwt_proc *p);

#endif /* PGWT_SPAWN_H */
