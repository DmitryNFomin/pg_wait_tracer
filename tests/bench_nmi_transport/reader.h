/* Portable decoding, sequence reconciliation and seal checks. Refs #871. */
#ifndef NMI_TRANSPORT_READER_H
#define NMI_TRANSPORT_READER_H
#include "transport.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* PERF_SAMPLE_RAW only: u32 size followed by unaligned data. The advertised
 * length includes kernel padding: 68 for a 64-byte record; frame is 80. */
static inline bool transport_decode_perf(struct transport_record *r,
                                         const void *sample, size_t size)
{
    const size_t raw_size = ((sizeof(*r) + sizeof(uint32_t) + 7) & ~(size_t)7)
        - sizeof(uint32_t);
    uint32_t advertised;
    if (size != sizeof(advertised) + raw_size) return false;
    memcpy(&advertised, sample, sizeof(advertised));
    if (advertised != raw_size) return false;
    memcpy(r, (const unsigned char *)sample + sizeof(advertised), sizeof(*r));
    return true;
}
static inline bool transport_record_valid(const struct transport_record *r,
                                          uint32_t expected_tid)
{
    if (!r->ts || !r->seq || r->site >= TRANSPORT_SITES) return false;
    if (r->site == TRANSPORT_DATA) {
        if (!expected_tid || r->tid != expected_tid) return false;
    } else if (r->tid) return false;
    for (size_t i = 0; i < sizeof(r->pad); ++i)
        if (r->pad[i]) return false;
    return true;
}
struct transport_site_result {
    uint64_t delivered, last_seq, gaps, reorders;
};
struct transport_cpu_result {
    struct transport_site_result site[TRANSPORT_SITES];
    uint64_t last_ts, last_data_ts, boundary, seal_violations, regressions;
    uint64_t boundary_seen_ns, max_boundary_gap_ns, max_boundary_stamp_gap_ns;
    uint64_t frontier_ns, frontier_seen_ns, max_frontier_gap_ns, max_frontier_stamp_gap_ns;
    uint64_t measurement_start_ns;
    uint32_t expected_tid;
    bool participating, qualified_perf;
};
static inline void transport_boundary_gap(struct transport_cpu_result *c, uint64_t now)
{
    if (now >= c->boundary_seen_ns && now - c->boundary_seen_ns > c->max_boundary_gap_ns)
        c->max_boundary_gap_ns = now - c->boundary_seen_ns;
}
static inline void transport_frontier_gap(struct transport_cpu_result *c, uint64_t now)
{
    if (now >= c->frontier_seen_ns && now - c->frontier_seen_ns > c->max_frontier_gap_ns)
        c->max_frontier_gap_ns = now - c->frontier_seen_ns;
}
static inline void transport_frontier_end(struct transport_cpu_result *c, uint64_t end)
{
    transport_frontier_gap(c, end);
    uint64_t stamp = c->frontier_ns ? c->frontier_ns : c->measurement_start_ns;
    if (end >= stamp && end - stamp > c->max_frontier_stamp_gap_ns)
        c->max_frontier_stamp_gap_ns = end - stamp;
}
static inline void transport_observe(struct transport_cpu_result *c,
                                      const struct transport_record *r, uint64_t now)
{
    struct transport_site_result *s = &c->site[r->site];
    bool fault = r->seq <= s->last_seq || r->ts < c->last_ts ||
        (r->site == TRANSPORT_DATA && r->ts < c->boundary);
    ++s->delivered;
    if (r->seq <= s->last_seq) ++s->reorders;
    else { s->gaps += r->seq - s->last_seq - 1; s->last_seq = r->seq; }
    /* Check the consumed stream, not a userspace-sorted copy. Equal stamps
     * are legal. A faulty boundary is retained, so the +1 ms control cannot
     * retract its promise before late DATA is checked. */
    if (r->ts < c->last_ts) ++c->regressions;
    if (r->ts > c->last_ts) c->last_ts = r->ts;
    if (r->site == TRANSPORT_DATA) {
        if (r->ts < c->boundary) ++c->seal_violations;
        if (r->ts > c->last_data_ts) c->last_data_ts = r->ts;
    } else {
        transport_boundary_gap(c, now);
        if (r->ts > c->boundary) c->boundary_seen_ns = now;
        if (c->boundary && r->ts >= c->boundary &&
            r->ts - c->boundary > c->max_boundary_stamp_gap_ns)
            c->max_boundary_stamp_gap_ns = r->ts - c->boundary;
        if (r->ts > c->boundary) c->boundary = r->ts;
    }
    /* merge-v1 section 3: qualified perf DATA supplies strict w=t too.
     * Equal/weaker stamps and faulting inputs cannot refresh progress. Keep
     * the explicit BOUNDARY diagnostics above independent of this frontier. */
    if (!fault && (r->site == TRANSPORT_BOUNDARY || c->qualified_perf) &&
        r->ts > c->frontier_ns) {
        transport_frontier_gap(c, now);
        uint64_t stamp = c->frontier_ns ? c->frontier_ns : c->measurement_start_ns;
        if (r->ts >= stamp && r->ts - stamp > c->max_frontier_stamp_gap_ns)
            c->max_frontier_stamp_gap_ns = r->ts - stamp;
        c->frontier_ns = r->ts;
        c->frontier_seen_ns = now;
    }
}
/* Include initial and trailing gaps only after ALL producers are stopped
 * and buffers drained. Pure function: repeated reports cannot add gaps. */
static inline bool transport_reconcile(const struct transport_site_result *s,
                                       const struct transport_counts *c,
                                       uint64_t *loss, uint64_t *gaps)
{
    if (c->attempts < s->delivered || c->attempts < s->last_seq) return false;
    *loss = c->attempts - s->delivered;
    *gaps = s->gaps + c->attempts - s->last_seq;
    return !s->reorders && *loss == *gaps &&
        c->successes <= c->attempts && c->failures == c->attempts - c->successes &&
        c->failures == c->eagain + c->enospc + c->other;
}
#endif
