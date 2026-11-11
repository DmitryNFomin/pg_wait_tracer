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
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[]}", valid_q, PGWT_CORE_OPEN_INVALID, 0},
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
        {"{\"life\":[0,null],\"coverage\":[],\"waits\":[],\"open\":[\"E\",0,1,2]}", valid_q, PGWT_CORE_OPEN_INVALID, 0},
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

/* Exit 1 means completed case comparisons found a mismatch. Exit 2 means a
 * harness failure (including too few cases), never a killed numerical mutant. */
int main(int argc, char **argv)
{
    const char *root = "contracts/wait_time-v1/contracts";
    if (argc > 2) { fprintf(stderr, "usage: %s [contract-directory]\n", argv[0]); return 2; }
    if (!adapter_checks()) return 2;
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
        pgwt_core_wait_metrics(&input.evidence, &input.query, &time, &completed);
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
    return harness_error ? 2 : mismatches ? 1 : 0;
}
