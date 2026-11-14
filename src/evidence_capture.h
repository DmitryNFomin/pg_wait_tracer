#ifndef PGWT_EVIDENCE_CAPTURE_H
#define PGWT_EVIDENCE_CAPTURE_H

#include <stddef.h>
#include "counter_vector.h"

struct pgwt_evidence_capture;

/* Opt in with PGWT_EVIDENCE_CAPTURE=1 and an existing trace directory.
 * PGWT_EVIDENCE_CAPTURE_DIR selects a separate existing output directory;
 * by default evidence goes into the trace directory. A trace directory is
 * required even when the evidence output is overridden.
 * PGWT_EVIDENCE_CAPTURE_MAX_BYTES bounds buffered records AND encoded bytes
 * (default 8 MiB, maximum 64 MiB). Exceeding either refuses the whole capture.
 * NULL means disabled or explicitly refused; observe/close accept NULL.
 * No file is published until close. The legacy writer is independent. */
struct pgwt_evidence_capture *pgwt_evidence_capture_init(const char *trace_dir,
                                                        unsigned pg_version);
/* Only observed FORK/EXIT payloads supply lifetime facts. INIT/text supply none.
 * No callback allocates; pid lookup is expected O(1), and timestamp lookup
 * walks at most 64 radix branches, including delayed/repeated generations.
 * One immutable fork identity per generation; EXIT refines its buffered LIFE.
 * An unmatched EXIT is ignored. No lifetime end is inferred at shutdown. */
void pgwt_evidence_capture_observe(struct pgwt_evidence_capture *capture,
                                  const void *payload, size_t size);
/* Consumes and clears *capture, including on refusal. Any invalid component
 * makes the closing loss vector null, as required by evidence-v1. If fork or
 * exit losses are nonzero/unknown (or the vector is invalid), omits all LIFE. */
int pgwt_evidence_capture_close(struct pgwt_evidence_capture **capture,
                                struct pgwt_counter_vector final_counters);

#endif
