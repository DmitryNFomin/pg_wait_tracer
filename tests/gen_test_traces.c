/* gen_test_traces.c — Generate synthetic trace files for correctness testing
 *
 * Reads a scenario from a JSON file (or command-line args) and writes
 * .trace + .summary files plus backends.jsonl + query_texts.jsonl.
 *
 * Usage:
 *   gen_test_traces -o <output-dir> -s <scenario.json>
 *   gen_test_traces -o <output-dir> --inline '<json-array>'
 *
 * Scenario JSON format:
 * {
 *   "backends": [{"pid": 1000, "type": "client", "user": "postgres", "db": "testdb"}],
 *   "queries":  [{"id": 12345, "text": "SELECT pg_sleep(1)"}],
 *   "events": [
 *     {"pid": 1000, "ts": 1000000000, "dur": 500000, "old": 167772181, "new": 0, "qid": 12345}
 *   ],
 *   "sample_period_ns": 100000000,
 *   "samples": [
 *     {"pid": 1000, "ts": 1000000000, "event": 167772181, "qid": 12345}
 *   ]
 * }
 *
 * "events"  → written as a TRANSITIONS block (trace format v2). The existing
 *             columnar layout: ts, pid, old, new, dur, qid.
 * "samples" → written as a SAMPLES block (trace format v2). Reduced layout:
 *             ts, pid, event, qid — no old/new/dur. "event" is the single
 *             sampled wait_event_info; "sample_period_ns" (top-level, default
 *             100000000 = 10 Hz) is stored in the block header. Samples must
 *             be sorted by ts ascending. A scenario may have events, samples,
 *             or both. Both arrays produce A3 fixtures.
 * "interleave": 1 (top-level, optional) → events and samples are written in
 *             global timestamp order, so the file's block structure matches
 *             a real tiered capture: a transitions block is flushed whenever
 *             the sample stream catches up, sample blocks land between
 *             transition blocks. Without it, all events are written first,
 *             then all samples (one big transitions block — NOT what the
 *             tiered daemon produces).
 *
 * Extra options (T1 fidelity tests):
 *   --wall-offset <ns>  patch headers so mono→wall offset = <ns> instead of 0
 *                       (simulates an NTP step between file opens — FID-7).
 *   --rotate <name>     after writing, rename current.trace →
 *                       <name>.trace.lz4 and current.summary →
 *                       <name>.summary.lz4 (footers are present because the
 *                       writers are closed first) and remove the .meta files.
 *                       Lets a test build a multi-file trace dir by invoking
 *                       the generator several times with the same -o dir.
 *
 * Event IDs: old_event/new_event/event are uint32 wait_event_info values.
 *   CPU = 0, IO:DataFileRead = 0x0A000001, Lock:relation = 0x03000001, etc.
 *   Class byte << 24 | event_number.
 *
 * Build: cc -O2 -I../src -I../include -o gen_test_traces gen_test_traces.c \
 *        ../build/server_event_writer.o ../build/server_summary_writer.o \
 *        ../build/server_wait_event.o -llz4
 */
#include "event_writer.h"
#include "summary_writer.h"
#include "pg_wait_tracer.h"
#include "query_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <getopt.h>
#include <dirent.h>

#define MAX_EVENTS       100000
#define MAX_TEST_BACKENDS 64
#define MAX_QUERIES       64

struct test_backend {
    uint32_t pid;
    uint32_t leader_pid;
    char     type[32];
    char     user[64];
    char     db[64];
};

struct test_query {
    uint64_t id;
    char     text[256];
    char     trace_id[36];
    char     parent_span_id[20];
};

struct scenario {
    struct pgwt_trace_event events[MAX_EVENTS];
    int num_events;

    /* SAMPLES block (trace format v2). The sampled event id is stored in
     * each record's new_event field (what the writer's sample encoder reads). */
    struct pgwt_trace_event samples[MAX_EVENTS];
    int num_samples;
    uint64_t sample_period_ns;
    int interleave;   /* write events+samples in global ts order */
    /* T8: emit trace format v3 with MEASURED cpu_ns per event (the per-event
     * "cpu" field). Default 0 = legacy: the writer stamps every record
     * PGWT_CPU_NS_UNKNOWN, so existing scenarios keep their gap-inference
     * behavior unchanged. Set "cpu_measured":1 (top-level) to freeze a v3
     * measured-CPU fixture. */
    int cpu_measured;

    struct test_backend backends[MAX_TEST_BACKENDS];
    int num_backends;

    struct test_query queries[MAX_QUERIES];
    int num_queries;
};

/* ── Minimal JSON parser (good enough for test scenarios) ─── */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *find_key(const char *json, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    p = skip_ws(p);
    if (*p != ':') return NULL;
    return skip_ws(p + 1);
}

static int parse_string(const char **pp, char *out, size_t out_size)
{
    const char *p = skip_ws(*pp);
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p+1)) { p++; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    if (*p == '"') p++;
    *pp = p;
    return 0;
}

/* Find matching bracket/brace end */
static const char *find_matching(const char *p, char open, char close)
{
    int depth = 0;
    bool in_string = false;
    for (; *p; p++) {
        if (*p == '"' && *(p-1) != '\\') in_string = !in_string;
        if (in_string) continue;
        if (*p == open) depth++;
        if (*p == close) { depth--; if (depth == 0) return p; }
    }
    return NULL;
}

static int parse_backends(const char *arr, struct scenario *sc)
{
    const char *p = skip_ws(arr);
    if (*p != '[') return -1;
    p++;

    while (*p) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;

        const char *end = find_matching(p, '{', '}');
        if (!end) break;

        /* Extract fields from this object */
        struct test_backend *b = &sc->backends[sc->num_backends];
        memset(b, 0, sizeof(*b));

        const char *v;
        if ((v = find_key(p, "pid"))) { b->pid = (uint32_t)strtoull(v, NULL, 10); }
        if ((v = find_key(p, "leader_pid")) && v < end) {
            b->leader_pid = (uint32_t)strtoull(v, NULL, 10);
        }
        if ((v = find_key(p, "type"))) { parse_string(&v, b->type, sizeof(b->type)); }
        if ((v = find_key(p, "user"))) { parse_string(&v, b->user, sizeof(b->user)); }
        if ((v = find_key(p, "db")))   { parse_string(&v, b->db, sizeof(b->db)); }

        sc->num_backends++;
        p = end + 1;
    }
    return 0;
}

static int parse_queries(const char *arr, struct scenario *sc)
{
    const char *p = skip_ws(arr);
    if (*p != '[') return -1;
    p++;

    while (*p) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;

        const char *end = find_matching(p, '{', '}');
        if (!end) break;

        struct test_query *q = &sc->queries[sc->num_queries];
        memset(q, 0, sizeof(*q));

        const char *v;
        if ((v = find_key(p, "id"))) { q->id = strtoull(v, NULL, 10); }
        if ((v = find_key(p, "text"))) { parse_string(&v, q->text, sizeof(q->text)); }
        if ((v = find_key(p, "trace_id")) && v < end) { parse_string(&v, q->trace_id, sizeof(q->trace_id)); }
        if ((v = find_key(p, "parent_span_id")) && v < end) { parse_string(&v, q->parent_span_id, sizeof(q->parent_span_id)); }

        sc->num_queries++;
        p = end + 1;
    }
    return 0;
}

static int parse_events(const char *arr, struct scenario *sc)
{
    const char *p = skip_ws(arr);
    if (*p != '[') return -1;
    p++;

    while (*p && sc->num_events < MAX_EVENTS) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;

        const char *end = find_matching(p, '{', '}');
        if (!end) break;

        struct pgwt_trace_event *ev = &sc->events[sc->num_events];
        memset(ev, 0, sizeof(*ev));

        const char *v;
        if ((v = find_key(p, "pid")))  ev->pid = (uint32_t)strtoull(v, NULL, 10);
        if ((v = find_key(p, "ts")))   ev->timestamp_ns = strtoull(v, NULL, 10);
        if ((v = find_key(p, "dur")))  ev->duration_ns = strtoull(v, NULL, 10);
        if ((v = find_key(p, "old")))  ev->old_event = (uint32_t)strtoull(v, NULL, 10);
        if ((v = find_key(p, "new")))  ev->new_event = (uint32_t)strtoull(v, NULL, 10);
        if ((v = find_key(p, "qid")))  ev->query_id = strtoull(v, NULL, 10);
        /* T8: measured on-CPU ns for the interval (only meaningful with
         * top-level "cpu_measured":1). Absent → 0; for we==0 CPU gaps that
         * makes CPU*=0/OffCPU*=full, so a measured scenario should set it. */
        v = find_key(p, "cpu");
        ev->cpu_ns = (v && v < end) ? strtoull(v, NULL, 10)
                                    : PGWT_CPU_NS_UNKNOWN;

        sc->num_events++;
        p = end + 1;
    }
    return 0;
}

static int parse_samples(const char *arr, struct scenario *sc)
{
    const char *p = skip_ws(arr);
    if (*p != '[') return -1;
    p++;

    while (*p && sc->num_samples < MAX_EVENTS) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;

        const char *end = find_matching(p, '{', '}');
        if (!end) break;

        struct pgwt_trace_event *ev = &sc->samples[sc->num_samples];
        memset(ev, 0, sizeof(*ev));

        const char *v;
        if ((v = find_key(p, "pid")))   ev->pid = (uint32_t)strtoull(v, NULL, 10);
        if ((v = find_key(p, "ts")))    ev->timestamp_ns = strtoull(v, NULL, 10);
        /* sampled event id → new_event (the field the sample encoder reads) */
        if ((v = find_key(p, "event"))) ev->new_event = (uint32_t)strtoull(v, NULL, 10);
        if ((v = find_key(p, "qid")))   ev->query_id = strtoull(v, NULL, 10);

        sc->num_samples++;
        p = end + 1;
    }
    return 0;
}

static int parse_scenario(const char *json, struct scenario *sc)
{
    const char *v;

    sc->sample_period_ns = 100000000ULL;  /* default 10 Hz */

    if ((v = find_key(json, "backends")))
        parse_backends(v, sc);
    if ((v = find_key(json, "queries")))
        parse_queries(v, sc);
    if ((v = find_key(json, "events")))
        parse_events(v, sc);
    if ((v = find_key(json, "sample_period_ns")))
        sc->sample_period_ns = strtoull(v, NULL, 10);
    if ((v = find_key(json, "samples")))
        parse_samples(v, sc);
    if ((v = find_key(json, "interleave")))
        sc->interleave = (int)strtol(v, NULL, 10);
    if ((v = find_key(json, "cpu_measured")))
        sc->cpu_measured = (int)strtol(v, NULL, 10);

    return 0;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* ── Output generation ─────────────────────────────────────── */

static int write_backends_jsonl(const char *dir, struct scenario *sc)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/backends.jsonl", dir);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }

    for (int i = 0; i < sc->num_backends; i++) {
        struct test_backend *b = &sc->backends[i];
        if (b->user[0] && b->db[0]) {
            fprintf(f, "{\"pid\":%u,\"type\":\"%s\",\"user\":\"%s\",\"db\":\"%s\",\"addr\":\"127.0.0.1(5432)\"",
                    b->pid, b->type, b->user, b->db);
        } else {
            fprintf(f, "{\"pid\":%u,\"type\":\"%s\"", b->pid, b->type);
        }
        if (b->leader_pid > 0)
            fprintf(f, ",\"leader_pid\":%u", b->leader_pid);
        fprintf(f, "}\n");
    }

    fclose(f);
    return 0;
}

static int write_query_texts(const char *dir, struct scenario *sc)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/query_texts.jsonl", dir);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }

    for (int i = 0; i < sc->num_queries; i++) {
        struct test_query *q = &sc->queries[i];
        if (q->trace_id[0] == '\0') {
            pgwt_parse_traceparent(q->text, q->trace_id, sizeof(q->trace_id),
                                   q->parent_span_id, sizeof(q->parent_span_id));
        }
        fprintf(f, "{\"q\":%llu,\"t\":\"%s\",\"ts\":%llu",
                (unsigned long long)q->id, q->text,
                (unsigned long long)1000000000ULL);
        if (q->trace_id[0] != '\0') {
            fprintf(f, ",\"trace_id\":\"%s\",\"parent_span_id\":\"%s\"",
                    q->trace_id, q->parent_span_id);
        }
        fprintf(f, "}\n");
    }

    fclose(f);
    return 0;
}

/* Earliest timestamp in the scenario — the synthetic "mono" clock anchor. */
static uint64_t scenario_first_ts(const struct scenario *sc)
{
    uint64_t first = UINT64_MAX;
    for (int i = 0; i < sc->num_events; i++)
        if (sc->events[i].timestamp_ns < first)
            first = sc->events[i].timestamp_ns;
    for (int i = 0; i < sc->num_samples; i++)
        if (sc->samples[i].timestamp_ns < first)
            first = sc->samples[i].timestamp_ns;
    return first == UINT64_MAX ? 0 : first;
}

/* Re-anchor the (lazily opened) summary writer's clocks to the synthetic
 * timestamp domain, so summary block wall timestamps come out as
 * scenario_ts + wall_offset — coherent with the patched trace headers.
 * Must run after the first push (which opens the file) and before the first
 * second-boundary flush. Without this, summary blocks carry wall values
 * derived from the real machine clocks and the summary path is untestable
 * with synthetic data. */
static void anchor_summary_clock(struct pgwt_summary_writer *sw,
                                 uint64_t anchor_mono, uint64_t wall_offset_ns)
{
    if (!sw->fp)
        return;
    sw->clock_offset_mono_ns = anchor_mono;
    sw->clock_offset_wall_ns = anchor_mono + wall_offset_ns;
    /* Fix up the accumulator second opened by the first push. */
    if (sw->accum_active)
        sw->accum.second_wall_ns = sw->accum.second_mono_ns + wall_offset_ns;
}

static int write_traces(const char *dir, struct scenario *sc,
                        uint64_t wall_offset_ns)
{
    struct pgwt_event_writer ew;
    struct pgwt_summary_writer sw;
    uint64_t anchor_mono = scenario_first_ts(sc);
    anchor_mono = (anchor_mono / 1000000000ULL) * 1000000000ULL;
    int summary_anchored = 0;

    if (pgwt_writer_init(&ew, dir, 18, 24, NULL) != 0) {
        fprintf(stderr, "Failed to init event writer\n");
        return -1;
    }
    /* T8: measured-CPU scenarios keep their per-event cpu_ns; the default
     * (legacy) path leaves cpu_measured=false so every record is stamped
     * UNKNOWN and existing fixtures behave exactly as before. */
    ew.cpu_measured = sc->cpu_measured;

    if (pgwt_summary_writer_init(&sw, dir, 24, NULL) != 0) {
        fprintf(stderr, "Failed to init summary writer\n");
        pgwt_writer_destroy(&ew);
        return -1;
    }
    #define PUSH_SUMMARY(evp) do { \
        pgwt_summary_push_event(&sw, (evp)); \
        if (!summary_anchored) { \
            anchor_summary_clock(&sw, anchor_mono, wall_offset_ns); \
            summary_anchored = 1; \
        } \
    } while (0)

    if (sc->interleave) {
        /* Realistic tiered block structure: walk events and samples in
         * global timestamp order. Contiguous runs of samples are pushed as
         * SAMPLES blocks; pgwt_writer_push_samples flushes any buffered
         * transitions first, so transition blocks are split exactly where
         * the sample stream interleaves — like the live daemon. */
        int ei = 0, si = 0;
        while (ei < sc->num_events || si < sc->num_samples) {
            if (si >= sc->num_samples ||
                (ei < sc->num_events &&
                 sc->events[ei].timestamp_ns <= sc->samples[si].timestamp_ns)) {
                if (pgwt_writer_push_event(&ew, &sc->events[ei]) != 0) {
                    fprintf(stderr, "Failed to write event %d\n", ei);
                    break;
                }
                PUSH_SUMMARY(&sc->events[ei]);
                ei++;
            } else {
                /* Collect the contiguous run of samples before the next event */
                int run = si;
                while (run < sc->num_samples &&
                       (ei >= sc->num_events ||
                        sc->samples[run].timestamp_ns < sc->events[ei].timestamp_ns))
                    run++;
                if (pgwt_writer_push_samples(&ew, &sc->samples[si], run - si,
                                             sc->sample_period_ns) != 0)
                    fprintf(stderr, "Failed to write samples %d..%d\n", si, run);
                si = run;
            }
        }
    } else {
        /* TRANSITIONS block(s): the "events" array. Also feeds the summary
         * writer (summaries are transition-based; sampled summaries are A3). */
        for (int i = 0; i < sc->num_events; i++) {
            if (pgwt_writer_push_event(&ew, &sc->events[i]) != 0) {
                fprintf(stderr, "Failed to write event %d\n", i);
                break;
            }
            PUSH_SUMMARY(&sc->events[i]);
        }

        /* SAMPLES block: the "samples" array (trace format v2). Written after
         * the transitions so a scenario with both produces a transition block
         * followed by a sample block. */
        if (sc->num_samples > 0) {
            if (pgwt_writer_push_samples(&ew, sc->samples, sc->num_samples,
                                         sc->sample_period_ns) != 0)
                fprintf(stderr, "Failed to write %d samples\n", sc->num_samples);
        }
    }

    /* Flush summary */
    pgwt_summary_flush(&sw);

    pgwt_writer_close(&ew);
    pgwt_summary_close(&sw);
    pgwt_writer_destroy(&ew);
    pgwt_summary_destroy(&sw);

    fprintf(stderr, "Wrote %d events + %d samples to %s\n",
            sc->num_events, sc->num_samples, dir);
    return 0;
}

/* ── Patch file headers so mono_to_wall = wall_offset ─────── */

static int patch_file_header(const char *path, uint64_t anchor_mono,
                             uint64_t wall_offset_ns)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return -1;

    struct pgwt_trace_file_header hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    /* Anchor the header clocks to the synthetic timestamp domain:
     * clock_offset_ns (mono at open) = first scenario timestamp,
     * start_time_ns (wall at open)   = same + wall_offset,
     * so mono_to_wall = wall_offset (0 by default: wall == mono and test
     * scenarios can use known values). A non-zero offset simulates an NTP
     * step between file opens (FID-7). */
    hdr.clock_offset_ns = anchor_mono;
    hdr.start_time_ns = anchor_mono + wall_offset_ns;

    fseek(f, 0, SEEK_SET);
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

static int patch_trace_headers(const char *dir, uint64_t anchor_mono,
                               uint64_t wall_offset_ns)
{
    DIR *d = opendir(dir);
    if (!d) return -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Only the files this run just wrote (current.*): repeated runs into
         * one dir must not re-patch already-rotated files. */
        if (strcmp(ent->d_name, "current.trace") == 0 ||
            strcmp(ent->d_name, "current.summary") == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            if (patch_file_header(path, anchor_mono, wall_offset_ns) != 0) {
                fprintf(stderr, "WARN: failed to patch header of %s\n", ent->d_name);
            }
        }
    }

    closedir(d);
    return 0;
}

/* ── --rotate: turn current.* into archived hourly files ──── */

static int rotate_output(const char *dir, const char *name)
{
    char oldp[600], newp[700];
    int rc = 0;

    snprintf(oldp, sizeof(oldp), "%s/current.trace", dir);
    snprintf(newp, sizeof(newp), "%s/%s.trace.lz4", dir, name);
    if (rename(oldp, newp) != 0) { perror(newp); rc = -1; }
    snprintf(oldp, sizeof(oldp), "%s/current.trace.meta", dir);
    unlink(oldp);

    snprintf(oldp, sizeof(oldp), "%s/current.summary", dir);
    snprintf(newp, sizeof(newp), "%s/%s.summary.lz4", dir, name);
    rename(oldp, newp);   /* summary may not exist (samples-only scenario) */
    snprintf(oldp, sizeof(oldp), "%s/current.summary.meta", dir);
    unlink(oldp);

    return rc;
}

/* ── Main ──────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s -o <output-dir> -s <scenario.json>\n", prog);
    fprintf(stderr, "       %s -o <output-dir> --inline '<json>'\n", prog);
}

int main(int argc, char **argv)
{
    const char *outdir = NULL;
    const char *scenario_file = NULL;
    const char *inline_json = NULL;
    const char *rotate_name = NULL;
    uint64_t wall_offset_ns = 0;

    static struct option long_opts[] = {
        {"output",      required_argument, NULL, 'o'},
        {"scenario",    required_argument, NULL, 's'},
        {"inline",      required_argument, NULL, 'i'},
        {"wall-offset", required_argument, NULL, 'w'},
        {"rotate",      required_argument, NULL, 'r'},
        {"help",        no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "o:s:i:w:r:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'o': outdir = optarg; break;
        case 's': scenario_file = optarg; break;
        case 'i': inline_json = optarg; break;
        case 'w': wall_offset_ns = strtoull(optarg, NULL, 10); break;
        case 'r': rotate_name = optarg; break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!outdir || (!scenario_file && !inline_json)) {
        usage(argv[0]);
        return 1;
    }

    /* Create output directory */
    mkdir(outdir, 0755);

    /* Parse scenario */
    struct scenario *sc = calloc(1, sizeof(*sc));
    if (!sc) { perror("calloc"); return 1; }

    char *json;
    if (scenario_file) {
        json = read_file(scenario_file);
        if (!json) return 1;
    } else {
        json = strdup(inline_json);
    }

    if (parse_scenario(json, sc) != 0) {
        fprintf(stderr, "Failed to parse scenario\n");
        free(json);
        free(sc);
        return 1;
    }
    free(json);

    fprintf(stderr, "Scenario: %d events, %d samples, %d backends, %d queries\n",
            sc->num_events, sc->num_samples, sc->num_backends, sc->num_queries);

    /* Write output files */
    int rc = 0;
    if (write_backends_jsonl(outdir, sc) != 0) rc = 1;
    if (write_query_texts(outdir, sc) != 0) rc = 1;
    if (write_traces(outdir, sc, wall_offset_ns) != 0) rc = 1;

    /* Patch file headers so mono_to_wall = wall_offset (default 0: wall
     * timestamps == mono timestamps). Synthetic events use known monotonic
     * timestamps; --wall-offset simulates an NTP step between files (FID-7). */
    {
        uint64_t anchor = scenario_first_ts(sc);
        anchor = (anchor / 1000000000ULL) * 1000000000ULL;
        patch_trace_headers(outdir, anchor, wall_offset_ns);
    }

    /* Optionally rotate this run's output aside so the next run can add
     * another file to the same trace dir. */
    if (rotate_name && rotate_output(outdir, rotate_name) != 0)
        rc = 1;

    free(sc);
    return rc;
}
