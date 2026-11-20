/* Real snapshot -> transport -> startup refusal, with injected map syscalls. */
#define PGWT_TRANSPORT_EMBEDDED
#define pgwt_daemon_counter_vector unused_daemon_counter_vector
#include "test_percpu_transport.c"
#undef pgwt_daemon_counter_vector
#undef main

static const char *startup_case;
static struct pgwt_daemon *startup_daemon;
static int startup_lookup(int fd, const void *key, void *out)
{
    if (!strcmp(startup_case, "map") && fd == 11 && *(const uint32_t *)key == 3) {
        errno = ENOTSUP;
        return -1;
    }
    /* Later successful lookups must not overwrite the failed component's errno. */
    errno = EBUSY;
    memset(out, 0, 2 * sizeof(uint64_t));
    return 0;
}
struct pgwt_counter_vector pgwt_daemon_counter_vector(struct pgwt_daemon *d)
{
    (void)d;
    struct pgwt_counter_sources sources = {.fail_fd=11, .site_fd=22, .event_ring_fd=33,
        .ncpu=2, .init_state_read_fail_total=!strcmp(startup_case,"unknown")};
    struct pgwt_counter_vector v = pgwt_read_counter_vector(&sources, startup_lookup);
    if (!strcmp(startup_case,"snapshot")) v.valid = false;
    return v;
}
static int startup_counters(void *ctx, ma_snapshot *snapshot)
{
    (void)ctx;
    uint64_t now;
    return pgwt_merge_capture_snapshot(startup_daemon, snapshot, &now);
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    startup_case = argv[1];
    struct pgwt_daemon *d = calloc(1, sizeof(*d));
    if (!d) return 2;
    startup_daemon = d;
    d->exact_perf = true;
    d->trace_dir = "/unused-startup-refusal";
    struct tb_driver driver = {0};
    struct pgwt_transport_io io = {.ctx=&driver, .now=tb_now,
        .control=tb_control, .wake=tb_wake, .attempts=tb_attempts,
        .failures=tb_failures, .discards=tb_discards, .positions=tb_positions,
        .counters=startup_counters, .drain=tb_drain, .envelope=tb_envelope,
        .fault=tb_fault, .cpu_set_unchanged=tb_topology};
    uint32_t cpus[] = {0,1};
    d->percpu_transport = pgwt_transport_create(cpus, 2, 16, &io);
    if (!d->percpu_transport) return 2;
    int rc;
    if (!strcmp(startup_case,"healthy")) {
        ma_snapshot snapshot;
        uint64_t now;
        rc = pgwt_merge_capture_snapshot(d,&snapshot,&now);
        if (rc || !snapshot.valid) return 2;
    } else if (!strcmp(startup_case,"disabled")) {
        d->exact_perf = false;
        rc = pgwt_merge_capture_open(d);
        if (rc) return 2;
    } else {
        rc = pgwt_merge_capture_open(d);
        if (rc != -EIO || d->merge_capture) return 2;
    }
    pgwt_transport_destroy(d->percpu_transport);
    free(d);
    return rc < 0 ? 1 : 0;
}
