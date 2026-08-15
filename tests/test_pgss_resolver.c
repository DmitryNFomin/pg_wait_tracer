#include "pgss_resolver.h"
#ifdef PGWT_TEST
#include "daemon.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int run, passed;
#define CHECK(c, msg) do { run++; if (c) passed++; else printf("  FAIL: %s\n", msg); } while (0)

int main(void)
{
    char schema[128];
    CHECK(pgwt_pgss_parse_discovery("SCHEMA|7765697264\n", schema,
                                    sizeof(schema)) ==
              PGWT_PGSS_DISCOVERY_SCHEMA && strcmp(schema, "weird") == 0,
          "extension schema discovery decodes a non-public schema");
    CHECK(pgwt_pgss_parse_discovery("ABSENT\n", schema, sizeof(schema)) ==
              PGWT_PGSS_DISCOVERY_ABSENT,
          "extension absent is a normal unavailable outcome");
    CHECK(pgwt_pgss_parse_discovery("PERMISSION\n", schema, sizeof(schema)) ==
              PGWT_PGSS_DISCOVERY_PERMISSION,
          "permission denial is a normal unavailable outcome");
    CHECK(pgwt_pgss_parse_discovery("garbage", schema, sizeof(schema)) ==
              PGWT_PGSS_DISCOVERY_ERROR,
          "unexpected discovery output is an error");
    char database[128];
    CHECK(pgwt_pgss_parse_database("DATABASE|6170706462\n", database,
                                   sizeof(database)) == 1 &&
          strcmp(database, "appdb") == 0,
          "sampled database OID resolves to its database-local extension context");
    CHECK(pgwt_pgss_parse_database("ABSENT\n", database,
                                   sizeof(database)) == 0,
          "dropped/non-connectable database is normally unavailable");

    struct pgwt_query_text_key keys[2] = {
        {.databaseid=5,.userid=10,.query_id=(uint64_t)(int64_t)-99},
        {.databaseid=6,.userid=11,.query_id=123},
    };
    char sql[4096];
    CHECK(pgwt_pgss_build_lookup_sql(sql, sizeof(sql), "odd\"schema", keys, 2) == 0 &&
          strstr(sql, "\"odd\"\"schema\".pg_stat_statements(true)") &&
          strstr(sql, "left(p.query,2048)") &&
          strstr(sql, "-99::bigint") && strstr(sql, "(6::oid,11::oid,123::bigint)"),
          "batch SQL uses context tuples, bounded text, signed qids, showtext=true and quoted schema");

    struct pgwt_query_text_key got;
    char text[128];
    CHECK(pgwt_pgss_parse_lookup_row("5|10|-99|T|53454c454354202431\n",
                                     &got, text, sizeof(text)) > 0 &&
          got.databaseid == 5 && got.userid == 10 &&
          got.query_id == (uint64_t)(int64_t)-99 &&
          strcmp(text, "SELECT $1") == 0,
          "batch row preserves context and decodes normalized text");
    CHECK(pgwt_pgss_parse_lookup_row("5|10|-99|U|", &got, text,
                                     sizeof(text)) == 0 && !text[0],
          "permission-hidden text is a normal unavailable row");
    CHECK(pgwt_pgss_parse_lookup_row("5|10|-99|X|xyz", &got, text,
                                     sizeof(text)) < 0,
          "malformed rows are rejected");
    CHECK(pgwt_pgss_miss_action(PGWT_PGSS_MAX_RETRIES - 1) ==
              PGWT_PGSS_MISS_RETRY &&
          pgwt_pgss_miss_action(PGWT_PGSS_MAX_RETRIES) ==
              PGWT_PGSS_MISS_EVICTED,
          "missing rows retry before terminal eviction/exhaustion");
    CHECK(PGWT_PGSS_REQUEUE_COOLDOWN_MS > 0 &&
          PGWT_PGSS_QUEUE_PROBES >= PGWT_PGSS_BATCH_MAX &&
          PGWT_PGSS_QUEUE_PROBES < QT_HT_SIZE &&
          PGWT_PGSS_SCAN_MIN_INTERVAL_MS < PGWT_PGSS_BATCH_INTERVAL_MS,
          "exhausted keys are eligible for later completion retry and enqueue work is bounded");

#ifdef PGWT_TEST
    struct pgwt_daemon *daemon = calloc(1, sizeof(*daemon));
    const char *qt_dir = "/tmp/pgwt_pgss_resolver_test";
    (void)system("rm -rf /tmp/pgwt_pgss_resolver_test");
    mkdir(qt_dir, 0755);
    struct pgwt_query_text_capture qt = {0};
    CHECK(pgwt_qt_init(&qt, qt_dir, 0, 0, (gid_t)-1) == 0,
          "query-text fixture initializes");
    struct pgwt_query_text_key persisted = {
        .databaseid = 5, .userid = 10, .query_id = 9001,
    };
    pgwt_qt_store_pgss(&qt, &persisted, "select $1");
    pgwt_qt_close(&qt);
    memset(&qt, 0, sizeof(qt));
    CHECK(pgwt_qt_init(&qt, qt_dir, 0, 0, (gid_t)-1) == 0 &&
          pgwt_qt_has_text(&qt, &persisted),
          "persisted text reload publishes lock-free presence");
    struct pgwt_pgss_resolver *resolver =
        pgwt_pgss_test_create(daemon, &qt);
    CHECK(resolver != NULL, "async queue test fixture initializes");
    struct pgwt_query_text_key qa = {
        .databaseid = 5, .userid = 10, .query_id = 1234,
    };
    struct pgwt_query_text_key qb = {
        .databaseid = 6, .userid = 10, .query_id = 1234,
    };
    pgwt_pgss_resolver_queue(resolver, &persisted);
    CHECK(daemon->counters.sampled_text_pending == 0,
          "restart-persisted key is rejected before resolver enqueue");
    pgwt_pgss_resolver_queue(resolver, &qa);
    pgwt_pgss_resolver_queue(resolver, &qa);
    pgwt_pgss_resolver_queue(resolver, &qb);
    CHECK(daemon->counters.sampled_text_pending == 2,
          "enqueue is context-keyed and duplicate sightings are nonblocking dedup");
    uint64_t next_due = 0;
    CHECK(pgwt_pgss_test_claim_batch(resolver, 10000, &next_due) == 1,
          "first due resolver batch is admitted");
    CHECK(pgwt_pgss_test_claim_batch(resolver, 10001, &next_due) == 0 &&
          next_due == 10000 + PGWT_PGSS_SCAN_MIN_INTERVAL_MS,
          "sparse drain uses the scan-frequency floor, not the full interval");
    CHECK(pgwt_pgss_test_cooldown(resolver, &qa, false) == 0,
          "retry-exhausted key enters cooldown");
    pgwt_pgss_resolver_queue(resolver, &qa);
    CHECK(daemon->counters.sampled_text_pending == 1,
          "cooldown suppresses immediate retry churn");
    CHECK(pgwt_pgss_test_cooldown(resolver, &qb, true) == 0,
          "second key can become due after simulated exhaustion");
    pgwt_pgss_resolver_queue(resolver, &qb);
    CHECK(daemon->counters.sampled_text_pending == 1,
          "later sampled sighting requeues an exhausted long statement");
    pgwt_pgss_resolver_stop(resolver);

    memset(&daemon->counters, 0, sizeof(daemon->counters));
    resolver = pgwt_pgss_test_create(daemon, &qt);
    for (uint64_t qid = 1; qid <= 300; qid++) {
        struct pgwt_query_text_key key = {
            .databaseid = 100, .userid = 10, .query_id = 100000 + qid,
        };
        pgwt_pgss_resolver_queue(resolver, &key);
    }
    for (uint32_t databaseid = 200; databaseid <= 300; databaseid += 100) {
        for (uint64_t qid = 1; qid <= 2; qid++) {
            struct pgwt_query_text_key key = {
                .databaseid = databaseid, .userid = 10,
                .query_id = 200000 + databaseid + qid,
            };
            pgwt_pgss_resolver_queue(resolver, &key);
        }
    }
    uint32_t batch_databaseid = 0;
    CHECK(pgwt_pgss_test_take_batch(resolver, 30000, &next_due,
                                    &batch_databaseid) ==
              PGWT_PGSS_BATCH_MAX && batch_databaseid == 100,
          "first tenant takes one full 256-key batch");
    CHECK(pgwt_pgss_test_claim_batch(resolver, 39999, &next_due) == 0 &&
          next_due == 30000 + PGWT_PGSS_BATCH_INTERVAL_MS,
          "full batch retains the ten-second SRF scan bound");
    CHECK(pgwt_pgss_test_take_batch(resolver, 40000, &next_due,
                                    &batch_databaseid) == 2 &&
          batch_databaseid == 200,
          "round robin serves a sparse second database before backlog repeats");
    CHECK(pgwt_pgss_test_claim_batch(resolver, 40001, &next_due) == 0 &&
          next_due == 40000 + PGWT_PGSS_SCAN_MIN_INTERVAL_MS,
          "sparse database does not burn a full ten-second window");
    CHECK(pgwt_pgss_test_take_batch(resolver, 41000, &next_due,
                                    &batch_databaseid) == 2 &&
          batch_databaseid == 300,
          "round robin reaches every due database without slot-order starvation");
    CHECK(pgwt_pgss_test_take_batch(resolver, 42000, &next_due,
                                    &batch_databaseid) == 44 &&
          batch_databaseid == 100 &&
          daemon->counters.sampled_text_pending == 0,
          "large tenant backlog resumes and drains after the fair round");
    pgwt_pgss_resolver_stop(resolver);

    memset(&daemon->counters, 0, sizeof(daemon->counters));
    resolver = pgwt_pgss_test_create(daemon, &qt);
    struct pgwt_query_text_key colliders[PGWT_PGSS_QUEUE_PROBES + 1];
    size_t ncolliders = 0;
    uint32_t collision_slot = 0;
    for (uint64_t qid = 1;
         qid <= (uint64_t)QT_HT_SIZE * (PGWT_PGSS_QUEUE_PROBES + 1) * 4 &&
         ncolliders < PGWT_PGSS_QUEUE_PROBES + 1; qid++) {
        struct pgwt_query_text_key key = {
            .databaseid = 777, .userid = 888, .query_id = qid,
        };
        uint32_t slot = pgwt_pgss_test_hash_slot(&key);
        if (ncolliders == 0) collision_slot = slot;
        if (slot == collision_slot)
            colliders[ncolliders++] = key;
    }
    CHECK(ncolliders == PGWT_PGSS_QUEUE_PROBES + 1,
          "collision fixture covers one slot beyond the fast probe window");
    for (size_t i = 0; i < ncolliders; i++)
        pgwt_pgss_resolver_queue(resolver, &colliders[i]);
    CHECK(daemon->counters.sampled_text_pending == ncolliders &&
          daemon->counters.sampled_text_error_total == 0,
          "pressure fallback finds distant free slots without false queue-full drops");
    pgwt_pgss_resolver_stop(resolver);

    memset(&daemon->counters, 0, sizeof(daemon->counters));
    resolver = pgwt_pgss_test_create(daemon, &qt);
    struct pgwt_query_text_key raced = {
        .databaseid = 5, .userid = 10, .query_id = 9002,
    };
    pgwt_pgss_resolver_queue(resolver, &raced);
    pgwt_qt_store_pgss(&qt, &raced, "select $1 + $2");
    CHECK(pgwt_pgss_test_claim_batch(resolver, 20000, &next_due) == 0 &&
          daemon->counters.sampled_text_pending == 0,
          "resolution gate drops text persisted after enqueue without SQL");
    pgwt_pgss_resolver_stop(resolver);
    pgwt_qt_close(&qt);
    (void)system("rm -rf /tmp/pgwt_pgss_resolver_test");
    free(daemon);
#endif

    printf("%d/%d tests passed\n", passed, run);
    return passed == run ? 0 : 1;
}
