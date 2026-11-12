#define _POSIX_C_SOURCE 200809L
#include "evidence_json.h"
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool in_scope(const char *id)
{
    const char *prefixes[] = {"01_", "08_", "16_", "17_", "25_", "27_", "28_", "29_", "30_"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
        if (!strncmp(id, prefixes[i], 3)) return true;
    return false;
}

/* JSON driver also serves the differential; unsupported input emits only an
 * error, with no metric object. A harness failure always exits 2. */
static int json_cases(int argc, char **argv)
{
    for (int i = 2; i < argc; ++i) {
        evidence_case input;
        char error[512] = {0};
        if (evidence_case_load(argv[i], &input, error, sizeof(error))) {
            fprintf(stderr, "%s: %s\n", argv[i], error); return 2;
        }
        pgwt_evidence_output out;
        const char *reason = NULL;
        pgwt_replay_status status = pgwt_evidence_replay(&input.input, &input.query, &out, &reason);
        int failed = 0;
        if (status == PGWT_REPLAY_UNSUPPORTED && !out.wait_time.metric && !out.completed_waits.metric)
            puts("{\"error\":\"unsupported\"}");
        else if (status != PGWT_REPLAY_OK || evidence_output_json(stdout, &out)) {
            fprintf(stderr, "%s: %s\n", argv[i], reason ? reason : "serialization error"); failed = 2;
        } else putchar('\n');
        pgwt_evidence_output_free(&out);
        evidence_case_free(&input);
        if (failed) return failed;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--json")) return json_cases(argc, argv);
    if (argc > 2) { fprintf(stderr, "usage: %s [contract-root] | --json FILE...\n", argv[0]); return 2; }
    const char *root = argc == 2 ? argv[1] : "contracts/evidence-v1";
    if (argc == 1 && access(root, R_OK)) root = "../contracts/evidence-v1";
    char pattern[4096], expected[4096], projections[4096];
    if (snprintf(pattern, sizeof(pattern), "%s/scenarios/*.json", root) >= (int)sizeof(pattern) ||
        snprintf(expected, sizeof(expected), "%s/expected.json", root) >= (int)sizeof(expected) ||
        snprintf(projections, sizeof(projections), "%s/projections.json", root) >= (int)sizeof(projections)) return 2;
    glob_t paths = {0};
    if (glob(pattern, 0, NULL, &paths) || paths.gl_pathc != 33) {
        fprintf(stderr, "evidence harness: need 33 frozen scenarios, found %zu\n", paths.gl_pathc);
        globfree(&paths); return 2;
    }
    int mismatch = 0, harness_error = 0;
    for (size_t i = 0; i < paths.gl_pathc; ++i) {
        evidence_case input;
        char field[512] = {0};
        if (evidence_case_load(paths.gl_pathv[i], &input, field, sizeof(field))) {
            fprintf(stderr, "evidence harness: %s: %s\n", paths.gl_pathv[i], field);
            harness_error = 1; continue;
        }
        pgwt_evidence_output out;
        const char *reason = NULL;
        pgwt_replay_status status = pgwt_evidence_replay(&input.input, &input.query, &out, &reason);
        if (status == PGWT_REPLAY_ERROR) {
            fprintf(stderr, "evidence %s ERROR %s\n", input.id, reason); harness_error = 1;
        } else if (!in_scope(input.id)) {
            /* Even the in-memory result slots must remain empty on refusal. */
            if (status == PGWT_REPLAY_UNSUPPORTED && !out.wait_time.metric && !out.completed_waits.metric)
                printf("evidence %s UNSUPPORTED-OK\n", input.id);
            else {
                printf("evidence %s FAIL unsupported\n", input.id); mismatch = 1;
            }
        } else if (status != PGWT_REPLAY_OK) {
            printf("evidence %s FAIL unexpected_unsupported\n", input.id); mismatch = 1;
        } else {
            int compared = evidence_compare(&input, &out, expected, projections, field, sizeof(field));
            if (compared < 0) { fprintf(stderr, "evidence %s ERROR %s\n", input.id, field); harness_error = 1; }
            else if (compared) { printf("evidence %s FAIL %s\n", input.id, field); mismatch = 1; }
            else printf("evidence %s PASS\n", input.id);
        }
        pgwt_evidence_output_free(&out);
        evidence_case_free(&input);
    }
    globfree(&paths);
    return harness_error ? 2 : mismatch;
}
