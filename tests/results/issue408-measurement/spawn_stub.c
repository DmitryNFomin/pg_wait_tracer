/* spawn_stub.c — Mac-local link stub for src/spawn.c, which uses pipe2(2).
 *
 * Only pgwt_load_event_names_from_pg() reaches these, and this harness never
 * calls it (every wait-event id is handled numerically). Refusing is the
 * fail-safe: if something did try to spawn psql, it gets "could not", not a
 * fabricated event table. */
#include "spawn.h"

int pgwt_proc_open(struct pgwt_proc *p, char *const argv[])
{
    (void)argv;
    if (p) { p->out = NULL; p->in = NULL; p->pid = -1; }
    return -1;
}

int pgwt_proc_open_rw(struct pgwt_proc *p, char *const argv[])
{
    return pgwt_proc_open(p, argv);
}

int pgwt_proc_close(struct pgwt_proc *p)
{
    (void)p;
    return -1;
}

int pgwt_proc_close_bounded(struct pgwt_proc *p, int timeout_ms)
{
    (void)p; (void)timeout_ms;
    return -1;
}
