/* Selected daemon read wrappers as host C; no Linux/libbpf dependency. */
#include "capture_counters.h"
#include <sys/types.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake_skeleton { struct { int fail_counters, ringbuf_site_drops, ringbuf_drops, state_map; } maps; };
struct pgwt_daemon {
    struct fake_skeleton *skel;
    struct {
        uint64_t lifecycle_init_lost_total, init_state_read_fail_total;
        uint64_t state_map_full_total, writer_push_fail_total;
    } counters;
};
static int lookup_error, lookups;
static int bpf_map__fd(int fd) { return fd; }
static int libbpf_num_possible_cpus(void) { return 2; }
static int bpf_map_lookup_elem(int fd, const void *key, void *out)
{
    (void)key;
    ++lookups;
    if (lookup_error) { errno = lookup_error; return -1; }
    if (fd == 4) {
        memset(out, 0, sizeof(struct pgwt_pid_state));
        return 0;
    }
    uint64_t *cpus = out;
    cpus[0] = 1; cpus[1] = 3;
    return 0;
}

/* PRODUCTION_FUNCTIONS */

int main(void)
{
    struct fake_skeleton skel = {.maps = {1, 2, 3, 4}};
    struct pgwt_daemon d = {.skel = &skel};
    struct pgwt_counter_value c = pgwt_read_bpf_fail_counter(&d, 0);
    assert(c.valid && c.value == 4);
    lookup_error = EIO;
    c = pgwt_read_bpf_fail_counter(&d, 0);
    assert(!c.valid && c.value == 0);
    c = pgwt_read_daemon_ring_site(&d, 0);
    assert(!c.valid && c.value == 0);
    assert(!pgwt_daemon_counter_vector(&d).valid);
    struct pgwt_pid_state st;
    assert(read_init_state(&d, 42, &st) == -EIO);
    assert(d.counters.init_state_read_fail_total == 1);
    lookup_error = 0;
    assert(read_init_state(&d, 42, &st) == 0);
    assert(!pgwt_daemon_counter_vector(&d).valid); /* unknown accounting is sticky */
    lookup_error = ENOENT;
    assert(read_init_state(&d, 42, &st) == -ENOENT);
    assert(d.counters.init_state_read_fail_total == 1); /* known absence has its own loss path */
    lookup_error = 0;
    d.counters.init_state_read_fail_total = 0;
    d.counters.lifecycle_init_lost_total = 7;
    d.counters.state_map_full_total = 10;
    d.counters.writer_push_fail_total = 11;
    struct pgwt_counter_vector v = pgwt_daemon_counter_vector(&d);
    assert(v.valid && v.components[1].value == 7 && v.components[12].value == 14 && v.components[14].value == 11);
    /* Existing control key retains the old combined state-map population;
     * the two certification components keep their separate sources. */
    assert(legacy_state_map_full(&v).valid && legacy_state_map_full(&v).value == 18);
    v.components[10].valid = false;
    assert(!legacy_state_map_full(&v).valid);
    v.components[10].valid = true;
    v.components[12].valid = false;
    assert(!legacy_state_map_full(&v).valid);
    skel.maps.fail_counters = -1;
    assert(!pgwt_read_bpf_fail_counter(&d, 0).valid);
    assert(!pgwt_daemon_counter_vector(&d).valid);
    int before = lookups;
    assert(!pgwt_read_bpf_fail_counter(&d, PGWT_BPF_FAIL_MAX).valid);
    assert(!pgwt_read_daemon_ring_site(&d, PGWT_RING_FAIL_SITE_MAX).valid);
    assert(lookups == before);
    d.skel = NULL;
    assert(!pgwt_read_bpf_fail_counter(&d, 0).valid);
    assert(!pgwt_read_daemon_ring_site(&d, 0).valid);
    assert(!pgwt_daemon_counter_vector(&d).valid);
    puts("daemon counter reads: PASS (validity, INIT read errors, known ENOENT, sticky unknown)");
    return 0;
}
