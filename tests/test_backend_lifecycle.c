#define _GNU_SOURCE
#include "backend.h"
#include "backend_init.h"
#include "daemon.h"
#include "pg_wait_tracer.skel.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static int checks, failures, opens, deletions, open_error;
static int enables, updates;
static bool open_success;
static struct pgwt_pid_state stored_state;
static bool state_present;
static int ring_batches;
#define CHECK(x, msg) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
int __wrap_pgwt_open_watchpoint(pid_t pid, uint64_t addr, int prog) {
    (void)pid; (void)addr; (void)prog; opens++;
    if (open_success) return open("/dev/null",O_RDONLY);
    errno=open_error; return -1;
}
int __wrap_pgwt_open_watchpoint_disabled(pid_t pid, uint64_t addr, int prog) {
    return __wrap_pgwt_open_watchpoint(pid,addr,prog);
}
int __wrap_bpf_map_delete_elem(int fd, const void *key) {
    (void)fd; (void)key; deletions++; state_present=false; return 0;
}
int __wrap_bpf_map_lookup_elem(int fd, const void *key, void *value) {
    (void)fd; (void)key;
    if (!state_present) { errno=ENOENT; return -1; }
    *(struct pgwt_pid_state *)value=stored_state;
    return 0;
}
int __wrap_bpf_map_update_elem(int fd, const void *key, const void *value, uint64_t flags) {
    (void)fd; (void)key; (void)flags;
    updates++; stored_state=*(const struct pgwt_pid_state *)value;
    state_present=true; return 0;
}
int __wrap_pgwt_watchpoint_enable(int fd) {
    (void)fd; enables++; return 0;
}
int __wrap_ring_buffer__consume(struct ring_buffer *rb) {
    (void)rb;
    ring_batches++;
    /* A bootstrap fires while another lifecycle batch is being delivered;
     * its hint is absent. Only the post-batch sweep can recover it now. */
    state_present=true;
    return 1;
}
static struct pgwt_daemon *daemon_fixture(void) {
    struct pgwt_daemon *d=calloc(1,sizeof(*d));
    if (!d) exit(2);
    d->postmaster_pid=getpid(); d->wait_offset_validated=true;
    d->skel=calloc(1,sizeof(*d->skel)); if (!d->skel) exit(2);
    pgwt_backend_init(&d->backends);
    opens=deletions=0; open_error=ESRCH;
    enables=updates=0; open_success=false;
    state_present=false;
    return d;
}
static void release(struct pgwt_daemon *d) { free(d->skel); free(d); }
int main(void) {
    /* The child inherits this address; the real preseed reads it through
     * /proc/<child>/mem while only map/hardware operations are substituted. */
    volatile uint32_t wait_value=0;
    int fds[2]; CHECK(pipe(fds)==0,"pipe created");
    pid_t child=fork();
    CHECK(child>=0,"child forked");
    if (child==0) { close(fds[1]); char byte; ssize_t got=read(fds[0],&byte,1); _exit(got==1 ? 0 : 2); }
    if (child<0) return 2;
    close(fds[0]);
    /* Real handler, fake state reader: a swept INIT followed by its queued
     * hint must leave the same live watchpoint and the captured timestamp. */
    struct pgwt_daemon *d=daemon_fixture();
    struct pgwt_backend *be=&d->backends.entries[0];
    d->backends.count=1; open_success=true;
    be->pid=child; be->is_alive=true; be->bootstrap_fd=open("/dev/null",O_RDONLY);
    pgwt_pending_init_add(&d->backends,be);
    CHECK(be->bootstrap_fd>=0,"fake bootstrap fd opened");
    stored_state=(struct pgwt_pid_state){.init_stored=1,
        .init_wait_addr=(uintptr_t)&wait_value, .init_ts=456};
    CHECK(pgwt_consume_lifecycle_ring(d)==1,"lifecycle callback batch consumed");
    CHECK(ring_batches==1 && d->tick==0,"batch recovery happens before any timer tick");
    CHECK(be->init_attached && d->backends.pending_init_count==0,"batch attaches and retires pending INIT");
    CHECK(opens==1 && enables==1 && updates==1,"sweep runs open/preseed/enable once");
    CHECK(stored_state.init_stored==1 && stored_state.init_ts==456,
          "watchpoint preseed preserves bootstrap evidence");
    CHECK(be->wp_addr==(uintptr_t)&wait_value && be->init_ts==456,
          "real attach uses stored address and INIT timestamp");
    CHECK(be->attach_ts>be->init_ts,"attach time stays distinct from INIT time");
    int real_fd=be->wp_fd;
    CHECK(pgwt_handle_init(d,child,0x1234,789)==0,"late INIT hint handled");
    CHECK(opens==1 && be->wp_fd==real_fd && enables==1 && updates==1,
          "double delivery does not reopen or reseed watchpoint");
    CHECK(be->init_ts==456,"stored INIT timestamp wins over delivery time");
    CHECK(pgwt_sweep_bootstrap_backends(d)==0,"attached backend skipped by sweep");
    CHECK(pgwt_handle_exit(d,child)==0,"attached backend retired");
    CHECK(d->counters.lifecycle_init_lost_total==0,"attached stored INIT not lost");
    release(d);

    d=daemon_fixture();
    be=&d->backends.entries[0]; d->backends.count=1;
    be->pid=child; be->is_alive=true; be->bootstrap_fd=open("/dev/null",O_RDONLY);
    pgwt_pending_init_add(&d->backends,be);
    state_present=true;
    CHECK(pgwt_sweep_bootstrap_backends(d)==0,"failed sweep attach is not success");
    CHECK(opens==1,"stored INIT uses real attach path");
    CHECK(pgwt_handle_exit(d,child)==0,"failed enrollment retired");
    CHECK(d->counters.lifecycle_init_lost_total==1,"failed attach retains stored-loss evidence");
    CHECK(pgwt_handle_exit(d,child)==0,"duplicate EXIT handled");
    CHECK(d->counters.lifecycle_init_lost_total==1,"duplicate EXIT does not recount lost INIT");
    release(d);

    d=daemon_fixture(); state_present=true;
    CHECK(pgwt_handle_exit(d,child)==0,"exit before sweep without registry handled");
    CHECK(d->counters.lifecycle_init_lost_total==1,"exit before sweep counts stored INIT");
    CHECK(opens==0,"exit before sweep does not attempt attachment");
    release(d);

    /* No fork-seeded state: a real delivered INIT can still attach, and its
     * diagnostic must not become an integrity loss. A failed attach does. */
    d=daemon_fixture(); open_success=true;
    CHECK(pgwt_handle_init(d,child,(uintptr_t)&wait_value,789)==0,"hint attaches without seeded state");
    CHECK(pgwt_handle_exit(d,child)==0,"missing-state hint backend retired");
    CHECK(d->counters.lifecycle_init_lost_total==0,"successful hint attach is not lost INIT");
    release(d);
    d=daemon_fixture();
    CHECK(pgwt_handle_init(d,child,(uintptr_t)&wait_value,789)==-1,"missing-state hint attach fails");
    CHECK(pgwt_handle_exit(d,child)==0,"missing-state failed hint backend retired");
    CHECK(d->counters.lifecycle_init_lost_total==1,"missing state is lost only after failed attachment");
    CHECK(pgwt_handle_exit(d,child)==0,"missing-state duplicate EXIT handled");
    CHECK(d->counters.lifecycle_init_lost_total==1,"missing-state loss counted once");
    release(d);
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
        CHECK(pgwt_handle_init(d,child,0x1234,123)==-1,"eligible real-watchpoint failure refused");
        CHECK(opens==1,"eligible real target gets exactly one hardware attempt");
        CHECK(d->counters.wp_attach_failures_total==1,"EVERY real-watchpoint failure remains counted");
        release(d);
    }
    CHECK(write(fds[1],"x",1)==1,"child released"); close(fds[1]);
    siginfo_t info={0}; CHECK(waitid(P_PID,child,&info,WEXITED|WNOWAIT)==0,"child exited but not reaped");
    /* BPF on_exit has no state entry and therefore emits no EXIT. The
     * pending sweep must still retire the dead pid and count its loss. */
    d=daemon_fixture();
    be=&d->backends.entries[0]; d->backends.count=1;
    be->pid=child; be->is_alive=true; be->bootstrap_fd=open("/dev/null",O_RDONLY);
    pgwt_pending_init_add(&d->backends,be);
    CHECK(pgwt_sweep_bootstrap_backends(d)==0,"missing-state dead backend swept without attach");
    CHECK(d->counters.lifecycle_init_lost_total==1,"missing-state dead backend counts loss on sweep");
    CHECK(d->backends.pending_init_count==0 && be->pid==0,"missing-state dead backend retired once");
    CHECK(pgwt_sweep_bootstrap_backends(d)==0 && d->counters.lifecycle_init_lost_total==1,
          "later batch cannot recount missing-state loss");
    release(d);
    /* Queue a late INIT for a positively exited process. */
    d=daemon_fixture();
    CHECK(pgwt_handle_init(d,child,0x1234,123)==0,"zombie INIT handled as stale");
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
