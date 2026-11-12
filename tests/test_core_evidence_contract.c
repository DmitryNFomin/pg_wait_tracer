#define _POSIX_C_SOURCE 200809L
#include "evidence_json.h"
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool in_scope(const char *id)
{
    const char *prefixes[] = {"01_", "02_", "03_", "04_", "06_", "07_", "24_", "08_", "16_", "17_", "25_", "27_", "28_", "29_", "30_", "09_", "10_", "11_", "33_", "18_", "19_", "20_", "21_", "32_"};
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

/* Native callers may leave ignored fork storage nonzero for provisional keys.
 * The has_fork flag, rather than those bytes, defines the capture-local key. */
static int native_provisional(void)
{
    pgwt_core_event capture = {"A", 1}, event = {"wait", 4};
    pgwt_evidence_record records[] = {
        {.kind = PGWT_EVIDENCE_LIFE, .pid = 101, .t = 10, .fork = 37},
        {.kind = PGWT_EVIDENCE_ATTACH_OK, .pid = 101, .t = 12, .fork = 38,
         .reread = true, .counters = {.known = true}},
        {.kind = PGWT_EVIDENCE_TRANS, .pid = 101, .t = 14, .new_event = event, .duration = 2},
        {.kind = PGWT_EVIDENCE_FRONTIER, .t = 20, .counters = {.known = true}},
        {.kind = PGWT_EVIDENCE_OTHER, .pid = 101, .t = 15, .new_event = event, .duration = 1}
    };
    pgwt_evidence_file file = {.version = 5, .boot = {"boot", 4}, .capture = capture,
        .present = true, .qualified = true, .baseline = {.known = true},
        .records = records, .record_count = 4};
    pgwt_evidence_input input = {.files = &file, .file_count = 1, .capture = capture,
        .pid = 101, .fork = 39};
    pgwt_core_query query = {.event = event, .from = 12, .to = 20};
    pgwt_evidence_output out;
    pgwt_replay_status status = pgwt_evidence_replay(&input, &query, &out, NULL);
    bool pass = status == PGWT_REPLAY_OK && !out.projection.invalid_fields &&
        out.projection.life_start == 0 && !out.projection.has_life_end &&
        out.projection.coverage_count == 1 && out.coverage.start == 12 && out.coverage.end == 20 &&
        out.projection.wait_count == 0 && out.projection.has_open &&
        out.projection.open.start == 14 && out.projection.open.as_of == 20 &&
        out.wait_time.value.present && out.wait_time.value.value == 6 &&
        out.completed_waits.value.present && out.completed_waits.value.value == 0;
    pgwt_evidence_output_free(&out);
    puts(pass ? "evidence native_provisional_fork_bytes PASS" :
                "evidence native_provisional_fork_bytes FAIL projection");
    /* Unknown kinds must be refused even when their native fields look like a
     * valid transition. The JSON loader cannot express this pid-bearing OTHER. */
    file.record_count = sizeof(records) / sizeof(records[0]);
    const char *reason = NULL;
    status = pgwt_evidence_replay(&input, &query, &out, &reason);
    bool kind_pass = status == PGWT_REPLAY_UNSUPPORTED && reason &&
        !strcmp(reason, "unsupported record kind") &&
        !out.wait_time.metric && !out.completed_waits.metric;
    pgwt_evidence_output_free(&out);
    puts(kind_pass ? "evidence native_unsupported_kind PASS" :
                     "evidence native_unsupported_kind FAIL unsupported");
    return !pass || !kind_pass;
}

/* Exercise public-API state validation without the JSON adapter's checks. */
static int native_attachment_validation(void)
{
    pgwt_evidence_record records[] = {
        {.kind = PGWT_EVIDENCE_LIFE, .pid = 101, .t = 0, .has_fork = true,
         .start_known = true},
        {.kind = PGWT_EVIDENCE_ATTACH_OK, .pid = 101, .t = 1, .has_fork = true,
         .reread = true, .counters = {.known = true}},
        {.kind = PGWT_EVIDENCE_FRONTIER, .t = 10, .counters = {.known = true}}
    };
    pgwt_evidence_file file = {.version = 5, .boot = {"boot", 4}, .capture = {"A", 1},
        .present = true, .qualified = true, .baseline = {.known = true},
        .records = records, .record_count = 3};
    pgwt_evidence_input input = {.files = &file, .file_count = 1, .capture = {"A", 1},
        .pid = 101, .has_fork = true};
    pgwt_core_query query = {.event = {"wait", 4}, .from = 1, .to = 10};
    const pgwt_core_event invalid[] = {{"$EXIT", 5}, {"", 0}, {NULL, 1}};
    int failed = 0;
    for (size_t field = 0; field < 2; ++field) {
        bool pass = true;
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            /* No anchor dependency can mask the field's own validation. */
            records[1].reread = false;
            records[1].seed = field ? (pgwt_core_event){0} : invalid[i];
            records[1].state = field ? invalid[i] : (pgwt_core_event){0};
            pgwt_evidence_output out;
            pgwt_replay_status status = pgwt_evidence_replay(&input, &query, &out, NULL);
            pass &= status == PGWT_REPLAY_UNSUPPORTED && !out.wait_time.metric && !out.completed_waits.metric;
            pgwt_evidence_output_free(&out);
        }
        printf("evidence native_invalid_%s %s\n", field ? "state" : "seed",
               pass ? "PASS" : "FAIL validation");
        failed |= !pass;
    }
    return failed;
}

/* Native END strings can be malformed in ways the JSON loader cannot encode.
 * Ignored fork bytes also must not split provisional END/attachment keys. */
static int native_end(void)
{
    pgwt_evidence_record records[] = {
        {.kind=PGWT_EVIDENCE_LIFE, .pid=101, .t=0, .fork=31},
        {.kind=PGWT_EVIDENCE_ATTACH_OK, .pid=101, .t=1, .fork=32,
         .reread=true, .counters={.known=true}},
        {.kind=PGWT_EVIDENCE_TRANS, .pid=101, .t=3, .new_event={"wait",4}, .duration=2},
        {.kind=PGWT_EVIDENCE_END, .pid=101, .t=8, .fork=33, .end_reason={"stop",4}},
        {.kind=PGWT_EVIDENCE_FRONTIER, .t=10, .counters={.known=true}},
        {.kind=PGWT_EVIDENCE_ATTACH_OK, .pid=101, .t=12, .fork=34,
         .reread=true, .counters={.known=true}},
        {.kind=PGWT_EVIDENCE_TRANS, .pid=101, .t=14, .new_event={"wait",4}, .duration=2},
        {.kind=PGWT_EVIDENCE_END, .pid=101, .t=20, .fork=35, .end_reason={"detach",6}},
        {.kind=PGWT_EVIDENCE_FRONTIER, .t=22, .counters={.known=true}}
    };
    pgwt_evidence_file file = {.version=5, .boot={"boot",4}, .capture={"A",1},
        .present=true, .qualified=true, .baseline={.known=true},
        .records=records, .record_count=9};
    pgwt_evidence_input input = {.files=&file, .file_count=1, .capture={"A",1}, .pid=101, .fork=36};
    pgwt_core_query query = {.event={"wait",4}, .from=1, .to=8};
    pgwt_evidence_output out;
    bool pass = true;
    for (size_t i=0; i<2; ++i) {
        if (i) { query.from=12; query.to=20; }
        pgwt_replay_status status=pgwt_evidence_replay(&input, &query, &out, NULL);
        pass &= status==PGWT_REPLAY_OK && out.projection.coverage_count==1 &&
            out.coverage.start==query.from && out.coverage.end==query.to &&
            out.projection.has_open && out.projection.wait_count==0 &&
            out.wait_time.value.present && out.wait_time.value.value==(i ? 6 : 5) &&
            out.completed_waits.value.present && out.completed_waits.value.value==0;
        pgwt_evidence_output_free(&out);
    }
    puts(pass ? "evidence native_end_provisional PASS" : "evidence native_end_provisional FAIL projection");
    bool valid = true;
    const pgwt_core_event invalid[] = {{NULL,4},{"",0},{"stopX",5},{"stop",3},{"$EXIT",5}};
    for (size_t i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
        records[3].end_reason=invalid[i];
        pgwt_replay_status status=pgwt_evidence_replay(&input, &query, &out, NULL);
        valid &= status==PGWT_REPLAY_UNSUPPORTED && !out.wait_time.metric && !out.completed_waits.metric;
        pgwt_evidence_output_free(&out);
    }
    puts(valid ? "evidence native_end_reason PASS" : "evidence native_end_reason FAIL validation");
    records[3].end_reason=(pgwt_core_event){"stop",4};
    pgwt_replay_status status=pgwt_evidence_replay(&input, NULL, &out, NULL);
    bool null_query = status==PGWT_REPLAY_OK && out.coverage.start==1 && out.coverage.end==8 &&
        !out.wait_time.value.present && !out.completed_waits.value.present;
    pgwt_evidence_output_free(&out);
    puts(null_query ? "evidence native_null_query PASS" : "evidence native_null_query FAIL projection");
    return !pass || !valid || !null_query;
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
    int mismatch = native_provisional() | native_attachment_validation() | native_end(), harness_error = 0;
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
