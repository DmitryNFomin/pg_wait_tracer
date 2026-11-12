#define _POSIX_C_SOURCE 200809L
#include "core/contract_json.h"

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The frozen cases exercise the numbers. These additional adapter checks cover
 * section 7.5's token/shape distinctions without consulting another oracle or
 * changing the 48-case count. They inspect only the adapter, so numerical
 * mutants must still reach and fail the independent case comparisons below. */
static bool adapter_input(const char *label, const char *evidence, const char *query,
                           unsigned evidence_flags, unsigned query_flags)
{
    char path[] = "/tmp/pgwt-core-shape-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return false; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(path); return false; }
    bool wrote = fprintf(f, "{\"evidence\":%s,\"query\":%s}", evidence, query) >= 0;
    if (fclose(f) != 0) wrote = false;
    char error[256] = {0};
    pgwt_contract_case input;
    int status = wrote ? pgwt_contract_case_load(path, &input, error, sizeof(error)) : -1;
    unlink(path);
    if (status != 0) {
        fprintf(stderr, "adapter %s: %s\n", label, error); return false;
    }
    bool ok = input.evidence.invalid_fields == evidence_flags &&
              input.query.invalid_fields == query_flags;
    if (!ok)
        fprintf(stderr, "adapter %s: flags evidence=%u (want %u), query=%u (want %u)\n",
                label, input.evidence.invalid_fields, evidence_flags,
                input.query.invalid_fields, query_flags);
    /* A direct assertion on decoded uint64_t prevents a shared rounding bug
     * in input parsing and expected comparison from certifying itself. */
    if (strcmp(label, "uint64 exact") == 0)
        ok = ok && input.evidence.life_start == UINT64_MAX &&
             input.query.from == UINT64_MAX - 1 && input.query.to == UINT64_MAX;
    if (strcmp(label, "escaped events") == 0)
        ok = ok && input.evidence.waits[0].event.length == 3 &&
             memcmp(input.evidence.waits[0].event.data, "A\0B", 3) == 0 &&
             input.query.event.length == 4 &&
             memcmp(input.query.event.data, "\xf0\x9f\x98\x80", 4) == 0;
    if (!ok) fprintf(stderr, "adapter %s failed\n", label);
    pgwt_contract_case_free(&input);
    return ok;
}

static bool adapter_checks(void)
{
    const char *valid_e = "{\"life\":[0,null],\"coverage\":[],\"waits\":[],\"open\":null}";
    const char *valid_q = "{\"event\":\"E\",\"from\":0,\"to\":10}";
    const char *bad_times[] = {"true", "false", "\"1\"", "1.0", "1e0", "-1",
                               "18446744073709551616", "null"};
    struct time_field { const char *format; unsigned flag; } fields[] = {
        {"{\"life\":[%s,null],\"coverage\":[],\"waits\":[],\"open\":null}", PGWT_CORE_LIFE_INVALID},
        {"{\"life\":[0,%s],\"coverage\":[],\"waits\":[],\"open\":null}", PGWT_CORE_LIFE_INVALID},
        {"{\"life\":[0,null],\"coverage\":[[%s,2]],\"waits\":[],\"open\":null}", PGWT_CORE_COVERAGE_INVALID},
        {"{\"life\":[0,null],\"coverage\":[[0,%s]],\"waits\":[],\"open\":null}", PGWT_CORE_COVERAGE_INVALID},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[[\"E\",%s,2]],\"open\":null}", PGWT_CORE_WAITS_INVALID},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[[\"E\",0,%s]],\"open\":null}", PGWT_CORE_WAITS_INVALID},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[],\"open\":[\"E\",%s,2]}", PGWT_CORE_OPEN_INVALID},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[],\"open\":[\"E\",0,%s]}", PGWT_CORE_OPEN_INVALID | PGWT_CORE_OPEN_AS_OF_INVALID}
    };
    char json[1024];
    for (size_t i = 0; i < sizeof(bad_times) / sizeof(bad_times[0]); ++i) {
        for (size_t j = 0; j < sizeof(fields) / sizeof(fields[0]); ++j) {
            snprintf(json, sizeof(json), fields[j].format, bad_times[i]);
            /* null is explicitly legal for life_end, and nowhere else. */
            unsigned flag = i == 7 && j == 1 ? 0 : fields[j].flag;
            if (!adapter_input("time token", json, valid_q, flag, 0)) return false;
        }
        snprintf(json, sizeof(json), "{\"event\":\"E\",\"from\":%s,\"to\":10}", bad_times[i]);
        if (!adapter_input("from token", valid_e, json, 0, PGWT_CORE_QUERY_FROM_INVALID)) return false;
        snprintf(json, sizeof(json), "{\"event\":\"E\",\"from\":0,\"to\":%s}", bad_times[i]);
        if (!adapter_input("to token", valid_e, json, 0, PGWT_CORE_QUERY_TO_INVALID)) return false;
    }
    struct shape_case { const char *evidence, *query; unsigned eflags, qflags; } cases[] = {
        {"{\"coverage\":[],\"waits\":[],\"open\":null}", valid_q, PGWT_CORE_LIFE_INVALID, 0},
        {"{\"life\":[0,null],\"waits\":[],\"open\":null}", valid_q, PGWT_CORE_COVERAGE_INVALID, 0},
        {"{\"life\":[0,null],\"coverage\":[],\"open\":null}", valid_q, PGWT_CORE_WAITS_INVALID, 0},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[]}", valid_q, PGWT_CORE_OPEN_INVALID | PGWT_CORE_OPEN_AS_OF_INVALID, 0},
        {valid_e, "{\"from\":0,\"to\":10}", 0, PGWT_CORE_QUERY_EVENT_INVALID},
        {valid_e, "{\"event\":\"E\",\"to\":10}", 0, PGWT_CORE_QUERY_FROM_INVALID},
        {valid_e, "{\"event\":\"E\",\"from\":0}", 0, PGWT_CORE_QUERY_TO_INVALID},
        {"{\"life\":[0],\"coverage\":[],\"waits\":[],\"open\":null}", valid_q, PGWT_CORE_LIFE_INVALID, 0},
        {"{\"life\":[0,null,1],\"coverage\":[],\"waits\":[],\"open\":null}", valid_q, PGWT_CORE_LIFE_INVALID, 0},
        {"{\"life\":[0,null],\"coverage\":[[0]],\"waits\":[],\"open\":null}", valid_q, PGWT_CORE_COVERAGE_INVALID, 0},
        {"{\"life\":[0,null],\"coverage\":[[0,1,2]],\"waits\":[],\"open\":null}", valid_q, PGWT_CORE_COVERAGE_INVALID, 0},
        {"{\"life\":[0,null],\"coverage\":{},\"waits\":[],\"open\":null}", valid_q, PGWT_CORE_COVERAGE_INVALID, 0},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":null,\"open\":null}", valid_q, PGWT_CORE_WAITS_INVALID, 0},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[[\"E\",0]],\"open\":null}", valid_q, PGWT_CORE_WAITS_INVALID, 0},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[[\"E\",0,1,2]],\"open\":null}", valid_q, PGWT_CORE_WAITS_INVALID, 0},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[[true,0,1]],\"open\":null}", valid_q, PGWT_CORE_WAITS_INVALID, 0},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[],\"open\":[\"E\",0]}", valid_q, PGWT_CORE_OPEN_INVALID | PGWT_CORE_OPEN_AS_OF_INVALID, 0},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[],\"open\":[\"E\",0,1,2]}", valid_q, PGWT_CORE_OPEN_INVALID | PGWT_CORE_OPEN_AS_OF_INVALID, 0},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[],\"open\":false}", valid_q, PGWT_CORE_OPEN_INVALID | PGWT_CORE_OPEN_AS_OF_INVALID, 0},
        {valid_e, "{\"event\":true,\"from\":0,\"to\":10}", 0, PGWT_CORE_QUERY_EVENT_INVALID},
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[],\"open\":null,\"extra\":false}",
         "{\"event\":\"E\",\"from\":0,\"to\":10,\"extra\":[1.0]}", 0, 0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        if (!adapter_input("required keys/list shape", cases[i].evidence, cases[i].query,
                           cases[i].eflags, cases[i].qflags)) return false;
    return adapter_input("uint64 exact",
                         "{\"life\":[18446744073709551615,null],\"coverage\":[],\"waits\":[],\"open\":null}",
                         "{\"event\":\"E\",\"from\":18446744073709551614,\"to\":18446744073709551615}", 0, 0) &&
           adapter_input("escaped events",
                         "{\"life\":[0,null],\"coverage\":[],\"waits\":[[\"A\\u0000B\",0,1]],\"open\":null}",
                         "{\"event\":\"\\ud83d\\ude00\",\"from\":0,\"to\":1}", 0, 0);
}

/* Preparation owns no input storage, handles large unordered evidence, and
 * supports repeated queries after its scratch has been released. */
static bool preparation_checks(void)
{
    const size_t count = 20000;
    pgwt_core_wait *waits = calloc(count, sizeof(*waits));
    pgwt_core_wait *scratch = calloc(count, sizeof(*scratch));
    if (!waits || !scratch) { free(waits); free(scratch); return false; }
    const pgwt_core_event event = {"A", 1};
    /* Reverse order, with a real zero-length close at every positive start. */
    for (size_t i = 0; i < count; ++i) {
        uint64_t start = (count - 1 - i) / 2 * 2;
        waits[i] = (pgwt_core_wait){event, start, start + (i % 2 == 0)};
    }
    pgwt_core_span span = {0, count};
    pgwt_core_evidence e = {.coverage = &span, .coverage_count = 1,
                            .waits = waits, .wait_count = count};
    pgwt_core_prepared p;
    bool ok = pgwt_core_prepare(&e, scratch, count - 1, &p) == -1 && !p.evidence;
    ok = ok && pgwt_core_prepare(&e, NULL, count, &p) == -1 && !p.evidence;
    ok = ok && pgwt_core_prepare(&e, scratch, count, &p) == 0 && p.evidence_valid;
    free(scratch);
    pgwt_core_result t, n;
    pgwt_core_query q = {.event = event, .from = 0, .to = count};
    pgwt_core_wait_metrics(&p, &q, &t, &n);
    pgwt_core_result again_t, again_n;
    pgwt_core_wait_metrics(&p, &q, &again_t, &again_n);
    ok = ok && memcmp(&t, &again_t, sizeof(t)) == 0 &&
         memcmp(&n, &again_n, sizeof(n)) == 0;
    for (size_t i = 0; i < count; ++i)
        ok = ok && waits[i].start == (count - 1 - i) / 2 * 2 &&
             waits[i].end == waits[i].start + (i % 2 == 0);
    free(waits);
    ok = ok && pgwt_core_prepare(NULL, NULL, 0, &p) == 0;
    pgwt_core_wait_metrics(&p, &q, &t, &n);
    ok = ok && !t.value.present && t.reason_count == 1 &&
         strcmp(t.reasons[0], "evidence_invalid") == 0;
    if (!ok) fprintf(stderr, "prepared evidence checks failed\n");
    return ok;
}

static bool rejected_prepared(const char *name, const pgwt_core_prepared *p,
                              const pgwt_core_query *q)
{
    pgwt_core_result t, n;
    pgwt_core_wait_metrics(p, q, &t, &n);
    const pgwt_core_result *results[] = {&t, &n};
    bool ok = true;
    for (size_t i = 0; i < 2; ++i) {
        const pgwt_core_result *r = results[i];
        ok = ok && !r->value.present && !r->detail_present &&
             strcmp(r->completeness, "unavailable") == 0 &&
             r->reason_count == 1 && strcmp(r->reasons[0], "evidence_invalid") == 0 &&
             !r->diagnostic.recorded.present && !r->diagnostic.covered_ns.present &&
             !r->diagnostic.required_ns.present;
    }
    printf("prepared %s %s\n", name, ok ? "PASS" : "MISMATCH (expected evidence_invalid)");
    return ok;
}

/* These public-API misuse regressions are separate from the frozen arithmetic
 * answers. Every rejection must cover both metrics and all diagnostics. */
static bool prepared_integrity_checks(void)
{
    char event_bytes[] = "A";
    const pgwt_core_event event = {event_bytes, 1};
    const pgwt_core_query q = {.event = {"A", 1}, .to = UINT64_MAX};
    pgwt_core_span coverage[] = {{0, UINT64_MAX}}, other_coverage[] = {{0, UINT64_MAX}};
    pgwt_core_wait waits[] = {{event, 0, UINT64_MAX}, {event, 0, UINT64_MAX}};
    pgwt_core_wait other_waits[] = {{event, 0, UINT64_MAX}};
    pgwt_core_wait scratch[2];
    pgwt_core_evidence e = {.coverage = coverage, .coverage_count = 1,
                            .waits = waits, .wait_count = 2};
    /* The original forged handle produced the wrapped sum UINT64_MAX - 1. */
    pgwt_core_prepared p = {.evidence = &e, .coverage_knowable = true, .evidence_valid = true};
    bool ok = rejected_prepared("forged_fields", &p, &q);
    e.wait_count = 1;
    if (pgwt_core_prepare(&e, scratch, 2, &p)) return false;
    pgwt_core_prepared copied;
    memcpy(&copied, &p, sizeof(p));
    ok = rejected_prepared("copied_handle", &copied, &q) && ok;

    /* Move the end while retaining readable storage; overlap would overflow
     * if the cached validation were accepted after a count/array change. */
    waits[0].end = UINT64_MAX - 1;
    ok = rejected_prepared("changed_wait_end", &p, &q) && ok;
    waits[0].end = UINT64_MAX;
    e.wait_count = 2;
    ok = rejected_prepared("increased_wait_count", &p, &q) && ok;
    e.wait_count = 1;
    e.waits = other_waits;
    ok = rejected_prepared("swapped_wait_array", &p, &q) && ok;
    e.waits = waits;
    e.coverage = other_coverage;
    ok = rejected_prepared("swapped_coverage_array", &p, &q) && ok;
    e.coverage = coverage;
    event_bytes[0] = 'B';
    ok = rejected_prepared("changed_event_bytes", &p, &q) && ok;
    event_bytes[0] = 'A';
    pgwt_core_result t, n;
    pgwt_core_wait_metrics(&p, &q, &t, &n);
    bool restored = t.value.present && t.value.value == UINT64_MAX &&
                    n.value.present && n.value.value == 0;
    printf("prepared restored_evidence %s\n", restored ? "PASS" : "MISMATCH (unchanged result)");
    return ok && restored;
}

/* Exit 1 means completed case comparisons found a mismatch. Exit 2 means a
 * harness failure (including too few cases), never a killed numerical mutant. */
int main(int argc, char **argv)
{
    const char *root = "contracts/wait_time-v1/contracts";
    if (argc > 2) { fprintf(stderr, "usage: %s [contract-directory]\n", argv[0]); return 2; }
    if (!adapter_checks() || !preparation_checks()) return 2;
    bool integrity_ok = prepared_integrity_checks();
    if (argc == 2) root = argv[1];
    else if (access(root, R_OK) != 0) root = "../contracts/wait_time-v1/contracts";
    char pattern[4096];
    int n = snprintf(pattern, sizeof(pattern), "%s/examples/*.json", root);
    if (n < 0 || (size_t)n >= sizeof(pattern)) return 2;
    glob_t paths = {0};
    int glob_status = glob(pattern, 0, NULL, &paths);
    if (glob_status != 0 && glob_status != GLOB_NOMATCH) {
        fprintf(stderr, "cannot enumerate contract cases\n");
        globfree(&paths);
        return 2;
    }
    size_t passed = 0, mismatches = 0;
    bool harness_error = paths.gl_pathc < 48;
    if (harness_error) fprintf(stderr, "need at least 48 cases, found %zu\n", paths.gl_pathc);
    for (size_t i = 0; i < paths.gl_pathc; ++i) {
        const char *path = paths.gl_pathv[i];
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;
        char expected[4096], error[512] = {0};
        n = snprintf(expected, sizeof(expected), "%s/expected/%s", root, name);
        if (n < 0 || (size_t)n >= sizeof(expected)) {
            printf("%s ERROR (path too long)\n", name); harness_error = true; continue;
        }
        pgwt_contract_case input;
        if (pgwt_contract_case_load(path, &input, error, sizeof(error)) != 0) {
            printf("%s ERROR (%s)\n", name, error); harness_error = true; continue;
        }
        pgwt_core_result time, completed;
        pgwt_core_prepared prepared;
        size_t count = input.evidence.wait_count;
        pgwt_core_wait *scratch = count ? calloc(count, sizeof(*scratch)) : NULL;
        if ((count && !scratch) ||
            pgwt_core_prepare(&input.evidence, scratch, count, &prepared) != 0) {
            free(scratch);
            pgwt_contract_case_free(&input);
            harness_error = true;
            printf("%s ERROR (prepare failed)\n", name);
            continue;
        }
        free(scratch); /* prepared borrows evidence only, not scratch. */
        pgwt_core_wait_metrics(&prepared, &input.query, &time, &completed);
        char time_json[2048], count_json[2048];
        int status;
        if (pgwt_contract_result_json(&time, time_json, sizeof(time_json)) != 0 ||
            pgwt_contract_result_json(&completed, count_json, sizeof(count_json)) != 0) {
            status = -1;
            snprintf(error, sizeof(error), "result serialization failed");
        } else {
            status = pgwt_contract_compare_results(time_json, count_json, expected,
                                                   error, sizeof(error));
        }
        pgwt_contract_case_free(&input);
        if (status == 0) { ++passed; printf("%s PASS\n", name); }
        else if (status == 1) { ++mismatches; printf("%s MISMATCH (%s)\n", name, error); }
        else { harness_error = true; printf("%s ERROR (%s)\n", name, error); }
    }
    globfree(&paths);
    printf("core wait_time contract: %zu/48 passed\n", passed);
    return harness_error ? 2 : mismatches || !integrity_ok ? 1 : 0;
}
