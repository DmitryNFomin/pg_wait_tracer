/* Portable scheduling decisions; the Linux caller owns the absolute timer. */
#ifndef NMI_TRANSPORT_CADENCE_H
#define NMI_TRANSPORT_CADENCE_H
#include <stdbool.h>
#include <stdint.h>

/* A periodic helper's first deadline gets one recorded offset, <1 ms. */
static inline uint64_t transport_first_deadline(uint64_t start, uint64_t jitter)
{
    return start + jitter;
}

/* Skip missed periods without a catch-up burst; never sleep past run end. */
static inline uint64_t transport_next_deadline(uint64_t next, uint64_t now,
                                               uint64_t end, uint64_t period)
{
    uint64_t periods = now >= next ? (now - next) / period + 1 : 1;
    uint64_t remaining = end - (next < end ? next : end);
    return periods > remaining / period ? end : next + periods * period;
}
#endif
