#ifndef PGWT_CAPTURE_COUNTERS_H
#define PGWT_CAPTURE_COUNTERS_H
#include "counter_vector.h"
struct pgwt_daemon;

/* Unavailable/unreadable maps return valid=false. */
struct pgwt_counter_value pgwt_read_bpf_fail_counter(struct pgwt_daemon *d, uint32_t slot);
struct pgwt_counter_value pgwt_read_daemon_ring_site(struct pgwt_daemon *d, uint32_t slot);
struct pgwt_counter_vector pgwt_daemon_counter_vector(struct pgwt_daemon *d);
#endif
