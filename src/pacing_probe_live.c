/* Linux daemon wiring for the BPF-free policy in pacing_probe.c. Refs #804. */
#define _GNU_SOURCE
#include "pacing_probe.h"
#include "daemon.h"
#include "pg_wait_tracer.skel.h"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint64_t pacing_fires(struct pgwt_daemon *d)
{
    return pgwt_exact_uprobe_fire_count(d, PGWT_UPROBE_FIRE_SELECT) +
           pgwt_exact_uprobe_fire_count(d, PGWT_UPROBE_FIRE_PSELECT6);
}

void pgwt_pacing_probe_finish(struct pgwt_daemon *d)
{
    if (!d->skel || !d->ckpt_pacing_probe_state ||
        !pgwt_needs_ckpt_pacing_probe(d->pg_major_version, d->pg_minor_version))
        return;
    /* Freeze exits first so teardown cannot manufacture unmatched exits by
     * removing enters while exits are still observing the checkpointer. */
#define DETACH_PACING(prog) do { \
    bpf_link__destroy(d->skel->links.prog); \
    d->skel->links.prog = NULL; \
} while (0)
    DETACH_PACING(on_select_exit);
    DETACH_PACING(on_pselect6_exit);
    DETACH_PACING(on_select_enter);
    DETACH_PACING(on_pselect6_enter);
#undef DETACH_PACING
    /* Final, frozen counters cover the entire capture, including its tail;
     * a last live control-socket scrape would leave that tail unobserved. */
    fprintf(stderr, "INFO: ckpt_pacing_final={\"ckpt_pacing_probe_state\":\"%s\","
            "\"fires\":%llu,\"unmatched_exits\":%llu}\n",
            d->ckpt_pacing_probe_state,
            (unsigned long long)pacing_fires(d),
            (unsigned long long)pgwt_exact_uprobe_fire_count(
                d, PGWT_UPROBE_FIRE_PACING_UNMATCHED_EXIT));
}

int pgwt_pacing_probe_attach(struct pgwt_daemon *d)
{
    d->ckpt_pacing_asleep_observations = 0;
    d->ckpt_pacing_last_check_ns = 0;
    d->ckpt_pacing_fatal = false;
    if (!pgwt_needs_ckpt_pacing_probe(d->pg_major_version, d->pg_minor_version)) {
        d->ckpt_pacing_probe_state = "not_applicable";
        return 0;
    }
    d->ckpt_pacing_probe_state = "unavailable";
    /* All four are mandatory. An attached but inert select-only probe was
     * the original failure: modern glibc implements pg_usleep via pselect6. */
#define ATTACH_PACING(prog) do { \
    d->skel->links.prog = bpf_program__attach(d->skel->progs.prog); \
    if (!d->skel->links.prog) { \
        fprintf(stderr, "FATAL: checkpointer pacing select/pselect6 probe " \
                "unavailable: cannot attach " #prog ": %s; refusing capture\n", \
                strerror(errno)); \
        return -1; \
    } \
} while (0)
    ATTACH_PACING(on_select_enter);
    ATTACH_PACING(on_select_exit);
    ATTACH_PACING(on_pselect6_enter);
    ATTACH_PACING(on_pselect6_exit);
#undef ATTACH_PACING
    d->ckpt_pacing_probe_state = "ok";
    return 0;
}

int pgwt_pacing_register_backend(struct pgwt_daemon *d, struct pgwt_backend *be)
{
    if (!pgwt_needs_ckpt_pacing_probe(d->pg_major_version, d->pg_minor_version) ||
        !be->is_alive || !be->meta_parsed ||
        be->meta.backend_type != PGWT_BT_CHECKPOINTER ||
        be->ckpt_pacing_registered)
        return 0;
    uint32_t pid = be->pid;
    uint8_t present = 1;
    int fd = bpf_map__fd(d->skel->maps.ckpt_pacing_pids);
    if (fd < 0 || bpf_map_update_elem(fd, &pid, &present, BPF_ANY) != 0) {
        d->ckpt_pacing_probe_state = "unavailable";
        d->ckpt_pacing_fatal = true;
        d->running = false;
        fprintf(stderr, "ERROR: checkpointer pacing select/pselect6 probe "
                "unavailable: cannot register PID %u; refusing capture\n", pid);
        return -1;
    }
    be->ckpt_pacing_registered = true;
    return 0;
}

void pgwt_pacing_probe_check(struct pgwt_daemon *d)
{
    if (d->ckpt_pacing_fatal ||
        !pgwt_needs_ckpt_pacing_probe(d->pg_major_version, d->pg_minor_version))
        return;
    uint64_t now = pgwt_debug_monotonic_ns();
    if (now - d->ckpt_pacing_last_check_ns < 1000000000ULL) return;
    d->ckpt_pacing_last_check_ns = now;
    uint64_t fires = pacing_fires(d);
    for (int i = 0; i < d->backends.count; i++) {
        struct pgwt_backend *be = &d->backends.entries[i];
        if (!be->is_alive || be->pid <= 0) continue;
        /* A freshly forked process may set its checkpointer title only after
         * enrollment. Retry metadata independently of capture mode. */
        if (!be->meta_parsed && pgwt_parse_cmdline(be->pid, &be->meta) == 0)
            be->meta_parsed = true;
        if (pgwt_pacing_register_backend(d, be) != 0) return;
        if (!be->ckpt_pacing_registered || !be->wp_addr || fires) continue;
        char path[64], line[1024];
        uint32_t wei;
        snprintf(path, sizeof(path), "/proc/%d/mem", be->pid);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        int valid = pread(fd, &wei, sizeof(wei), (off_t)be->wp_addr) == sizeof(wei);
        close(fd);
        if (!valid || wei != 0) continue;
        snprintf(path, sizeof(path), "/proc/%d/stat", be->pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int got = fgets(line, sizeof(line), f) != NULL;
        fclose(f);
        char *end = got ? strrchr(line, ')') : NULL;
        char state = 0;
        if (end && sscanf(end + 1, " %c", &state) == 1 && state == 'S')
            d->ckpt_pacing_asleep_observations++;
    }
    /* Re-read after evidence collection: an enter concurrent with proc reads
     * proves the hook is alive and masks all inert accounting for this run. */
    fires = pacing_fires(d);
    if (fires) d->ckpt_pacing_asleep_observations = 0;
    if (pgwt_pacing_probe_verdict(fires, d->ckpt_pacing_asleep_observations,
                                 PGWT_PACING_INERT_THRESHOLD) == PGWT_PACING_INERT) {
        d->ckpt_pacing_probe_state = "inert";
        fprintf(stderr, "ERROR: INERT checkpointer pacing probe: select/pselect6 "
                "matched zero fires with %llu unclassified asleep observations; "
                "ckpt_pacing_probe_state=inert; refusing capture\n",
                (unsigned long long)d->ckpt_pacing_asleep_observations);
        d->ckpt_pacing_fatal = true;
        d->running = false;
    }
}
