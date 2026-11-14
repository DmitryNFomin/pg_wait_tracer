#define _POSIX_C_SOURCE 200809L
#include "evidence_json.h"
#include <glob.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* This driver never loads product-generated expectations. The Python companion
 * supplies admitted logical arrays from the frozen redelivery catalog, retaining
 * an external map to physical positions. The library reports input indices. */
static void event_json(pgwt_core_event e)
{
    putchar('"');
    for (size_t i = 0; i < e.length; ++i) {
        unsigned char c = (unsigned char)e.data[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 32) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void nullable(bool known, uint64_t value)
{
    if (known) printf("%" PRIu64, value); else printf("null");
}

static void counters(const pgwt_evidence_counters *c)
{
    if (!c->known) { printf("null"); return; }
    putchar('[');
    for (size_t i = 0; i < 15; ++i) printf("%s%" PRIu64, i ? "," : "", c->value[i]);
    putchar(']');
}

static void frontiers(const uint64_t *a, size_t n)
{
    putchar('[');
    for (size_t i = 0; i < n; ++i) printf("%s%" PRIu64, i ? "," : "", a[i]);
    putchar(']');
}

static int json_cases(int argc, char **argv)
{
    for (int i = 2; i < argc; ++i) {
        evidence_case input;
        char error[512];
        if (evidence_case_load(argv[i], &input, error, sizeof(error))) {
            fprintf(stderr, "%s: %s\n", argv[i], error); return 2;
        }
        pgwt_evidence_output out;
        pgwt_evidence_report report;
        const char *reason = NULL;
        pgwt_replay_status status = pgwt_evidence_replay_report(&input.input, &input.query, &out, &report, &reason);
        if (status == PGWT_REPLAY_ERROR) {
            fprintf(stderr, "%s: %s\n", argv[i], reason ? reason : "replay error");
            evidence_case_free(&input); return 2;
        }
        printf("{\"status\":%d,\"result\":", status);
        if (status == PGWT_REPLAY_OK) {
            if (evidence_output_json(stdout, &out)) return 2;
        } else printf("null");
        printf(",\"captures\":[");
        for (size_t k = 0; k < report.capture_count; ++k) {
            const pgwt_evidence_capture_report *r = &report.captures[k];
            printf("%s{\"boot\":", k ? "," : ""); event_json(r->boot);
            printf(",\"capture\":"); event_json(r->capture);
            printf(",\"frontier\":"); nullable(r->has_frontier, r->frontier);
            printf(",\"high_water\":"); nullable(r->has_high_water, r->high_water);
            printf(",\"baseline\":"); counters(&r->baseline);
            printf(",\"frontiers\":"); frontiers(r->frontiers, r->frontier_count);
            printf(",\"revoked\":"); frontiers(r->revoked_frontiers, r->revoked_frontier_count);
            printf(",\"lifecycle_uncertainty\":"); nullable(r->has_lifecycle_uncertainty, r->lifecycle_uncertainty);
            printf(",\"stop\":");
            if (!r->stop.stopped) printf("null");
            else {
                const pgwt_evidence_stop *s = &r->stop;
                printf("{\"file_index\":%zu,\"record_index\":", s->file_index);
                if (s->descriptor) printf("null"); else printf("%zu", s->record_index);
                printf(",\"descriptor\":%s,\"reason\":\"%s\"", s->descriptor ? "true" : "false",
                       pgwt_evidence_stop_label(s->reason));
                if (s->reason == PGWT_REPLAY_STOP_BASELINE_MISMATCH) {
                    printf(",\"expected\":"); counters(&s->expected_baseline);
                    printf(",\"declared\":"); counters(&s->declared_baseline);
                }
                putchar('}');
            }
            putchar('}');
        }
        puts("]}");
        pgwt_evidence_report_free(&report);
        pgwt_evidence_output_free(&out);
        evidence_case_free(&input);
    }
    return 0;
}

/* First pass meets malformed record at joined index 4. At EOF of that prefix,
 * the second pass resolves the overlapping attachment at joined index 3. The
 * final cutoff is file 2 / record 0, AFTER its descriptor pseudo-record. Capture
 * x is independent of a, and must remain un-stopped with F=H=10. */
static int two_faults(void)
{
    pgwt_evidence_record independent[] = {
        {.kind=PGWT_EVIDENCE_LIFE, .pid=1, .has_fork=true, .start_known=true},
        {.kind=PGWT_EVIDENCE_ATTACH_OK, .pid=1, .has_fork=true, .reread=true, .counters={.known=true}},
        {.kind=PGWT_EVIDENCE_FRONTIER, .t=10, .counters={.known=true}}
    };
    pgwt_evidence_record suffix[] = {independent[1], {.kind=PGWT_EVIDENCE_OTHER}};
    suffix[0].t=1;
    pgwt_evidence_file files[] = {
        {.version=5, .boot={"b",1}, .capture={"a",1}, .present=true, .qualified=true,
         .baseline={.known=true}, .records=independent, .record_count=2},
        {.version=5, .boot={"b",1}, .capture={"x",1}, .present=true, .qualified=true,
         .baseline={.known=true}, .records=independent, .record_count=3},
        {.version=5, .boot={"b",1}, .capture={"a",1}, .present=true, .qualified=true,
         .file_no=1, .has_previous=true, .baseline={.known=true}, .records=suffix, .record_count=2}
    };
    pgwt_evidence_file swap=files[0]; files[0]=files[1]; files[1]=swap;
    pgwt_evidence_input in = {.files=files, .file_count=3, .capture={"x",1}, .pid=1, .has_fork=true};
    pgwt_core_query query = {.event={"wait",4}, .from=0, .to=10};
    pgwt_evidence_output out;
    pgwt_evidence_report report;
    pgwt_replay_status status=pgwt_evidence_replay_report(&in, &query, &out, &report, NULL);
    bool pass=status==PGWT_REPLAY_OK && report.capture_count==2;
    if (pass) {
        const pgwt_evidence_capture_report *a=&report.captures[1], *x=&report.captures[0];
        bool final_cutoff=a->stop.stopped && !a->stop.descriptor &&
            a->stop.file_index==2 && a->stop.record_index==0 &&
            a->stop.reason==PGWT_REPLAY_STOP_OVERLAPPING_ATTACHMENT;
        bool capture_stop_owner=!x->stop.stopped && a->stop.stopped;
        bool prefix_state=!a->has_frontier && !a->has_high_water && a->baseline.known &&
            !a->has_lifecycle_uncertainty && x->has_frontier && x->frontier==10 &&
            x->has_high_water && x->high_water==10 && out.projection.coverage_count==1 &&
            out.coverage.start==0 && out.coverage.end==10;
        if (!final_cutoff) puts("report two_faults FAIL final_cutoff");
        if (!capture_stop_owner) puts("report two_faults FAIL capture_stop_owner");
        if (!prefix_state) puts("report two_faults FAIL prefix_state");
        pass=final_cutoff && capture_stop_owner && prefix_state;
    }
    pgwt_evidence_report_free(&report);
    pgwt_evidence_output_free(&out);
    printf("report two_faults %s final=file[2].record[0] first=file[2].record[1]; independent x F=H=10\n", pass ? "PASS" : "FAIL");
    return !pass;
}

static int api_errors(void)
{
    pgwt_evidence_output out;
    pgwt_evidence_report report;
    const char *why=NULL;
    memset(&out, 0x7f, sizeof(out));
    memset(&report, 0x7f, sizeof(report));
    bool pass=pgwt_evidence_replay_report(NULL, NULL, &out, &report, &why)==PGWT_REPLAY_ERROR &&
        !report.capture_count && !report.captures && !out.waits && !out.wait_time.metric;
    pgwt_evidence_report_free(&report);
    pgwt_evidence_output_free(&out);
    pass &= pgwt_evidence_replay_report(NULL, NULL, &out, NULL, &why)==PGWT_REPLAY_ERROR &&
        why && !out.wait_time.metric;
    pass &= pgwt_evidence_replay_report(NULL, NULL, NULL, &report, &why)==PGWT_REPLAY_ERROR &&
        why && !report.capture_count;
    pgwt_evidence_report_free(&report);
    pgwt_evidence_report_free(&report);
    pgwt_evidence_report_free(NULL);
    printf("report api_errors %s\n", pass ? "PASS" : "FAIL");
    return !pass;
}

static int r3_metrics(const char *root)
{
    char pattern[4096], expected[4096], projections[4096];
    if (snprintf(pattern, sizeof(pattern), "%s/r3/scenarios/*.json", root)>=(int)sizeof(pattern) ||
        snprintf(expected, sizeof(expected), "%s/r3/expected.json", root)>=(int)sizeof(expected) ||
        snprintf(projections, sizeof(projections), "%s/r3/projections.json", root)>=(int)sizeof(projections)) return 2;
    glob_t paths={0};
    if (glob(pattern, 0, NULL, &paths) || paths.gl_pathc!=31) { globfree(&paths); return 2; }
    int failed=0;
    size_t matched=0;
    for (size_t i=0; i<paths.gl_pathc; ++i) {
        evidence_case input;
        char field[512];
        if (evidence_case_load(paths.gl_pathv[i], &input, field, sizeof(field))) { globfree(&paths); return 2; }
        bool pass=true;
        for (int api=0; api<2; ++api) {
            pgwt_evidence_output out;
            pgwt_evidence_report report={0};
            const char *why=NULL;
            pgwt_replay_status status=api ? pgwt_evidence_replay_report(&input.input, &input.query, &out, &report, &why) :
                pgwt_evidence_replay(&input.input, &input.query, &out, &why);
            if (status!=PGWT_REPLAY_OK) { fprintf(stderr, "%s ERROR %s\n", input.id, why ? why : "status"); return 2; }
            int compared=evidence_compare(&input, &out, expected, projections, field, sizeof(field));
            if (compared) fprintf(stderr, "report r3 %s %s API: %s\n", input.id, api ? "report" : "old", field);
            pass &= compared==0;
            pgwt_evidence_report_free(&report);
            pgwt_evidence_output_free(&out);
            if (compared<0) return 2;
        }
        if (pass) ++matched; else failed=1;
        evidence_case_free(&input);
    }
    globfree(&paths);
    printf("report r3 metrics unchanged: %zu/31 scenarios, both APIs (124 metric objects)\n", matched);
    return failed;
}

int main(int argc, char **argv)
{
    if (argc>=3 && !strcmp(argv[1], "--json")) return json_cases(argc, argv);
    if (argc==2 && !strcmp(argv[1], "--two-faults")) return two_faults();
    if (argc>2) return 2;
    const char *root=argc==2 ? argv[1] : "contracts/evidence-v1";
    if (argc==1 && access(root, R_OK)) root="../contracts/evidence-v1";
    int native=two_faults() | api_errors();
    int r3=r3_metrics(root);
    return r3==2 ? 2 : (r3 | native);
}
