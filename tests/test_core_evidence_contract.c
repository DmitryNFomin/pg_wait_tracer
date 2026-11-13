#define _POSIX_C_SOURCE 200809L
#include "evidence_json.h"
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
        if (status == PGWT_REPLAY_UNSUPPORTED && !out.wait_time.metric && !out.completed_waits.metric) {
            if (!strcmp(argv[1], "--json-reasons"))
                printf("{\"error\":\"unsupported\",\"reason\":\"%s\"}\n", reason ? reason : "unspecified");
            else puts("{\"error\":\"unsupported\"}");
        }
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
        {.kind = PGWT_EVIDENCE_OTHER, .pid = 101, .t = 15, .new_event = event, .duration = 1},
        {.kind = PGWT_EVIDENCE_FRONTIER, .t = 30, .counters = {.known = true}}
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
    /* An unknown kind cuts after the earlier certificate, preserving its value.
     * The JSON loader cannot express this pid-bearing OTHER. */
    file.record_count = sizeof(records) / sizeof(records[0]);
    const char *reason = NULL;
    status = pgwt_evidence_replay(&input, &query, &out, &reason);
    bool kind_pass = status == PGWT_REPLAY_OK && !reason &&
        out.wait_time.value.present && out.wait_time.value.value == 6 &&
        out.coverage.start == 12 && out.coverage.end == 20;
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
        {.kind = PGWT_EVIDENCE_FRONTIER, .t = 10, .counters = {.known = true}},
        {.kind = PGWT_EVIDENCE_LIFE, .pid = 101, .t = 20, .has_fork = true,
         .start_known = true, .has_exit = true, .exit = 20}
    };
    pgwt_evidence_file file = {.version = 5, .boot = {"boot", 4}, .capture = {"A", 1},
        .present = true, .qualified = true, .baseline = {.known = true},
        .records = records, .record_count = 4};
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
            pass &= status == PGWT_REPLAY_OK && !out.projection.invalid_fields &&
                out.projection.coverage_count == 0 && !out.projection.has_life_end && !out.wait_time.value.present;
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
        valid &= status==PGWT_REPLAY_OK && !out.projection.invalid_fields &&
            out.projection.coverage_count==0 && !out.wait_time.value.present;
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

/* Native flags can leave arbitrary bytes in inactive union-like fields.
 * VOID has no backend key; reason is length-delimited opaque metadata. */
static int native_void(void)
{
    pgwt_evidence_record records[] = {
        {.kind=PGWT_EVIDENCE_LIFE, .pid=101, .has_fork=true, .start_known=true},
        {.kind=PGWT_EVIDENCE_ATTACH_OK, .t=1, .pid=101, .has_fork=true,
         .reread=true, .counters={.known=true}},
        {.kind=PGWT_EVIDENCE_TRANS, .t=3, .pid=101, .new_event={"A",1}},
        {.kind=PGWT_EVIDENCE_FRONTIER, .t=8, .counters={.known=true}},
        {.kind=PGWT_EVIDENCE_VOID, .t=12, .pid=999, .has_fork=true, .fork=777,
         .counters={.known=true,.value={[11]=1}}, .void_reason={"loss",4},
         .invalid_from=1},
        {.kind=PGWT_EVIDENCE_FRONTIER, .t=24, .counters={.known=true,.value={[11]=1}}},
        {.kind=PGWT_EVIDENCE_LIFE, .pid=101, .has_fork=true, .start_known=true,
         .t=30, .has_exit=true, .exit=30}
    };
    pgwt_evidence_file file={.version=5,.boot={"boot",4},.capture={"A",1},
        .present=true,.qualified=true,.baseline={.known=true},.records=records,.record_count=7};
    pgwt_evidence_input input={.files=&file,.file_count=1,.capture={"A",1},.pid=101,.has_fork=true};
    pgwt_core_query query={.event={"A",1},.from=1,.to=8};
    pgwt_evidence_output out;
    bool flags=true;
    for (size_t revoke=0; revoke<2; ++revoke) {
        records[4].has_invalid_from=revoke;
        pgwt_replay_status status=pgwt_evidence_replay(&input,&query,&out,NULL);
        flags &= status==PGWT_REPLAY_OK && !out.projection.invalid_fields &&
            out.projection.coverage_count==(revoke ? 0 : 1) &&
            out.projection.has_open==!revoke && out.wait_time.value.present==!revoke &&
            out.completed_waits.value.present==!revoke;
        if (!revoke) flags &= out.coverage.start==1 && out.coverage.end==8 &&
            out.projection.open.start==3 && out.projection.open.as_of==8 &&
            out.wait_time.value.value==5 && out.completed_waits.value.value==0;
        pgwt_evidence_output_free(&out);
    }
    puts(flags ? "evidence native_void_flags PASS" : "evidence native_void_flags FAIL projection");
    records[4].has_invalid_from=false;
    const pgwt_core_event invalid[]={{NULL,4},{"",0},{NULL,0}};
    bool reason=true;
    for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        records[4].void_reason=invalid[i];
        pgwt_replay_status status=pgwt_evidence_replay(&input,&query,&out,NULL);
        reason &= status==PGWT_REPLAY_OK && out.projection.coverage_count==1 &&
            out.coverage.end==8 && !out.projection.has_life_end && out.wait_time.value.present && out.wait_time.value.value==5;
        pgwt_evidence_output_free(&out);
    }
    puts(reason ? "evidence native_void_reason PASS" : "evidence native_void_reason FAIL validation");
    const char opaque_bytes[]={'a',0,'b'};
    records[4].void_reason=(pgwt_core_event){opaque_bytes,sizeof(opaque_bytes)};
    pgwt_replay_status status=pgwt_evidence_replay(&input,&query,&out,NULL);
    bool opaque=status==PGWT_REPLAY_OK && out.wait_time.value.present && out.wait_time.value.value==5;
    pgwt_evidence_output_free(&out);
    puts(opaque ? "evidence native_void_opaque PASS" : "evidence native_void_opaque FAIL projection");
    records[4].counters.known=false;
    for (size_t i=0;i<15;++i) records[4].counters.value[i]=UINT64_MAX;
    status=pgwt_evidence_replay(&input,&query,&out,NULL);
    bool unknown=status==PGWT_REPLAY_OK && out.projection.coverage_count==1 &&
        out.coverage.end==8 && out.wait_time.value.present && out.wait_time.value.value==5;
    pgwt_evidence_output_free(&out);
    puts(unknown ? "evidence native_void_unknown_counter_bytes PASS" :
                  "evidence native_void_unknown_counter_bytes FAIL projection");
    /* An unexpected certificate vector is an implicit VOID, whose inactive
     * has_invalid_from/invalid_from fields must not revoke the earlier proof. */
    records[4].kind=PGWT_EVIDENCE_FRONTIER;
    records[4].counters.known=true;
    memset(records[4].counters.value,0,sizeof(records[4].counters.value));
    records[4].counters.value[11]=1;
    records[4].has_invalid_from=true;
    status=pgwt_evidence_replay(&input,&query,&out,NULL);
    bool unused=status==PGWT_REPLAY_OK && out.projection.coverage_count==1 &&
        out.coverage.end==8 && out.wait_time.value.present && out.wait_time.value.value==5;
    pgwt_evidence_output_free(&out);
    puts(unused ? "evidence native_certificate_unused_VOID_fields PASS" :
                 "evidence native_certificate_unused_VOID_fields FAIL projection");
    records[2].pid=0;
    status=pgwt_evidence_replay(&input,&query,&out,NULL);
    bool pid=status==PGWT_REPLAY_OK && !out.projection.invalid_fields &&
        out.projection.coverage_count==0 && !out.wait_time.value.present;
    pgwt_evidence_output_free(&out);
    puts(pid ? "evidence native_zero_pid PASS" : "evidence native_zero_pid FAIL validation");
    return !flags || !reason || !opaque || !unknown || !unused || !pid;
}

/* Descriptor flags and impossible JSON array lengths through the public API. */
static int native_chains(void)
{
    pgwt_evidence_record records[] = {
        {.kind=PGWT_EVIDENCE_LIFE,.pid=101,.has_fork=true,.start_known=true},
        {.kind=PGWT_EVIDENCE_ATTACH_OK,.pid=101,.has_fork=true,.t=1,
         .reread=true,.counters={.known=true}},
        {.kind=PGWT_EVIDENCE_TRANS,.pid=101,.t=3,.new_event={"A",1}},
        {.kind=PGWT_EVIDENCE_FRONTIER,.t=8,.counters={.known=true}},
        {.kind=PGWT_EVIDENCE_TRANS,.pid=101,.t=16,.old_event={"A",1},.duration=13},
        {.kind=PGWT_EVIDENCE_FRONTIER,.t=20,.counters={.known=true}}
    };
    pgwt_evidence_file files[] = {
        {.version=5,.boot={"b",1},.capture={"A",1},.previous=UINT64_MAX,
         .present=true,.qualified=true,.baseline={.known=true},.records=records,.record_count=4},
        {.version=5,.boot={"b",1},.capture={"A",1},.file_no=1,.has_previous=true,
         .present=true,.qualified=true,.baseline={.known=true},.records=records+4,.record_count=2}
    };
    pgwt_evidence_input input={.files=files,.file_count=2,.capture={"A",1},.pid=101,.has_fork=true};
    pgwt_core_query query={.event={"A",1},.from=3,.to=20};
    pgwt_evidence_output out;
    pgwt_replay_status status=pgwt_evidence_replay(&input,&query,&out,NULL);
    bool flags=status==PGWT_REPLAY_OK && out.projection.coverage_count==1 &&
        out.coverage.start==1 && out.coverage.end==20 && out.projection.wait_count==1 &&
        out.wait_time.value.present && out.wait_time.value.value==13 &&
        out.completed_waits.value.present && out.completed_waits.value.value==1;
    pgwt_evidence_output_free(&out);
    puts(flags ? "evidence native_chain_inactive_previous PASS" : "evidence native_chain_inactive_previous FAIL projection");
    files[1].unsupported_shape=true;
    status=pgwt_evidence_replay(&input,&query,&out,NULL);
    bool shape=status==PGWT_REPLAY_OK && out.projection.coverage_count==1 && out.coverage.end==8;
    pgwt_evidence_output_free(&out);
    puts(shape ? "evidence native_chain_descriptor_shape PASS" : "evidence native_chain_descriptor_shape FAIL validation");
    files[1].unsupported_shape=false;
    const pgwt_evidence_record *saved=files[1].records;
    files[1].records=NULL;
    status=pgwt_evidence_replay(&input,&query,&out,NULL);
    bool arrays=status==PGWT_REPLAY_OK && out.projection.coverage_count==1 && out.coverage.end==8;
    pgwt_evidence_output_free(&out);
    puts(arrays ? "evidence native_chain_null_records PASS" : "evidence native_chain_null_records FAIL validation");
    // A missing file's unreadable record array is never inspected or salvaged.
    files[1].present=false;
    status=pgwt_evidence_replay(&input,&query,&out,NULL);
    bool gap=status==PGWT_REPLAY_OK && out.projection.coverage_count==1 &&
        out.coverage.end==8 && out.projection.has_open && out.projection.open.as_of==8;
    pgwt_evidence_output_free(&out);
    puts(gap ? "evidence native_chain_gap_ignores_array PASS" : "evidence native_chain_gap_ignores_array FAIL projection");
    files[1].present=true;
    files[1].records=saved;
    files[1].boot=(pgwt_core_event){NULL,1};
    status=pgwt_evidence_replay(&input,&query,&out,NULL);
    bool identity=status==PGWT_REPLAY_OK && out.projection.coverage_count==1 && out.coverage.end==8;
    pgwt_evidence_output_free(&out);
    files[1].boot=files[0].boot;
    files[1].capture=(pgwt_core_event){NULL,1};
    status=pgwt_evidence_replay(&input,&query,&out,NULL);
    identity &= status==PGWT_REPLAY_ERROR && !out.wait_time.metric && !out.completed_waits.metric;
    pgwt_evidence_output_free(&out);
    files[1].capture=files[0].capture;
    files[0].boot=(pgwt_core_event){NULL,1};
    status=pgwt_evidence_replay(&input,&query,&out,NULL);
    identity &= status==PGWT_REPLAY_UNSUPPORTED && !out.wait_time.metric && !out.completed_waits.metric;
    pgwt_evidence_output_free(&out);
    files[0].boot=files[1].boot;
    puts(identity ? "evidence native_chain_invalid_identity PASS" : "evidence native_chain_invalid_identity FAIL validation");
    files[1].record_count=SIZE_MAX;
    status=pgwt_evidence_replay(&input,&query,&out,NULL);
    bool overflow=status==PGWT_REPLAY_ERROR && !out.wait_time.metric && !out.completed_waits.metric;
    pgwt_evidence_output_free(&out);
    input.file_count=SIZE_MAX;
    status=pgwt_evidence_replay(&input,&query,&out,NULL);
    overflow &= status==PGWT_REPLAY_ERROR && !out.wait_time.metric && !out.completed_waits.metric;
    pgwt_evidence_output_free(&out);
    puts(overflow ? "evidence native_chain_size_overflow PASS" : "evidence native_chain_size_overflow FAIL validation");
    return !flags || !shape || !arrays || !gap || !identity || !overflow;
}

/* The logical reference assumes intact descriptors. Verify their decoder
 * separately, without inventing reference answers for out-of-domain containers. */
static int malformed_baselines(const char *root)
{
    char path[4096], error[512];
    if (snprintf(path, sizeof(path), "%s/../../tests/fixtures/evidence-r3-malformed-baselines.json", root) >= (int)sizeof(path)) return 1;
    evidence_case input;
    if (evidence_case_load(path, &input, error, sizeof(error))) return 1;
    bool pass = input.input.file_count == 4;
    for (size_t i = 0; i < input.input.file_count; ++i)
        pass &= input.files[i].unsupported_shape;
    evidence_case_free(&input);
    puts(pass ? "evidence native_malformed_baselines PASS" : "evidence native_malformed_baselines FAIL validation");
    return !pass;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && (!strcmp(argv[1], "--json") || !strcmp(argv[1], "--json-reasons"))) return json_cases(argc, argv);
    if (argc > 2) { fprintf(stderr, "usage: %s [contract-root] | --json FILE...\n", argv[0]); return 2; }
    const char *root = argc == 2 ? argv[1] : "contracts/evidence-v1";
    if (argc == 1 && access(root, R_OK)) root = "../contracts/evidence-v1";
    int mismatch = native_provisional() | native_attachment_validation() | native_end() | native_void() | native_chains() | malformed_baselines(root), harness_error = 0;
    for (size_t revision = 0; revision < 2; ++revision) {
    char pattern[4096], expected[4096], projections[4096], revision_root[4096];
    if (snprintf(revision_root, sizeof(revision_root), "%s%s", root, revision ? "/r3" : "") >= (int)sizeof(revision_root)) return 2;
    if (snprintf(pattern, sizeof(pattern), "%s/scenarios/*.json", revision_root) >= (int)sizeof(pattern) ||
        snprintf(expected, sizeof(expected), "%s/expected.json", revision_root) >= (int)sizeof(expected) ||
        snprintf(projections, sizeof(projections), "%s/projections.json", revision_root) >= (int)sizeof(projections)) return 2;
    glob_t paths = {0};
    if (glob(pattern, 0, NULL, &paths) || paths.gl_pathc != (revision ? 31 : 33)) {
        fprintf(stderr, "evidence harness: need %u frozen scenarios in %s, found %zu\n", revision ? 31 : 33, revision_root, paths.gl_pathc);
        globfree(&paths); return 2;
    }
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
    }
    return harness_error ? 2 : mismatch;
}
