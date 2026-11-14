#define _GNU_SOURCE
#include "event_writer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    assert(w);
    struct pgwt_trace_event ev = {.timestamp_ns = 1234, .pid = 42, .duration_ns = 10};
    uint64_t failures = 0;
    /* Intentional no-recording is accepted and never counted as a failure. */
    assert(pgwt_writer_push_event_counted(w, &ev, &failures) == 0);
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
    /* A fresh writer's forced failure is sticky too. */
    memset(w, 0, sizeof(*w));
    w->enabled = true;
    w->test_fail_push = true;
    failures = 0;
    for (int i = 1; i <= 3; ++i) {
        assert(pgwt_writer_push_event_counted(w, &ev, &failures) < 0);
        assert(failures == (uint64_t)i && w->failed);
    }
    free(w);
    puts("writer failure: PASS (intentional disable, real open failure, repeated rejection/counting)");
    return 0;
}
