/* Portable execution of the LIVE scheduler with virtual OS/kernel I/O.
 * Checks the harness itself, not the kernel premises or completion core. */
#include "../src/percpu_transport.h"
#include "../src/merge_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>

struct queued_wire { struct pgwt_wire_record record; };
struct pgwt_percpu_transport {
    uint32_t *cpus;
    size_t ncpu;
    uint64_t *seq, *targets, cut[2];
    bool requested, e_seen, l_seen, fencing, cut_taken, *acks;
    struct queued_wire *queue;
    size_t count, queue_head, capacity;
    struct pgwt_transport_stats stats;
};
struct fake_skeleton { void *obj; };
struct fake_daemon {
    struct pgwt_percpu_transport *percpu_transport;
    struct pgwt_merge_capture *merge_capture;
    struct fake_skeleton *skel;
};
struct linux_transport { struct fake_daemon *d; int possible, attempts_fd, discards_fd, failures_fd; };
static struct pgwt_capture_stats capture_stats;
static uint64_t produced[2] = {56, 56}, consumed[2] = {56, 56}, attempts[6], discards[6], failures[6];
static unsigned affinity = 3, registered, pulses;

struct pgwt_capture_stats pgwt_capture_stats(const struct pgwt_merge_capture *c)
{ (void)c; return capture_stats; }
static int percpu_triples(struct linux_transport *l, int fd, uint64_t *out, size_t n)
{
    (void)l;
    memcpy(out, fd == 1 ? attempts : fd == 2 ? discards : failures, n * sizeof(*out));
    return 0;
}
static int fake_getaffinity(int pid, size_t n, unsigned *out)
{ (void)pid; (void)n; *out = affinity; return 0; }
static int fake_setaffinity(int pid, size_t n, const unsigned *mask)
{ (void)pid; (void)n; affinity = *mask; return 0; }
static int bpf_object__find_map_fd_by_name(void *obj, const char *name)
{ (void)obj; return strcmp(name, "helper_cpus") ? -1 : 1; }
static int bpf_map_update_elem(int fd, const uint32_t *tid, const uint32_t *cpu, int flags)
{ (void)fd; (void)tid; (void)cpu; (void)flags; registered = 1; return 0; }
static int bpf_map_delete_elem(int fd, const uint32_t *tid)
{ (void)fd; (void)tid; registered = 0; return 0; }
static long fake_syscall(int nr)
{
    if (nr == 1) return 123; /* virtual SYS_gettid */
    if (!registered) return -1;
    ++pulses;
    if (affinity == 2) { ++attempts[4]; ++discards[4]; produced[0] += 56; }
    else { produced[0] += 56; produced[1] += 56; }
    return 456;
}
static void cd_positions(struct linux_transport *, const uint64_t [2], const uint64_t [2]);
static int linux_positions(struct linux_transport *l, uint64_t p[2], uint64_t c[2])
{
    memcpy(p, produced, sizeof(produced)); memcpy(c, consumed, sizeof(consumed));
    cd_positions(l, p, c); return 0;
}
/* Virtual I/O only. This compilation neither includes libbpf nor emits BPF. */
#define cpu_set_t unsigned
#define CPU_ALLOC_SIZE(n) sizeof(unsigned)
#define CPU_ALLOC(n) calloc(1, sizeof(unsigned))
#define CPU_FREE(p) free(p)
#define CPU_ZERO_S(n, p) (*(p) = 0)
#define CPU_SET_S(cpu, n, p) (*(p) |= 1U << (cpu))
#define sched_getaffinity fake_getaffinity
#define sched_setaffinity fake_setaffinity
#define syscall fake_syscall
#define SYS_gettid 1
#define SYS_getppid 2
#define BPF_NOEXIST 1
#include "transport_cutdiscard.h"

int main(int argc, char **argv)
{
    struct rlimit core_limit = {0, 0};
    setrlimit(RLIMIT_CORE, &core_limit);
    uint32_t cpus[2] = {0, 1}; uint64_t seq[6] = {0}, targets[6] = {0};
    bool acks[2] = {false, false};
    struct queued_wire waiting = {.record = {.dest=PGWT_WIRE_DEST_CPU,
        .envelope={.kind=MA_TRACE, .cookie_present=true, .cookie=1}}};
    struct pgwt_percpu_transport t = {.cpus=cpus, .ncpu=2, .seq=seq, .targets=targets,
        .requested=true, .e_seen=true, .l_seen=true, .acks=acks, .queue=&waiting, .count=1, .capacity=1};
    struct fake_skeleton skel = {0};
    struct fake_daemon d = {.percpu_transport=&t, .skel=&skel};
    struct linux_transport l = {.d=&d, .possible=2, .attempts_fd=1, .discards_fd=2, .failures_fd=3};
    cd_before_targets(&l);
    cd_require(cd_injected && pulses == 1 && !registered && affinity == 3,
               "kernel pulse restored registration and affinity");
    cd_require(!cd_allow_ring(&l), "defer rings before acknowledgements");
    targets[4] = 1; t.fencing = true; acks[0] = true;
    if (argc < 2 || strcmp(argv[1], "missing-ack")) acks[1] = true;
    uint64_t p[2], c[2]; linux_positions(&l, p, c);
    t.cut_taken = true; t.cut[0] = p[0]; t.cut[1] = p[1];
    cd_require(!cd_allow_ring(&l), "defer until completion actually holds");
    t.stats.cuts = 1; t.stats.holds = 1;
    if (argc > 1 && !strcmp(argv[1], "premature-data")) ++capture_stats.released_data;
    if (argc > 1 && !strcmp(argv[1], "premature-text")) ++capture_stats.released_text;
    if (argc > 1 && !strcmp(argv[1], "premature-frontier")) ++capture_stats.frontiers;
    if (argc > 1 && !strcmp(argv[1], "premature-completion")) ++t.stats.rounds_completed;
    cd_require(cd_allow_ring(&l) && cd_draining && pulses == 2 && !registered && affinity == 3,
               "resume only after HOLD and traffic beyond fixed cut");
    puts("CUTDISCARD HOST PASS: scheduler, pinned pulse cleanup, all acks, cut HOLD, later traffic");
    return 0;
}
