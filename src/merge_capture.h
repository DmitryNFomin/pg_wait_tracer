#ifndef PGWT_MERGE_CAPTURE_H
#define PGWT_MERGE_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include "core/pgwt_merge_adapter.h"

struct pgwt_daemon;
struct pgwt_merge_capture;

/* Userspace attachment facts, NOT a wire record. event is ma_attach_event;
 * known/true use MA_ALLOCATOR through MA_IDEMPOTENT_INIT, in that bit order.
 * A missing known bit is unknown, not evidence. No facts are supplied here. */
struct pgwt_attach_report {
    uint32_t pid;          /* target backend */
    uint32_t event;        /* MA_BEGIN/COMPLETE/INTERRUPTED/etc. */
    uint64_t cookie;       /* target lifetime; 0 means absent */
    uint32_t facts_known;  /* twelve MA_* fact bits whose values are known */
    uint32_t facts_true;   /* true subset of facts_known */
};
_Static_assert(sizeof(struct pgwt_attach_report) == 24, "attach report size");
_Static_assert(offsetof(struct pgwt_attach_report, pid) == 0, "attach pid");
_Static_assert(offsetof(struct pgwt_attach_report, event) == 4, "attach event");
_Static_assert(offsetof(struct pgwt_attach_report, cookie) == 8, "attach cookie");
_Static_assert(offsetof(struct pgwt_attach_report, facts_known) == 16, "attach known");
_Static_assert(offsetof(struct pgwt_attach_report, facts_true) == 20, "attach true");

/* A2 delivers reports through this single callback. B installs it at open. */
typedef int (*pgwt_attach_report_fn)(void *ctx,
                                    const struct pgwt_attach_report *report);

/* Flat merge output: B's qualification dump is the first consumer; the
 * later capture writer can supply a second sink without changing daemon.c.
 * Callback borrows the output for this call and returns false on rejection. */
struct pgwt_merge_capture_sink {
    pgwt_merge_sink emit;
    void *ctx;
};

int pgwt_merge_capture_open(struct pgwt_daemon *d);
int pgwt_merge_capture_poll(struct pgwt_daemon *d);
/* Display timer has already been read; B owns flagged tick maintenance. */
int pgwt_merge_capture_tick(struct pgwt_daemon *d);
/* Final drain/held-data disposition before any capture state is destroyed. */
int pgwt_merge_capture_finish(struct pgwt_daemon *d);
int pgwt_merge_capture_record(struct pgwt_daemon *d, size_t source,
                             const void *bytes, size_t size);
int pgwt_merge_capture_attach(struct pgwt_daemon *d,
                             const struct pgwt_attach_report *report);
void pgwt_merge_capture_close(struct pgwt_daemon *d);

#endif
