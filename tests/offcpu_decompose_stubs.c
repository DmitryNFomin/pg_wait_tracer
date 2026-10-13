/* offcpu_decompose_stubs.c — macOS-only link stubs for the offline analysis
 * build of tests/offcpu_decompose.c.
 *
 * src/spawn.c (pgwt_proc_open/close, used by wait_event.c's
 * pgwt_load_event_names_from_pg) calls pipe2(), which macOS does not have,
 * so the real file does not compile here. The analysis tool never spawns
 * psql: it takes its event names from the trace's wait_event_names.json
 * sidecar and the built-in tables. These stubs make that explicit and FAIL
 * LOUDLY if anything ever does try to spawn, instead of silently returning
 * "no names" and mislabelling every row.
 *
 * Link src/spawn.c instead of this file on Linux. Never link both.
 */
#include "spawn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int pgwt_proc_open(struct pgwt_proc *p, char *const argv[])
{
    (void)p;
    fprintf(stderr, "offcpu_decompose: REFUSE -- tried to spawn %s; this "
                    "offline build has no process spawning. Event names must "
                    "come from the trace sidecar.\n",
            argv && argv[0] ? argv[0] : "(null)");
    exit(2);
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
