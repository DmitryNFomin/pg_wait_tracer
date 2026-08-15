/* synthetic_query.h -- PG13 sampled normalized-text grouping. */
#ifndef PGWT_SYNTHETIC_QUERY_H
#define PGWT_SYNTHETIC_QUERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PGWT_PG13_SYNTH_VERSION "pg13-synth-v1"
#define PGWT_PG13_SYNTH_TEXT_MAX 4096

/* Normalize one complete PostgreSQL activity string.  Comments are removed,
 * whitespace is collapsed, and literal constants are replaced with '?'.
 * Returns the normalized byte length, or 0 when the input is unusable. */
size_t pgwt_pg13_normalize_query(const char *input, char *output,
                                 size_t output_size);

/* Stable, versioned grouping key.  Database and user OIDs are deliberately
 * part of the hash domain: this is a sampled grouping key, not a PostgreSQL
 * pg_stat_statements queryid and must never be joined to pgss by id. */
uint64_t pgwt_pg13_synthetic_key(uint32_t databaseid, uint32_t userid,
                                 const char *normalized);

/* One-shot helper used by the sampler. */
int pgwt_pg13_synthetic_query(uint32_t databaseid, uint32_t userid,
                              const char *activity, char *normalized,
                              size_t normalized_size, uint64_t *key);

#endif
