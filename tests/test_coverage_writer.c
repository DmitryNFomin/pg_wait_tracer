/* test_coverage_writer.c — #947 coverage label U1: trace v6 COVERAGE records.
 *
 * Spec r2 §1 and §5 U1 (Refs #947, #871). BPF-free and daemon-free: the
 * record codec (src/coverage_record.h), the writer API
 * (pgwt_writer_coverage_*), the master-path tracker and floor fold
 * (src/open_snapshot.c, the pure half) and the v4-era data paths of the
 * reader, all against real files in a temporary directory.
 *
 *   golden_bytes        payload bytes and CRC32C against an independent
 *                       encoder (Python, values pasted below), and every
 *                       decode refusal
 *   boot_per_file       BOOT is block 0 of every file; file_no runs on
 *                       across rotation; one nonce per writer session
 *   w_lags_one_tick     WATERMARK(t) carries publish_from of the PREVIOUS
 *                       good tick, never the current one
 *   heartbeat_on_yield  a forced drain yield repeats the last W
 *   counter_gaps        a counter change is a closed gap from the last W;
 *                       unknown counters are one open gap until known again
 *   floors              floor_db / floor_idle fold (C1)
 *   seal_only_clean     SEAL after a clean close; none after rotation or a
 *                       simulated SIGKILL; recovery keeps every type-3 block
 *   v4_paths_skip       decode / candidacy / file span / replay give the
 *                       same answers with and without COVERAGE blocks
 *   idle_publish        a tick with nothing appended adds no data block
 *   version_gate        v6 read, v5 refused
 *
 * Built with -DPGWT_SERVER against the server objects; runs on a Mac. */
#include "event_writer.h"
#include "event_reader.h"
#include "coverage_record.h"
#include "open_snapshot.h"
#include "map_reader.h"
#include "trace_closure.h"
#include "pg_wait_tracer.h"
#include "summary_reader.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

/* compute.c (replay_accum's link closure) names the summary stream; this
 * raw-only test never reaches it. */
int pgwt_visit_summaries(const char *dir, uint64_t from, uint64_t to,
                         pgwt_summary_visitor visitor, void *ctx)
{
    (void)dir; (void)from; (void)to; (void)visitor; (void)ctx;
    return -1;
}

static int failures;
static int checks;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("  FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define MS 1000000ULL
#define SEC 1000000000ULL
/* Records live 100 s after the test starts, on the real monotonic clock, so
 * a file's span (which starts at its header's mono anchor) ends at its data
 * and a COVERAGE block that leaked into it would show. */
static uint64_t base_ns;
#define BASE base_ns

static void rm_rf(const char *dir)
{
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* best effort */ }
}

static int make_dir(char *tmpl)
{
    return mkdtemp(tmpl) ? 0 : -1;
}

/* Every COVERAGE record of one file, in file order. */
struct covlist {
    struct pgwt_coverage_record rec[512];
    uint64_t obs[512];
    int block[512];
    int n;
    int bad;            /* -2 results */
    int data_blocks;
    int total_blocks;
};

static int read_cov(const char *path, struct covlist *out)
{
    memset(out, 0, sizeof(*out));
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, path) != 0)
        return -1;
    out->total_blocks = r.num_blocks;
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_coverage_record rec;
        uint64_t obs = 0;
        int rc = pgwt_reader_coverage_record(&r, b, &rec, &obs);
        if (rc == 1) {
            out->data_blocks++;
            continue;
        }
        if (rc == -2) {
            out->bad++;
            continue;
        }
        if (rc != 0 || out->n >= 512)
            continue;
        out->rec[out->n] = rec;
        out->obs[out->n] = obs;
        out->block[out->n] = b;
        out->n++;
    }
    pgwt_reader_close(&r);
    return 0;
}

static int count_kind(const struct covlist *c, int kind)
{
    int n = 0;
    for (int i = 0; i < c->n; i++)
        if (c->rec[i].kind == kind) n++;
    return n;
}

/* The archive a rotation or recovery produced (the only *.trace.lz4). */
static int find_archive(const char *dir, char *out, size_t out_sz, int nth)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    char names[16][300];
    int n = 0;
    while ((e = readdir(d)) && n < 16) {
        size_t l = strlen(e->d_name);
        if (l > 10 && strcmp(e->d_name + l - 10, ".trace.lz4") == 0)
            snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
    }
    closedir(d);
    /* name order = creation order for these tests (".N." suffixes) */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strlen(names[j]) < strlen(names[j - 1]); j--) {
            char t[300];
            memcpy(t, names[j], sizeof(t));
            memcpy(names[j], names[j - 1], sizeof(t));
            memcpy(names[j - 1], t, sizeof(t));
        }
    if (nth >= n) return -1;
    snprintf(out, out_sz, "%s/%s", dir, names[nth]);
    return 0;
}

static struct pgwt_trace_event tev(uint32_t pid, uint64_t ts, uint64_t dur,
                                   uint32_t old_e, uint32_t new_e)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid = pid;
    e.old_event = old_e;
    e.new_event = new_e;
    e.duration_ns = dur;
    e.query_id = 77;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

#define IO_READ  0x0A000015u   /* IO:DataFileRead */
#define LOCK_REL 0x03000000u   /* Lock:relation */
#define CLIENT_READ 0x06000000u /* Client:ClientRead (class 0x06, id 0) */
#define ACTIVITY 0x05000000u   /* Activity class */

static struct pgwt_counter_vector cv_with(uint64_t drop)
{
    struct pgwt_counter_vector v;
    memset(&v, 0, sizeof(v));
    v.valid = true;
    for (int i = 0; i < PGWT_COUNTER_COMPONENTS; i++)
        v.components[i] = (struct pgwt_counter_value){0, true};
    v.components[PGWT_C_EVENT_RING_DROP].value = drop;
    return v;
}

static struct pgwt_cov_tick_in tick_in(uint64_t pf, bool fence_reached,
                                       const struct pgwt_counter_vector *cv,
                                       uint64_t fd, uint64_t fi)
{
    struct pgwt_cov_tick_in in;
    memset(&in, 0, sizeof(in));
    in.publish_from_ns = pf;
    in.observation_ns = pf + 2 * MS;
    in.floors_known = true;
    in.floor_db_ns = fd;
    in.floor_idle_ns = fi;
    in.positions_known = true;
    in.producer_pos = pf;
    in.consumer_pos = fence_reached ? pf : 0;
    in.publish_ok = true;
    in.cv = cv;
    return in;
}

/* ── golden bytes ─────────────────────────────────────────────────────── */

static void test_golden_bytes(void)
{
    CHECK(pgwt_crc32c("123456789", 9) == 0xE3069283u,
          "CRC32C check value: got 0x%08x", pgwt_crc32c("123456789", 9));

    /* Independent encoder: struct.pack('<IHBBQQQQ', ...) + crc32c (bitwise,
     * reflected 0x82F63B78) + u32 0, run in Python. */
    static const uint8_t wm_gold[48] = {
        0x50, 0x47, 0x57, 0x43, 0x01, 0x00, 0x02, 0x00, 0x08, 0x07, 0x06, 0x05,
        0x04, 0x03, 0x02, 0x01, 0x00, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x00, 0x00, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xe1, 0x2f, 0xa0, 0x68, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t boot_gold[80] = {
        0x50, 0x47, 0x57, 0x43, 0x01, 0x00, 0x01, 0x00, 0x00, 0xca, 0x9a, 0x3b,
        0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xc0, 0x66, 0x15, 0xe8, 0x35, 0x79, 0x18, 0x02, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xa0, 0xa1, 0xa2, 0xa3,
        0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
        0x96, 0xcb, 0xa3, 0x86, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t gap_gold[48] = {
        0x50, 0x47, 0x57, 0x43, 0x01, 0x00, 0x03, 0x02, 0x15, 0xcd, 0x5b, 0x07,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb1, 0x68, 0xde, 0x3a,
        0x00, 0x00, 0x00, 0x00, 0x08, 0x32, 0x2d, 0x58, 0x00, 0x00, 0x00, 0x00,
    };
    uint8_t buf[PGWT_COVERAGE_BOOT_PAYLOAD_SIZE];

    struct pgwt_coverage_record wm = {
        .kind = PGWT_COVK_WATERMARK,
        .a = 0x0102030405060708ULL, .b = 0x0102030405060700ULL,
        .c = 0x0102030405060000ULL,
    };
    size_t n = pgwt_coverage_encode(&wm, buf);
    CHECK(n == 48 && memcmp(buf, wm_gold, 48) == 0,
          "WATERMARK golden bytes (size %zu)", n);

    struct pgwt_coverage_record boot = {
        .kind = PGWT_COVK_BOOT, .a = 1000000000ULL, .b = 7,
        .c = 1763500000000000000ULL, .d = 0x102,
    };
    for (int i = 0; i < 16; i++) {
        boot.boot_nonce[i] = (uint8_t)i;
        boot.kernel_boot_id[i] = (uint8_t)(0xa0 + i);
    }
    n = pgwt_coverage_encode(&boot, buf);
    CHECK(n == 80 && memcmp(buf, boot_gold, 80) == 0,
          "BOOT golden bytes (size %zu)", n);

    struct pgwt_coverage_record gap = {
        .kind = PGWT_COVK_GAP_OPEN, .reason = PGWT_COVR_DROP,
        .a = 123456789, .c = 5, .d = 987654321,
    };
    n = pgwt_coverage_encode(&gap, buf);
    CHECK(n == 48 && memcmp(buf, gap_gold, 48) == 0,
          "GAP_OPEN golden bytes (size %zu)", n);

    /* Round trip, and every refusal. */
    struct pgwt_coverage_record back;
    CHECK(pgwt_coverage_decode(boot_gold, 80, &back) == PGWT_COVD_OK &&
          back.kind == PGWT_COVK_BOOT && back.b == 7 && back.d == 0x102 &&
          back.kernel_boot_id[15] == 0xaf, "BOOT decodes");
    uint8_t m[80];
    memcpy(m, wm_gold, 48);
    m[20] ^= 1;
    CHECK(pgwt_coverage_decode(m, 48, &back) == PGWT_COVD_BAD_CRC &&
          back.kind == 0, "one flipped payload bit is a CRC failure");
    memcpy(m, wm_gold, 48);
    m[0] = 'X';
    CHECK(pgwt_coverage_decode(m, 48, &back) == PGWT_COVD_BAD_MAGIC, "magic");
    memcpy(m, wm_gold, 48);
    m[4] = 2;
    CHECK(pgwt_coverage_decode(m, 48, &back) == PGWT_COVD_BAD_SCHEMA, "schema");
    memcpy(m, wm_gold, 48);
    m[6] = 9;
    CHECK(pgwt_coverage_decode(m, 48, &back) == PGWT_COVD_BAD_KIND, "kind");
    CHECK(pgwt_coverage_decode(wm_gold, 40, &back) == PGWT_COVD_SHORT, "short");
    CHECK(pgwt_coverage_decode(boot_gold, 48, &back) == PGWT_COVD_SHORT,
          "BOOT cut to 48 bytes");
    memcpy(m, wm_gold, 48);
    m[47] = 1;
    CHECK(pgwt_coverage_decode(m, 48, &back) == PGWT_COVD_BAD_PAD, "pad");
    /* CRC over the right range: bytes 0..39 only (not the CRC, not pad). */
    CHECK(pgwt_crc32c(wm_gold, 40) == 0x68a02fe1u, "CRC covers bytes 0-39");
}

/* ── BOOT per file, file_no across rotation ───────────────────────────── */

static void test_boot_per_file(void)
{
    char dir[] = "/tmp/pgwt_cov_boot_XXXXXX";
    if (make_dir(dir) != 0) { failures++; return; }
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 17, 24, NULL) == 0, "init");
    pgwt_writer_coverage_configure(&w, BASE, 1763500000000000000ULL, 0x2);
    struct pgwt_trace_event e = tev(10, BASE + 5 * MS, 1 * MS, IO_READ, 0);
    CHECK(pgwt_writer_push_event(&w, &e) == 0, "push 1");
    w.force_rotate = true;
    CHECK(pgwt_writer_check_rotation(&w) == 0, "rotation 1");
    /* A WATERMARK alone opens the next file, BOOT first. */
    CHECK(pgwt_writer_coverage_watermark(&w, BASE + 2 * SEC, BASE + SEC,
                                         BASE + SEC, BASE) == 0, "wm opens");
    e.timestamp_ns = BASE + 3 * SEC;
    CHECK(pgwt_writer_push_event(&w, &e) == 0, "push 2");
    w.force_rotate = true;
    CHECK(pgwt_writer_check_rotation(&w) == 0, "rotation 2");
    e.timestamp_ns = BASE + 5 * SEC;
    CHECK(pgwt_writer_push_event(&w, &e) == 0, "push 3");
    CHECK(pgwt_writer_close(&w) == 0, "close");
    pgwt_writer_destroy(&w);

    char p0[600], p1[600], p2[600];
    snprintf(p2, sizeof(p2), "%s/current.trace", dir);
    CHECK(find_archive(dir, p0, sizeof(p0), 0) == 0 &&
          find_archive(dir, p1, sizeof(p1), 1) == 0, "two archives");
    struct covlist c[3];
    const char *paths[3] = { p0, p1, p2 };
    for (int f = 0; f < 3; f++) {
        CHECK(read_cov(paths[f], &c[f]) == 0, "read file %d", f);
        CHECK(c[f].n >= 1 && c[f].block[0] == 0 &&
              c[f].rec[0].kind == PGWT_COVK_BOOT,
              "file %d: BOOT is block 0", f);
        CHECK(count_kind(&c[f], PGWT_COVK_BOOT) == 1,
              "file %d: exactly one BOOT", f);
        CHECK(c[f].rec[0].b == (uint64_t)f, "file %d: file_no %llu", f,
              (unsigned long long)c[f].rec[0].b);
        CHECK(c[f].rec[0].a == BASE && c[f].rec[0].c == 1763500000000000000ULL &&
              c[f].rec[0].d == 0x2, "file %d: BOOT carries the configured "
              "boot_start, postmaster start and mode", f);
        CHECK(memcmp(c[f].rec[0].boot_nonce, c[0].rec[0].boot_nonce, 16) == 0,
              "file %d: same session nonce", f);
        CHECK(c[f].bad == 0, "file %d: every record verifies", f);
    }
    static const uint8_t zero[16];
    CHECK(memcmp(c[0].rec[0].boot_nonce, zero, 16) != 0, "nonce is not zero");
    CHECK(count_kind(&c[1], PGWT_COVK_WATERMARK) == 1 &&
          c[1].rec[1].kind == PGWT_COVK_WATERMARK,
          "file 1: the WATERMARK follows its BOOT");
    rm_rf(dir);

    /* A second session in the same directory is a new boot: new nonce,
     * file_no from 0 again. */
    char dir2[] = "/tmp/pgwt_cov_boot2_XXXXXX";
    if (make_dir(dir2) != 0) { failures++; return; }
    uint8_t n1[16];
    for (int s = 0; s < 2; s++) {
        struct pgwt_event_writer w2;
        CHECK(pgwt_writer_init(&w2, dir2, 17, 24, NULL) == 0, "init %d", s);
        pgwt_writer_coverage_configure(&w2, BASE, 0, 0);
        CHECK(pgwt_writer_coverage_watermark(&w2, BASE, BASE, BASE, BASE) == 0,
              "wm %d", s);
        pgwt_writer_close(&w2);
        pgwt_writer_destroy(&w2);
        char p[600];
        snprintf(p, sizeof(p), "%s/current.trace", dir2);
        struct covlist cl;
        CHECK(read_cov(p, &cl) == 0 && cl.n == 2, "session %d records", s);
        CHECK(cl.rec[0].b == 0, "session %d starts at file_no 0", s);
        if (s == 0)
            memcpy(n1, cl.rec[0].boot_nonce, 16);
        else
            CHECK(memcmp(n1, cl.rec[0].boot_nonce, 16) != 0,
                  "a new session has a new nonce");
    }
    rm_rf(dir2);
}

/* ── the tracker: lag, heartbeat, gaps ────────────────────────────────── */

struct wm { uint64_t w, fd, fi, obs; };

static int wms_of(const char *dir, struct wm *out, int max,
                  struct covlist *all)
{
    char p[600];
    snprintf(p, sizeof(p), "%s/current.trace", dir);
    if (read_cov(p, all) != 0)
        return -1;
    int n = 0;
    for (int i = 0; i < all->n && n < max; i++)
        if (all->rec[i].kind == PGWT_COVK_WATERMARK)
            out[n++] = (struct wm){ all->rec[i].a, all->rec[i].b,
                                    all->rec[i].c, all->obs[i] };
    return n;
}

static void test_w_lags_one_tick_and_heartbeat(void)
{
    char dir[] = "/tmp/pgwt_cov_lag_XXXXXX";
    if (make_dir(dir) != 0) { failures++; return; }
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 17, 24, NULL) == 0, "init");
    pgwt_writer_coverage_configure(&w, BASE, 0, 0);
    struct pgwt_counter_vector cv = cv_with(0);
    struct pgwt_cov_tracker t;
    pgwt_cov_tracker_init(&t, BASE, true, &cv);

    struct pgwt_cov_tick_in in;
    in = tick_in(BASE + 1 * SEC, true, &cv, BASE + 900 * MS, BASE + 500 * MS);
    CHECK(pgwt_cov_tracker_tick(&t, &w, &in) == 0, "tick 1");
    in = tick_in(BASE + 2 * SEC, true, &cv, BASE + 1900 * MS, BASE + 1500 * MS);
    CHECK(pgwt_cov_tracker_tick(&t, &w, &in) == 0, "tick 2");
    /* The consumer is still below the pending reservation fence. */
    in = tick_in(BASE + 3 * SEC, false, &cv, BASE + 2900 * MS, BASE + 2500 * MS);
    CHECK(pgwt_cov_tracker_tick(&t, &w, &in) == 0, "tick 3 (yield)");
    /* A failed publish is not good either. */
    in = tick_in(BASE + 4 * SEC, true, &cv, BASE + 3900 * MS, BASE + 3500 * MS);
    in.publish_ok = false;
    CHECK(pgwt_cov_tracker_tick(&t, &w, &in) == 0, "tick 4 (publish failed)");
    in = tick_in(BASE + 5 * SEC, true, &cv, BASE + 4900 * MS, BASE + 4500 * MS);
    CHECK(pgwt_cov_tracker_tick(&t, &w, &in) == 0, "tick 5");
    in = tick_in(BASE + 6 * SEC, true, &cv, BASE + 5900 * MS, BASE + 5500 * MS);
    CHECK(pgwt_cov_tracker_tick(&t, &w, &in) == 0, "tick 6");
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);

    struct wm x[16];
    struct covlist all;
    int n = wms_of(dir, x, 16, &all);
    CHECK(n == 6, "one WATERMARK per tick (%d)", n);
    if (n == 6) {
        /* tick 1: nothing pending yet, the boot start is all it can say */
        CHECK(x[0].w == BASE && x[0].fd == BASE && x[0].fi == BASE,
              "tick 1 writes W = boot_start");
        /* tick 2 writes tick 1's publish_from and floors: ONE tick of lag */
        CHECK(x[1].w == BASE + 1 * SEC && x[1].fd == BASE + 900 * MS &&
              x[1].fi == BASE + 500 * MS,
              "tick 2 writes W = P(tick 1) (got %llu)",
              (unsigned long long)(x[1].w - BASE));
        CHECK(x[1].w < BASE + 2 * SEC, "W never equals this tick's P");
        /* ticks 3 and 4: heartbeats repeat the last written triple */
        CHECK(x[2].w == x[1].w && x[2].fd == x[1].fd && x[2].fi == x[1].fi,
              "an incomplete reservation prefix repeats W (heartbeat)");
        CHECK(x[3].w == x[1].w, "a failed publish repeats W");
        CHECK(x[2].obs == BASE + 3 * SEC + 2 * MS,
              "a heartbeat is still a fresh observation");
        /* tick 5: the pending triple from tick 2 survives the bad ticks */
        CHECK(x[4].w == BASE + 2 * SEC && x[4].fd == BASE + 1900 * MS,
              "tick 5 writes P(tick 2), the last GOOD tick's");
        CHECK(x[5].w == BASE + 5 * SEC, "tick 6 writes P(tick 5)");
        for (int i = 1; i < n; i++)
            CHECK(x[i].w >= x[i - 1].w, "W never decreases (%d)", i);
        for (int i = 0; i < n; i++)
            CHECK(x[i].fi <= x[i].fd && x[i].fd <= x[i].w,
                  "floor_idle <= floor_db <= W (%d)", i);
    }
    CHECK(count_kind(&all, PGWT_COVK_SEAL) == 0, "no SEAL without seal");
    rm_rf(dir);
}

static void test_counter_gaps(void)
{
    char dir[] = "/tmp/pgwt_cov_gap_XXXXXX";
    if (make_dir(dir) != 0) { failures++; return; }
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 17, 24, NULL) == 0, "init");
    pgwt_writer_coverage_configure(&w, BASE, 0, 0);
    struct pgwt_counter_vector c0 = cv_with(0), c1 = cv_with(3);
    struct pgwt_counter_vector unk = cv_with(3);
    unk.components[PGWT_C_WAIT_READ_FAIL].valid = false;
    struct pgwt_cov_tracker t;
    pgwt_cov_tracker_init(&t, BASE, true, &c0);
    struct pgwt_cov_tick_in in;
    in = tick_in(BASE + 1 * SEC, true, &c0, BASE + SEC, BASE + SEC);
    pgwt_cov_tracker_tick(&t, &w, &in);
    in = tick_in(BASE + 2 * SEC, true, &c0, BASE + 2 * SEC, BASE + 2 * SEC);
    pgwt_cov_tracker_tick(&t, &w, &in);          /* W = BASE+1s written */
    in = tick_in(BASE + 3 * SEC, true, &c1, BASE + 3 * SEC, BASE + 3 * SEC);
    pgwt_cov_tracker_tick(&t, &w, &in);          /* drop: gap [W, 3s) */
    in = tick_in(BASE + 4 * SEC, true, &c1, BASE + 4 * SEC, BASE + 4 * SEC);
    pgwt_cov_tracker_tick(&t, &w, &in);          /* good again */
    in = tick_in(BASE + 5 * SEC, true, &unk, BASE + 5 * SEC, BASE + 5 * SEC);
    pgwt_cov_tracker_tick(&t, &w, &in);          /* unknown: open */
    in = tick_in(BASE + 6 * SEC, true, &unk, BASE + 6 * SEC, BASE + 6 * SEC);
    pgwt_cov_tracker_tick(&t, &w, &in);          /* still unknown */
    in = tick_in(BASE + 7 * SEC, true, &c1, BASE + 7 * SEC, BASE + 7 * SEC);
    pgwt_cov_tracker_tick(&t, &w, &in);          /* known: close */
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);

    struct covlist all;
    struct wm x[16];
    int n = wms_of(dir, x, 16, &all);
    CHECK(n == 7, "a WATERMARK every tick, gaps or not (%d)", n);
    int go = 0, gc = 0;
    struct pgwt_coverage_record g[8];
    int ng = 0;
    for (int i = 0; i < all.n && ng < 8; i++)
        if (all.rec[i].kind == PGWT_COVK_GAP_OPEN ||
            all.rec[i].kind == PGWT_COVK_GAP_CLOSE) {
            g[ng++] = all.rec[i];
            if (all.rec[i].kind == PGWT_COVK_GAP_OPEN) go++; else gc++;
        }
    CHECK(go == 2 && gc == 2, "two gaps, each opened and closed (%d/%d)",
          go, gc);
    if (ng == 4) {
        CHECK(g[0].kind == PGWT_COVK_GAP_OPEN && g[0].reason == PGWT_COVR_DROP &&
              g[0].a == BASE + 1 * SEC && g[0].c == 1,
              "drop gap opens at the last WRITTEN W (%llu)",
              (unsigned long long)(g[0].a - BASE));
        CHECK(g[1].kind == PGWT_COVK_GAP_CLOSE && g[1].a == BASE + 3 * SEC &&
              g[1].c == 1, "drop gap closes at this tick's publish_from");
        CHECK(g[2].reason == PGWT_COVR_COUNTERS_UNKNOWN && g[2].c == 2,
              "unknown counters: ONE open gap, not one per tick");
        CHECK(g[3].kind == PGWT_COVK_GAP_CLOSE && g[3].a == BASE + 7 * SEC &&
              g[3].c == 2, "closed when the counters are known again");
        CHECK(g[0].d == BASE + 3 * SEC + 2 * MS, "detection = observation");
    }
    /* The drop tick and the two unknown ticks and the reopening tick do not
     * advance W; tick 4 (good) wrote P(tick 2). */
    if (n == 7) {
        CHECK(x[2].w == x[1].w, "the drop tick is a heartbeat");
        CHECK(x[3].w == BASE + 2 * SEC, "the next good tick writes P(tick 2)");
        CHECK(x[4].w == x[3].w && x[5].w == x[3].w && x[6].w == x[3].w,
              "no W advance while the counters are unknown or reopening");
    }
    rm_rf(dir);

    /* Base unknown at boot: the first known reading is a closed gap from
     * boot_start, never a silent baseline. */
    char dir2[] = "/tmp/pgwt_cov_gap2_XXXXXX";
    if (make_dir(dir2) != 0) { failures++; return; }
    CHECK(pgwt_writer_init(&w, dir2, 17, 24, NULL) == 0, "init 2");
    pgwt_writer_coverage_configure(&w, BASE, 0, 0);
    pgwt_cov_tracker_init(&t, BASE, true, NULL);
    in = tick_in(BASE + 1 * SEC, true, &c1, BASE + SEC, BASE + SEC);
    pgwt_cov_tracker_tick(&t, &w, &in);
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    n = wms_of(dir2, x, 16, &all);
    CHECK(count_kind(&all, PGWT_COVK_GAP_OPEN) == 1 &&
          count_kind(&all, PGWT_COVK_GAP_CLOSE) == 1,
          "unknown base: one closed gap");
    for (int i = 0; i < all.n; i++)
        if (all.rec[i].kind == PGWT_COVK_GAP_OPEN)
            CHECK(all.rec[i].a == BASE &&
                  all.rec[i].reason == PGWT_COVR_COUNTERS_UNKNOWN,
                  "it starts at boot_start");
    rm_rf(dir2);
}

/* Exercise the SAME collection sequence as the daemon, with an actual
 * daemon-side loss during publish. A post-publish counter read would consume it
 * at tick k and close its only gap BEFORE the loss. */
struct drain_loss {
    struct pgwt_counter_vector cv;
    uint64_t now, lost_at;
    bool inject;
};
static struct pgwt_counter_vector loss_counters(void *ctx)
{
    return ((struct drain_loss *)ctx)->cv;
}
static uint64_t loss_clock(void *ctx)
{
    return ((struct drain_loss *)ctx)->now;
}
static void loss_positions(void *ctx, struct pgwt_cov_tick_in *in)
{
    (void)ctx;
    in->positions_known = true;
    in->producer_pos = in->consumer_pos = 0;
}
static bool loss_publish(void *ctx)
{
    struct drain_loss *d = ctx;
    if (d->inject) {
        d->lost_at = d->now + MS;
        d->cv.components[PGWT_C_WRITER_PUSH_FAIL].value++;
        d->inject = false;
    }
    d->now += 2 * MS;
    return true;
}
static void test_loss_during_publish(void)
{
    char dir[] = "/tmp/pgwt_cov_drain_loss_XXXXXX";
    if (make_dir(dir) != 0) { failures++; return; }
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 17, 24, NULL) == 0, "init");
    pgwt_writer_coverage_configure(&w, BASE, 0, 0);
    struct drain_loss d = { .cv = cv_with(0) };
    struct pgwt_cov_tracker t;
    pgwt_cov_tracker_init(&t, BASE, true, &d.cv);
    const struct pgwt_cov_tick_ops ops = {
        loss_counters, loss_clock, loss_positions, loss_publish,
    };
    for (int k = 1; k <= 6; k++) {
        d.now = BASE + k * SEC;
        d.inject = k == 3;
        struct pgwt_cov_tick_in in = {0};
        struct pgwt_counter_vector cv;
        pgwt_cov_collect_tick(&in, &cv, &d, &ops);
        CHECK(pgwt_cov_tracker_tick(&t, &w, &in) == 0, "tick %d", k);
    }
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    char path[600];
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    struct covlist all;
    CHECK(read_cov(path, &all) == 0, "read");
    uint64_t start = 0, end = 0, id = 0;
    int pairs = 0;
    for (int i = 0; i < all.n; i++) {
        struct pgwt_coverage_record *r = &all.rec[i];
        if (r->kind == PGWT_COVK_GAP_OPEN && r->reason == PGWT_COVR_DROP) {
            start = r->a;
            id = r->c;
        }
        if (r->kind == PGWT_COVK_GAP_CLOSE && r->c == id) {
            end = r->a;
            pairs++;
        }
    }
    CHECK(d.lost_at == BASE + 3 * SEC + MS, "stimulus during tick 3 publish");
    CHECK(pairs == 1 && start <= d.lost_at && d.lost_at < end,
          "writer_push_fail during publish is covered by a drop gap");
    CHECK(end == BASE + 4 * SEC, "next tick detects loss and bounds its gap");
    CHECK(t.w_ns > d.lost_at, "W later passes the loss, which remains gapped");
    rm_rf(dir);
}

/* ── floors (C1) ──────────────────────────────────────────────────────── */

static void test_floors(void)
{
    struct pgwt_cov_floors f;
    pgwt_cov_floors_begin(&f, 1000);
    CHECK(f.floor_db_ns == 1000 && f.floor_idle_ns == 1000,
          "no backend: both floors = snap");
    pgwt_cov_floors_add(&f, true, 800, IO_READ, false, 0);
    CHECK(f.floor_db_ns == 800 && f.floor_idle_ns == 800, "a counted wait");
    /* idle wait, no command open: only floor_idle */
    pgwt_cov_floors_add(&f, true, 300, CLIENT_READ, false, 0);
    CHECK(f.floor_db_ns == 800 && f.floor_idle_ns == 300,
          "ClientRead outside a command lowers floor_idle only");
    /* idle wait inside a command counts */
    pgwt_cov_floors_add(&f, true, 700, CLIENT_READ, true, 0);
    CHECK(f.floor_db_ns == 700, "an idle event inside a command counts");
    pgwt_cov_floors_add(&f, true, 100, ACTIVITY, false, 0);
    CHECK(f.floor_db_ns == 700 && f.floor_idle_ns == 100,
          "an Activity sleep reaches floor_idle only");
    /* a transition after the snapshot opened nothing before it */
    pgwt_cov_floors_add(&f, true, 5000, IO_READ, false, 0);
    CHECK(f.floor_db_ns == 700, "last_ts after snap does not raise or lower");
    /* unreadable: fail closed, both floors, at the bound */
    pgwt_cov_floors_add(&f, false, 0, 0, false, 650);
    CHECK(f.floor_db_ns == 650 && f.floor_idle_ns == 100,
          "unreadable state counts for floor_db at its bound");
    pgwt_cov_floors_add(&f, false, 0, 0, false, 50);
    CHECK(f.floor_db_ns == 50 && f.floor_idle_ns == 50,
          "and for floor_idle");
}

/* ── SEAL only on a clean close; recovery keeps type 3 ───────────────── */

static void run_ticks(struct pgwt_cov_tracker *t, struct pgwt_event_writer *w,
                      int n, uint64_t from)
{
    struct pgwt_counter_vector cv = cv_with(0);
    for (int i = 1; i <= n; i++) {
        struct pgwt_trace_event e = tev(20, from + i * SEC - 10 * MS, 5 * MS,
                                        LOCK_REL, 0);
        pgwt_writer_push_event(w, &e);
        pgwt_writer_publish(w);
        struct pgwt_cov_tick_in in = tick_in(from + i * SEC, true, &cv,
                                             from + i * SEC - 20 * MS,
                                             from + i * SEC - 30 * MS);
        pgwt_cov_tracker_tick(t, w, &in);
    }
}

static void test_seal_only_on_clean_close(void)
{
    struct pgwt_counter_vector cv = cv_with(0);

    /* (a) clean shutdown: final tick promotes the pending W, then SEAL */
    char dir[] = "/tmp/pgwt_cov_seal_XXXXXX";
    if (make_dir(dir) != 0) { failures++; return; }
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 17, 24, NULL) == 0, "init");
    pgwt_writer_coverage_configure(&w, BASE, 0, 0);
    struct pgwt_cov_tracker t;
    pgwt_cov_tracker_init(&t, BASE, true, &cv);
    run_ticks(&t, &w, 4, BASE);
    struct pgwt_cov_tick_in in = tick_in(BASE + 4 * SEC + 300 * MS, true, &cv,
                                         0, 0);
    in.floors_known = false;
    CHECK(pgwt_cov_tracker_seal(&t, &w, &in) == 0, "seal");
    CHECK(pgwt_writer_close(&w) == 0, "close");
    pgwt_writer_destroy(&w);
    char p[600];
    snprintf(p, sizeof(p), "%s/current.trace", dir);
    struct covlist c;
    CHECK(read_cov(p, &c) == 0, "read");
    CHECK(count_kind(&c, PGWT_COVK_SEAL) == 1, "one SEAL after a clean close");
    CHECK(c.n > 0 && c.rec[c.n - 1].kind == PGWT_COVK_SEAL,
          "SEAL is the last record");
    if (c.n >= 2) {
        CHECK(c.rec[c.n - 1].a == BASE + 4 * SEC,
              "W_final = the last good tick's publish_from (%llu)",
              (unsigned long long)(c.rec[c.n - 1].a - BASE));
        CHECK(c.rec[c.n - 2].kind == PGWT_COVK_WATERMARK &&
              c.rec[c.n - 2].a == c.rec[c.n - 1].a,
              "and the last WATERMARK says the same W");
    }
    enum pgwt_closure_status cs;
    CHECK(pgwt_trace_file_closure(p, &cs) == 0 && cs == PGWT_CLOSURE_CLEAN,
          "closure CLEAN");
    rm_rf(dir);

    /* (b) simulated SIGKILL: the process vanishes after a few ticks — no
     * seal, no close. The file is abandoned exactly as a killed daemon
     * leaves it (flushed blocks, no footer, no closure bit). */
    char dir2[] = "/tmp/pgwt_cov_kill_XXXXXX";
    if (make_dir(dir2) != 0) { failures++; return; }
    CHECK(pgwt_writer_init(&w, dir2, 17, 24, NULL) == 0, "init 2");
    pgwt_writer_coverage_configure(&w, BASE, 0, 0);
    pgwt_cov_tracker_init(&t, BASE, true, &cv);
    run_ticks(&t, &w, 5, BASE);
    fclose(w.fp);                       /* the kill */
    w.fp = NULL;
    pgwt_writer_destroy(&w);
    snprintf(p, sizeof(p), "%s/current.trace", dir2);
    struct covlist before;
    CHECK(read_cov(p, &before) == 0, "read the killed file");
    CHECK(count_kind(&before, PGWT_COVK_SEAL) == 0, "no SEAL after a kill");
    CHECK(count_kind(&before, PGWT_COVK_WATERMARK) == 5, "five WATERMARKs");
    int data_before = before.data_blocks;

    /* The next start recovers it: RECOVERED, every type-3 block kept. */
    struct pgwt_event_writer w2;
    CHECK(pgwt_writer_init(&w2, dir2, 17, 24, NULL) == 0, "restart");
    pgwt_writer_destroy(&w2);
    char arch[600];
    CHECK(find_archive(dir2, arch, sizeof(arch), 0) == 0, "recovered archive");
    struct covlist after;
    CHECK(read_cov(arch, &after) == 0, "read the recovered archive");
    CHECK(after.n == before.n && after.bad == 0,
          "recovery keeps every COVERAGE block (%d of %d)", after.n, before.n);
    CHECK(after.data_blocks == data_before, "and every data block");
    int same = after.n == before.n;
    for (int i = 0; same && i < after.n; i++)
        same = after.rec[i].kind == before.rec[i].kind &&
               after.rec[i].a == before.rec[i].a &&
               after.obs[i] == before.obs[i];
    CHECK(same, "byte-identical coverage facts after recovery");
    CHECK(count_kind(&after, PGWT_COVK_SEAL) == 0,
          "recovery does not seal a killed boot");
    CHECK(pgwt_trace_file_closure(arch, &cs) == 0 &&
          cs == PGWT_CLOSURE_UNCLEAN, "closure UNCLEAN (RECOVERED bit)");
    rm_rf(dir2);

    /* (c) rotation is not a shutdown */
    char dir3[] = "/tmp/pgwt_cov_rot_XXXXXX";
    if (make_dir(dir3) != 0) { failures++; return; }
    CHECK(pgwt_writer_init(&w, dir3, 17, 24, NULL) == 0, "init 3");
    pgwt_writer_coverage_configure(&w, BASE, 0, 0);
    pgwt_cov_tracker_init(&t, BASE, true, &cv);
    run_ticks(&t, &w, 2, BASE);
    w.force_rotate = true;
    pgwt_writer_check_rotation(&w);
    run_ticks(&t, &w, 2, BASE + 2 * SEC);
    in = tick_in(BASE + 4 * SEC + 300 * MS, true, &cv, 0, 0);
    pgwt_cov_tracker_seal(&t, &w, &in);
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    CHECK(find_archive(dir3, arch, sizeof(arch), 0) == 0, "rotated archive");
    CHECK(read_cov(arch, &c) == 0 && count_kind(&c, PGWT_COVK_SEAL) == 0,
          "a rotated file carries no SEAL");
    snprintf(p, sizeof(p), "%s/current.trace", dir3);
    CHECK(read_cov(p, &c) == 0 && count_kind(&c, PGWT_COVK_SEAL) == 1,
          "the boot's last file does");
    rm_rf(dir3);

    /* (d) a tracker without watermarks (lightweight) and a session that
     * never opened a file: nothing to seal, and no file is created. */
    char dir4[] = "/tmp/pgwt_cov_lw_XXXXXX";
    if (make_dir(dir4) != 0) { failures++; return; }
    CHECK(pgwt_writer_init(&w, dir4, 17, 24, NULL) == 0, "init 4");
    pgwt_writer_coverage_configure(&w, BASE, 0, PGWT_COVERAGE_MODE_LIGHTWEIGHT);
    pgwt_cov_tracker_init(&t, BASE, false, &cv);
    in = tick_in(BASE + SEC, true, &cv, BASE, BASE);
    CHECK(pgwt_cov_tracker_tick(&t, &w, &in) == 0 && w.fp == NULL,
          "lightweight: a tick writes nothing");
    CHECK(pgwt_cov_tracker_seal(&t, &w, &in) == 0 && w.fp == NULL,
          "lightweight: nothing to seal, no file created");
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    snprintf(p, sizeof(p), "%s/current.trace", dir4);
    CHECK(access(p, F_OK) != 0, "no current.trace");
    rm_rf(dir4);
}

/* ── v4-era data paths skip v6 type-3 blocks ─────────────────────────── */

static int write_stream(const char *dir, bool with_cov)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 17, 24, NULL) != 0)
        return -1;
    if (with_cov)
        pgwt_writer_coverage_configure(&w, BASE, 0, 0);
    w.cpu_measured = true;
    struct pgwt_counter_vector cv = cv_with(0);
    struct pgwt_cov_tracker t;
    pgwt_cov_tracker_init(&t, BASE, true, &cv);
    for (int s = 0; s < 6; s++) {
        for (int i = 0; i < 300; i++) {
            uint64_t ts = BASE + (uint64_t)s * SEC + (uint64_t)i * 3 * MS;
            struct pgwt_trace_event e = tev(100 + i % 4, ts, 2 * MS,
                                            i % 3 ? IO_READ : LOCK_REL, 0);
            e.cpu_ns = 0;
            if (pgwt_writer_push_event(&w, &e) != 0) return -1;
        }
        struct pgwt_trace_event smp = tev(200, BASE + (uint64_t)s * SEC
                                          + 500 * MS, 0, 0, IO_READ);
        if (pgwt_writer_push_samples(&w, &smp, 1, 100 * MS) != 0) return -1;
        if (pgwt_writer_publish(&w) != 0) return -1;
        if (with_cov) {
            /* observation far beyond every record: a COVERAGE block that
             * reached a data branch would move a span or a window */
            struct pgwt_cov_tick_in in = tick_in(BASE + (uint64_t)(s + 1) * SEC,
                                                 true, &cv, BASE, BASE);
            in.observation_ns = BASE + 900 * SEC + (uint64_t)s;
            pgwt_cov_tracker_tick(&t, &w, &in);
            if (s == 3)
                pgwt_writer_coverage_gap(&w, BASE + 950 * SEC, true, 9,
                                         BASE, PGWT_COVR_DROP);
            if (s == 4) {
                if (pgwt_writer_coverage_untraced_lifetime(&w, BASE + 951 * SEC,
                        true, BASE + MS, 4321, 10, BASE + 3 * MS) != 0 ||
                    pgwt_writer_coverage_untraced_lifetime(&w, BASE + 952 * SEC,
                        false, BASE + 2 * MS, 17, 10, BASE + MS) != 0 ||
                    pgwt_writer_coverage_a2_bucket(&w, BASE + 953 * SEC,
                        BASE, BASE + SEC, 10, 1) != 0)
                    return -1;
            }
        }
    }
    if (with_cov)
        pgwt_writer_coverage_seal(&w, BASE + 999 * SEC, BASE + 6 * SEC);
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    return 0;
}

struct replay_out {
    uint64_t transitions, samples, unreadable, decoded;
    int nsys;
    uint64_t total[64], count[64];
    uint32_t we[64];
    struct pgwt_trace_file_span span;
    int data_blocks, cov_blocks, decode_errors, cand_nonzero_cov;
};

static int replay_dir(const char *dir, struct replay_out *o)
{
    memset(o, 0, sizeof(*o));
    char p[600];
    snprintf(p, sizeof(p), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, p) != 0)
        return -1;
    struct pgwt_trace_event *buf = malloc(PGWT_BLOCK_EVENTS * sizeof(*buf));
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&r, b, buf, PGWT_BLOCK_EVENTS,
                                              &bi);
        if (n < 0) o->decode_errors++;
        if (bi.block_type == PGWT_BLOCK_COVERAGE) {
            o->cov_blocks++;
            if (n != 0) o->decode_errors++;
            int ro = 0;
            if (pgwt_reader_block_candidate(&r, b, 0, 0, NULL, &ro) != 0 || ro)
                o->cand_nonzero_cov++;
        } else {
            o->data_blocks++;
        }
    }
    if (pgwt_reader_file_span(&r, &o->span) != 0)
        return -1;
    struct pgwt_accumulator *acc = calloc(1, sizeof(*acc));
    pgwt_accum_init(acc);
    struct pgwt_replay_stats st;
    memset(&st, 0, sizeof(st));
    int budget = PGWT_REPLAY_LOOKAHEAD_BLOCKS;
    uint64_t decoded = 0;
    int rc = pgwt_replay_file(&r, acc, 0, 0, buf, PGWT_BLOCK_EVENTS, &budget,
                              &st, &decoded);
    pgwt_replay_finish(acc);
    o->transitions = st.transitions;
    o->samples = st.samples;
    o->unreadable = st.blocks_unreadable;
    o->decoded = decoded;
    o->nsys = acc->num_system_events < 64 ? acc->num_system_events : 64;
    for (int i = 0; i < o->nsys; i++) {
        o->we[i] = acc->system_events[i].wait_event;
        o->total[i] = acc->system_events[i].total_ns;
        o->count[i] = acc->system_events[i].count;
    }
    free(acc);
    free(buf);
    pgwt_reader_close(&r);
    return rc;
}

static void test_v4_paths_skip_type3(void)
{
    char a[] = "/tmp/pgwt_cov_v4a_XXXXXX", b[] = "/tmp/pgwt_cov_v4b_XXXXXX";
    if (make_dir(a) != 0 || make_dir(b) != 0) { failures++; return; }
    CHECK(write_stream(a, true) == 0 && write_stream(b, false) == 0, "write");
    struct replay_out x, y;
    CHECK(replay_dir(a, &x) == 0 && replay_dir(b, &y) == 0, "replay");
    CHECK(x.cov_blocks >= 8 && y.cov_blocks == 0,
          "the v6 file interleaves COVERAGE blocks (%d / %d)",
          x.cov_blocks, y.cov_blocks);
    CHECK(x.data_blocks == y.data_blocks && x.data_blocks > 0,
          "same data blocks (%d / %d)", x.data_blocks, y.data_blocks);
    CHECK(x.decode_errors == 0, "a COVERAGE block decodes to 0 records, "
          "never an error");
    CHECK(x.cand_nonzero_cov == 0, "a COVERAGE block is never a candidate "
          "and never a right-open straddler block");
    CHECK(x.unreadable == 0, "replay counts no unreadable block");
    CHECK(x.transitions == y.transitions && x.samples == y.samples &&
          x.decoded == y.decoded && x.transitions > 0,
          "replay: same records (%llu/%llu transitions, %llu/%llu samples)",
          (unsigned long long)x.transitions, (unsigned long long)y.transitions,
          (unsigned long long)x.samples, (unsigned long long)y.samples);
    int same = x.nsys == y.nsys && x.nsys > 0;
    for (int i = 0; same && i < x.nsys; i++)
        same = x.we[i] == y.we[i] && x.total[i] == y.total[i] &&
               x.count[i] == y.count[i];
    CHECK(same, "replay: identical per-event totals and counts");
    CHECK(x.span.mono_end == y.span.mono_end &&
          x.span.mono_end == BASE + 5 * SEC + 897 * MS,
          "file span ignores COVERAGE write times (end %llu vs %llu)",
          (unsigned long long)x.span.mono_end,
          (unsigned long long)y.span.mono_end);
    CHECK(x.span.mono_end < BASE + 900 * SEC, "span ends at the data");
    rm_rf(a);
    rm_rf(b);
}

/* ── a tick with nothing appended writes no data block ───────────────── */

static void test_idle_publish(void)
{
    char dir[] = "/tmp/pgwt_cov_idle_XXXXXX";
    if (make_dir(dir) != 0) { failures++; return; }
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 17, 24, NULL) == 0, "init");
    pgwt_writer_coverage_configure(&w, BASE, 0, 0);
    struct pgwt_counter_vector cv = cv_with(0);
    struct pgwt_cov_tracker t;
    pgwt_cov_tracker_init(&t, BASE, true, &cv);
    struct pgwt_trace_event e = tev(1, BASE + 100 * MS, MS, IO_READ, 0);
    pgwt_writer_push_event(&w, &e);
    CHECK(pgwt_writer_publish(&w) == 0, "publish with a record");
    int blocks = w.num_blocks;
    for (int i = 1; i <= 3; i++) {
        CHECK(pgwt_writer_publish(&w) == 0, "idle publish %d", i);
        struct pgwt_cov_tick_in in = tick_in(BASE + i * SEC, true, &cv,
                                             BASE, BASE);
        pgwt_cov_tracker_tick(&t, &w, &in);
    }
    CHECK(w.num_blocks == blocks + 3,
          "three idle ticks add exactly three COVERAGE blocks (%d -> %d)",
          blocks, w.num_blocks);
    CHECK(t.good_ticks == 3, "idle ticks are good ticks: W still advances");
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    rm_rf(dir);
}

/* ── version gate ─────────────────────────────────────────────────────── */

static void test_version_gate(void)
{
    CHECK(PGWT_TRACE_VERSION == 6, "trace format v6");
    CHECK(pgwt_trace_version_readable(2) && pgwt_trace_version_readable(3) &&
          pgwt_trace_version_readable(4) && pgwt_trace_version_readable(6),
          "reads 2, 3, 4, 6");
    CHECK(!pgwt_trace_version_readable(5), "never 5 (evidence codec)");
    CHECK(!pgwt_trace_version_readable(1) && !pgwt_trace_version_readable(7),
          "not 1, not 7");
    char dir[] = "/tmp/pgwt_cov_ver_XXXXXX";
    if (make_dir(dir) != 0) { failures++; return; }
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 17, 24, NULL) == 0, "init");
    struct pgwt_trace_event e = tev(1, BASE + 100 * MS, MS, IO_READ, 0);
    pgwt_writer_push_event(&w, &e);
    CHECK(pgwt_writer_coverage_watermark(&w, BASE, BASE, BASE, BASE) != 0 &&
          pgwt_writer_coverage_gap(&w, BASE, true, 1, BASE, 2) != 0 &&
          pgwt_writer_coverage_seal(&w, BASE, BASE) != 0,
          "coverage calls refuse until coverage_configure");
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    char p[600];
    snprintf(p, sizeof(p), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    CHECK(pgwt_reader_open(&r, p) == 0 && r.header.version == 6,
          "the writer writes v6 and the reader reads it");
    pgwt_reader_close(&r);
    /* A session that never enabled coverage (library callers, the unit
     * tests' writers) writes no COVERAGE block, and its calls refuse. */
    struct covlist c;
    CHECK(read_cov(p, &c) == 0 && c.n == 0 && c.data_blocks == 1,
          "unconfigured session: no COVERAGE block, not even a BOOT");
    FILE *f = fopen(p, "r+b");
    uint16_t v5 = 5;
    if (f) {
        fseek(f, 4, SEEK_SET);
        fwrite(&v5, 2, 1, f);
        fclose(f);
    }
    CHECK(pgwt_reader_open(&r, p) != 0, "a v5 header is refused");
    rm_rf(dir);
}

/* A2 carrier only: read while live (fflush), then read the closed trace. */
static void test_a2_carrier(void)
{
    char dir[] = "/tmp/pgwt_cov_a2_XXXXXX";
    if (make_dir(dir) != 0) { failures++; return; }
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 18, 24, NULL) == 0, "init A2");
    CHECK(pgwt_writer_coverage_untraced_lifetime(&w, BASE, true,
              BASE, 4321, 90, BASE + 3 * MS) != 0 &&
          pgwt_writer_coverage_a2_bucket(&w, BASE, BASE, BASE + SEC, 10, 1) != 0,
          "A2 APIs refuse unconfigured writer");
    pgwt_writer_coverage_configure(&w, BASE, 0, 0);
    CHECK(pgwt_writer_coverage_untraced_lifetime(&w, BASE + 4 * MS, true,
              BASE + MS, 4321, 90, BASE + 3 * MS) == 0, "A2 open");
    CHECK(pgwt_writer_coverage_untraced_lifetime(&w, BASE + 5 * MS, false,
              BASE + 2 * MS, 0x1020304050607080ULL, 90, BASE + MS) == 0,
          "A2 close: receive can precede exit");
    CHECK(pgwt_writer_coverage_a2_bucket(&w, BASE + 2 * SEC,
              BASE, BASE + SEC, 10, 1) == 0, "A2 bucket");
    CHECK(pgwt_writer_coverage_a2_bucket(&w, BASE + 3 * SEC,
              BASE + SEC, BASE + 2 * SEC, 0, 0) == 0, "empty A2 bucket");
    char path[600];
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    for (int closed = 0; closed <= 1; closed++) {
        if (closed) CHECK(pgwt_writer_close(&w) == 0, "close A2");
        struct covlist c;
        CHECK(read_cov(path, &c) == 0 && c.n == 5 && c.bad == 0 &&
              c.data_blocks == 0, "live/closed A2 records decode as valid");
        if (c.n != 5) continue;
        CHECK(c.rec[1].kind == PGWT_COVK_GAP_OPEN && c.rec[1].reason == 5 &&
              c.rec[1].a == BASE + MS && c.rec[1].b == 4321 &&
              c.rec[1].c == 90 && c.rec[1].d == BASE + 3 * MS &&
              c.obs[1] == BASE + 4 * MS, "A2 open preserves all fields");
        CHECK(c.rec[2].kind == PGWT_COVK_GAP_CLOSE && c.rec[2].reason == 0 &&
              c.rec[2].a == BASE + 2 * MS && c.rec[2].b == 0x1020304050607080ULL &&
              c.rec[2].c == 90 && c.rec[2].d == BASE + MS &&
              c.obs[2] == BASE + 5 * MS, "A2 close preserves all fields");
        CHECK(c.rec[3].kind == 7 && c.rec[3].reason == 0 &&
              c.rec[3].a == BASE && c.rec[3].b == BASE + SEC &&
              c.rec[3].c == 10 && c.rec[3].d == 1,
              "A2_BUCKET preserves start/end/counts");
        CHECK(c.rec[4].kind == 7 && c.rec[4].a == BASE + SEC &&
              c.rec[4].b == BASE + 2 * SEC && c.rec[4].c == 0 && c.rec[4].d == 0,
              "A2_BUCKET preserves empty finalized cohort");
    }
    pgwt_writer_destroy(&w);
    rm_rf(dir);

    /* Real lazy-open failure, then both APIs must refuse every later call. */
    char bad[] = "/tmp/pgwt_cov_a2_fail_XXXXXX";
    if (make_dir(bad) != 0) { failures++; return; }
    CHECK(pgwt_writer_init(&w, bad, 18, 24, NULL) == 0, "init failing A2");
    pgwt_writer_coverage_configure(&w, BASE, 0, 0);
    CHECK(rmdir(bad) == 0, "remove output directory");
    CHECK(pgwt_writer_coverage_untraced_lifetime(&w, BASE, true,
              BASE, 4321, 1, BASE) != 0 && w.failed && !w.enabled,
          "A2 open failure sticks");
    for (int i = 0; i < 2; i++) {
        CHECK(pgwt_writer_coverage_untraced_lifetime(&w, BASE, false,
                  BASE, 7, 1, BASE) != 0 &&
              pgwt_writer_coverage_a2_bucket(&w, BASE, BASE, BASE + SEC, 1, 1) != 0,
              "failed writer refuses A2 pair/certificate");
    }
    pgwt_writer_destroy(&w);
}

int main(void)
{
    printf("test_coverage_writer (#947 U1)\n");
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    base_ns = (uint64_t)ts.tv_sec * SEC + (uint64_t)ts.tv_nsec + 100 * SEC;
    test_golden_bytes();
    test_boot_per_file();
    test_w_lags_one_tick_and_heartbeat();
    test_counter_gaps();
    test_loss_during_publish();
    test_floors();
    test_seal_only_on_clean_close();
    test_v4_paths_skip_type3();
    test_idle_publish();
    test_version_gate();
    test_a2_carrier();
    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0)
        printf("ALL PASS\n");
    return failures ? 1 : 0;
}
