#ifndef MERGE_TIE_COUNT_H
#define MERGE_TIE_COUNT_H
#include "event_reader.h"

struct tie_record {
    uint64_t ts, position;
    uint32_t pid;
    enum { TIE_IGNORED, TIE_MARKER, TIE_TRANSITION } kind;
};
struct tie_capture {
    struct tie_record *records;
    size_t count, capacity;
    uint64_t scanned, position;
};
struct tie_counts {
    uint64_t captures, records_scanned, groups, mixed_groups, N;
    uint64_t marker_before_transition, marker_after_transition;
    uint64_t ambiguous_generation_groups;
};
/* Feed in physical file/block/row order, never replay/sort the input first. */
int tie_add(struct tie_capture *, const struct pgwt_trace_event *,
            enum pgwt_block_type);
int tie_finish(struct tie_capture *, struct tie_counts *);
int tie_scan(const char *directory, struct tie_counts *);
#endif
