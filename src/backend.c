/* backend.c — Backend lifecycle: scan, fork, init, exit handling */
#include "backend.h"
#include "daemon.h"
#include "perf_event.h"
#include "discovery.h"
#include "cmdline.h"
#include "backend_meta.h"
#include "pg_wait_tracer.h"
#include "wait_event.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

void pgwt_backend_init(struct pgwt_backend_table *bt)
{
    memset(bt, 0, sizeof(*bt));
    for (int i = 0; i < MAX_BACKENDS; i++) {
        bt->entries[i].wp_fd = -1;
        bt->entries[i].bootstrap_fd = -1;
        bt->entries[i].wp_addr_shared = -1;
    }
}

struct pgwt_backend *pgwt_find_backend(struct pgwt_backend_table *bt, pid_t pid)
{
    for (int i = 0; i < bt->count; i++) {
        if (bt->entries[i].pid == pid)
            return &bt->entries[i];
    }
    return NULL;
}

static struct pgwt_backend *alloc_backend(struct pgwt_backend_table *bt, pid_t pid)
{
    /* Reuse dead slot */
    for (int i = 0; i < bt->count; i++) {
        if (!bt->entries[i].is_alive && bt->entries[i].pid == 0) {
            memset(&bt->entries[i], 0, sizeof(bt->entries[i]));
            bt->entries[i].wp_fd = -1;
            bt->entries[i].bootstrap_fd = -1;
            bt->entries[i].wp_addr_shared = -1;
            bt->entries[i].pid = pid;
            bt->entries[i].is_alive = true;
            return &bt->entries[i];
        }
    }
    /* Append */
    if (bt->count >= MAX_BACKENDS) {
        fprintf(stderr, "WARN: max backends (%d) reached, cannot track PID %d\n",
                MAX_BACKENDS, pid);
        return NULL;
    }
    struct pgwt_backend *be = &bt->entries[bt->count++];
    memset(be, 0, sizeof(*be));
    be->wp_fd = -1;
    be->bootstrap_fd = -1;
    be->wp_addr_shared = -1;
    be->pid = pid;
    be->is_alive = true;
    return be;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Resolve a backend's wait_event_info address using the version-appropriate
 * path. PG17+: my_wait_ptr_addr is the my_wait_event_info global (a uint32*
 * that already points at the field). PG<17 (use_myproc): my_wait_ptr_addr is
 * the MyProc PGPROC* global — deref it and add pgproc_wait_offset. Returns 0
 * if the backend has not set MyProc / the pointer yet. */
uint64_t pgwt_resolve_backend_wait_addr(struct pgwt_daemon *d, pid_t pid)
{
    uint64_t started_ns = pgwt_debug_block_begin(d);
    uint64_t addr;
    if (d->use_myproc)
        addr = pgwt_resolve_wait_addr_via_myproc(pid, d->my_wait_ptr_addr,
                                                 d->pgproc_wait_offset);
    else
        addr = pgwt_read_pointer(pid, d->my_wait_ptr_addr);
    pgwt_debug_block_end(d, "backend_wait_addr_resolve", pid, started_ns);
    return addr;
}

/* Loud, once-only report that the BPF state_map is full (CAP-1). Every
 * failed insert is a backend that will record NOTHING — never silent. */
static void report_state_map_full(struct pgwt_daemon *d, pid_t pid)
{
    d->counters.state_map_full_total++;
    if (d->state_map_full_logged)
        return;
    d->state_map_full_logged = true;
    fprintf(stderr,
            "ERROR: BPF state_map is FULL — cannot track PID %d (and any "
            "further backend).\n"
            "  Backends without a state_map entry record NO events/samples. "
            "state_map_full_total on the control socket counts every "
            "affected backend.\n",
            pid);
}

/* TEST HOOK (PGWT_TEST_STALE_SEED): the FAKE wait the poisoned seed carries —
 * Timeout:PgSleep (class 0x09, event 2), a real encoding the smoke tests
 * already know, so the stale state renders with a recognizable name. */
#define PGWT_TEST_STALE_WEI (((uint32_t)PG_WAIT_TIMEOUT << 24) | 2)

/* Pre-seed the BPF state_map for a backend whose wait_event_info address is
 * already resolved (be->wp_addr). Reads the backend's CURRENT wait_event_info
 * and query_id and writes them as the initial state, so the first watchpoint
 * fire ACCUMULATES the in-progress wait instead of discarding it (otherwise a
 * backend already blocked at attach time — e.g. Lock:relation — loses its
 * opening interval).
 *
 * CAP-2/5 backstop: the value read here is classified on EVERY call (all PG
 * versions, re-exercised on each escalation): a garbage class byte means the
 * resolved address is wrong — counted + logged loudly, and never seeded as
 * if it were a real wait. A non-zero valid reading (re)confirms the offset.
 *
 * sweep_reseed: false for the attach-time seed (scan/init/escalation), true
 * when called as pgwt_sweep_stale_state's REPAIR. The only difference is the
 * PGWT_TEST_STALE_SEED hook below, which must poison ONLY the attach seed —
 * poisoning the repair too would re-inject the very staleness the sweep just
 * removed, forever.
 * Returns 0 on success, -1 if the state_map insert failed (map full). */
static int preseed_state_map(struct pgwt_daemon *d, pid_t pid, uint64_t addr,
                             uint64_t attach_ts, bool sweep_reseed)
{
    char mem_path[64];
    snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    int mem_fd = open(mem_path, O_RDONLY);
    if (mem_fd < 0)
        return 0;   /* backend gone — nothing to seed */

    int rc = 0;
    uint32_t current_wei = 0;
    if (pread(mem_fd, &current_wei, sizeof(current_wei), addr) ==
        sizeof(current_wei)) {
        switch (pgwt_classify_wei(current_wei)) {
        case PGWT_WEI_VALID_NONZERO:
            d->wait_offset_validated = true;   /* ongoing re-validation */
            break;
        case PGWT_WEI_GARBAGE:
            d->counters.invalid_wait_reads_total++;
            if (!d->invalid_wait_reads_logged) {
                d->invalid_wait_reads_logged = true;
                fprintf(stderr,
                        "ERROR: wait_event_info read for PID %d returned "
                        "0x%08x — invalid wait-event class byte.\n"
                        "  The resolved address/offset is wrong for this "
                        "PostgreSQL build; data from this address is NOT "
                        "recorded (invalid_wait_reads_total counts these).\n",
                        pid, current_wei);
            }
            /* Do not seed a garbage wait state; seed as on-CPU so nothing
             * fabricated ever enters the trace. */
            current_wei = 0;
            break;
        default:
            break;   /* zero: consistent, not proof */
        }

        /* TEST HOOK (PGWT_TEST_STALE_SEED): deterministically reproduce the
         * seed→arm race (docs/ROADMAP_AND_STATUS.md). In the real race the
         * wait read here ends in the microseconds before PERF_EVENT_IOC_ENABLE
         * arms the watchpoint: the wei write to 0 never fires, so the entry
         * stays last_event=<ended wait>, on_cpu_ts=0 forever (a waitless loop
         * produces no later fire and a quiet host no sched_switch). Force that
         * exact end state on EVERY attach seed: a FAKE nonzero wait
         * (Timeout:PgSleep) regardless of the actual reading — on_cpu_ts then
         * seeds 0 via the current_wei != 0 branch below, matching the missed
         * fire. pgwt_sweep_stale_state must repair it. NEVER applied to the
         * sweep's own repair reseed (sweep_reseed — see the function comment);
         * never set in production (it fabricates the seeded opening label). */
        if (!sweep_reseed && getenv("PGWT_TEST_STALE_SEED")) {
            char fake_name[64];
            pgwt_event_full_name(PGWT_TEST_STALE_WEI, fake_name,
                                 sizeof(fake_name));
            fprintf(stderr, "WARN: PGWT_TEST_STALE_SEED — seeding PID %d with "
                    "FAKE stale wait %s (actual 0x%08x; "
                    "deterministic seed→arm-race repro; TEST ONLY)\n",
                    pid, fake_name, current_wei);
            current_wei = PGWT_TEST_STALE_WEI;
        }

        /* Read current query_id from MyBEEntry->st_query_id */
        uint64_t current_qid = 0;
        if (d->my_be_entry_addr && d->st_query_id_offset > 0) {
            uint64_t be_ptr = 0;
            if (pread(mem_fd, &be_ptr, sizeof(be_ptr),
                      d->my_be_entry_addr) == sizeof(be_ptr)
                && be_ptr
                && pread(mem_fd, &current_qid,
                         sizeof(current_qid),
                         be_ptr + d->st_query_id_offset)
                   != sizeof(current_qid))
                current_qid = 0;
        }

        int state_fd = bpf_map__fd(d->skel->maps.state_map);
        /* Preserve the command-open gate across the (re-)seed: the
         * on_report_activity uprobe only fires on boundaries, so zeroing
         * cmd_open here would mis-gate the backend until its next command
         * flip (matters on tiered re-escalation mid-command). */
        uint16_t prev_cmd_open = 0;
        {
            uint32_t k = pid;
            struct pgwt_pid_state prev;
            if (bpf_map_lookup_elem(state_fd, &k, &prev) == 0)
                prev_cmd_open = prev.cmd_open;
        }
        /* Straddle fix (exact tier): a backend already RUNNING a command when
         * we first seed it missed the on_report_activity START edge, so its
         * cmd_open would stay 0 and its we==0 (CPU) intervals would be
         * mislabeled non-command churn and dropped — exact-tier CPU* collapses
         * to 0 for that command (the full-mode analogue of the sampler
         * straddle bug). Level-check the backend's own debug_query_string at
         * seed time: non-NULL == inside a command (the pg_stat_activity
         * state='active' window). This seeds the gate correctly from the first
         * watchpoint fire; the emitted events then carry CMD_OPEN and the live
         * accumulator counts the CPU. (mem_fd is already open on this backend;
         * the read is against its own process-local global.) */
        uint16_t seed_cmd_open = prev_cmd_open;
        if (!seed_cmd_open && d->debug_query_string_addr) {
            uint64_t dqs = 0;
            if (pread(mem_fd, &dqs, sizeof(dqs),
                      (off_t)d->debug_query_string_addr) == sizeof(dqs) && dqs)
                seed_cmd_open = 1;
        }
        struct pgwt_pid_state init_state = {
            .last_event = current_wei,
            .wp_live = 1,   /* a live watchpoint now owns last_event/last_ts */
            .cmd_open = seed_cmd_open,
            .last_ts = attach_ts,
            .last_query_id = current_qid,
            .wait_event_addr = addr,
            /* S3: exact CPU is accumulated by the sched_switch program into
             * cpu_ns_total/on_cpu_ts. Seed the base at 0 (deltas count from
             * attach); open the on-CPU stretch here iff the backend is running
             * (we==0) so a command straddling attach has its CPU measured from
             * the seed instant. attach_ts is CLOCK_MONOTONIC == bpf_ktime. */
            .cpu_ns_total = 0,
            /* Open the on-CPU stretch at the seed iff the backend is running
             * (we==0) now. TEST HOOK PGWT_TEST_NO_SCHED_ONCPU also forces this
             * to 0 so on_cpu_ts can ONLY come from a watchpoint fire, making the
             * straddle live-CPU repro deterministic (background backends on-CPU
             * at seed can't then pollute the system-wide live CPU*) — EXCEPT
             * the sweep's repair reseed (sweep_reseed): the sweep exists
             * precisely to recover from a MISSED fire, so under the hook its
             * repair is the one sanctioned non-fire opener. Without this
             * exemption the hook would neuter the very repair
             * phase_stale_seed_sweep uses it to isolate (the backend it
             * repairs is waitless — no fire will ever open the stretch). */
            .on_cpu_ts    = (d->cpu_accounting && current_wei == 0 &&
                             (sweep_reseed ||
                              !getenv("PGWT_TEST_NO_SCHED_ONCPU"))) ? attach_ts : 0,
            .last_cpu_ns  = 0,
        };
        uint32_t pid_key = pid;
        /* BPF_ANY (not NOEXIST): on first full-mode attach this creates the
         * entry; on tiered RE-escalation it REFRESHES a reset/sampled entry to
         * the backend's CURRENT wait state, so an in-progress wait at escalate
         * time is captured instead of being lost behind a stale last_event.
         * (This BPF_ANY refresh also covers the PID-reuse window — CAP-9: a
         * stale entry from a recycled PID is overwritten on every attach.)
         * CAP-1: the insert is CHECKED — a full map is a backend that records
         * nothing, which must never be silent. */
        if (bpf_map_update_elem(state_fd, &pid_key, &init_state, BPF_ANY) != 0) {
            report_state_map_full(d, pid);
            rc = -1;
        }
    }
    close(mem_fd);
    return rc;
}

int pgwt_attach_backend_watchpoint(struct pgwt_daemon *d,
                                   struct pgwt_backend *be)
{
    if (be->wp_addr == 0 || be->wp_fd >= 0)
        return be->wp_fd >= 0 ? 0 : -1;

    /* CAP-8 ordering: open DISABLED → preseed state_map → enable. Opening
     * enabled first left a window where the first transition fired against
     * an unseeded (or stale, on re-escalation) state entry and the opening
     * interval was mis-timed. */
    int wp_prog_fd = bpf_program__fd(d->skel->progs.on_watchpoint);
    uint64_t started_ns = pgwt_debug_block_begin(d);
    be->wp_fd = pgwt_open_watchpoint_disabled(be->pid, be->wp_addr, wp_prog_fd);
    pgwt_debug_block_end(d, be->wp_fd < 0 ? "watchpoint_open_failed"
                                          : "watchpoint_open",
                         be->pid, started_ns);
    if (be->wp_fd < 0) {
        /* Process likely exited before attach — caller decides on cleanup. */
        d->counters.wp_attach_failures_total++;
        return -1;
    }

    if (be->attach_ts == 0)
        be->attach_ts = now_ns();

    /* A backend initialized after an exact generation began did not exist in
     * the all-backend snapshot. Seed it now before its watchpoint is armed.
     * Post-boundary query/activity edges already captured by the BPF fork
     * enrollment win over this separate seed during resolution. */
    if (d->exact_probes.core.generation != 0)
        pgwt_exact_seed_backend(d, be->pid,
                                d->exact_probes.core.generation, false);
    started_ns = pgwt_debug_block_begin(d);
    preseed_state_map(d, be->pid, be->wp_addr, be->attach_ts, false);
    pgwt_debug_block_end(d, "watchpoint_preseed", be->pid, started_ns);

    started_ns = pgwt_debug_block_begin(d);
    int enable_rc = pgwt_watchpoint_enable(be->wp_fd);
    pgwt_debug_block_end(d, enable_rc != 0 ? "watchpoint_enable_failed"
                                           : "watchpoint_enable",
                         be->pid, started_ns);
    if (enable_rc != 0) {
        started_ns = pgwt_debug_block_begin(d);
        pgwt_close_watchpoint(be->wp_fd);
        pgwt_debug_block_end(d, "watchpoint_close", be->pid, started_ns);
        be->wp_fd = -1;
        d->counters.wp_attach_failures_total++;
        return -1;
    }
    return 0;
}

int pgwt_scan_existing_backends(struct pgwt_daemon *d)
{
    DIR *proc = opendir("/proc");
    if (!proc) {
        perror("opendir /proc");
        return -1;
    }

    /* TEST HOOK (PGWT_TEST_STRADDLE_SKIP): deterministically reproduce the
     * initial-scan straddle race by skipping any backend that is inside a
     * command at scan time (debug_query_string set) — exactly the pre-existing
     * pure-CPU straddler the race loses. pgwt_recover_unattached_backends must
     * then pick it up on a later tick. Never set in production. */
    int skip_straddle = getenv("PGWT_TEST_STRADDLE_SKIP") != NULL;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(proc)) != NULL) {
        pid_t pid = atoi(ent->d_name);
        if (pid <= 0)
            continue;

        /* Read /proc/<pid>/stat to get ppid (field 4) */
        char stat_path[64];
        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
        FILE *f = fopen(stat_path, "r");
        if (!f) continue;

        /* Format: pid (comm) state ppid ...
         * comm can contain spaces and parens, so find last ')' */
        char stat_line[512];
        if (!fgets(stat_line, sizeof(stat_line), f)) { fclose(f); continue; }
        fclose(f);
        char *last_paren = strrchr(stat_line, ')');
        if (!last_paren) continue;
        char state;
        int ppid;
        if (sscanf(last_paren + 1, " %c %d", &state, &ppid) != 2)
            continue;

        if (ppid != d->postmaster_pid)
            continue;

        if (pgwt_pgbs_exclusion_contains(&d->pgbs_validation_exclusions,
                                         pid)) {
            if (d->verbose)
                fprintf(stderr, "INFO: excluding PgBackendStatus validation "
                        "PID %d from startup capture enrollment\n", pid);
            continue;
        }

        /* TEST HOOK: skip a straddling in-command backend (see top of fn) so
         * pgwt_recover_unattached_backends must attach it later. */
        if (skip_straddle && d->debug_query_string_addr) {
            char mp[64];
            snprintf(mp, sizeof(mp), "/proc/%d/mem", pid);
            int mf = open(mp, O_RDONLY);
            if (mf >= 0) {
                uint64_t dqs = 0;
                ssize_t got = pread(mf, &dqs, sizeof(dqs),
                                    (off_t)d->debug_query_string_addr);
                close(mf);
                if (got == (ssize_t)sizeof(dqs) && dqs) {
                    fprintf(stderr, "WARN: PGWT_TEST_STRADDLE_SKIP — skipping "
                            "in-command PID %d at scan (recovery must attach)\n",
                            pid);
                    continue;
                }
            }
        }

        /* This is a postgres child. Resolve its wait_event_info address
         * (PG17+ my_wait_event_info global; PG<17 MyProc + offset).
         *
         * Note: on PG17+ a long-lived process whose pointer still targets
         * the process-local dummy (aux processes without a PGPROC, e.g.
         * syslogger) is deliberately accepted here — the dummy IS that
         * process's wait_event_info, so watchpointing it is correct. The
         * one residual edge is a backend caught in its few-ms init window
         * DURING this one-time scan (dummy now, PGPROC in a moment); the
         * fork path (pgwt_handle_fork) closes the same race properly via
         * the shared-memory check + bootstrap watchpoint. */
        uint64_t ptr = pgwt_resolve_backend_wait_addr(d, pid);
        if (ptr == 0) {
            /* Not yet initialized — treat as fresh fork */
            if (d->verbose)
                fprintf(stderr, "INFO: PID %d not yet initialized, attaching bootstrap\n", pid);
            pgwt_handle_fork(d, pid);
            continue;
        }

        /* Runtime validation (CAP-2/3/5) — ALL versions. Classify the value
         * at the resolved address for EVERY backend until one produces
         * proof:
         *   garbage class byte  → the offset/address is wrong (custom build
         *                         / mis-detected layout) — refuse to attach
         *                         rather than trace nonsense;
         *   non-zero valid      → proof, offset validated;
         *   zero                → NOT proof (zero is also the most likely
         *                         reading from a wrong offset) — keep
         *                         checking the remaining backends; if the
         *                         whole scan proves nothing,
         *                         pgwt_confirm_wait_offset() re-polls. */
        if (!d->wait_offset_validated) {
            uint64_t started_ns = pgwt_debug_block_begin(d);
            int v = pgwt_validate_wait_addr(pid, ptr);
            pgwt_debug_block_end(d, "backend_wait_addr_validate", pid,
                                 started_ns);
            if (v == PGWT_WEI_GARBAGE) {
                fprintf(stderr,
                        "FATAL: resolved wait_event_info for PID %d holds a value "
                        "with an invalid wait-event class byte.\n"
                        "  %s is likely wrong for this PG%d build — refusing "
                        "to attach.\n",
                        pid,
                        d->use_myproc
                            ? "offsetof(PGPROC, wait_event_info)"
                            : "the resolved my_wait_event_info address",
                        d->pg_major_version);
                closedir(proc);
                return -2;   /* validation refusal — caller must ABORT */
            }
            if (v == PGWT_WEI_VALID_NONZERO)
                d->wait_offset_validated = true;
            /* v == PGWT_WEI_ZERO or v < 0: inconclusive — next backend. */
        }

        /* Already initialized — record the address. */
        struct pgwt_backend *be = alloc_backend(&d->backends, pid);
        if (!be) continue;

        be->wp_addr = ptr;

        /* Sampled mode: register the backend (address + metadata) but arm
         * NO watchpoint. The sampler reads wp_addr directly each tick. Keep a
         * state_map entry for a degraded uprobe fallback and for a
         * future exact generation's watchpoint enrollment. */
        if (!pgwt_mode_uses_watchpoints(d)) {
            be->attach_ts = now_ns();
            if (pgwt_parse_cmdline(pid, &be->meta) == 0)
                be->meta_parsed = true;
            if (d->backend_meta)
                pgwt_bm_write(d->backend_meta, pid, &be->meta);

            int state_fd = bpf_map__fd(d->skel->maps.state_map);
            struct pgwt_pid_state init_state = {
                .last_ts = be->attach_ts,
                .wait_event_addr = ptr,
            };
            uint32_t pid_key = pid;
            /* CAP-1: EEXIST is fine (idempotent seed); anything else means
             * the map is full — this backend's query_ids will be missing. */
            int urc = bpf_map_update_elem(state_fd, &pid_key, &init_state,
                                          BPF_NOEXIST);
            if (urc != 0 && urc != -EEXIST && errno != EEXIST)
                report_state_map_full(d, pid);

            if (d->verbose)
                fprintf(stderr, "INFO: tracking PID %d (%s), sampled at 0x%lx\n",
                        pid, pgwt_backend_type_name(be->meta.backend_type),
                        (unsigned long)ptr);
            count++;
            continue;
        }

        /* Full mode — attach real watchpoint directly. The shared helper
         * opens the watchpoint and pre-seeds the BPF state_map with the
         * backend's current wait state (so an in-progress wait at attach
         * time is not lost). The same helper is reused on escalation. */
        be->attach_ts = now_ns();
        if (pgwt_attach_backend_watchpoint(d, be) != 0) {
            /* Silently skip — process likely exited before attach */
            be->is_alive = false;
            be->pid = 0;
            continue;
        }

        uint64_t started_ns = pgwt_debug_block_begin(d);
        int cmdline_rc = pgwt_parse_cmdline(pid, &be->meta);
        pgwt_debug_block_end(d, "backend_cmdline_read", pid, started_ns);
        if (cmdline_rc == 0)
            be->meta_parsed = true;
        if (d->backend_meta) {
            started_ns = pgwt_debug_block_begin(d);
            pgwt_bm_write(d->backend_meta, pid, &be->meta);
            pgwt_debug_block_end(d, "backend_meta_write", pid, started_ns);
        }

        if (d->verbose)
            fprintf(stderr, "INFO: attached to PID %d (%s), watchpoint at 0x%lx\n",
                    pid, pgwt_backend_type_name(be->meta.backend_type),
                    (unsigned long)ptr);
        count++;
    }
    closedir(proc);
    return count;
}

/* Periodic recovery for the initial-scan straddle race (docs/ROADMAP_AND_STATUS.md).
 *
 * pgwt_scan_existing_backends runs ONCE at startup. A pre-existing backend
 * whose wait_event_info was transiently unresolvable at that single instant
 * (a /proc read race under load — much likelier on a 2-CPU CI runner, and on
 * PG13 where the address is *MyProc + offset, an extra indirection) is either
 * skipped outright (its /proc/stat read failed) or routed to the bootstrap
 * path. The bootstrap watchpoint fires only on a wait_event_info WRITE, which
 * a backend already past InitProcess never does again — so a WAITLESS command
 * (pure compute: no wait boundaries, so the trace stays empty and only the
 * live open-interval read would show its CPU) stays untraced for its whole
 * life and reads CPU* = 0. There was no recovery: the timer GC only reaps DEAD
 * backends, never re-attaches a missed live one.
 *
 * This re-scans /proc each tick and attaches any postmaster child that still
 * lacks a live watchpoint but is now resolvable AND initialized (its PGPROC is
 * in shared memory — the same predicate handle_fork uses to attach directly).
 * Idempotent: a backend already holding a live watchpoint (wp_fd >= 0) is
 * skipped cheaply. A freshly-forked backend still in InitProcess points at its
 * process-LOCAL dummy (addr_is_shared != 1) and is left to its bootstrap
 * watchpoint, which WILL fire for it. Cheap: one /proc pass, the same
 * resolution the scan uses. Returns the number of backends recovered. */
int pgwt_recover_unattached_backends(struct pgwt_daemon *d)
{
    if (!pgwt_mode_uses_watchpoints(d))
        return 0;

    DIR *proc = opendir("/proc");
    if (!proc)
        return 0;

    int recovered = 0;
    struct dirent *ent;
    while ((ent = readdir(proc)) != NULL) {
        pid_t pid = atoi(ent->d_name);
        if (pid <= 0)
            continue;

        /* Already attached with a live watchpoint — nothing to recover. This
         * is the common case (every healthy backend), kept cheap. */
        struct pgwt_backend *be = pgwt_find_backend(&d->backends, pid);
        if (be && be->wp_fd >= 0)
            continue;

        /* Postmaster child? (ppid == postmaster, field 4 of /proc/pid/stat). */
        char stat_path[64];
        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
        FILE *f = fopen(stat_path, "r");
        if (!f)
            continue;
        char stat_line[512];
        if (!fgets(stat_line, sizeof(stat_line), f)) { fclose(f); continue; }
        fclose(f);
        char *last_paren = strrchr(stat_line, ')');
        if (!last_paren)
            continue;
        char state;
        int ppid;
        if (sscanf(last_paren + 1, " %c %d", &state, &ppid) != 2)
            continue;
        if (ppid != d->postmaster_pid)
            continue;

        if (pgwt_pgbs_exclusion_contains(&d->pgbs_validation_exclusions,
                                         pid))
            continue;

        /* Resolvable AND initialized (PGPROC in shm)? Attach directly — the
         * level-triggered version of handle_fork's direct-attach fast path. A
         * still-initializing backend (local dummy) is left to bootstrap. */
        uint64_t ptr = pgwt_resolve_backend_wait_addr(d, pid);
        uint64_t started_ns = pgwt_debug_block_begin(d);
        int addr_shared = ptr != 0 ? pgwt_addr_is_shared(pid, ptr) : 0;
        pgwt_debug_block_end(d, "backend_addr_shared", pid, started_ns);
        if (ptr != 0 && addr_shared == 1) {
            if (pgwt_handle_init(d, pid, ptr) == 0) {
                recovered++;
                if (d->verbose)
                    fprintf(stderr, "INFO: recovered unattached backend PID %d "
                            "(initial-scan straddle race)\n", pid);
            }
        }
    }
    closedir(proc);
    return recovered;
}

/* Staleness predicate knobs (pgwt_sweep_stale_state below).
 * FROZEN: the entry must have seen NO EMITTED transition for at least this
 * long (last_ts is stamped by BPF on every emitted transition and by the
 * seed; redundant-write-suppressed fires return BEFORE stamping — but a
 * suppressed fire implies wei == last_event, so it can never satisfy the
 * mismatch arm simultaneously). A healthy backend transitioning normally
 * always has a fresher last_ts, so only a genuinely transition-less entry
 * can qualify. With a 1s display interval the seed
 * is already ~interval old at the first tick, so repair lands on the first or
 * (borderline) second tick — level-triggered, so either is fine.
 * RECHECK: the wei mismatch must survive a re-read this far apart. A real
 * in-flight transition (wei read mid-flip) cannot hold one value across the
 * recheck AND leave last_ts frozen for >= 1s at the same time. */
#define PGWT_SWEEP_FROZEN_NS   1000000000ULL   /* 1s */
#define PGWT_SWEEP_RECHECK_US  2000            /* 2ms */

/* Level-triggered state-consistency sweep for ATTACHED backends — the sibling
 * of pgwt_recover_unattached_backends above, which only covers UNattached
 * ones. Closes the residual seed→arm race (docs/ROADMAP_AND_STATUS.md): the
 * preseed reads wait_event_info microseconds before PERF_EVENT_IOC_ENABLE
 * arms the watchpoint; if the seeded wait ENDS inside that window, the wei
 * write to 0 lands before the arm and never fires. The entry then stays
 * frozen at last_event=<a wait that ended long ago>, on_cpu_ts=0 — a waitless
 * pure-CPU loop produces no later fire, and on a quiet low-CPU host no
 * sched_switch opens the stretch either — so the live view attributes the
 * whole open interval to the dead wait and reads CPU* = 0 (PR #56's
 * fire-time on_cpu_ts open never runs: there IS no fire).
 *
 * PREDICATE (conservative — a genuine in-flight transition must never trip
 * it): the entry is FROZEN (no fire for >= PGWT_SWEEP_FROZEN_NS) AND the
 * actual /proc wei differs from last_event STABLY (two reads
 * PGWT_SWEEP_RECHECK_US apart, equal to each other, both != last_event) AND
 * the entry is still unchanged when re-checked after the second read (a real
 * fire in between updates last_event/last_ts and disqualifies — this is the
 * reseed-vs-real-fire clobber guard). Garbage readings never trigger anything
 * (CAP-2/5) — counted like the preseed counts them.
 *
 * REPAIR = RESEED, never fabricate: the exact attach-time seed
 * (preseed_state_map — last_event=actual wei, last_ts=now, CPU base reset,
 * on_cpu_ts opened iff actually on-CPU, cmd_open preserved). The stale open
 * interval tracked under the dead label is DROPPED, not emitted — its
 * boundary was never observed, and emitting an invented duration would
 * fabricate data. Loud: one INFO line per repair + state_reseeds_total on the
 * control socket. Idempotent; cheap when healthy (one map lookup per attached
 * backend, /proc reads only on a frozen entry). Returns the number of entries
 * reseeded. */
int pgwt_sweep_stale_state(struct pgwt_daemon *d)
{
    if (!pgwt_mode_uses_watchpoints(d))
        return 0;

    int state_fd = bpf_map__fd(d->skel->maps.state_map);
    uint64_t now = now_ns();
    int repaired = 0;

    for (int i = 0; i < d->backends.count; i++) {
        struct pgwt_backend *be = &d->backends.entries[i];
        if (!be->is_alive || be->pid <= 0 || be->wp_fd < 0 || be->wp_addr == 0)
            continue;

        uint32_t pid_key = be->pid;
        struct pgwt_pid_state st;
        if (bpf_map_lookup_elem(state_fd, &pid_key, &st) != 0)
            continue;
        /* Seed-only entries (no live watchpoint) carry no interval state. */
        if (!st.wp_live)
            continue;
        /* FROZEN gate — the common case (fresh last_ts) exits here. */
        if (now < st.last_ts || now - st.last_ts < PGWT_SWEEP_FROZEN_NS)
            continue;

        char mem_path[64];
        snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", be->pid);
        int mem_fd = open(mem_path, O_RDONLY);
        if (mem_fd < 0)
            continue;   /* backend gone — the exit path owns the entry */

        uint32_t wei_a = 0;
        if (pread(mem_fd, &wei_a, sizeof(wei_a), be->wp_addr) !=
            sizeof(wei_a)) {
            close(mem_fd);
            continue;
        }
        if (pgwt_classify_wei(wei_a) == PGWT_WEI_GARBAGE) {
            /* Same backstop as the preseed (CAP-2/5): a garbage class byte
             * means the address is suspect — count it, never act on it. */
            d->counters.invalid_wait_reads_total++;
            close(mem_fd);
            continue;
        }
        if (wei_a == st.last_event) {
            close(mem_fd);
            continue;   /* consistent — a long genuine wait or CPU stretch */
        }

        /* STABILITY recheck: the mismatch must hold across a short delay so a
         * transition caught mid-flip can never qualify. */
        usleep(PGWT_SWEEP_RECHECK_US);
        uint32_t wei_b = 0;
        ssize_t got = pread(mem_fd, &wei_b, sizeof(wei_b), be->wp_addr);
        close(mem_fd);
        if (got != (ssize_t)sizeof(wei_b) || wei_b != wei_a)
            continue;

        /* Clobber guard: a REAL fire during the recheck window updates
         * last_event/last_ts — re-check the entry and stand down if the
         * watchpoint turned out to be alive after all. This shrinks the
         * reseed-vs-real-fire race to the µs between this lookup and the
         * BPF_ANY update below, and a fire-less entry (the only kind that
         * reaches here) by definition has nothing racing in that window. */
        struct pgwt_pid_state st2;
        if (bpf_map_lookup_elem(state_fd, &pid_key, &st2) != 0)
            continue;
        if (st2.last_event != st.last_event || st2.last_ts != st.last_ts)
            continue;

        /* REPAIR = RESEED (see the function comment). The reseed re-reads the
         * backend's CURRENT wei itself — level-triggered: seed present truth,
         * never a value from a few ms ago. */
        char stale_name[80], actual_name[80];
        pgwt_event_full_name(st.last_event, stale_name, sizeof(stale_name));
        pgwt_event_full_name(wei_a, actual_name, sizeof(actual_name));
        if (preseed_state_map(d, be->pid, be->wp_addr, now_ns(), true) == 0) {
            d->counters.state_reseeds_total++;
            fprintf(stderr, "INFO: reseeded stale state for PID %d: state "
                    "said %s, actual %s (missed watchpoint fire — seed→arm "
                    "race; state_reseeds_total counts these)\n",
                    be->pid, stale_name, actual_name);
            repaired++;
        }
    }
    return repaired;
}

/* CAP-2/3: post-scan offset confirmation. Only reached when the initial scan
 * resolved backends but NONE of them read a non-zero class-valid value at
 * that instant. On a real PostgreSQL this is transient (aux processes sit in
 * Activity:*Main waits almost always), so a short re-poll settles it; a
 * WRONG offset pointing into zeroed memory reads zero FOREVER — exactly the
 * case CAP-2 exists for — and must not be blessed. */
int pgwt_confirm_wait_offset(struct pgwt_daemon *d)
{
    if (d->wait_offset_validated)
        return 0;

    /* Collect resolved live backends. */
    int have_resolved = 0;
    for (int i = 0; i < d->backends.count; i++)
        if (d->backends.entries[i].is_alive
            && d->backends.entries[i].pid > 0
            && d->backends.entries[i].wp_addr != 0)
            have_resolved++;
    if (have_resolved == 0)
        return 0;   /* nothing to confirm yet — handle_init validates later */

    /* Re-poll for up to ~2s (40 × 50ms). Exits on the FIRST proof, so on a
     * healthy system this costs one round. */
    for (int round = 0; round < 40; round++) {
        for (int i = 0; i < d->backends.count; i++) {
            struct pgwt_backend *be = &d->backends.entries[i];
            if (!be->is_alive || be->pid <= 0 || be->wp_addr == 0)
                continue;
            int v = pgwt_validate_wait_addr(be->pid, be->wp_addr);
            if (v == PGWT_WEI_VALID_NONZERO) {
                d->wait_offset_validated = true;
                if (d->verbose)
                    fprintf(stderr, "INFO: wait_event_info offset confirmed "
                            "(PID %d, non-zero class-valid reading)\n",
                            be->pid);
                return 0;
            }
            if (v == PGWT_WEI_GARBAGE) {
                fprintf(stderr,
                        "FATAL: resolved wait_event_info for PID %d holds a "
                        "value with an invalid wait-event class byte.\n"
                        "  %s is wrong for this PG%d build — refusing to "
                        "attach.\n",
                        be->pid,
                        d->use_myproc
                            ? "offsetof(PGPROC, wait_event_info)"
                            : "the resolved my_wait_event_info address",
                        d->pg_major_version);
                return -1;
            }
        }
        usleep(50000);
    }

    if (d->use_myproc) {
        /* Hardcoded-offset path (PG<17): an offset that cannot be confirmed
         * is indistinguishable from a wrong one — fail safe, never trace
         * garbage (or nothing) labeled as real. */
        fprintf(stderr,
                "FATAL: cannot confirm offsetof(PGPROC, wait_event_info)=%d "
                "for PG%d: every reading from %d resolved backend(s) was "
                "zero.\n"
                "  A wrong offset into zeroed memory reads exactly this way. "
                "Refusing to attach rather than record garbage or nothing.\n"
                "  If this instance is genuinely all-idle, retry while a "
                "session is waiting (e.g. run SELECT pg_sleep(60)).\n",
                d->pgproc_wait_offset, d->pg_major_version, have_resolved);
        return -1;
    }

    /* PG17+: the address comes from the backend's own my_wait_event_info
     * pointer, so an unconfirmed (all-zero) state is far less suspicious —
     * degrade to a loud warning; preseed/sampler reads keep re-validating. */
    fprintf(stderr,
            "WARN: wait_event_info address not yet confirmed by a non-zero "
            "reading (%d backends, all on-CPU/zero). Validation continues on "
            "every read; garbage readings will be counted and reported.\n",
            have_resolved);
    return 0;
}

int pgwt_handle_fork(struct pgwt_daemon *d, pid_t child_pid)
{
    if (pgwt_pgbs_exclusion_contains(&d->pgbs_validation_exclusions,
                                     child_pid)) {
        if (d->verbose)
            fprintf(stderr, "INFO: excluding PgBackendStatus validation "
                    "PID %d from fork capture enrollment\n", child_pid);
        return 0;
    }

    /* Check for duplicate */
    if (pgwt_find_backend(&d->backends, child_pid))
        return 0;

    struct pgwt_backend *be = alloc_backend(&d->backends, child_pid);
    if (!be) return -1;

    /* Sampled mode: no bootstrap watchpoint. Register the backend now; the
     * sampler lazily resolves wp_addr once the backend sets the pointer. */
    if (!pgwt_mode_uses_watchpoints(d)) {
        if (d->verbose)
            fprintf(stderr, "INFO: fork detected PID %d (sampled, no watchpoint)\n",
                    child_pid);
        return 0;
    }

    /* Attach bootstrap watchpoint on my_wait_event_info pointer address */
    int bootstrap_prog_fd = bpf_program__fd(d->skel->progs.on_bootstrap);
    uint64_t started_ns = pgwt_debug_block_begin(d);
    be->bootstrap_fd = pgwt_open_watchpoint(child_pid, d->my_wait_ptr_addr,
                                             bootstrap_prog_fd);
    pgwt_debug_block_end(d,
                         be->bootstrap_fd < 0
                             ? "bootstrap_watchpoint_open_failed"
                             : "bootstrap_watchpoint_open",
                         child_pid, started_ns);
    if (be->bootstrap_fd < 0) {
        /* Race: child may have already exited or initialized.
         * Try direct init path (version-aware address resolution). */
        uint64_t ptr = pgwt_resolve_backend_wait_addr(d, child_pid);
        if (ptr != 0) {
            return pgwt_handle_init(d, child_pid, ptr);
        }
        /* Silently skip — process exited before bootstrap attach */
        d->counters.wp_attach_failures_total++;
        be->is_alive = false;
        be->pid = 0;
        return -1;
    }

    /* fork→attach race (T4/CAP-8, observed live by the T0 CI smoke job):
     * the sched_process_fork event travels fork → BPF ringbuf → daemon
     * epoll → here, while the child races through InitProcess(). If the
     * child already WROTE the my_wait_event_info/MyProc pointer before the
     * bootstrap watchpoint armed, the watchpoint NEVER fires and the backend
     * records nothing, silently, for its whole life. Close the race by
     * checking the pointer NOW that the watchpoint is armed: either the
     * write comes later (watchpoint catches it) or it already happened
     * (visible right here) — no gap between the two.
     *
     * "Already happened" must mean the pointer targets SHARED memory: on
     * PG17+ my_wait_event_info is non-zero from the very first instruction
     * (its static initializer points at the process-LOCAL dummy
     * local_my_wait_event_info), so non-zero alone proves nothing —
     * InitProcess is done only once the pointer targets the backend's
     * PGPROC, which lives in shm. A local (non-shared) target here means
     * init has not switched the pointer yet — the armed bootstrap
     * watchpoint will catch that write. (PG<17: MyProc starts NULL, and a
     * non-NULL PGPROC is in shm, so the same predicate holds.) */
    uint64_t ptr = pgwt_resolve_backend_wait_addr(d, child_pid);
    started_ns = pgwt_debug_block_begin(d);
    int addr_shared = ptr != 0 ? pgwt_addr_is_shared(child_pid, ptr) : 0;
    pgwt_debug_block_end(d, "backend_addr_shared", child_pid, started_ns);
    if (ptr != 0 && addr_shared == 1) {
        if (d->verbose)
            fprintf(stderr, "INFO: fork PID %d already initialized "
                    "(fork->attach race) — attaching directly\n", child_pid);
        return pgwt_handle_init(d, child_pid, ptr);
    }

    if (d->verbose)
        fprintf(stderr, "INFO: fork detected PID %d, bootstrap watchpoint attached\n",
                child_pid);
    return 0;
}

int pgwt_handle_init(struct pgwt_daemon *d, pid_t pid, uint64_t addr)
{
    if (pgwt_pgbs_exclusion_contains(&d->pgbs_validation_exclusions, pid))
        return 0;

    struct pgwt_backend *be = pgwt_find_backend(&d->backends, pid);
    if (!be) {
        /* Unknown PID — might have been forked before daemon started */
        be = alloc_backend(&d->backends, pid);
        if (!be) return -1;
    }

    /* Close bootstrap watchpoint if any */
    if (be->bootstrap_fd >= 0) {
        uint64_t started_ns = pgwt_debug_block_begin(d);
        pgwt_close_watchpoint(be->bootstrap_fd);
        pgwt_debug_block_end(d, "bootstrap_watchpoint_close", pid, started_ns);
        be->bootstrap_fd = -1;
    }

    /* Close previous watchpoint if re-initializing */
    if (be->wp_fd >= 0) {
        uint64_t started_ns = pgwt_debug_block_begin(d);
        pgwt_close_watchpoint(be->wp_fd);
        pgwt_debug_block_end(d, "watchpoint_close", pid, started_ns);
        be->wp_fd = -1;
    }

    /* Runtime offset validation (CAP-2/5) — all versions. Covers the case
     * where PG was idle at daemon start (scan validated nothing) and the
     * first backend arrives via bootstrap→init. A garbage class byte means
     * the offset/address is wrong — refuse rather than trace nonsense; a
     * zero reading is NOT proof (keep validating on later backends). */
    if (!d->wait_offset_validated) {
        uint64_t started_ns = pgwt_debug_block_begin(d);
        int v = pgwt_validate_wait_addr(pid, addr);
        pgwt_debug_block_end(d, "backend_wait_addr_validate", pid, started_ns);
        if (v == PGWT_WEI_GARBAGE) {
            fprintf(stderr,
                    "FATAL: resolved wait_event_info for PID %d holds a value "
                    "with an invalid wait-event class byte.\n"
                    "  %s is likely wrong for this PG%d build — refusing to "
                    "attach.\n",
                    pid,
                    d->use_myproc
                        ? "offsetof(PGPROC, wait_event_info)"
                        : "the resolved my_wait_event_info address",
                    d->pg_major_version);
            return -1;
        }
        if (v == PGWT_WEI_VALID_NONZERO)
            d->wait_offset_validated = true;
    }

    /* Attach real watchpoint on PGPROC->wait_event_info via the shared
     * helper: open DISABLED → preseed state_map → enable (CAP-8). This also
     * closes the old gap where init-path backends were never preseeded (the
     * raw BPF first-event path had to reconstruct their state). */
    be->wp_addr = addr;
    be->wp_addr_shared = -1;   /* address may have changed — re-detect */
    be->attach_ts = 0;         /* stamp attach time fresh in the helper */
    if (pgwt_attach_backend_watchpoint(d, be) != 0) {
        /* Silently skip — process likely exited before attach */
        be->is_alive = false;
        be->pid = 0;
        return -1;
    }

    uint64_t started_ns = pgwt_debug_block_begin(d);
    int cmdline_rc = pgwt_parse_cmdline(pid, &be->meta);
    pgwt_debug_block_end(d, "backend_cmdline_read", pid, started_ns);
    if (cmdline_rc == 0)
        be->meta_parsed = true;
    if (d->backend_meta) {
        started_ns = pgwt_debug_block_begin(d);
        pgwt_bm_write(d->backend_meta, pid, &be->meta);
        pgwt_debug_block_end(d, "backend_meta_write", pid, started_ns);
    }

    if (d->verbose)
        fprintf(stderr, "INFO: PID %d initialized (%s), real watchpoint at 0x%lx\n",
                pid, pgwt_backend_type_name(be->meta.backend_type),
                (unsigned long)addr);
    return 0;
}

int pgwt_handle_exit(struct pgwt_daemon *d, pid_t pid)
{
    struct pgwt_backend *be = pgwt_find_backend(&d->backends, pid);
    if (!be) return 0;

    /* Close watchpoint fds */
    uint64_t started_ns = pgwt_debug_block_begin(d);
    pgwt_close_watchpoint(be->wp_fd);
    be->wp_fd = -1;
    pgwt_close_watchpoint(be->bootstrap_fd);
    be->bootstrap_fd = -1;
    pgwt_debug_block_end(d, "watchpoint_close", pid, started_ns);

    /* Delete BPF map entries for this PID */
    int state_fd = bpf_map__fd(d->skel->maps.state_map);
    int attr_fd = bpf_map__fd(d->skel->maps.exact_attr_map);
    int seed_fd = bpf_map__fd(d->skel->maps.exact_seed_map);
    uint32_t key = pid;
    started_ns = pgwt_debug_block_begin(d);
    bpf_map_delete_elem(state_fd, &key);
    bpf_map_delete_elem(attr_fd, &key);
    bpf_map_delete_elem(seed_fd, &key);
    pgwt_debug_block_end(d, "backend_state_delete", pid, started_ns);

    be->is_alive = false;

    if (d->verbose)
        fprintf(stderr, "INFO: PID %d exited (%s)\n",
                pid, pgwt_backend_type_name(be->meta.backend_type));

    /* Reset PID so the slot can be reused by alloc_backend() */
    be->pid = 0;
    return 0;
}

void pgwt_close_all_backends(struct pgwt_backend_table *bt)
{
    for (int i = 0; i < bt->count; i++) {
        pgwt_close_watchpoint(bt->entries[i].wp_fd);
        bt->entries[i].wp_fd = -1;
        pgwt_close_watchpoint(bt->entries[i].bootstrap_fd);
        bt->entries[i].bootstrap_fd = -1;
    }
}
