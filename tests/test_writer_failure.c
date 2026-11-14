#define _GNU_SOURCE
#include "event_writer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_sample_open_failure(struct pgwt_event_writer *w,
                                      const struct pgwt_trace_event *ev)
{
    uint64_t failures = 0;
    char dir[] = "/tmp/pgwt-sample-fail-XXXXXX";
    assert(mkdtemp(dir));
    assert(pgwt_writer_init(w, dir, 18, 24, NULL) == 0);
    assert(rmdir(dir) == 0);
    for (int i = 1; i <= 3; ++i) {
        assert(pgwt_writer_push_samples_counted(w, ev, 1, 100, &failures) < 0);
        assert(failures == (uint64_t)i);
        assert(w->failed && !w->enabled && w->num_sample_buf == 0);
    }
    assert(pgwt_writer_push_samples(w, ev, 1, 100) < 0);
    assert(pgwt_writer_push_event_counted(w, ev, &failures) < 0);
    assert(failures == 4);
    pgwt_writer_destroy(w);
}

/* Each sample flush trigger must make a returned error sticky, including
 * compression errors that do not disable the writer in write_typed_block. */
static void test_sample_flush_failures(struct pgwt_event_writer *w,
                                        const struct pgwt_trace_event *ev)
{
    for (int trigger = 0; trigger < 3; ++trigger) {
        uint64_t failures = 0;
        char dir[] = "/tmp/pgwt-sample-flush-XXXXXX";
        assert(mkdtemp(dir));
        assert(pgwt_writer_init(w, dir, 18, 24, NULL) == 0);
        int count = trigger == 1 ? PGWT_BLOCK_EVENTS - 1 : 1;
        struct pgwt_trace_event *batch = calloc((size_t)count, sizeof(*batch));
        assert(batch);
        for (int i = 0; i < count; ++i)
            batch[i] = *ev;
        assert(pgwt_writer_push_samples_counted(w, batch, count, 100, &failures) == 0);
        free(batch);
        w->compress_buf_size = 0; /* real LZ4 failure on the next flush */
        struct pgwt_trace_event next = *ev;
        if (trigger == 0)
            next.timestamp_ns += 101; /* discontinuity */
        else if (trigger == 1)
            next.timestamp_ns += 100; /* capacity */
        else
            next.timestamp_ns += PGWT_SAMPLE_BATCH_NS; /* time */
        uint64_t period = trigger == 2 ? PGWT_SAMPLE_BATCH_NS : 100;
        assert(pgwt_writer_push_samples_counted(w, &next, 1, period, &failures) < 0);
        assert(w->failed && !w->enabled && failures == 1);
        assert(pgwt_writer_push_samples_counted(w, &next, 1, period, &failures) < 0);
        assert(failures == 2);
        assert(pgwt_writer_push_event(w, ev) < 0);
        pgwt_writer_destroy(w);
        char path[512];
        snprintf(path, sizeof(path), "%s/current.trace", dir);
        assert(unlink(path) == 0);
        assert(rmdir(dir) == 0);
    }
}

int main(void)
{
    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    assert(w);
    struct pgwt_trace_event ev = {.timestamp_ns = 1234, .pid = 42, .duration_ns = 10};
    uint64_t failures = 0;
    /* Intentional no-recording is accepted and never counted as a failure. */
    assert(pgwt_writer_push_event_counted(w, &ev, &failures) == 0);
    assert(pgwt_writer_push_samples_counted(w, &ev, 1, 100, &failures) == 0);
    assert(failures == 0);
    char dir[] = "/tmp/pgwt-writer-fail-XXXXXX";
    assert(mkdtemp(dir));
    assert(pgwt_writer_init(w, dir, 18, 24, NULL) == 0);
    /* Real lazy-open failure; not just the test knob. */
    assert(rmdir(dir) == 0);
    for (int i = 1; i <= 4; ++i) {
        assert(pgwt_writer_push_event_counted(w, &ev, &failures) < 0);
        assert(failures == (uint64_t)i);
        assert(w->failed && !w->enabled && w->num_events == 0);
    }
    /* The raw API also rejects every later push. */
    assert(pgwt_writer_push_event(w, &ev) < 0);
    pgwt_writer_destroy(w);
    test_sample_open_failure(w, &ev);
    test_sample_flush_failures(w, &ev);
    /* A fresh writer's forced failure is sticky too. */
    memset(w, 0, sizeof(*w));
    w->enabled = true;
    w->test_fail_push = true;
    failures = 0;
    for (int i = 1; i <= 3; ++i) {
        assert(pgwt_writer_push_event_counted(w, &ev, &failures) < 0);
        assert(failures == (uint64_t)i && w->failed);
    }
    assert(pgwt_writer_push_samples_counted(w, &ev, 1, 100, &failures) < 0);
    assert(failures == 4);
    memset(w, 0, sizeof(*w));
    w->enabled = true;
    w->test_fail_push = true;
    failures = 0;
    for (int i = 1; i <= 3; ++i) {
        assert(pgwt_writer_push_samples_counted(w, &ev, 1, 100, &failures) < 0);
        assert(failures == (uint64_t)i && w->failed && !w->enabled);
    }
    free(w);
    puts("writer failure: PASS (events/samples: intentional disable, open/flush/forced failure, sticky rejection/counting)");
    return 0;
}
