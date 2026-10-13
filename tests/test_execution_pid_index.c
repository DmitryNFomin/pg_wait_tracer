/* #356: compare every byte with the pre-index scan, including under churn.
 * Including the implementation also lets us prove the real lookup populated
 * and consulted its PID map; equal rows alone would permit a linear fallback.
 */
#include "../src/compute.c"
#include "executions_linear_oracle.h"
#include <assert.h>

int pgwt_visit_summaries(const char *dir, uint64_t from, uint64_t to,
                         pgwt_summary_visitor visitor, void *ctx)
{
    return -1;
}

#define NPID 700
#define BASE 1000000ULL
static struct pgwt_trace_event events[NPID * 14];
static int count;

static void add(uint32_t pid, uint32_t kind, uint64_t qid, uint64_t dur,
                 uint32_t flags)
{
    struct pgwt_trace_event *e = &events[count++];
    memset(e, 0, sizeof(*e));
    e->pid = pid;
    e->timestamp_ns = BASE + count;
    e->duration_ns = dur;
    e->old_event = kind;
    e->new_event = kind;
    e->query_id = qid;
    e->flags = flags;
}

static int comparisons, nonempty, workers, counted, inferred, plans, clipped;
static void compare(uint64_t from, uint64_t to, struct pgwt_filter *f,
                     struct pgwt_backend_link *links, int n_links)
{
    struct pgwt_executions_result a, b;
    pgwt_compute_executions(events, count, from, to, f, links, n_links, &a);
    linear_compute_executions(events, count, from, to, f, links, n_links, &b);
    assert(!a.failed && !b.failed && a.num_rows == b.num_rows);
    if (a.num_rows) {
        assert(memcmp(a.rows, b.rows, a.num_rows * sizeof(*a.rows)) == 0);
        nonempty++;
    }
    for (int i = 0; i < a.num_rows; i++) {
        workers += a.rows[i].n_workers > 0;
        counted += a.rows[i].n_events > 0;
        inferred += a.rows[i].end_inferred != 0;
        plans += a.rows[i].has_plan != 0;
        clipped += a.rows[i].started_before_window != 0;
    }
    free(a.rows);
    free(b.rows);
    comparisons++;
}

static void provenance_and_refusal(void)
{
    struct exec_pid_state *states = NULL;
    struct pgwt_pid_index ix;
    int n = 0, cap = 0;
    pgwt_pid_index_init(&ix);
    /* A populated map is an assertion on the product helper itself. */
    for (int i = 0; i < NPID; i++) {
        uint32_t pid = (uint32_t)i * 128; /* hash collisions; includes key 0 */
        assert(exec_pid_state_get(&states, &n, &cap, &ix, pid) == i);
        assert(pgwt_pid_index_find(&ix, pid) == i);
    }
    assert(ix.n == NPID && n == NPID && ix.cap > 128);
    for (int i = NPID-1; i >= 0; i--)
        assert(exec_pid_state_get(&states, &n, &cap, &ix, (uint32_t)i*128) == i);
    assert(ix.n == NPID && n == NPID);
    free(states);
    pgwt_pid_index_free(&ix);

    /* Refusal at the FIRST insertion and at a LATER map growth. */
    for (int initial = 0; initial <= 89; initial += 89) {
        states = NULL; n = 0; cap = 0;
        pgwt_pid_index_init(&ix);
        for (int i = 0; i < initial; i++)
            assert(exec_pid_state_get(&states, &n, &cap, &ix, i) == i);
        assert(setenv("PGWT_TEST_ALLOC_FAIL", "pid_index_grow", 1) == 0);
        assert(exec_pid_state_get(&states, &n, &cap, &ix, initial) == -1);
        assert(ix.n == initial && n == initial);
        if (initial)
            assert(exec_pid_state_get(&states, &n, &cap, &ix, 0) == 0);
        assert(unsetenv("PGWT_TEST_ALLOC_FAIL") == 0);
        assert(exec_pid_state_get(&states, &n, &cap, &ix, initial) == initial);
        free(states);
        pgwt_pid_index_free(&ix);
    }
    struct pgwt_executions_result out;
    assert(setenv("PGWT_TEST_ALLOC_FAIL", "pid_index_grow", 1) == 0);
    pgwt_compute_executions(events, count, BASE, BASE+count+1, NULL, NULL, 0, &out);
    assert(out.failed && out.rows == NULL && out.num_rows == 0);
    assert(unsetenv("PGWT_TEST_ALLOC_FAIL") == 0);
    assert(setenv("PGWT_TEST_ALLOC_FAIL", "unrelated", 1) == 0);
    pgwt_compute_executions(events, count, BASE, BASE+count+1, NULL, NULL, 0, &out);
    assert(!out.failed && out.num_rows > NPID);
    free(out.rows);
    assert(unsetenv("PGWT_TEST_ALLOC_FAIL") == 0);
}

int main(void)
{
    /* Round-robin order grows and relocates state while stacks remain open. */
    for (int i = 0; i < NPID; i++) add(i*128, PGWT_MARKER_PLAN_START, i+1, 0, 0);
    for (int i = 0; i < NPID; i++) add(i*128, PGWT_MARKER_PLAN_END, i+1, 0, 0);
    for (int i = 0; i < NPID; i++) add(i*128, PGWT_MARKER_EXEC_START, i+1, 0, 0);
    for (int i = 0; i < NPID; i++) add(i*128, 0x0a000015, i+1, 3, 0);
    for (int i = 0; i < NPID; i++) add(i*128, PGWT_MARKER_EXEC_START, 0, 0, 0);
    for (int i = 0; i < NPID; i++) add(i*128+1, 0x0a000015, i+1, 2, 0);
    for (int i = 0; i < NPID; i++) add(i*128, 0, i+1, 0, 0);
    for (int i = 0; i < NPID; i++) add(i*128, PGWT_MARKER_EXEC_END, i+1, 0, 0);
    /* Popping must not re-attribute to the orphan; workers obey it too. */
    for (int i = 0; i < NPID; i++) add(i*128, 0x0a000015, i+1, 1, 0);
    for (int i = 0; i < NPID; i++) add(i*128+1, 0x0a000015, i+1, 1, 0);
    for (int i = 0; i < NPID; i++) add(i*128, PGWT_MARKER_CMD_END, i+1, 0, 0);
    for (int i = 0; i < NPID; i++) add(i*128, PGWT_MARKER_EXEC_START, i+1, 0, 0);
    for (int i = 0; i < NPID; i++) add(i*128, 0x0a000015, i+1, 2, PGWT_EVENT_FLAG_SAMPLE);
    for (int i = 0; i < NPID; i++) add(i*128, PGWT_MARKER_EXEC_END, i+1, 0, 0);
    struct pgwt_backend_link links[NPID];
    for (int i = 0; i < NPID; i++)
        links[i] = (struct pgwt_backend_link){.pid = i*128+1, .leader_pid = i*128};
    struct pgwt_filter filters[] = {{0}, {.pid = 128}, {.query_id = 2},
                                    {.event_id = 0x0a000015, .has_event_filter = 1}, {.class_name = "io"}};
    uint64_t bounds[] = {BASE-1, BASE+1, BASE+3*NPID, BASE+7*NPID,
                         BASE+8*NPID, BASE+11*NPID, BASE+13*NPID, BASE+count, BASE+count+1};
    for (unsigned i = 0; i < sizeof(bounds)/sizeof(bounds[0]); i++)
        for (unsigned j = i; j < sizeof(bounds)/sizeof(bounds[0]); j++)
            for (unsigned k = 0; k < sizeof(filters)/sizeof(filters[0]); k++) {
                compare(bounds[i], bounds[j], &filters[k], links, NPID);
                compare(bounds[i], bounds[j], &filters[k], NULL, 0);
            }
    /* Empty input must agree too, but cannot satisfy the fixture ledger. */
    int saved = count; count = 0;
    compare(BASE, BASE+1, NULL, NULL, 0); count = saved;
    assert(comparisons == 451 && nonempty && workers && counted && inferred && plans && clipped);
    provenance_and_refusal();
    printf("PASS: %d byte-exact comparisons; map provenance=%d PIDs, growth/reuse/refusal exercised\n", comparisons, NPID);
    return 0;
}
