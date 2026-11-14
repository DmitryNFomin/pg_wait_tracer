#define _POSIX_C_SOURCE 200809L
#include "pgwt_merge.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Linked from the base commit's source with only its exported names changed. */
pgwt_merge *old_pgwt_merge_create(const pgwt_merge_config *, pgwt_merge_status *);
void old_pgwt_merge_destroy(pgwt_merge *);
pgwt_merge_status old_pgwt_merge_feed(pgwt_merge *, const pgwt_merge_input *);
pgwt_merge_report old_pgwt_merge_get_report(const pgwt_merge *);
pgwt_merge_epoch_report old_pgwt_merge_get_epoch_report(const pgwt_merge *, uint64_t);

static const char *case_name = "corpus";
static size_t inputs, scenarios, outputs, voids, rejections, rearms;

static void require(bool condition, const char *field)
{
    if (!condition) {
        fprintf(stderr, "queue %s MISMATCH %s at input %zu\n", case_name, field, inputs);
        exit(1);
    }
}

static bool counters_equal(pgwt_merge_counters a, pgwt_merge_counters b)
{
    return a.known == b.known && !memcmp(a.value, b.value, sizeof(a.value));
}

static bool records_equal(pgwt_merge_record a, pgwt_merge_record b)
{
    return a.source == b.source && a.o == b.o && a.t == b.t && a.pid == b.pid &&
        a.g == b.g && a.q == b.q && a.rank == b.rank &&
        a.identity_known == b.identity_known && a.has_q == b.has_q &&
        a.fork_seed_proved == b.fork_seed_proved && a.dependency == b.dependency &&
        a.dependency_id == b.dependency_id && a.payload_size == b.payload_size &&
        (!a.payload_size || !memcmp(a.payload, b.payload, a.payload_size));
}

static bool outputs_equal(pgwt_merge_output a, pgwt_merge_output b)
{
    return a.kind == b.kind && records_equal(a.record, b.record) &&
        a.prior_interval_certified == b.prior_interval_certified &&
        a.F == b.F && a.d == b.d && a.epoch == b.epoch &&
        counters_equal(a.counters, b.counters) && a.reason == b.reason;
}

struct captured {
    pgwt_merge_output out;
    unsigned char payload[32];
    pgwt_merge_observation post;
    bool accepted;
};
struct sink_state {
    struct captured attempted[24010];
    size_t count, reject_data, refresh_data;
    bool reject_frontier, reject_void, make_unknown;
};

static bool sink(void *context, const pgwt_merge_output *out, pgwt_merge_observation *post)
{
    struct sink_state *s = context;
    require(s->count < 24010 && out->record.payload_size <= 32, "capture capacity");
    bool accepted = true;
    if ((out->kind == PGWT_MERGE_OUT_DATA && s->reject_data && !--s->reject_data) ||
        (out->kind == PGWT_MERGE_OUT_FRONTIER && s->reject_frontier) ||
        (out->kind == PGWT_MERGE_OUT_VOID && s->reject_void)) {
        accepted = false;
        s->reject_frontier = s->reject_void = false;
        ++post->counters.value[14];
        post->d += 17;
    }
    if (out->kind == PGWT_MERGE_OUT_DATA && s->refresh_data && !--s->refresh_data) {
        ++post->counters.value[0];
        if (s->make_unknown) post->counters.known = false;
        post->d += 23;
    }
    struct captured *c = &s->attempted[s->count++];
    c->out = *out;
    if (out->record.payload_size)
        memcpy(c->payload, out->record.payload, out->record.payload_size);
    c->out.record.payload = c->payload;
    c->accepted = accepted;
    c->post = *post;
    return accepted;
}

struct pair {
    pgwt_merge *old, *now;
    struct sink_state a, b;
    uint64_t positions[4];
    pgwt_merge_observation obs;
};

static void compare(struct pair *p, pgwt_merge_status a, pgwt_merge_status b)
{
    require(a == b, "status");
    require(p->a.count == p->b.count, "output count");
    for (size_t i = 0; i < p->a.count; ++i) {
        struct captured *x = &p->a.attempted[i], *y = &p->b.attempted[i];
        require(x->accepted == y->accepted && outputs_equal(x->out, y->out), "output");
        require(x->post.d == y->post.d && counters_equal(x->post.counters, y->post.counters),
                "post observation");
        if (x->accepted) {
            ++outputs;
            if (x->out.kind == PGWT_MERGE_OUT_VOID) ++voids;
        } else ++rejections;
    }
    pgwt_merge_report x = old_pgwt_merge_get_report(p->old);
    pgwt_merge_report y = pgwt_merge_get_report(p->now);
    require(x.status == y.status && x.epoch == y.epoch &&
        x.last_invalid_epoch == y.last_invalid_epoch &&
        x.diagnostic_cutoff == y.diagnostic_cutoff && x.epoch_invalid == y.epoch_invalid &&
        x.has_frontier == y.has_frontier && x.void_failed == y.void_failed &&
        x.qualified == y.qualified && x.frontier == y.frontier &&
        !memcmp(x.faults, y.faults, sizeof(x.faults)), "report");
    for (uint64_t epoch = 0; epoch <= x.epoch + 1; ++epoch) {
        pgwt_merge_epoch_report e = old_pgwt_merge_get_epoch_report(p->old, epoch);
        pgwt_merge_epoch_report f = pgwt_merge_get_epoch_report(p->now, epoch);
        require(e.has_frontier == f.has_frontier && e.invalid == f.invalid &&
                e.frontier == f.frontier, "epoch report");
    }
}

static void start(struct pair *p, pgwt_merge_mode mode, size_t capacity,
                  const pgwt_merge_attachment *attachments, size_t n)
{
    memset(p, 0, sizeof(*p));
    p->obs.counters.known = true;
    pgwt_merge_source sources[4] = {{.qualified = true}, {.qualified = true},
                                    {.qualified = true}, {.qualified = true}};
    pgwt_merge_config config = {.mode = mode, .sources = sources, .source_count = 4,
        .staged_capacity = 24000, .payload_capacity = 32, .attachment_capacity = capacity,
        .frontier_capacity = 512, .dependency_capacity = 512, .attachments = attachments,
        .attachment_count = n, .observation = p->obs, .last_observed = p->obs.counters,
        .sink = sink, .sink_context = &p->a};
    pgwt_merge_status a, b;
    p->old = old_pgwt_merge_create(&config, &a);
    config.sink_context = &p->b;
    p->now = pgwt_merge_create(&config, &b);
    require((p->old != NULL) == (p->now != NULL), "create");
    compare(p, a, b);
    require(p->old && p->now, "setup");
    ++scenarios;
}

static void finish(struct pair *p)
{
    old_pgwt_merge_destroy(p->old);
    pgwt_merge_destroy(p->now);
}

static pgwt_merge_status feed(struct pair *p, pgwt_merge_input in)
{
    ++inputs;
    in.observation = p->obs;
    in.observation.d = inputs;
    p->a.count = p->b.count = 0;
    if (in.kind == PGWT_MERGE_DATA || in.kind == PGWT_MERGE_TEXT)
        in.record.o = ++p->positions[in.record.source];
    if (in.kind == PGWT_MERGE_BOUNDARY) in.p = ++p->positions[in.source];
    pgwt_merge_status a = old_pgwt_merge_feed(p->old, &in);
    pgwt_merge_status b = pgwt_merge_feed(p->now, &in);
    compare(p, a, b);
    if (in.kind == PGWT_MERGE_REARM && a == PGWT_MERGE_OK) ++rearms;
    return a;
}

static void seal(struct pair *p, uint64_t w)
{
    for (size_t s = 0; s < 4; ++s)
        feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_BOUNDARY, .source = s, .w = w});
}

static pgwt_merge_input data(uint64_t pid, uint64_t g, uint64_t t, pgwt_merge_rank rank)
{
    static const unsigned char payload[] = {0, 1, 255, 0, 42};
    return (pgwt_merge_input){.kind = PGWT_MERGE_DATA, .record = {
        .source = 2, .pid = pid, .g = g, .t = t, .rank = rank, .identity_known = true,
        .has_q = true, .q = 1, .payload = payload, .payload_size = sizeof(payload)}};
}

static void rearm(struct pair *p, const pgwt_merge_attachment *attachments, size_t n)
{
    feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_REARM, .attachments = attachments,
        .attachment_count = n, .attachment_proofs_fresh = true, .identity_repaired = true});
}

static bool allow_cost_void;
static bool cost_sink(void *context, const pgwt_merge_output *out,
                      pgwt_merge_observation *post)
{
    (void)post;
    require(out->kind != PGWT_MERGE_OUT_VOID || allow_cost_void, "cost release");
    ++*(size_t *)context;
    return true;
}

static uint64_t nanoseconds(void)
{
    struct timespec t;
    require(clock_gettime(CLOCK_MONOTONIC, &t) == 0, "clock");
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

/* A large ordered publication, discard, rejected publication, and active REARM
 * all start with the same 12,000-record out-of-order, four-source backlog.
 * Q causal ties run opposite source order; N uses different proved PIDs so
 * equal timestamps exercise pid-before-source and same-source position ties. */
static void backlog(pgwt_merge_mode mode, unsigned action)
{
    struct pair *p = calloc(1, sizeof(*p));
    require(p != NULL, "pair allocation");
    pgwt_merge_attachment proofs[4];
    for (size_t i = 0; i < 4; ++i)
        proofs[i] = (pgwt_merge_attachment){.pid = i + 1, .g = 1,
            .identity_proved = true, .sequence_proved = true, .next_q = 1};
    start(p, mode, 16, proofs, 4);
    for (size_t source = 0; source < 4; ++source) {
        for (size_t i = 0; i < 3000; ++i) {
            pgwt_merge_input in = data(mode == PGWT_MERGE_Q ? 1 : 4 - source,
                                      1, 100 + i / 4, PGWT_MERGE_TRANS);
            unsigned char payload[16];
            for (size_t j = 0; j < sizeof(payload); ++j)
                payload[j] = (unsigned char)(i * 13 + source * 17 + j);
            in.record.source = source;
            in.record.q = (i / 4) * 16 + (3 - source) * 4 + i % 4 + 1;
            in.record.payload = payload;
            in.record.payload_size = sizeof(payload);
            if (action >= 5 && source == 3 && i == 0) {
                in.record.dependency = PGWT_MERGE_WAIT;
                in.record.dependency_id = 7;
            }
            feed(p, in);
            memset(payload, 0xa5, sizeof(payload)); /* caller storage is reused */
        }
    }
    require(pgwt_merge_get_report(p->now).status == PGWT_MERGE_OK, "backlog setup");
    printf("queue backlog REACHED mode=%c action=%u held=12000\n",
           mode == PGWT_MERGE_Q ? 'Q' : 'N', action);
    if (action == 1) {
        feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_FAULT,
            .reason = PGWT_MERGE_UNKNOWN_EVIDENCE});
        rearm(p, proofs, 4);
    } else if (action == 2) {
        p->a.reject_data = p->b.reject_data = 6000;
    } else if (action == 3) {
        require(feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_REARM}) ==
                PGWT_MERGE_REARM_REJECTED, "active REARM preserves backlog");
    } else if (action == 4) {
        p->a.refresh_data = p->b.refresh_data = 6000;
    }
    seal(p, 1000);
    if (action >= 5) {
        /* The entire eligible batch must validate, even though one dependency
         * holds it. Silence and ineligible arrivals may reuse that validation. */
        for (unsigned repeat = 0; repeat < 3; ++repeat)
            feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_CHECK});
        if (action == 6) {
            feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_FAULT,
                .reason = PGWT_MERGE_UNKNOWN_EVIDENCE});
            rearm(p, proofs, 4);
        } else {
            if (action == 7)
                p->a.reject_data = p->b.reject_data = 6000;
            if (action == 8)
                require(feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_REARM}) ==
                    PGWT_MERGE_REARM_REJECTED, "active REARM during dependency hold");
            feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_TEXT,
                .record = {.source = 3, .dependency_id = 7}});
        }
    }
    if (pgwt_merge_get_report(p->now).status == PGWT_MERGE_SUSPENDED) {
        p->a.reject_data = p->b.reject_data = 0;
        p->a.refresh_data = p->b.refresh_data = 0;
        rearm(p, proofs, 4);
        /* Earlier published epoch invalidation after recovery. */
        seal(p, 1100);
        feed(p, data(1, 1, 99, PGWT_MERGE_TRANS));
    }
    finish(p);
    free(p);
}

static void incremental_backlog(bool bad_suffix)
{
    struct pair *p = calloc(1, sizeof(*p));
    require(p != NULL, "incremental pair allocation");
    pgwt_merge_attachment proofs[2] = {
        {.pid = 1, .g = 1, .identity_proved = true, .sequence_proved = true, .next_q = 1},
        {.pid = 1, .g = 2, .identity_proved = true, .sequence_proved = true, .next_q = 1}};
    start(p, PGWT_MERGE_Q, 16, proofs, 2);
    for (size_t i = 0; i < 12000; ++i) {
        size_t group = (11999 - i) / 4;
        pgwt_merge_input in = data(1, 1, 100 + group, PGWT_MERGE_TRANS);
        in.record.q = group * 4 + i % 4 + 1;
        if (i == 11996 || i == 11997) {
            in.record.dependency = PGWT_MERGE_WAIT;
            in.record.dependency_id = i == 11996 ? 7 : 8;
        }
        feed(p, in);
    }
    seal(p, 500);
    for (unsigned i = 0; i < 3; ++i)
        feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_CHECK});
    feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_TEXT,
        .record = {.source = 3, .dependency_id = 7}});
    require(feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_REARM}) ==
            PGWT_MERGE_REARM_REJECTED, "incremental active REARM");
    pgwt_merge_input in = data(1, 1, 4000, PGWT_MERGE_TRANS);
    in.record.q = 12001;
    feed(p, in);
    /* These sort before the cached first unvalidated record and exercise
     * generation-before-rank, even though both have the same PID and time. */
    feed(p, data(1, 1, 500, PGWT_MERGE_INIT));
    feed(p, data(1, 2, 500, PGWT_MERGE_INIT));
    if (bad_suffix) {
        in = data(1, 1, 500, PGWT_MERGE_INIT);
        in.record.dependency = PGWT_MERGE_LOST;
        feed(p, in);
    }
    puts("queue incremental REACHED held=12000");
    seal(p, 1000);
    if (bad_suffix) {
        require(pgwt_merge_get_report(p->now).faults[PGWT_MERGE_DEPENDENCY_LOST] == 1,
                "new suffix still validates while held");
        finish(p);
        free(p);
        return;
    }
    feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_TEXT,
        .record = {.source = 3, .dependency_id = 8}});
    seal(p, 5000);
    require(pgwt_merge_get_report(p->now).status == PGWT_MERGE_OK,
            "incremental complete publication");
    finish(p);
    free(p);
}

static void lifecycle_backlog(unsigned action)
{
    struct pair *p = calloc(1, sizeof(*p));
    require(p != NULL, "lifecycle pair allocation");
    start(p, PGWT_MERGE_Q, 4000, NULL, 0);
    for (size_t i = 0; i < 4000; ++i) {
        uint64_t pid = 4000 - i;
        pgwt_merge_input in = data(pid, 1, 100, PGWT_MERGE_FORK);
        in.record.source = 2 + i % 2;
        in.record.fork_seed_proved = true;
        feed(p, in);
        in.record.rank = PGWT_MERGE_TRANS;
        feed(p, in);
        in.record.rank = PGWT_MERGE_EXIT;
        feed(p, in);
    }
    printf("queue lifecycle REACHED action=%u held=12000 generations=4000\n", action);
    if (action == 0)
        feed(p, (pgwt_merge_input){.kind = PGWT_MERGE_FAULT,
            .reason = PGWT_MERGE_UNKNOWN_EVIDENCE});
    else {
        p->a.reject_data = p->b.reject_data = 3;
        seal(p, 101);
    }
    rearm(p, NULL, 0);
    /* Discarded creation evidence cannot resurrect after VOID/REARM. */
    pgwt_merge_input in = data(4000, 1, 200, PGWT_MERGE_FORK);
    in.record.fork_seed_proved = true;
    feed(p, in);
    seal(p, 201);
    require(pgwt_merge_get_report(p->now).status == PGWT_MERGE_SUSPENDED,
            "discarded lifecycle remains consumed");
    finish(p);
    free(p);
}

#ifdef PGWT_MERGE_TEST_WORK
#include "core_merge_work.h"
static size_t comparisons, moves, published;
static bool measuring;
void pgwt_merge_test_work(enum pgwt_merge_work kind, size_t count)
{
    if (!measuring) return;
    if (kind == PGWT_WORK_QCOMPARE) comparisons += count;
    if (kind == PGWT_WORK_QMOVE) {
        moves += count;
        /* Fail before a restored memmove can corrupt the indexed slots. */
        require(moves <= 4 * (published + 1), "publication queue moves <= 4/record");
    }
}
#endif

static bool measured_sink(void *context, const pgwt_merge_output *out,
                           pgwt_merge_observation *post)
{
#ifdef PGWT_MERGE_TEST_WORK
    if (out->kind == PGWT_MERGE_OUT_DATA) ++published;
#endif
    return cost_sink(context, out, post);
}

static void queue_cost(size_t held)
{
    size_t emitted = 0;
    pgwt_merge_source sources[4] = {{.qualified = true, .cutoff = 1},
        {.qualified = true, .cutoff = 1}, {.qualified = true, .cutoff = 1},
        {.qualified = true, .cutoff = 1}};
    pgwt_merge_attachment proof = {.pid = 1, .g = 1, .identity_proved = true,
        .sequence_proved = true, .next_q = 1};
    pgwt_merge_config config = {.sources = sources, .source_count = 4,
        .staged_capacity = held + 1, .attachment_capacity = 1, .attachments = &proof,
        .attachment_count = 1, .payload_capacity = 32, .sink = measured_sink,
        .sink_context = &emitted, .observation = {.counters = {.known = true}},
        .last_observed = {.known = true}};
    pgwt_merge_status status;
    pgwt_merge *m = pgwt_merge_create(&config, &status);
    require(m && status == PGWT_MERGE_OK, "cost setup");
    pgwt_merge_input in = data(1, 1, 100, PGWT_MERGE_TRANS);
    in.observation = config.observation;
    /* At 100 held, the old pass needs >=99 comparisons and the old pop
     * moves 99 envelopes. At 50,000 those costs are >=49,999 each. */
    size_t comparison_limit = held == 100 ? 24 : 64;
    size_t max_comparisons = 0, max_moves = 0;
    uint64_t start_time = nanoseconds();
    for (size_t i = 0; i < held; ++i) {
        in.record.o = i + 1;
        in.record.t = 100 + (i * UINT64_C(104729)) % held;
#ifdef PGWT_MERGE_TEST_WORK
        comparisons = moves = published = 0;
        measuring = true;
#endif
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "cost populate");
#ifdef PGWT_MERGE_TEST_WORK
        require(comparisons <= comparison_limit, "input queue comparisons <= log bound");
        require(moves <= 4, "input queue moves <= 4");
        if (comparisons > max_comparisons) max_comparisons = comparisons;
        if (moves > max_moves) max_moves = moves;
#endif
    }
    double populate = (double)(nanoseconds() - start_time) / held;
    in.kind = PGWT_MERGE_CHECK;
    start_time = nanoseconds();
    for (size_t i = 0; i < 100; ++i) {
#ifdef PGWT_MERGE_TEST_WORK
        comparisons = moves = published = 0;
#endif
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "cost CHECK");
#ifdef PGWT_MERGE_TEST_WORK
        require(comparisons <= 4, "held queue comparisons <= 4");
        require(moves == 0, "held queue moves == 0");
#endif
    }
    double check_cost = (double)(nanoseconds() - start_time) / 100;
    /* Publish just the minimum, leaving the large remainder held. */
    in.kind = PGWT_MERGE_BOUNDARY;
    in.w = 101;
    for (size_t source = 0; source < 4; ++source) {
        in.source = source;
        in.p = source == 2 ? held + 1 : 1;
#ifdef PGWT_MERGE_TEST_WORK
        comparisons = moves = published = 0;
#endif
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "cost single publish");
#ifdef PGWT_MERGE_TEST_WORK
        require(comparisons <= 64 * (published + 1), "publication queue comparisons <= 64/record");
        require(moves <= 4 * (published + 1), "publication queue moves <= 4/record");
#endif
    }
#ifdef PGWT_MERGE_TEST_WORK
    printf("queue work held=%zu input comparisons=%zu/%zu moves=%zu/4; "
           "single-publish comparisons=%zu/128 moves=%zu/8 outputs=%zu\n",
           held, max_comparisons, comparison_limit, max_moves, comparisons, moves, published);
    comparisons = moves = published = 0;
#endif
    start_time = nanoseconds();
    in.w = UINT64_MAX;
    for (size_t source = 0; source < 4; ++source) {
        in.source = source;
        in.p = source == 2 ? held + 2 : 2;
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "cost batch publish");
    }
    double publish_cost = (double)(nanoseconds() - start_time) / (held - 1);
#ifdef PGWT_MERGE_TEST_WORK
    require(comparisons <= 64 * (published + 4), "batch queue comparisons <= 64/record");
    require(moves <= 4 * (published + 4), "batch queue moves <= 4/record");
    require(published == held - 1, "published coverage");
    printf("queue batch held=%zu comparisons=%zu/%zu moves=%zu/%zu published=%zu\n",
           held, comparisons, 64 * (published + 4), moves, 4 * (published + 4), published);
    measuring = false;
#else
    (void)comparison_limit;
    (void)max_comparisons;
    (void)max_moves;
#endif
    printf("queue time held=%zu populate=%.3f us/input CHECK=%.3f us/input "
           "publish=%.3f us/record (informational)\n",
           held, populate / 1000, check_cost / 1000, publish_cost / 1000);
    pgwt_merge_destroy(m);
}

#ifdef PGWT_MERGE_TEST_WORK
static void dependency_cost(size_t held)
{
    size_t emitted = 0;
    pgwt_merge_source sources[4] = {{.qualified = true}, {.qualified = true},
        {.qualified = true}, {.qualified = true}};
    pgwt_merge_attachment proof = {.pid = 1, .g = 1, .identity_proved = true};
    pgwt_merge_config config = {.sources = sources, .source_count = 4,
        .staged_capacity = held + 4, .attachment_capacity = 1, .attachments = &proof,
        .attachment_count = 1, .sink = measured_sink, .sink_context = &emitted,
        .observation = {.counters = {.known = true}}, .last_observed = {.known = true}};
    pgwt_merge_status status;
    pgwt_merge *m = pgwt_merge_create(&config, &status);
    require(m && status == PGWT_MERGE_OK, "dependency cost setup");
    pgwt_merge_input in = {.kind = PGWT_MERGE_DATA, .observation = config.observation,
        .record = {.source = 2, .pid = 1, .g = 1, .identity_known = true,
                   .rank = PGWT_MERGE_TRANS, .dependency = PGWT_MERGE_WAIT}};
    for (size_t i = 0; i < held; ++i) {
        in.record.o = i + 1;
        in.record.t = 100 + i;
        in.record.dependency_id = i % 2 + 1;
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "dependency populate");
    }
    in.kind = PGWT_MERGE_BOUNDARY;
    in.w = 100 + held / 2;
    for (size_t source = 0; source < 4; ++source) {
        in.source = source;
        in.p = source == 2 ? held + 1 : 1;
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "dependency initial prefix");
    }
    size_t maximum = 0;
    measuring = true;
    for (unsigned step = 0; step < 105; ++step) {
        comparisons = moves = published = 0;
        in.kind = PGWT_MERGE_CHECK;
        if (step == 100) {
            in.kind = PGWT_MERGE_TEXT;
            in.record = (pgwt_merge_record){.source = 3, .o = 2, .dependency_id = 1};
        } else if (step > 100) {
            in.kind = PGWT_MERGE_BOUNDARY;
            in.source = step - 101;
            in.w = 101 + held / 2; /* only one newly eligible record */
            in.p = in.source == 2 ? held + 2 : in.source == 3 ? 3 : 2;
        }
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "dependency held input");
        require(published == 0, "dependency still holds publication");
        require(comparisons <= 64, "dependency held queue comparisons <= 64");
        require(moves <= 1, "dependency held queue moves <= 1");
        if (comparisons > maximum) maximum = comparisons;
    }
    comparisons = moves = published = 0;
    in.kind = PGWT_MERGE_TEXT;
    in.record = (pgwt_merge_record){.source = 3, .o = 4, .dependency_id = 2};
    require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "dependency unlock");
    require(published == held / 2 + 1, "dependency published prefix");
    require(comparisons <= 64 * (published + 1), "dependency unlock comparisons <= 64/record");
    printf("queue dependency held=%zu max_input_comparisons=%zu/64 "
           "unlock_comparisons=%zu/%zu published=%zu\n", held, maximum,
           comparisons, 64 * (published + 1), published);
    measuring = false;
    pgwt_merge_destroy(m);
}
/* Many distinct unresolved and resolved IDs, in addition to the many-records
 * per ID case above. Metadata must not replace queue scans with ID scans. */
static void distinct_dependency_cost(size_t held)
{
    size_t emitted = 0;
    pgwt_merge_source sources[4] = {{.qualified = true}, {.qualified = true},
        {.qualified = true}, {.qualified = true}};
    pgwt_merge_attachment proof = {.pid = 1, .g = 1, .identity_proved = true};
    pgwt_merge_config config = {.sources = sources, .source_count = 4,
        .staged_capacity = 2 * held + 1, .dependency_capacity = held,
        .attachment_capacity = 1, .attachments = &proof, .attachment_count = 1,
        .sink = measured_sink, .sink_context = &emitted,
        .observation = {.counters = {.known = true}}, .last_observed = {.known = true}};
    pgwt_merge_status status;
    pgwt_merge *m = pgwt_merge_create(&config, &status);
    require(m && status == PGWT_MERGE_OK, "distinct dependency setup");
    pgwt_merge_input in = {.kind = PGWT_MERGE_DATA, .observation = config.observation,
        .record = {.source = 2, .pid = 1, .g = 1, .t = 100, .identity_known = true,
                   .rank = PGWT_MERGE_TRANS, .dependency = PGWT_MERGE_WAIT}};
    for (size_t i = 0; i < held; ++i) {
        in.record.o = in.record.dependency_id = i + 1;
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "distinct dependency populate");
    }
    in.kind = PGWT_MERGE_BOUNDARY;
    in.w = 101;
    for (size_t source = 0; source < 4; ++source) {
        in.source = source;
        in.p = source == 2 ? held + 1 : 1;
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "distinct dependency hold");
    }
    size_t maximum = 0, limit = held == 100 ? 64 : 128;
    measuring = true;
    in.kind = PGWT_MERGE_TEXT;
    in.record = (pgwt_merge_record){.source = 3};
    for (size_t i = 1; i < held; ++i) {
        comparisons = moves = published = 0;
        in.record.o = i + 1;
        in.record.dependency_id = i;
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "distinct dependency input");
        require(published == 0, "distinct dependency still held");
        require(comparisons <= limit, "distinct dependency queue comparisons <= bound");
        require(moves == 1, "distinct dependency queue moves == 1");
        if (comparisons > maximum) maximum = comparisons;
    }
    comparisons = moves = published = 0;
    in.record.o = held + 1;
    in.record.dependency_id = held;
    require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "distinct dependency unlock");
    require(published == held, "distinct dependency published count");
    require(comparisons <= 64 * (published + 1), "distinct dependency unlock comparisons <= 64/record");
    measuring = false;
    /* Capacity is still the public distinct-ID capacity, even after release. */
    in.record.o = held + 2;
    in.record.dependency_id = held + 1;
    allow_cost_void = true;
    require(pgwt_merge_feed(m, &in) == PGWT_MERGE_SUSPENDED,
            "distinct dependency exhaustion");
    require(pgwt_merge_get_report(m).faults[PGWT_MERGE_ALLOCATION_FAIL] == 1,
            "distinct dependency capacity unchanged");
    allow_cost_void = false;
    printf("queue distinct dependencies=%zu max_input_comparisons=%zu/%zu "
           "unlock_comparisons=%zu/%zu moves=1/1 published=%zu\n", held, maximum, limit,
           comparisons, 64 * (published + 1), published);
    pgwt_merge_destroy(m);
}
#endif

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--cost")) {
        case_name = "work";
        puts("queue work REACHED");
        queue_cost(100);
        queue_cost(50000);
#ifdef PGWT_MERGE_TEST_WORK
        dependency_cost(100);
        dependency_cost(50000);
        distinct_dependency_cost(100);
        distinct_dependency_cost(50000);
#endif
        puts("queue work PASS");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--incremental")) {
        case_name = "incremental";
        incremental_backlog(true);
        return 0;
    }
    for (unsigned mode = 0; mode < 2; ++mode)
        for (unsigned action = 0; action < 9; ++action) backlog((pgwt_merge_mode)mode, action);
    incremental_backlog(false);
    incremental_backlog(true);
    lifecycle_backlog(0);
    lifecycle_backlog(1);
    printf("queue differential: %zu inputs, %zu scenarios, %zu outputs, "
           "%zu VOIDs, %zu rejections, %zu REARMs; identical to a95a214c\n",
           inputs, scenarios, outputs, voids, rejections, rearms);
    return 0;
}
