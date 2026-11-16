/* Included ONLY in the temporary daemon built by transport_cutdiscard.py.
 * No product build includes this file. All positions, attempts, discards,
 * acknowledgements and releases below belong to the real transport. */
static bool cd_injected, cd_draining, cd_cut_seen;
static uint64_t cd_completed, cd_released, cd_text, cd_frontiers, cd_cut[2];

static void cd_require(bool ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "CUTDISCARD ASSERT: %s\n", message);
        abort();
    }
}

/* A synchronous, test-only producer invocation. Temporarily admit the main
 * thread as a helper and pin it, then undo both changes before returning.
 * The existing helper program's test variant reserves and discards on the
 * victim CPU, or publishes ordinary boundaries on the issuer CPU. */
static void cd_pulse(struct linux_transport *l, uint32_t cpu)
{
    size_t bytes = CPU_ALLOC_SIZE(l->possible);
    cpu_set_t *saved = CPU_ALLOC(l->possible), *pin = CPU_ALLOC(l->possible);
    cd_require(saved && pin, "allocate affinity masks");
    cd_require(!sched_getaffinity(0, bytes, saved), "read main-thread affinity");
    CPU_ZERO_S(bytes, pin); CPU_SET_S(cpu, bytes, pin);
    uint32_t tid = (uint32_t)syscall(SYS_gettid);
    int fd = bpf_object__find_map_fd_by_name(l->d->skel->obj, "helper_cpus");
    cd_require(fd >= 0 && !bpf_map_update_elem(fd, &tid, &cpu, BPF_NOEXIST),
               "temporarily register main thread");
    int pinned = sched_setaffinity(0, bytes, pin);
    long result = pinned ? -1 : syscall(SYS_getppid);
    int restored = sched_setaffinity(0, bytes, saved);
    int removed = bpf_map_delete_elem(fd, &tid);
    CPU_FREE(saved); CPU_FREE(pin);
    cd_require(!pinned && result >= 0 && !restored && !removed,
               "invoke kernel producer and restore main thread");
}

/* Called immediately before the REAL frozen-target read. E/L candidates
 * have already been consumed. PostgreSQL is pinned away from CPU 1 (by
 * ordinal), so this is that CPU's final E attempt until capture shutdown. */
static void cd_before_targets(struct linux_transport *l)
{
    struct pgwt_percpu_transport *t = l->d->percpu_transport;
    if (cd_injected) return;
    if (access(PGWT_CUTDISCARD_READY, F_OK)) return; /* pgbench not launched yet */
    bool data_waiting = false;
    for (size_t i = 0; i < t->count; ++i) {
        const struct pgwt_wire_record *r = &t->queue[(t->queue_head + i) % t->capacity].record;
        if (r->dest == PGWT_WIRE_DEST_CPU && r->envelope.kind == MA_TRACE && r->envelope.cookie_present)
            data_waiting = true;
    }
    if (!data_waiting) return; /* require real watchpoint DATA held by this round */
    cd_require(t->ncpu > 1 && t->requested && t->e_seen && t->l_seen && !t->fencing,
               "inject after both candidates, before target freeze");
    size_t k = 3 + PGWT_WIRE_DEST_E;
    uint64_t *attempts = calloc(t->ncpu * 3, sizeof(*attempts));
    cd_require(attempts != NULL, "allocate attempt snapshot");
    cd_require(!percpu_triples(l, l->attempts_fd, attempts, t->ncpu * 3) &&
               attempts[k] == 0 && t->seq[k] == 0,
               "non-issuer E key is unused before stimulus");
    cd_pulse(l, t->cpus[1]);
    cd_require(!percpu_triples(l, l->attempts_fd, attempts, t->ncpu * 3) && attempts[k] == 1,
               "kernel made exactly one final attempt on non-issuer E");
    cd_require(!percpu_triples(l, l->discards_fd, attempts, t->ncpu * 3) &&
               attempts[k] == (PGWT_CUTDISCARD_UNCOUNTED ? 0U : 1U),
               "kernel discard accounting matches counted or mutant stimulus");
    cd_require(!percpu_triples(l, l->failures_fd, attempts, t->ncpu * 3) && !attempts[k],
               "stimulus reserved successfully, not a failed attempt");
    free(attempts);
    cd_completed = t->stats.rounds_completed;
    struct pgwt_capture_stats s = pgwt_capture_stats(l->d->merge_capture);
    cd_released = s.released_data; cd_text = s.released_text; cd_frontiers = s.frontiers;
    cd_injected = true;
    fprintf(stderr, "CUTDISCARD injected cpu=%u issuer=%u dest=E attempted=1 consumed_seq=0 waiting_data=1 "
            "completed=%llu data=%llu text=%llu frontiers=%llu\n",
            t->cpus[1], t->cpus[0], (unsigned long long)cd_completed,
            (unsigned long long)cd_released, (unsigned long long)cd_text, (unsigned long long)cd_frontiers);
}

static void cd_no_release(struct linux_transport *l)
{
    struct pgwt_percpu_transport *t = l->d->percpu_transport;
    struct pgwt_capture_stats s = pgwt_capture_stats(l->d->merge_capture);
    cd_require(t->stats.rounds_completed == cd_completed &&
               s.released_data == cd_released && s.released_text == cd_text && s.frontiers == cd_frontiers,
               "no completion, DATA/TEXT release or FRONTIER before passing cut");
}

/* Observe, never replace, the real mmap positions returned to completion. */
static void cd_positions(struct linux_transport *l, const uint64_t produced[2],
                         const uint64_t consumed[2])
{
    if (!cd_injected || cd_draining) return;
    struct pgwt_percpu_transport *t = l->d->percpu_transport;
    cd_no_release(l);
    for (size_t i = 0; i < t->ncpu; ++i)
        cd_require(t->acks[i], "every CPU acknowledgement consumed before cut");
    cd_require(t->targets[3 + PGWT_WIRE_DEST_E] == 1 &&
               t->seq[3 + PGWT_WIRE_DEST_E] == 0 && produced[0] > consumed[0],
               "final discard is below a real, unconsumed E cut");
    if (!cd_cut_seen) {
        cd_cut[0] = produced[0]; cd_cut[1] = produced[1]; cd_cut_seen = true;
        fprintf(stderr, "CUTDISCARD cut E=%llu L=%llu consumer_E=%llu acks=%zu\n",
                (unsigned long long)cd_cut[0], (unsigned long long)cd_cut[1],
                (unsigned long long)consumed[0], t->ncpu);
    }
}

/* Both Linux ring-drain paths consult this scheduler; perf acknowledgements
 * continue to drain normally. No timeout, sleep, fabricated record or counter
 * substitutes for the causal conditions. */
static bool cd_allow_ring(struct linux_transport *l)
{
    if (!cd_injected || cd_draining) return true;
    struct pgwt_percpu_transport *t = l->d->percpu_transport;
    cd_no_release(l);
    if (!cd_cut_seen || !t->stats.holds) return false;
    cd_require(t->cut_taken && t->stats.cuts &&
               t->cut[0] == cd_cut[0] && t->cut[1] == cd_cut[1],
               "completion holds at the fixed cut");
    /* Publish more E/L traffic AFTER the fixed cut, on the issuer's keys.
     * The non-issuer E key stays terminal. pgbench continues independently. */
    cd_pulse(l, t->cpus[0]);
    uint64_t produced[2], consumed[2];
    cd_draining = true;
    cd_require(!linux_positions(l, produced, consumed) &&
               produced[0] > cd_cut[0] && produced[1] > cd_cut[1],
               "other kernel traffic advanced both producers beyond fixed cut");
    cd_no_release(l);
    fprintf(stderr, "CUTDISCARD held no_premature_release=1 traffic_after_cut=1 draining=1\n");
    return true;
}
