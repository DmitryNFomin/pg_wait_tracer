#define _GNU_SOURCE
#include "backend.h"
#include "daemon.h"
#include "pg_wait_tracer.skel.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static int checks, failures, opens, deletions, open_error;
#define CHECK(x, msg) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
int __wrap_pgwt_open_watchpoint(pid_t pid, uint64_t addr, int prog) {
    (void)pid; (void)addr; (void)prog; opens++; errno=open_error; return -1;
}
int __wrap_pgwt_open_watchpoint_disabled(pid_t pid, uint64_t addr, int prog) {
    return __wrap_pgwt_open_watchpoint(pid,addr,prog);
}
int __wrap_bpf_map_delete_elem(int fd, const void *key) {
    (void)fd; (void)key; deletions++; return 0;
}
static struct pgwt_daemon *daemon_fixture(void) {
    struct pgwt_daemon *d=calloc(1,sizeof(*d));
    if (!d) exit(2);
    d->postmaster_pid=getpid(); d->wait_offset_validated=true;
    d->skel=calloc(1,sizeof(*d->skel)); if (!d->skel) exit(2);
    pgwt_backend_init(&d->backends);
    opens=deletions=0; open_error=ESRCH;
    return d;
}
static void release(struct pgwt_daemon *d) { free(d->skel); free(d); }
int main(void) {
    int fds[2]; CHECK(pipe(fds)==0,"pipe created");
    pid_t child=fork();
    CHECK(child>=0,"child forked");
    if (child==0) { close(fds[1]); char byte; ssize_t got=read(fds[0],&byte,1); _exit(got==1 ? 0 : 2); }
    if (child<0) return 2;
    close(fds[0]);
    /* Real, eligible child: ALL failed attempts still count, including ESRCH
     * after eligibility. No errno reclassification and no counter reset. */
    int errors[]={ESRCH,EACCES,EMFILE,ENFILE,ENOSPC,EIO};
    for (size_t i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        struct pgwt_daemon *d=daemon_fixture(); open_error=errors[i];
        CHECK(pgwt_handle_fork(d,child)==-1,"eligible failed attach is refused");
        CHECK(opens==1,"eligible target gets exactly one hardware attempt");
        CHECK(d->counters.wp_attach_failures_total==1,"EVERY attempted failure remains counted");
        release(d);
        d=daemon_fixture(); open_error=errors[i];
        CHECK(pgwt_handle_init(d,child,0x1234)==-1,"eligible real-watchpoint failure refused");
        CHECK(opens==1,"eligible real target gets exactly one hardware attempt");
        CHECK(d->counters.wp_attach_failures_total==1,"EVERY real-watchpoint failure remains counted");
        release(d);
    }
    CHECK(write(fds[1],"x",1)==1,"child released"); close(fds[1]);
    siginfo_t info={0}; CHECK(waitid(P_PID,child,&info,WEXITED|WNOWAIT)==0,"child exited but not reaped");
    /* Queue a late INIT for a positively exited process. */
    struct pgwt_daemon *d=daemon_fixture();
    CHECK(pgwt_handle_init(d,child,0x1234)==0,"zombie INIT handled as stale");
    CHECK(opens==0,"zombie INIT never attempts a watchpoint");
    CHECK(d->counters.wp_attach_failures_total==0,"no invented attach failure without an attempt");
    CHECK(deletions==3,"kernel-enrolled maps cleaned without a registry entry");
    release(d);
    CHECK(waitpid(child,NULL,0)==child,"child reaped");
    d=daemon_fixture();
    CHECK(pgwt_handle_fork(d,child)==0,"reaped FORK handled as stale");
    CHECK(opens==0,"reaped FORK never attempts a watchpoint");
    CHECK(d->counters.wp_attach_failures_total==0,"stale event is not a hardware failure");
    CHECK(deletions==3,"reaped target kernel maps cleaned");
    release(d);
    d=daemon_fixture();
    CHECK(pgwt_handle_exit(d,child)==0,"unregistered EXIT handled");
    CHECK(deletions==3,"unregistered EXIT always clears BPF enrollment");
    release(d);
    d=daemon_fixture();
    CHECK(pgwt_handle_fork(d,-1)==-1,"unknown eligibility refused");
    CHECK(opens==0,"unknown target never passed to hardware");
    CHECK(d->counters.wp_attach_failures_total==1,"unknown eligibility sets failure counter");
    release(d);
    printf("%d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
