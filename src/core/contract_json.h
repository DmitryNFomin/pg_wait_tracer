#ifndef PGWT_CONTRACT_JSON_H
#define PGWT_CONTRACT_JSON_H

/* Test-only JSON adapter, not a core dependency. It owns allocated parse trees
 * and input arrays until case_free. Allocation/IO/JSON syntax errors return -1;
 * valid JSON with invalid contract shape loads successfully, preserving the
 * per-field invalid flags needed by contract 7.2-5. Integer tokens are decoded
 * directly into uint64_t with checked decimal arithmetic, never via double.
 */
#include "pgwt_core.h"

typedef struct {
    pgwt_core_evidence evidence;
    pgwt_core_query query;
    void *document;
} pgwt_contract_case;

int pgwt_contract_case_load(const char *path, pgwt_contract_case *out,
                            char *error, size_t error_size);
void pgwt_contract_case_free(pgwt_contract_case *input);

/* Writes one result object, returns -1 if the caller's buffer is too small.
 * 2048 bytes suffice for every result emitted by pgwt_core_wait_metrics. */
int pgwt_contract_result_json(const pgwt_core_result *result,
                              char *buffer, size_t size);

/* Compare the serialized objects to the expected file as JSON values:
 * object key order is irrelevant, array order matters, numbers remain exact.
 * Ignore only top-level derivation and members that are not result objects.
 * Return 0 for equality, 1 for a case mismatch, -1 for a harness/IO/JSON error.
 */
int pgwt_contract_compare_results(const char *wait_time_json,
                                  const char *completed_json,
                                  const char *expected_path,
                                  char *error, size_t error_size);

#endif
