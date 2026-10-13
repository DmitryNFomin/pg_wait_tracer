/* wait_cpu_scan.c — per-class wait CPU over a FULL raw walk of a trace dir.
 *
 * Criteria item 2.5 (docs/DEMO_REHEARSAL_CRITERIA.md §3) asks for per-class
 * wait cpu_ns on the pure-sleep classes, because over-attribution is the one
 * direction the DB-Time identity cannot see. The earlier attempt went through
 * the `execution_detail` API and could not answer it: that route needs an
 * execution start to anchor on, caps a lane at 2000 events, and reports
 * unknown CPU as null — on the retained Oct-2 capture that left 55 truncated
 * lanes, 1,018,050 omitted records and every Lock:relation event
 * unrepresented.
 *
 * struct pgwt_trace_event carries cpu_ns on EVERY transition record, so this
 * tool walks the trace files record by record instead: no anchor, no cap, no
 * nulls from truncation.
 *
 * It is a measurement tool, not a registered rehearsal gate — but it is still
 * a check, so it refuses rather than approves whenever it cannot see:
 *
 *   exit 0  every requested pure-sleep class measured, coverage verified,
 *           nothing at or above the threshold
 *   exit 2  REFUSED: coverage not established (no trace files, a block that
 *           failed to decode, fewer records than the block headers promise,
 *           a record census that disagrees with the server's own total, an
 *           unresolvable wait-event name, no measured wait records at all,
 *           or no external total supplied to check against)
 *   exit 3  CROSSING: a pure-sleep record carries cpu_ns >= threshold
 *   exit 4  UNMEASURED: coverage fine, but a requested pure-sleep class has
 *           no events, or none with measured CPU. Unmeasured is never clean.
 *
 * Usage:
 *   wait_cpu_scan --trace-dir DIR --threshold-ns N
 *                 --expect-transitions N --expect-samples N
 *                 [--pure-sleep "Timeout:PgSleep,Lock:relation,Lock:advisory"]
 *                 [--output FILE] [--max-crossings N]
 *                 [--no-expect-check]   (always refuses; for the bypass tests)
 */
#include "event_reader.h"
#include "wait_event.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILES    64
#define MAX_KEYS     512
#define NAME_LEN     64

struct ident {
    uint32_t pid;
    uint64_t ts_wall_ns;
    uint64_t ts_mono_ns;
    uint64_t duration_ns;
    uint64_t cpu_ns;
    int64_t  query_id;
    int      file_idx;
    int      block;
    int      record;
    char     name[NAME_LEN];
};

struct key_stats {
    uint32_t event_id;
    char     name[NAME_LEN];
    int      pure_sleep;
    uint64_t count;          /* records seen with this old_event */
    uint64_t measured;       /* cpu_ns != PGWT_CPU_NS_UNKNOWN */
    uint64_t unknown;        /* cpu_ns == PGWT_CPU_NS_UNKNOWN */
    uint64_t zero_dur;       /* duration_ns == 0 (ratio undefined) */
    uint64_t sum_dur;
    uint64_t sum_cpu;        /* measured records only */
    double   sum_ratio;      /* Σ per-event cpu/dur, measured & dur>0 */
    uint64_t ratio_n;
    uint64_t over_threshold;
    struct ident max_cpu;
    struct ident max_ratio;
    double   max_ratio_val;
    uint64_t *cpu;           /* measured cpu_ns samples, for percentiles */
    size_t   n_cpu, cap_cpu;
};

static struct key_stats keys[MAX_KEYS];
static int num_keys;

static struct key_stats *key_for(uint32_t id)
{
    for (int i = 0; i < num_keys; i++)
        if (keys[i].event_id == id) return &keys[i];
    if (num_keys >= MAX_KEYS) return NULL;
    struct key_stats *k = &keys[num_keys++];
    memset(k, 0, sizeof(*k));
    k->event_id = id;
    pgwt_event_full_name(id, k->name, sizeof(k->name));
    return k;
}

static int push_cpu(struct key_stats *k, uint64_t v)
{
    if (k->n_cpu == k->cap_cpu) {
        size_t cap = k->cap_cpu ? k->cap_cpu * 2 : 64;
        uint64_t *p = realloc(k->cpu, cap * sizeof(*p));
        if (!p) return -1;
        k->cpu = p;
        k->cap_cpu = cap;
    }
    k->cpu[k->n_cpu++] = v;
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentile over the sorted samples (p in [0,100]). */
static uint64_t pct(const uint64_t *sorted, size_t n, double p)
{
    if (!n) return 0;
    size_t idx = (size_t)((p / 100.0) * (double)(n - 1) + 0.5);
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

static int name_in_list(const char *list, const char *name)
{
    size_t len = strlen(name);
    for (const char *p = list; p && *p;) {
        const char *comma = strchr(p, ',');
        size_t seg = comma ? (size_t)(comma - p) : strlen(p);
        if (seg == len && strncmp(p, name, len) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* An event id whose NAME this build cannot resolve must never be silently
 * bucketed away: a pure-sleep class we cannot name looks like a class that is
 * absent, and absent reads as clean. Both forms pgwt_event_full_name() emits
 * for an unresolved id are caught here. */
static int unresolved_name(const char *name)
{
    return strstr(name, ":id=") != NULL || strncmp(name, "Unknown", 7) == 0;
}

static void json_ident(FILE *f, const char *tag, const struct ident *id)
{
    fprintf(f, "\"%s\":{\"name\":\"%s\",\"pid\":%u,\"ts_wall_ns\":%" PRIu64
               ",\"ts_mono_ns\":%" PRIu64 ",\"duration_ns\":%" PRIu64
               ",\"cpu_ns\":%" PRIu64 ",\"query_id\":%" PRId64
               ",\"file_idx\":%d,\"block\":%d,\"record\":%d}",
            tag, id->name, id->pid, id->ts_wall_ns, id->ts_mono_ns,
            id->duration_ns, id->cpu_ns, id->query_id,
            id->file_idx, id->block, id->record);
}

int main(int argc, char **argv)
{
    const char *trace_dir = NULL, *out_path = NULL;
    const char *pure_sleep = "Timeout:PgSleep,Lock:relation,Lock:advisory";
    uint64_t threshold = 0;
    long long expect_transitions = -1, expect_samples = -1;
    int no_expect_check = 0, max_crossings = 64;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace-dir") && i + 1 < argc) trace_dir = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--pure-sleep") && i + 1 < argc) pure_sleep = argv[++i];
        else if (!strcmp(argv[i], "--threshold-ns") && i + 1 < argc)
            threshold = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--expect-transitions") && i + 1 < argc)
            expect_transitions = strtoll(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--expect-samples") && i + 1 < argc)
            expect_samples = strtoll(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--max-crossings") && i + 1 < argc)
            max_crossings = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-expect-check")) no_expect_check = 1;
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }
    if (!trace_dir || !threshold) {
        fprintf(stderr, "usage: %s --trace-dir DIR --threshold-ns N "
                        "--expect-transitions N --expect-samples N\n", argv[0]);
        return 2;
    }

    /* Refusal reasons, accumulated; any one of them forbids a clean verdict. */
    char refusals[16][128];
    int num_refusals = 0;
#define REFUSE(...) do { \
        if (num_refusals < 16) \
            snprintf(refusals[num_refusals++], 128, __VA_ARGS__); \
    } while (0)

    struct pgwt_trace_file_entry files[MAX_FILES];
    int num_files = pgwt_scan_trace_files(trace_dir, files, MAX_FILES);
    if (num_files < 0) {
        num_files = 0;
        REFUSE("trace_dir_unreadable");
    } else if (num_files == 0) {
        REFUSE("no_trace_files");
    }

    /* Name tables BEFORE any lookup, from the sidecar when present and the
     * header major otherwise — in the other order a PG13 trace is classified
     * with PG18's Timeout ids and the class names silently shift. */
    int header_major = 0;
    for (int fi = 0; fi < num_files; fi++) {
        struct pgwt_event_reader probe;
        if (pgwt_reader_open(&probe, files[fi].path) == 0) {
            header_major = (int)probe.header.pg_version;
            pgwt_reader_close(&probe);
            break;
        }
    }
    int names_major = pgwt_init_event_names_for_trace(trace_dir, header_major);

    uint64_t records_walked = 0, header_records = 0;
    uint64_t markers = 0, samples = 0, cpu_gaps = 0, wait_records = 0;
    uint64_t cpu_gap_unknown = 0, unresolved_records = 0;
    int blocks_total = 0, blocks_decoded = 0, blocks_failed = 0;
    int files_opened = 0, files_failed = 0;
    struct ident crossings[64];
    int num_crossings = 0;
    if (max_crossings > 64) max_crossings = 64;

    for (int fi = 0; fi < num_files; fi++) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, files[fi].path) != 0) {
            files_failed++;
            REFUSE("trace_file_unreadable:%s", files[fi].path);
            continue;
        }
        files_opened++;
        for (int b = 0; b < r.num_blocks; b++) {
            blocks_total++;
            struct pgwt_block_info info;
            if (pgwt_reader_block_info(&r, b, &info) != 0) {
                blocks_failed++;
                REFUSE("block_header_unreadable:file%d:block%d", fi, b);
                continue;
            }
            header_records += info.num_events;
            struct pgwt_trace_event *ev =
                calloc(info.num_events ? info.num_events : 1, sizeof(*ev));
            if (!ev) {
                blocks_failed++;
                REFUSE("alloc_failed:file%d:block%d", fi, b);
                continue;
            }
            int n = pgwt_reader_decode_block_info(&r, b, ev,
                                                 (int)info.num_events, &info);
            if (n < 0) {
                blocks_failed++;
                REFUSE("block_decode_failed:file%d:block%d", fi, b);
                free(ev);
                continue;
            }
            blocks_decoded++;
            if ((uint32_t)n != info.num_events)
                REFUSE("block_short_decode:file%d:block%d:%d_of_%u",
                       fi, b, n, info.num_events);
            records_walked += (uint64_t)n;

            for (int i = 0; i < n; i++) {
                const struct pgwt_trace_event *e = &ev[i];
                if (info.block_type == PGWT_BLOCK_SAMPLES ||
                    (e->flags & PGWT_EVENT_FLAG_SAMPLE)) {
                    samples++;              /* point observation, no cpu_ns */
                    continue;
                }
                uint32_t we = e->old_event;
                if (PGWT_IS_MARKER(we)) { markers++; continue; }
                if (we == 0) {
                    cpu_gaps++;
                    if (e->cpu_ns == PGWT_CPU_NS_UNKNOWN) cpu_gap_unknown++;
                    continue;
                }
                wait_records++;
                struct key_stats *k = key_for(we);
                if (!k) { REFUSE("key_table_full"); continue; }
                if (unresolved_name(k->name)) unresolved_records++;
                k->count++;
                k->sum_dur += e->duration_ns;
                if (e->duration_ns == 0) k->zero_dur++;
                if (e->cpu_ns == PGWT_CPU_NS_UNKNOWN) { k->unknown++; continue; }
                k->measured++;
                k->sum_cpu += e->cpu_ns;
                if (push_cpu(k, e->cpu_ns) != 0) REFUSE("alloc_failed:cpu_samples");

                struct ident id = {
                    .pid = e->pid,
                    .ts_wall_ns = pgwt_reader_mono_to_wall(&r, e->timestamp_ns),
                    .ts_mono_ns = e->timestamp_ns,
                    .duration_ns = e->duration_ns,
                    .cpu_ns = e->cpu_ns,
                    .query_id = (int64_t)e->query_id,
                    .file_idx = fi, .block = b, .record = i,
                };
                snprintf(id.name, sizeof(id.name), "%s", k->name);

                if (k->measured == 1 || e->cpu_ns > k->max_cpu.cpu_ns)
                    k->max_cpu = id;
                if (e->duration_ns) {
                    double ratio = (double)e->cpu_ns / (double)e->duration_ns;
                    k->sum_ratio += ratio;
                    k->ratio_n++;
                    if (k->ratio_n == 1 || ratio > k->max_ratio_val) {
                        k->max_ratio_val = ratio;
                        k->max_ratio = id;
                    }
                }
                if (e->cpu_ns >= threshold) {
                    k->over_threshold++;
                    if (num_crossings < max_crossings) crossings[num_crossings++] = id;
                }
            }
            free(ev);
        }
        pgwt_reader_close(&r);
    }

    for (int i = 0; i < num_keys; i++)
        if (keys[i].n_cpu) qsort(keys[i].cpu, keys[i].n_cpu,
                                 sizeof(*keys[i].cpu), cmp_u64);

    /* Mark the requested pure-sleep set; a requested name with no key at all
     * still has to appear in the output as UNMEASURED. */
    for (int i = 0; i < num_keys; i++)
        keys[i].pure_sleep = name_in_list(pure_sleep, keys[i].name);

    char unmeasured[16][NAME_LEN];
    int num_unmeasured = 0;
    for (const char *p = pure_sleep; p && *p;) {
        const char *comma = strchr(p, ',');
        size_t seg = comma ? (size_t)(comma - p) : strlen(p);
        char want[NAME_LEN];
        snprintf(want, sizeof(want), "%.*s", (int)seg, p);
        struct key_stats *found = NULL;
        for (int i = 0; i < num_keys; i++)
            if (!strcmp(keys[i].name, want)) { found = &keys[i]; break; }
        if ((!found || found->measured == 0) && num_unmeasured < 16)
            snprintf(unmeasured[num_unmeasured++], NAME_LEN, "%s", want);
        if (!comma) break;
        p = comma + 1;
    }

    /* Coverage: the record census has to agree with an INDEPENDENT total, and
     * "no total was supplied" is a refusal, not a pass — otherwise a missing
     * or exit-127 `pgwt-server --dump` turns the completeness check off and
     * the scan still reports clean. */
    uint64_t transitions_total = wait_records + cpu_gaps;
    if (no_expect_check) {
        REFUSE("coverage_unverified:expect_check_disabled");
    } else {
        if (expect_transitions < 0) REFUSE("coverage_unverified:no_expect_transitions");
        else if ((uint64_t)expect_transitions != transitions_total + markers)
            REFUSE("transition_census_mismatch:walked=%" PRIu64 ":expected=%lld",
                   transitions_total + markers, expect_transitions);
        if (expect_samples < 0) REFUSE("coverage_unverified:no_expect_samples");
        else if ((uint64_t)expect_samples != samples)
            REFUSE("sample_census_mismatch:walked=%" PRIu64 ":expected=%lld",
                   samples, expect_samples);
    }
    if (records_walked == 0) REFUSE("no_records_walked");
    if (records_walked != header_records)
        REFUSE("record_shortfall:walked=%" PRIu64 ":headers=%" PRIu64,
               records_walked, header_records);
    if (wait_records == 0) REFUSE("no_wait_records");
    if (unresolved_records)
        REFUSE("unresolved_event_names:%" PRIu64, unresolved_records);

    uint64_t total_crossings = 0;
    uint64_t sleep_crossings = 0;
    for (int i = 0; i < num_keys; i++) {
        total_crossings += keys[i].over_threshold;
        if (keys[i].pure_sleep) sleep_crossings += keys[i].over_threshold;
    }

    const char *verdict;
    int rc;
    if (num_refusals) { verdict = "REFUSED"; rc = 2; }
    else if (sleep_crossings) { verdict = "CROSSING"; rc = 3; }
    else if (num_unmeasured) { verdict = "UNMEASURED"; rc = 4; }
    else { verdict = "CLEAN"; rc = 0; }

    FILE *f = stdout;
    if (out_path) {
        f = fopen(out_path, "w");
        if (!f) { fprintf(stderr, "cannot write %s\n", out_path); return 2; }
    }
    fprintf(f, "{\n  \"verdict\":\"%s\",\n  \"exit_code\":%d,\n", verdict, rc);
    fprintf(f, "  \"trace_dir\":\"%s\",\n  \"threshold_ns\":%" PRIu64 ",\n",
            trace_dir, threshold);
    fprintf(f, "  \"pure_sleep_requested\":\"%s\",\n", pure_sleep);
    fprintf(f, "  \"names_major\":%d,\n  \"header_major\":%d,\n",
            names_major, header_major);
    fprintf(f, "  \"coverage\":{\"files_found\":%d,\"files_opened\":%d,"
               "\"files_failed\":%d,\"blocks_total\":%d,\"blocks_decoded\":%d,"
               "\"blocks_failed\":%d,\"records_walked\":%" PRIu64 ","
               "\"header_records\":%" PRIu64 ",\"transitions\":%" PRIu64 ","
               "\"markers\":%" PRIu64 ",\"samples\":%" PRIu64 ","
               "\"wait_records\":%" PRIu64 ",\"cpu_gap_records\":%" PRIu64 ","
               "\"cpu_gap_cpu_unknown\":%" PRIu64 ","
               "\"unresolved_records\":%" PRIu64 ","
               "\"expect_transitions\":%lld,\"expect_samples\":%lld},\n",
            num_files, files_opened, files_failed, blocks_total, blocks_decoded,
            blocks_failed, records_walked, header_records,
            transitions_total + markers, markers, samples, wait_records,
            cpu_gaps, cpu_gap_unknown, unresolved_records,
            expect_transitions, expect_samples);

    fprintf(f, "  \"refusals\":[");
    for (int i = 0; i < num_refusals; i++)
        fprintf(f, "%s\"%s\"", i ? "," : "", refusals[i]);
    fprintf(f, "],\n");

    fprintf(f, "  \"unmeasured_pure_sleep_classes\":[");
    for (int i = 0; i < num_unmeasured; i++)
        fprintf(f, "%s\"%s\"", i ? "," : "", unmeasured[i]);
    fprintf(f, "],\n");

    fprintf(f, "  \"pure_sleep_crossings\":%" PRIu64 ",\n", sleep_crossings);
    fprintf(f, "  \"all_class_crossings\":%" PRIu64 ",\n", total_crossings);
    fprintf(f, "  \"crossing_events\":[");
    for (int i = 0; i < num_crossings; i++) {
        fprintf(f, "%s{", i ? "," : "");
        json_ident(f, "event", &crossings[i]);
        fprintf(f, "}");
    }
    fprintf(f, "],\n");

    fprintf(f, "  \"classes\":[\n");
    for (int i = 0; i < num_keys; i++) {
        struct key_stats *k = &keys[i];
        double mean_cpu = k->measured ? (double)k->sum_cpu / (double)k->measured : 0;
        double mean_ratio = k->ratio_n ? k->sum_ratio / (double)k->ratio_n : 0;
        double agg_ratio = k->sum_dur ? (double)k->sum_cpu / (double)k->sum_dur : 0;
        fprintf(f, "    {\"name\":\"%s\",\"event_id\":%u,\"pure_sleep\":%s,"
                   "\"count\":%" PRIu64 ",\"measured\":%" PRIu64 ","
                   "\"cpu_unknown\":%" PRIu64 ",\"zero_duration\":%" PRIu64 ","
                   "\"sum_duration_ns\":%" PRIu64 ",\"sum_cpu_ns\":%" PRIu64 ","
                   "\"mean_cpu_ns\":%.3f,\"max_cpu_ns\":%" PRIu64 ","
                   "\"median_cpu_ns\":%" PRIu64 ",\"p95_cpu_ns\":%" PRIu64 ","
                   "\"p99_cpu_ns\":%" PRIu64 ",\"mean_cpu_over_dur\":%.9f,"
                   "\"max_cpu_over_dur\":%.9f,\"agg_cpu_over_dur\":%.9f,"
                   "\"over_threshold\":%" PRIu64 ",",
                k->name, k->event_id, k->pure_sleep ? "true" : "false",
                k->count, k->measured, k->unknown, k->zero_dur,
                k->sum_dur, k->sum_cpu, mean_cpu,
                k->measured ? k->max_cpu.cpu_ns : 0,
                pct(k->cpu, k->n_cpu, 50), pct(k->cpu, k->n_cpu, 95),
                pct(k->cpu, k->n_cpu, 99), mean_ratio,
                k->ratio_n ? k->max_ratio_val : 0.0, agg_ratio,
                k->over_threshold);
        json_ident(f, "max_cpu_event", &k->max_cpu);
        fprintf(f, ",");
        json_ident(f, "max_ratio_event", &k->max_ratio);
        fprintf(f, "}%s\n", i + 1 < num_keys ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    if (out_path) fclose(f);

    fprintf(stderr, "wait_cpu_scan: %s (exit %d) — records=%" PRIu64
                    " waits=%" PRIu64 " blocks=%d/%d refusals=%d"
                    " sleep_crossings=%" PRIu64 " unmeasured=%d\n",
            verdict, rc, records_walked, wait_records, blocks_decoded,
            blocks_total, num_refusals, sleep_crossings, num_unmeasured);
    for (int i = 0; i < num_keys; i++) free(keys[i].cpu);
    return rc;
}
