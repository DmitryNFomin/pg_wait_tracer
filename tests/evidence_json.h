#ifndef PGWT_TEST_EVIDENCE_JSON_H
#define PGWT_TEST_EVIDENCE_JSON_H

/* Test-only logical scenario loader/comparator. Owns decoded strings, arrays
 * and JSON documents until free. Integer tokens never pass through double.
 * A valid JSON document with an unsupported shape loads with adapter flags;
 * syntax, allocation and IO errors return -1 (harness error).
 */
#include "core/evidence_replay.h"
#include "core/contract_json.h"
#include <stdio.h>

typedef struct {
    pgwt_evidence_input input;
    pgwt_core_query query;
    const char *id;
    void *document;
    pgwt_evidence_file *files;
} evidence_case;

int evidence_case_load(const char *path, evidence_case *out, char *error, size_t size);
void evidence_case_free(evidence_case *input);
/* One JSON object, containing projection and both result objects. */
int evidence_output_json(FILE *stream, const pgwt_evidence_output *out);
/* 0 match, 1 mismatch with field path, -1 harness error. */
int evidence_compare(const evidence_case *input, const pgwt_evidence_output *out,
                     const char *expected, const char *projections,
                     char *field, size_t size);
#endif
