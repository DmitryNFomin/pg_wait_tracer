/* event_stream.h — BPF ringbuf event consumer + in-memory accumulation */
#ifndef PGWT_EVENT_STREAM_H
#define PGWT_EVENT_STREAM_H

#include <stddef.h>

struct pgwt_accumulator;
struct pgwt_daemon;

/* Ring buffer callback for trace events.
 * ctx must be struct pgwt_daemon*. Called from ring_buffer__consume(). */
int pgwt_handle_trace_event(void *ctx, void *data, size_t data_sz);

/* Process one decoded 48-byte TRACE payload (transition or marker), after
 * wire validation. Default callback and B share this path; it never enters
 * transport again. */
int pgwt_process_trace_payload(struct pgwt_daemon *d,
                              void *data, size_t data_sz);

/* Copy populated entries from src to dst accumulator.
 * Only copies active PIDs and their events, not the full MAX_BACKENDS array.
 * Resets current_event/current_wait_ns to 0 (filled later by state_map read). */
void pgwt_accum_copy_used(struct pgwt_accumulator *dst,
                           const struct pgwt_accumulator *src);

#endif /* PGWT_EVENT_STREAM_H */
