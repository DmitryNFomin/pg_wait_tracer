#include "pgss_resolver.h"
#ifdef PGWT_TEST
#include "daemon.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
          PGWT_PGSS_QUEUE_PROBES < 4096,
          "exhausted keys are eligible for later completion retry and enqueue work is bounded");

#ifdef PGWT_TEST
    struct pgwt_daemon *daemon = calloc(1, sizeof(*daemon));
    struct pgwt_query_text_capture qt = {0};
    struct pgwt_pgss_resolver *resolver =
        pgwt_pgss_test_create(daemon, &qt);
    CHECK(resolver != NULL, "async queue test fixture initializes");
    struct pgwt_query_text_key qa = {
        .databaseid = 5, .userid = 10, .query_id = 1234,
    };
    struct pgwt_query_text_key qb = {
        .databaseid = 6, .userid = 10, .query_id = 1234,
    };
    pgwt_pgss_resolver_queue(resolver, &qa);
    pgwt_pgss_resolver_queue(resolver, &qa);
    pgwt_pgss_resolver_queue(resolver, &qb);
    CHECK(daemon->counters.sampled_text_pending == 2,
          "enqueue is context-keyed and duplicate sightings are nonblocking dedup");
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
    free(daemon);
#endif

    printf("%d/%d tests passed\n", passed, run);
    return passed == run ? 0 : 1;
}
