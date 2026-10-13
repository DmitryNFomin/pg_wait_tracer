/* #355: exercise the SHIPPING cache functions, including the admission gate.
 * Including server.c keeps its private functions private in the product and
 * ensures deleting the event charge there makes this test fail. No copy of
 * the production function, clock, live trace, or BPF dependency is involved.
 */
#define main pgwt_server_main
#include "../src/server.c"
#undef main

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        failures++; \
    } \
} while (0)

static void build_block(struct pgwt_block_agg *a, int distinct)
{
    struct pgwt_trace_event records[PGWT_BLOCK_EVENTS] = {0};
    for (int i = 0; i < PGWT_BLOCK_EVENTS; i++) {
        records[i].timestamp_ns = 1000000 + i * 1000;
        records[i].duration_ns = 100;
        records[i].pid = 123;
        records[i].old_event = WEI(PG_WAIT_IO, 1 + i % distinct);
        records[i].new_event = WEI(PG_WAIT_IO, 1 + (i + 1) % distinct);
    }
    struct pgwt_block_identity id = {
        .trace = {
            .trace_version = 3, .pg_version = 170004,
            .start_time_ns = 1700000000000000000ULL, .clock_offset_ns = 42,
        },
        .num_events = PGWT_BLOCK_EVENTS,
        .file_offset = 128,
        .first_timestamp_ns = records[0].timestamp_ns,
        .last_timestamp_ns = records[PGWT_BLOCK_EVENTS - 1].timestamp_ns,
    };
    int rc = pgwt_block_agg_build(a, &id, PGWT_BLOCK_TRANSITIONS, 1,
                                records, PGWT_BLOCK_EVENTS);
    if (rc != PGWT_BAGG_OK) {
        fprintf(stderr, "FAIL: fixture build refused: %d\n", rc);
        exit(1);                 /* never let a missing fixture look green */
    }
}

static void check_block_charge(int distinct, int capacity)
{
    struct pgwt_block_agg a;
    build_block(&a, distinct);
    CHECK(a.pairs && a.nodes && a.events && a.events_on,
          "fixture must own all three hash tables");
    CHECK(a.n_pairs == distinct && a.n_nodes == distinct &&
          a.n_events == distinct && a.ev_records == PGWT_BLOCK_EVENTS,
          "fixture must populate each table with %d distinct rows", distinct);
    CHECK(a.pair_cap == capacity && a.node_cap == capacity &&
          a.ev_cap == capacity && a.key_cap == 0,
          "fixture must allocate %d slots per hash table", capacity);

    /* Expected capacity comes from fixture geometry, not the charged fields.
     * Live rows occupy only 8/64 or 45/128 slots; charging live count fails. */
    size_t old_bytes = (size_t)capacity *
        (sizeof(struct pgwt_block_agg_pair) + sizeof(struct pgwt_block_agg_node));
    size_t event_bytes = (size_t)capacity * sizeof(struct pgwt_block_agg_event);
    size_t expected = old_bytes + event_bytes;
    CHECK(bagg_entry_bytes(&a) == expected,
          "%d-event block: expected %zu bytes, got %zu (events: %zu)",
          distinct, expected, bagg_entry_bytes(&a), event_bytes);
    printf("%d records, %d events, %d slots/table: pairs+nodes=%zu, "
           "events=%zu, total=%zu bytes\n",
           PGWT_BLOCK_EVENTS, distinct, capacity, old_bytes, event_bytes, expected);

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv) {
        fprintf(stderr, "FAIL: allocating fixture server\n");
        exit(1);
    }
    /* Seed the byte counter as though other entries already consumed it.
     * One byte short of enough room MUST refuse and retain caller ownership. */
    srv->bagg_bytes = BAGG_CACHE_MAX_BYTES - expected + 1;
    bagg_cache_put(srv, &a);
    CHECK(srv->bagg_count == 0 && srv->bagg_evictions == 1 && a.events,
          "budget exceeded by one byte must refuse, keeping caller ownership");
    CHECK(srv->bagg_bytes == BAGG_CACHE_MAX_BYTES - expected + 1,
          "refusal must leave byte counter unchanged");
    bagg_cache_drop(srv);
    pgwt_block_agg_free(&a);

    build_block(&a, distinct);
    struct pgwt_block_identity id = a.id;
    srv->bagg_bytes = BAGG_CACHE_MAX_BYTES - expected;
    bagg_cache_put(srv, &a);
    CHECK(srv->bagg_count == 1 && a.events == NULL && a.ev_cap == 0,
          "exactly enough room must admit and transfer ownership");
    CHECK(srv->bagg_bytes == BAGG_CACHE_MAX_BYTES,
          "admission must charge every allocated byte: got %zu", srv->bagg_bytes);
    struct bagg_hook_ctx ctx = { .srv = srv };
    CHECK(bagg_cache_get(&ctx, &id) != NULL, "admitted aggregate must be usable");
    id.num_events++;             /* rewritten block: invalidate the entry */
    CHECK(bagg_cache_get(&ctx, &id) == NULL && srv->bagg_count == 0,
          "stale block must be removed");
    CHECK(srv->bagg_bytes == BAGG_CACHE_MAX_BYTES - expected,
          "stale removal must release the whole capacity charge");
    bagg_cache_drop(srv);
    CHECK(srv->bagg_bytes == 0 && srv->bagg == NULL,
          "cache drop must clear the byte counter and storage");
    pgwt_block_agg_free(&a);
    free(srv);
}

int main(void)
{
    struct pgwt_block_agg empty = {0};
    CHECK(bagg_entry_bytes(&empty) == 0, "empty aggregate has no table charge");

    /* Pin the key allocation too (used by window accumulators), and ensure
     * readability flags/live counts cannot hide storage that is still owned. */
    struct pgwt_block_agg tables = {
        .pair_cap = 2, .node_cap = 3, .key_cap = 5, .ev_cap = 7,
    };
    size_t expected = 2 * sizeof(struct pgwt_block_agg_pair) +
                      3 * sizeof(struct pgwt_block_agg_node) +
                      5 * sizeof(struct pgwt_block_agg_key) +
                      7 * sizeof(struct pgwt_block_agg_event);
    CHECK(bagg_entry_bytes(&tables) == expected,
          "all four capacities must be charged even with zero live rows");

    check_block_charge(8, 64);
    check_block_charge(45, 128);  /* force growth; unused slots still cost memory */
    printf("%s: test_bagg_cache (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
