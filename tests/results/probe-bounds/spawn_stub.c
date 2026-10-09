/* spawn_stub.c — src/spawn.c uses pipe2(), which macOS does not have.
 * These stubs let a macOS-side probe link against wait_event.o without a
 * live PostgreSQL; no probe here resolves event names from a server. */
#include "spawn.h"
int pgwt_proc_open(struct pgwt_proc *p, char *const argv[]) { (void)p; (void)argv; return -1; }
int pgwt_proc_open_rw(struct pgwt_proc *p, char *const argv[]) { (void)p; (void)argv; return -1; }
int pgwt_proc_close(struct pgwt_proc *p) { (void)p; return -1; }
int pgwt_proc_close_bounded(struct pgwt_proc *p, int t) { (void)p; (void)t; return -1; }
