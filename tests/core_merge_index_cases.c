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
static uint64_t random_state = UINT64_C(0x926a95a214c);

static void require(bool condition, const char *field)
{
    if (!condition) {
        fprintf(stderr, "index %s MISMATCH %s at input %zu\n", case_name, field, inputs);
        exit(1);
    }
}

static uint64_t random_value(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
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
    struct captured attempted[256];
    size_t count, reject_data, refresh_data;
    bool reject_frontier, reject_void, make_unknown;
};

static bool sink(void *context, const pgwt_merge_output *out, pgwt_merge_observation *post)
{
    struct sink_state *s = context;
    require(s->count < 256 && out->record.payload_size <= 32, "capture capacity");
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
        .staged_capacity = 128, .payload_capacity = 32, .attachment_capacity = capacity,
        .frontier_capacity = 256, .dependency_capacity = 512, .attachments = attachments,
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

static void focused(const char *name)
{
    case_name = name;
    printf("index %s REACHED\n", name);
    struct pair p;
    pgwt_merge_attachment proof = {.pid = 10, .g = 1, .next_q = 1,
        .identity_proved = true, .sequence_proved = true};
    if (!strcmp(name, "plan_reset")) {
        start(&p, PGWT_MERGE_Q, 16, &proof, 1);
        pgwt_merge_input in = data(10, 1, 10, PGWT_MERGE_TRANS);
        in.record.dependency = PGWT_MERGE_WAIT;
        in.record.dependency_id = 7;
        feed(&p, in);
        seal(&p, 11);
        for (unsigned i = 0; i < 3; ++i)
            feed(&p, (pgwt_merge_input){.kind = PGWT_MERGE_CHECK});
        feed(&p, (pgwt_merge_input){.kind = PGWT_MERGE_TEXT,
            .record = {.source = 3, .dependency_id = 7}});
        require(pgwt_merge_get_report(p.now).status == PGWT_MERGE_OK, "held setup");
    } else {
        start(&p, PGWT_MERGE_Q, 64, NULL, 0);
        /* Includes zero and full-width keys, table collisions and PID reuse.
         * Each FORK/EXIT pair precedes the next generation of that PID. */
        for (size_t i = 0; i < 64; ++i) {
            uint64_t pid = i < 4 ? 0 : UINT64_MAX - i / 4;
            pgwt_merge_input in = data(pid, i, i * 4 + 1, PGWT_MERGE_FORK);
            in.record.fork_seed_proved = true;
            feed(&p, in);
            feed(&p, data(pid, i, i * 4 + 2, PGWT_MERGE_TRANS));
            feed(&p, data(pid, i, i * 4 + 3, PGWT_MERGE_EXIT));
            seal(&p, i * 4 + 4);
        }
        /* Full capacity still permits existing keys, but never a new key. */
        pgwt_merge_input existing = data(UINT64_MAX - 63 / 4, 63, 257, PGWT_MERGE_TRANS);
        existing.record.q = 2;
        feed(&p, existing);
        seal(&p, 258);
        feed(&p, data(123456, 100, 259, PGWT_MERGE_FORK));
        require(pgwt_merge_get_report(p.now).faults[PGWT_MERGE_ALLOCATION_FAIL] == 1,
                "exhaustion setup");
    }
    finish(&p);
    printf("index %s PASS\n", name);
}

static void churn(pgwt_merge_mode mode)
{
    struct pair p;
    start(&p, mode, 4096, NULL, 0);
    for (size_t batch = 0; batch < 512; ++batch) {
        for (size_t j = 0; j < 8; ++j) {
            uint64_t i = batch * 8 + j, t = i * 8 + 1;
            uint64_t pid = i / 2, g = i % 2 + 1;
            bool seeded = random_value() & 1;
            pgwt_merge_input in = data(pid, g, t, PGWT_MERGE_FORK);
            in.record.fork_seed_proved = seeded;
            feed(&p, in);
            if (!seeded || (random_value() & 1))
                feed(&p, data(pid, g, t + 1, PGWT_MERGE_INIT));
            in = data(pid, g, t + 2, PGWT_MERGE_TRANS);
            if (j == 0) {
                in.record.dependency = PGWT_MERGE_WAIT;
                in.record.dependency_id = batch + 1;
            }
            feed(&p, in);
            in.record.t++;
            in.record.q++;
            feed(&p, in);
            feed(&p, data(pid, g, t + 4, PGWT_MERGE_EXIT));
        }
        /* Sometimes validate a held batch before TEXT arrives. */
        if (batch % 3 == 0) seal(&p, (batch + 1) * 64);
        feed(&p, (pgwt_merge_input){.kind = PGWT_MERGE_TEXT,
            .record = {.source = 3, .dependency_id = batch + 1}});
        if (batch % 17 == 0) p.a.reject_data = p.b.reject_data = random_value() % 12 + 1;
        if (batch % 19 == 0) p.a.refresh_data = p.b.refresh_data = random_value() % 12 + 1;
        seal(&p, (batch + 1) * 64);
        if (batch % 13 == 0)
            feed(&p, (pgwt_merge_input){.kind = PGWT_MERGE_FAULT,
                .reason = PGWT_MERGE_UNKNOWN_EVIDENCE});
        if (pgwt_merge_get_report(p.now).status == PGWT_MERGE_SUSPENDED) {
            p.a.reject_data = p.b.reject_data = p.a.refresh_data = p.b.refresh_data = 0;
            rearm(&p, NULL, 0);
        }
    }
    feed(&p, data(999999, 1, 40000, PGWT_MERGE_FORK)); /* retained-history cap */
    finish(&p);
}

static void generated(void)
{
    for (size_t scenario = 0; scenario < 256; ++scenario) {
        struct pair p;
        pgwt_merge_attachment proofs[4];
        for (size_t i = 0; i < 4; ++i)
            proofs[i] = (pgwt_merge_attachment){.pid = i, .g = 1, .next_q = 1,
                .identity_proved = true, .sequence_proved = true};
        start(&p, scenario % 2 ? PGWT_MERGE_Q : PGWT_MERGE_N,
              scenario % 7 ? 32 : 4, proofs, 4);
        uint64_t t = 1;
        for (size_t step = 0; step < 256; ++step) {
            uint64_t r = random_value();
            t += r % 3;
            pgwt_merge_input in = {.kind = PGWT_MERGE_CHECK};
            switch (r % 16) {
            case 0: case 1: case 2: case 3: case 4: case 5:
                in = data((r >> 8) % 12, (r >> 16) % 3, t, (r >> 24) % 4);
                in.record.source = (r >> 27) % 4;
                in.record.q = (r >> 32) % 4;
                in.record.has_q = (r >> 36) % 5 != 0;
                in.record.fork_seed_proved = (r >> 40) & 1;
                in.record.dependency = (r >> 48) % 3;
                in.record.dependency_id = (r >> 52) % 8;
                break;
            case 6: case 7:
                in.kind = PGWT_MERGE_BOUNDARY;
                in.source = (r >> 8) % 4;
                in.w = t + (r >> 12) % 3;
                break;
            case 8:
                in = (pgwt_merge_input){.kind = PGWT_MERGE_TEXT, .record = {
                    .source = 3, .dependency_id = (r >> 8) % 8}};
                break;
            case 9:
                in = (pgwt_merge_input){.kind = PGWT_MERGE_REARM,
                    .attachments = proofs, .attachment_count = (r >> 8) % 5,
                    .attachment_proofs_fresh = (r >> 12) % 5 != 0,
                    .identity_repaired = (r >> 16) % 5 != 0};
                break;
            case 10:
                in = (pgwt_merge_input){.kind = PGWT_MERGE_FAULT,
                    .reason = (r >> 8) % PGWT_MERGE_DUPLICATE_IDENTITY};
                break;
            case 11:
                in = (pgwt_merge_input){.kind = PGWT_MERGE_LOSS,
                    .source = (r >> 8) % 2, .n = (r >> 12) % 2};
                break;
            case 12:
                ++p.obs.counters.value[(r >> 8) % PGWT_MERGE_COUNTERS];
                break;
            case 13:
                p.obs.counters.known = (r >> 8) % 4 != 0;
                break;
            case 14:
                p.a.reject_frontier = p.b.reject_frontier = true;
                p.a.reject_void = p.b.reject_void = (r >> 8) & 1;
                break;
            case 15:
                p.a.reject_data = p.b.reject_data = (r >> 8) % 3 + 1;
                p.a.refresh_data = p.b.refresh_data = (r >> 12) % 3 + 1;
                p.a.make_unknown = p.b.make_unknown = (r >> 16) & 1;
                break;
            }
            feed(&p, in);
        }
        finish(&p);
    }
}

static void transactions(void)
{
    struct pair p;
    pgwt_merge_attachment proof = {.pid = 10, .g = 1, .next_q = 1,
        .identity_proved = true, .sequence_proved = true};
    start(&p, PGWT_MERGE_Q, 4, &proof, 1);
    feed(&p, data(10, 1, 10, PGWT_MERGE_TRANS));
    pgwt_merge_input in = data(10, 1, 11, PGWT_MERGE_TRANS);
    in.record.q = 2;
    feed(&p, in);
    p.a.reject_data = p.b.reject_data = 2;
    seal(&p, 12); /* successful prefix followed by a rejected publication */
    require(pgwt_merge_get_report(p.now).status == PGWT_MERGE_SUSPENDED,
            "rejection setup");
    proof.next_q = 2;
    rearm(&p, &proof, 1);
    in.record.t = 12;
    in.record.dependency = PGWT_MERGE_WAIT;
    in.record.dependency_id = 99;
    feed(&p, in);
    seal(&p, 13); /* held tentative sequence state must not survive VOID */
    feed(&p, (pgwt_merge_input){.kind = PGWT_MERGE_FAULT,
        .reason = PGWT_MERGE_DEPENDENCY_LOST});
    rearm(&p, &proof, 1);
    in.record.t = 13;
    in.record.dependency = PGWT_MERGE_READY;
    feed(&p, in);
    seal(&p, 14); /* epoch 3 publishes F13 */
    feed(&p, (pgwt_merge_input){.kind = PGWT_MERGE_LOSS, .source = 0, .n = 1});
    proof.next_q = 3;
    rearm(&p, &proof, 1);
    seal(&p, 15); /* epoch 4 publishes F14 */
    in.record.t = 5;
    in.record.source = 1;
    feed(&p, in);
    require(pgwt_merge_get_epoch_report(p.now, 3).invalid &&
            pgwt_merge_get_epoch_report(p.now, 4).invalid, "invalidation setup");
    finish(&p);
}

static bool cost_sink(void *context, const pgwt_merge_output *out,
                      pgwt_merge_observation *post)
{
    (void)post;
    require(out->kind != PGWT_MERGE_OUT_VOID, "cost release");
    ++*(size_t *)context;
    return true;
}

static uint64_t nanoseconds(void)
{
    struct timespec t;
    require(clock_gettime(CLOCK_MONOTONIC, &t) == 0, "clock");
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static double cost(size_t retained)
{
    size_t emitted = 0;
    pgwt_merge_source sources[3] = {{.qualified = true},
        {.qualified = true, .cutoff = UINT64_MAX},
        {.qualified = true, .cutoff = UINT64_MAX}};
    pgwt_merge_config config = {.sources = sources, .source_count = 3,
        .staged_capacity = 4, .attachment_capacity = retained, .sink = cost_sink,
        .sink_context = &emitted, .observation = {.counters = {.known = true}},
        .last_observed = {.known = true}};
    pgwt_merge_status status;
    pgwt_merge *m = pgwt_merge_create(&config, &status);
    require(m && status == PGWT_MERGE_OK, "cost setup");
    pgwt_merge_input in = {.kind = PGWT_MERGE_DATA,
        .observation = config.observation, .record = {.identity_known = true}};
    /* Populate through real FORK/EXIT release, rather than charging creation
     * or a large initial-proof validation to the measured feed cost. */
    for (size_t i = 0; i < retained; ++i) {
        in.record.pid = i;
        in.record.g = i ^ UINT64_MAX;
        in.record.rank = PGWT_MERGE_FORK;
        in.record.fork_seed_proved = true;
        ++in.record.t;
        ++in.record.o;
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "cost populate FORK");
        in.record.rank = PGWT_MERGE_EXIT;
        in.record.fork_seed_proved = false;
        ++in.record.t;
        ++in.record.o;
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "cost populate EXIT");
    }
    const size_t iterations = 1000000;
    size_t before = emitted;
    in.record.rank = PGWT_MERGE_TRANS;
    uint64_t start_time = nanoseconds();
    for (size_t i = 0; i < iterations; ++i) {
        /* Walk the entire retained table in a deterministic permuted order. */
        in.record.pid = (i * UINT64_C(104729)) % retained;
        in.record.g = in.record.pid ^ UINT64_MAX;
        ++in.record.t;
        ++in.record.o;
        require(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK, "cost feed");
    }
    uint64_t elapsed = nanoseconds() - start_time;
    require(emitted - before == 2 * iterations, "cost output count");
    pgwt_merge_destroy(m);
    return (double)elapsed / iterations;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--cost")) {
        double small = cost(1000), large = cost(200000);
        printf("core merge cost: 1000=%.2f ns/feed, 200000=%.2f ns/feed, ratio=%.2fx (limit 4x)\n",
               small, large, large / small);
        require(large <= 4 * small, "cost ratio <= 4x");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--case")) {
        if (strcmp(argv[2], "probe") && strcmp(argv[2], "insert") && strcmp(argv[2], "plan_reset"))
            return 2;
        focused(argv[2]);
        return 0;
    }
    focused("probe");
    focused("insert");
    focused("plan_reset");
    case_name = "corpus";
    churn(PGWT_MERGE_N);
    churn(PGWT_MERGE_Q);
    generated();
    transactions();
    require(voids && rejections && rearms, "corpus coverage");
    printf("core merge differential: %zu inputs, %zu scenarios, %zu outputs, "
           "%zu VOIDs, %zu rejections, %zu REARMs; identical to a95a214c\n",
           inputs, scenarios, outputs, voids, rejections, rearms);
    return 0;
}
