/* pgss_resolver.h -- asynchronous sampled query-text resolution. */
#ifndef PGWT_PGSS_RESOLVER_H
#define PGWT_PGSS_RESOLVER_H

#include "query_text.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PGWT_PGSS_BATCH_MAX 256
#define PGWT_PGSS_MAX_RETRIES 6
#define PGWT_PGSS_TEXT_CHARS 2048
#define PGWT_PGSS_TEXT_BYTES (PGWT_PGSS_TEXT_CHARS * 4 + 1)
#define PGWT_PGSS_QUEUE_PROBES 64
#define PGWT_PGSS_REQUEUE_COOLDOWN_MS 30000
#define PGWT_PGSS_BATCH_INTERVAL_MS 10000

enum pgwt_pgss_discovery_result {
    PGWT_PGSS_DISCOVERY_SCHEMA = 0,
    PGWT_PGSS_DISCOVERY_ABSENT,
    PGWT_PGSS_DISCOVERY_PERMISSION,
    PGWT_PGSS_DISCOVERY_ERROR,
};

enum pgwt_pgss_miss_action {
    PGWT_PGSS_MISS_RETRY = 0,
    PGWT_PGSS_MISS_EVICTED,
};

enum pgwt_pgss_discovery_result
pgwt_pgss_parse_discovery(const char *output, char *schema,
                          size_t schema_size);
int pgwt_pgss_parse_database(const char *output, char *database,
                             size_t database_size);
int pgwt_pgss_build_lookup_sql(char *sql, size_t sql_size,
                               const char *schema,
                               const struct pgwt_query_text_key *keys,
                               size_t count);
int pgwt_pgss_parse_lookup_row(const char *line,
                               struct pgwt_query_text_key *key,
                               char *text, size_t text_size);
enum pgwt_pgss_miss_action pgwt_pgss_miss_action(unsigned attempts);

struct pgwt_daemon;
struct pgwt_pgss_resolver;

#ifndef PGWT_SERVER
int pgwt_pgss_resolver_start(struct pgwt_pgss_resolver **out,
                             struct pgwt_daemon *daemon,
                             struct pgwt_query_text_capture *query_text,
                             const char *postgres_binary,
                             pid_t postmaster_pid);
void pgwt_pgss_resolver_queue(struct pgwt_pgss_resolver *resolver,
                              const struct pgwt_query_text_key *key);
void pgwt_pgss_resolver_stop(struct pgwt_pgss_resolver *resolver);
#ifdef PGWT_TEST
struct pgwt_pgss_resolver *pgwt_pgss_test_create(
    struct pgwt_daemon *daemon, struct pgwt_query_text_capture *query_text);
int pgwt_pgss_test_cooldown(struct pgwt_pgss_resolver *resolver,
                            const struct pgwt_query_text_key *key,
                            bool immediately_due);
size_t pgwt_pgss_test_claim_batch(struct pgwt_pgss_resolver *resolver,
                                  uint64_t now, uint64_t *next_due);
#endif
#endif

#endif
