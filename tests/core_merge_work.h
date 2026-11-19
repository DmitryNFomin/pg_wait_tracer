/* Private instrumentation for the retained-history cost test only. */
#ifndef CORE_MERGE_WORK_H
#define CORE_MERGE_WORK_H
#ifndef PGWT_MERGE_TEST_WORK
#error "work counters require the test build"
#endif
#include <stddef.h>
enum pgwt_merge_work { PGWT_WORK_LOOKUP, PGWT_WORK_INDEX, PGWT_WORK_STATE, PGWT_WORK_COPY,
    PGWT_WORK_QCOMPARE, PGWT_WORK_QMOVE, PGWT_WORK_QVISIT };
void pgwt_merge_test_work(enum pgwt_merge_work, size_t);
#endif
