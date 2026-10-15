/* test_open_ckpt.c — #424 deliverable 2: the open-interval checkpoint, as ONE
 * contract over checkpoint identity, durable publication, recovery and
 * suppression. Those four cannot be split: identity without suppression
 * double counts, suppression without identity counts the wrong capture's
 * time, and either without durable publication records a watermark the trace
 * never reached.
 *
 * So the groups below mirror them:
 *
 *   A. format + suppression, pure: the CRC is the standard one (a reader in
 *      another language must agree), every refusal reason is reachable and
 *      named, and the suppression predicate is exercised on inputs where it
 *      must say yes AND inputs where it must say no — a rule that suppresses
 *      everything, or nothing, fails here.
 *   B. the writer: identity taken from the file it describes, a sequence that
 *      advances, a watermark that is published BEFORE it is recorded, and
 *      refusals that leave the previous checkpoint standing.
 *   C. recovery and the lifecycle: a crash carries the checkpoint next to its
 *      archive; a clean close and a rotation DELETE it (both would otherwise
 *      double count against real closing records); a new file discards it.
 *   D. bypass suite: a stale sidecar from another file, a corrupt one, a
 *      truncated one, one with an insane count, one whose trace file is
 *      absent entirely, and an empty checkpoint that must SUPERSEDE a
 *      non-empty one. A checkpoint that cannot be validated is not used, and
 *      "cannot be validated" must never resolve to "use it anyway".
 *
 * Server build (-DPGWT_SERVER).
 */
#include "event_writer.h"
#include "open_ckpt.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do {                                          \
    tests_run++;                                                            \
    if (cond) { tests_passed++; }                                           \
    else { printf("  FAIL(%d): " fmt "\n", __LINE__, ##__VA_ARGS__); }      \
} while (0)

static char g_base[300];

static void rm_rf(const char *dir)
{
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* ignore */ }
}

static const char *fresh_dir(const char *name)
{
    static char buf[600];
    snprintf(buf, sizeof(buf), "%s/%s", g_base, name);
    rm_rf(buf);
    mkdir(buf, 0750);
    return buf;
}

static const char *pathcat(const char *dir, const char *name)
{
    static char buf[700];
    snprintf(buf, sizeof(buf), "%s/%s", dir, name);
    return buf;
}

static void push_n(struct pgwt_event_writer *w, int n, uint64_t t0)
{
    for (int i = 0; i < n; i++) {
        struct pgwt_trace_event e = {
            .timestamp_ns = t0 + (uint64_t)i * 1000000ULL,
            .pid = 1000 + (uint32_t)(i % 5),
            .old_event = 0,
            .new_event = 0x01000004,
            .duration_ns = 500000,
            .query_id = 7,
            .cpu_ns = 1000,
        };
        pgwt_writer_push_event(w, &e);
    }
}

static struct pgwt_open_ckpt_record mkrec(uint32_t pid, uint64_t ts,
                                         uint64_t dur)
{
    struct pgwt_open_ckpt_record r = {
        .timestamp_ns = ts,
        .duration_ns = dur,
        .query_id = 99,
        .cpu_ns = PGWT_CPU_NS_UNKNOWN,
        .pid = pid,
        .event = 0x01000004,
        .flags = PGWT_EVENT_FLAG_CMD_OPEN,
        .reserved = 0,
    };
    return r;
}

static const char *newest_archive(const char *dir)
{
    static char best[700];
    best[0] = '\0';
    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t n = strlen(de->d_name);
        if (n >= 10 && strcmp(de->d_name + n - 10, ".trace.lz4") == 0)
            snprintf(best, sizeof(best), "%s/%s", dir, de->d_name);
    }
    closedir(d);
    return best[0] ? best : NULL;
}

/* Serialize a checkpoint by hand so a test can corrupt any single field. */
static int write_ckpt_file(const char *path,
                           struct pgwt_open_ckpt_header h,
                           const struct pgwt_open_ckpt_record *recs,
                           int n, int fix_checksum)
{
    if (fix_checksum)
        h.checksum = pgwt_open_ckpt_crc32(recs, (size_t)n * sizeof(*recs));
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(&h, sizeof(h), 1, f) == 1;
    if (ok && n > 0)
        ok = fwrite(recs, sizeof(*recs), (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok ? 0 : -1;
}

/* ── A. format + suppression ──────────────────────────────── */

static void test_crc_is_the_standard_one(void)
{
    printf("\n[A1] the checksum is CRC-32/ISO-HDLC\n");
    /* The canonical check value. If this drifts, a sidecar written by this
     * build and read by anything else (or by a future table-driven version of
     * this function) silently fails validation and the evidence is thrown
     * away as "corrupt". */
    CHECK(pgwt_open_ckpt_crc32("123456789", 9) == 0xCBF43926u,
          "CRC-32 check value must be 0xCBF43926, got 0x%08X",
          pgwt_open_ckpt_crc32("123456789", 9));
    CHECK(pgwt_open_ckpt_crc32("", 0) == 0,
          "the empty CRC must be 0, got 0x%08X", pgwt_open_ckpt_crc32("", 0));
    /* A one-bit change must change the checksum (otherwise corruption of a
     * record would pass validation). */
    CHECK(pgwt_open_ckpt_crc32("123456780", 9) != 0xCBF43926u,
          "a changed byte must change the checksum");
}

static void test_parse_refusals(void)
{
    printf("\n[A2] every refusal reason is reachable and named\n");

    struct pgwt_open_ckpt_record recs[2] = {
        mkrec(11, 5000, 1000), mkrec(12, 5000, 2000),
    };
    struct pgwt_open_ckpt_identity id = { 111, 222 };
    struct pgwt_open_ckpt_header base = {
        .magic = PGWT_OPEN_CKPT_MAGIC,
        .version = PGWT_OPEN_CKPT_VERSION,
        .flags = 0,
        .trace_start_wall_ns = 111,
        .trace_start_mono_ns = 222,
        .seq = 4,
        .checkpoint_mono_ns = 5000,
        .committed_through_ns = 4000,
        .num_records = 2,
        .checksum = pgwt_open_ckpt_crc32(recs, sizeof(recs)),
    };

    uint8_t buf[sizeof(base) + sizeof(recs)];
    memcpy(buf, &base, sizeof(base));
    memcpy(buf + sizeof(base), recs, sizeof(recs));

    struct pgwt_open_ckpt_view v;
    CHECK(pgwt_open_ckpt_parse(buf, sizeof(buf), &id, &v) == PGWT_OPEN_CKPT_OK,
          "the well-formed, identity-matching buffer must parse");
    CHECK(v.count == 2 && v.records && v.records[1].pid == 12 &&
          v.header.seq == 4 && v.header.committed_through_ns == 4000,
          "the view must describe the records and the header");

    /* Nothing / too short. */
    CHECK(pgwt_open_ckpt_parse(NULL, 0, &id, &v) == PGWT_OPEN_CKPT_TOO_SHORT,
          "NULL must refuse");
    CHECK(pgwt_open_ckpt_parse(buf, 0, &id, &v) == PGWT_OPEN_CKPT_TOO_SHORT,
          "zero bytes must refuse");
    CHECK(pgwt_open_ckpt_parse(buf, sizeof(base) - 1, &id, &v) ==
          PGWT_OPEN_CKPT_TOO_SHORT, "a partial header must refuse");

    /* A refused parse must leave NOTHING usable behind: a caller that
     * ignores the return code must not find a plausible view. */
    v.count = 7;
    v.records = recs;
    CHECK(pgwt_open_ckpt_parse(buf, 0, &id, &v) != PGWT_OPEN_CKPT_OK &&
          v.count == 0 && v.records == NULL,
          "a refusal must zero the output view");

    uint8_t bad[sizeof(buf)];

    memcpy(bad, buf, sizeof(buf));
    ((struct pgwt_open_ckpt_header *)bad)->magic = 0xDEADBEEF;
    CHECK(pgwt_open_ckpt_parse(bad, sizeof(bad), &id, &v) ==
          PGWT_OPEN_CKPT_BAD_MAGIC, "wrong magic must refuse");

    memcpy(bad, buf, sizeof(buf));
    ((struct pgwt_open_ckpt_header *)bad)->version = 99;
    CHECK(pgwt_open_ckpt_parse(bad, sizeof(bad), &id, &v) ==
          PGWT_OPEN_CKPT_BAD_VERSION, "an unknown version must refuse");

    memcpy(bad, buf, sizeof(buf));
    ((struct pgwt_open_ckpt_header *)bad)->num_records = 0xFFFFFFFFu;
    CHECK(pgwt_open_ckpt_parse(bad, sizeof(bad), &id, &v) ==
          PGWT_OPEN_CKPT_COUNT_INSANE,
          "a count beyond the cap must refuse BEFORE any size arithmetic");

    memcpy(bad, buf, sizeof(buf));
    ((struct pgwt_open_ckpt_header *)bad)->num_records = 2;
    CHECK(pgwt_open_ckpt_parse(bad, sizeof(base) + sizeof(recs) / 2, &id, &v) ==
          PGWT_OPEN_CKPT_TRUNCATED,
          "a count larger than the bytes present must refuse");

    memcpy(bad, buf, sizeof(buf));
    bad[sizeof(base) + 3] ^= 0x01;        /* one bit inside a record */
    CHECK(pgwt_open_ckpt_parse(bad, sizeof(bad), &id, &v) ==
          PGWT_OPEN_CKPT_BAD_CHECKSUM, "a flipped record bit must refuse");

    /* IDENTITY: the stale-sidecar case. Either stamp differing is enough. */
    struct pgwt_open_ckpt_identity other = { 111, 999 };
    CHECK(pgwt_open_ckpt_parse(buf, sizeof(buf), &other, &v) ==
          PGWT_OPEN_CKPT_IDENTITY_MISMATCH,
          "a different mono stamp must refuse");
    other = (struct pgwt_open_ckpt_identity){ 999, 222 };
    CHECK(pgwt_open_ckpt_parse(buf, sizeof(buf), &other, &v) ==
          PGWT_OPEN_CKPT_IDENTITY_MISMATCH,
          "a different wall stamp must refuse");

    /* Documented precedence: damage is reported as damage even when the
     * identity is also wrong — the two mean different things to an operator. */
    memcpy(bad, buf, sizeof(buf));
    bad[sizeof(base) + 3] ^= 0x01;
    CHECK(pgwt_open_ckpt_parse(bad, sizeof(bad), &other, &v) ==
          PGWT_OPEN_CKPT_BAD_CHECKSUM,
          "a damaged AND stale buffer reports damage");

    /* NULL identity inspects but must not be mistaken for validation. */
    CHECK(pgwt_open_ckpt_parse(buf, sizeof(buf), NULL, &v) ==
          PGWT_OPEN_CKPT_OK, "a NULL identity still parses (inspection)");

    for (int rc = PGWT_OPEN_CKPT_OK; rc <= PGWT_OPEN_CKPT_IDENTITY_MISMATCH; rc++)
        CHECK(strcmp(pgwt_open_ckpt_rc_name((enum pgwt_open_ckpt_rc)rc),
                     "unknown") != 0,
              "refusal reason %d must have a name an operator can read", rc);
}

static void test_suppression_rule(void)
{
    printf("\n[A3] suppression: yes for a closed stretch, no for an open one\n");

    /* An interval open from 1000, checkpointed at 5000 (4 us so far). */
    struct pgwt_open_ckpt_record r = mkrec(42, 5000, 4000);
    CHECK(pgwt_open_ckpt_start_ns(&r) == 1000,
          "start = timestamp - duration, got %llu",
          (unsigned long long)pgwt_open_ckpt_start_ns(&r));

    struct pgwt_trace_event c = { .pid = 42, .timestamp_ns = 6000,
                                  .duration_ns = 5000 };
    CHECK(pgwt_open_ckpt_suppressed_by(&r, &c),
          "a closing record for this pid AFTER the start means it closed");

    c.pid = 43;
    CHECK(!pgwt_open_ckpt_suppressed_by(&r, &c),
          "another backend's record must never suppress this one");

    c.pid = 42;
    c.timestamp_ns = 1000;
    CHECK(!pgwt_open_ckpt_suppressed_by(&r, &c),
          "a record ending exactly AT the start is the PREVIOUS stretch "
          "([start,end) is half-open) and must not suppress");

    c.timestamp_ns = 999;
    CHECK(!pgwt_open_ckpt_suppressed_by(&r, &c),
          "an earlier record must not suppress");

    c.timestamp_ns = 1001;
    CHECK(pgwt_open_ckpt_suppressed_by(&r, &c),
          "one nanosecond after the start is already a closed stretch");

    CHECK(!pgwt_open_ckpt_suppressed_by(NULL, &c) &&
          !pgwt_open_ckpt_suppressed_by(&r, NULL),
          "a missing side must not suppress (and must not crash)");

    /* Anti-vacuity: apply the rule to a mixed set and check the exact total.
     * A rule that suppressed everything would give 0; one that suppressed
     * nothing would give 9000. Only the correct rule gives 7000. */
    struct pgwt_open_ckpt_record ck[3] = {
        mkrec(1, 5000, 4000),    /* pid 1 open from 1000 — stays  (4000) */
        mkrec(2, 5000, 2000),    /* pid 2 open from 3000 — closed  (drop) */
        mkrec(3, 5000, 3000),    /* pid 3 open from 2000 — stays  (3000) */
    };
    struct pgwt_trace_event closed[2] = {
        { .pid = 2, .timestamp_ns = 4800, .duration_ns = 1800 },
        { .pid = 9, .timestamp_ns = 4900, .duration_ns = 100  },
    };
    uint64_t total = 0;
    int kept = 0, dropped = 0;
    for (int i = 0; i < 3; i++) {
        int supp = 0;
        for (int j = 0; j < 2; j++)
            if (pgwt_open_ckpt_suppressed_by(&ck[i], &closed[j])) supp = 1;
        if (supp) dropped++;
        else { kept++; total += ck[i].duration_ns; }
    }
    CHECK(kept == 2 && dropped == 1,
          "exactly one of the three must be suppressed (kept=%d dropped=%d)",
          kept, dropped);
    CHECK(total == 7000,
          "recovered open time must be 7000 ns, got %llu "
          "(0 = suppresses everything, 9000 = suppresses nothing)",
          (unsigned long long)total);
}

/* ── B. the writer ────────────────────────────────────────── */

static void test_writer_identity_and_publication(void)
{
    printf("\n[B1] identity from the file, watermark published first\n");
    const char *dir = fresh_dir("write");
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "init");

    /* No file yet: a checkpoint has nothing to name, so it must be refused
     * rather than written with a zero identity that would later match
     * whatever happens to have zero stamps. */
    struct pgwt_open_ckpt_record r0 = mkrec(1, 1000, 100);
    CHECK(pgwt_writer_checkpoint_open(&w, 1000, &r0, 1) == -1,
          "a checkpoint before any trace file must be refused");
    CHECK(access(pathcat(dir, PGWT_OPEN_CKPT_NAME), F_OK) != 0,
          "...and must write nothing");

    uint64_t t0 = 10000000000ULL;
    push_n(&w, 50, t0);             /* pending: no block written yet */
    CHECK(pgwt_writer_committed_through_ns(&w) == 0,
          "nothing is published before the checkpoint call");

    uint64_t last_ts = t0 + 49 * 1000000ULL;
    struct pgwt_open_ckpt_record recs[2] = {
        mkrec(1001, last_ts + 1000, 500), mkrec(1002, last_ts + 1000, 900),
    };
    CHECK(pgwt_writer_checkpoint_open(&w, last_ts + 1000, recs, 2) == 0,
          "checkpoint");

    /* DURABLE PUBLICATION: the pending block went out BEFORE the checkpoint
     * recorded a watermark, so the watermark is a value the trace reached. */
    CHECK(pgwt_writer_committed_through_ns(&w) == last_ts,
          "the checkpoint must publish first: watermark %llu, expected %llu",
          (unsigned long long)pgwt_writer_committed_through_ns(&w),
          (unsigned long long)last_ts);

    void *buf = NULL;
    struct pgwt_open_ckpt_view v;
    struct pgwt_open_ckpt_identity id = {
        .trace_start_wall_ns = w.file_start_wall_ns,
        .trace_start_mono_ns = w.file_start_mono_ns,
    };
    enum pgwt_open_ckpt_rc rc =
        pgwt_open_ckpt_load(pathcat(dir, PGWT_OPEN_CKPT_NAME), &id, &v, &buf);
    CHECK(rc == PGWT_OPEN_CKPT_OK, "the written checkpoint must validate "
          "against its own file's identity (got %s)",
          pgwt_open_ckpt_rc_name(rc));
    if (rc == PGWT_OPEN_CKPT_OK) {
        CHECK(v.header.seq == 1, "first checkpoint is seq 1, got %llu",
              (unsigned long long)v.header.seq);
        CHECK(v.count == 2 && v.records[1].pid == 1002 &&
              v.records[1].duration_ns == 900,
              "the records must round-trip");
        CHECK(v.records[0].flags == PGWT_EVENT_FLAG_CMD_OPEN,
              "the sidecar preserves flags the trace block layout drops");
        CHECK(v.header.committed_through_ns == last_ts,
              "the recorded watermark must be the published one");
    }
    free(buf);

    /* Sequence advances; a refused checkpoint leaves the previous standing. */
    CHECK(pgwt_writer_checkpoint_open(&w, last_ts + 2000, recs, 2) == 0, "ckpt 2");
    struct pgwt_open_ckpt_record badrec = mkrec(7, 100, 500);  /* dur > ts */
    CHECK(pgwt_writer_checkpoint_open(&w, last_ts + 3000, &badrec, 1) == -1,
          "a self-inconsistent record must be refused");
    rc = pgwt_open_ckpt_load(pathcat(dir, PGWT_OPEN_CKPT_NAME), &id, &v, &buf);
    CHECK(rc == PGWT_OPEN_CKPT_OK && v.header.seq == 2,
          "the refused write must leave checkpoint 2 in place (seq=%llu)",
          rc == PGWT_OPEN_CKPT_OK ? (unsigned long long)v.header.seq : 0);
    free(buf);
    CHECK(access(pathcat(dir, PGWT_OPEN_CKPT_TMP), F_OK) != 0,
          "a refused write must leave no tmp behind");

    pgwt_writer_destroy(&w);
    rm_rf(dir);
}

/* ── C. recovery + lifecycle ──────────────────────────────── */

/* Child: capture, checkpoint `n_ckpt` records, die by SIGKILL. */
static pid_t crash_with_ckpt(const char *dir, int n_ckpt, uint64_t t0)
{
    pid_t pid = fork();
    if (pid != 0) return pid;
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 16, 0, NULL) != 0) _exit(2);
    push_n(&w, 60, t0);
    struct pgwt_open_ckpt_record recs[4];
    for (int i = 0; i < n_ckpt && i < 4; i++)
        recs[i] = mkrec(2000 + (uint32_t)i, t0 + 100000000ULL,
                        1000000ULL * (uint64_t)(i + 1));
    if (pgwt_writer_checkpoint_open(&w, t0 + 100000000ULL, recs, n_ckpt) != 0)
        _exit(4);
    raise(SIGKILL);
    _exit(3);
}

static int reap_killed(pid_t pid)
{
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return 0;
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL;
}

static void test_recovery_carries_checkpoint(void)
{
    printf("\n[C1] a crash carries the checkpoint next to its archive\n");
    const char *dir = fresh_dir("carry");

    CHECK(reap_killed(crash_with_ckpt(dir, 3, 11000000000ULL)), "child killed");
    CHECK(access(pathcat(dir, PGWT_OPEN_CKPT_NAME), F_OK) == 0,
          "the checkpoint survived the kill");

    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "restart");
    const char *arch = newest_archive(dir);
    CHECK(arch != NULL, "the crashed trace was archived");
    if (arch) {
        char side[800];
        snprintf(side, sizeof(side), "%s%s", arch, PGWT_OPEN_CKPT_SUFFIX);
        CHECK(access(side, F_OK) == 0,
              "the checkpoint must be carried to %s", side);

        /* It must validate against the ARCHIVE's identity — that is what
         * makes it evidence about this file and not just a file nearby. */
        FILE *f = fopen(arch, "rb");
        struct pgwt_trace_file_header fh;
        int got = f && fread(&fh, sizeof(fh), 1, f) == 1;
        if (f) fclose(f);
        CHECK(got, "read the archive header");
        if (got) {
            struct pgwt_open_ckpt_identity id = {
                .trace_start_wall_ns = fh.start_time_ns,
                .trace_start_mono_ns = fh.clock_offset_ns,
            };
            void *buf = NULL;
            struct pgwt_open_ckpt_view v;
            enum pgwt_open_ckpt_rc rc = pgwt_open_ckpt_load(side, &id, &v, &buf);
            CHECK(rc == PGWT_OPEN_CKPT_OK,
                  "the carried checkpoint must validate against the archive "
                  "it sits next to (got %s)", pgwt_open_ckpt_rc_name(rc));
            if (rc == PGWT_OPEN_CKPT_OK) {
                CHECK(v.count == 3, "all three open intervals survived, got %d",
                      v.count);
                CHECK(v.records[2].duration_ns == 3000000ULL,
                      "the open time survived the crash");
            }
            free(buf);
        }
        CHECK(access(pathcat(dir, PGWT_OPEN_CKPT_NAME), F_OK) != 0,
              "the live name must be free for the next capture");
    }
    pgwt_writer_destroy(&w);
    rm_rf(dir);
}

static void test_clean_close_drops_checkpoint(void)
{
    printf("\n[C2] a clean close and a rotation DROP the checkpoint\n");

    /* Clean close: the daemon's shutdown flush wrote real closing records for
     * these very intervals, so keeping the checkpoint double counts them. */
    const char *dir = fresh_dir("closedrop");
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "init");
    push_n(&w, 40, 12000000000ULL);
    struct pgwt_open_ckpt_record r = mkrec(3001, 12100000000ULL, 1000000);
    CHECK(pgwt_writer_checkpoint_open(&w, 12100000000ULL, &r, 1) == 0, "ckpt");
    pgwt_writer_close(&w);
    CHECK(access(pathcat(dir, PGWT_OPEN_CKPT_NAME), F_OK) != 0,
          "a clean close must drop the checkpoint");
    pgwt_writer_destroy(&w);

    /* And the archive that recovery then makes must carry no sidecar. */
    struct pgwt_event_writer w2;
    CHECK(pgwt_writer_init(&w2, dir, 16, 0, NULL) == 0, "restart");
    pgwt_writer_destroy(&w2);
    const char *arch = newest_archive(dir);
    if (arch) {
        char side[800];
        snprintf(side, sizeof(side), "%s%s", arch, PGWT_OPEN_CKPT_SUFFIX);
        CHECK(access(side, F_OK) != 0,
              "a cleanly closed archive must have no checkpoint beside it");
    }
    rm_rf(dir);

    /* Rotation: the same intervals continue into the NEXT file, where they
     * get real closing records. Keeping the checkpoint double counts. */
    dir = fresh_dir("rotdrop");
    struct pgwt_event_writer w3;
    CHECK(pgwt_writer_init(&w3, dir, 16, 0, NULL) == 0, "init");
    push_n(&w3, 40, 13000000000ULL);
    r = mkrec(3002, 13100000000ULL, 2000000);
    CHECK(pgwt_writer_checkpoint_open(&w3, 13100000000ULL, &r, 1) == 0, "ckpt");
    w3.current_hour = -99;                 /* force the hour check, no sleep */
    CHECK(pgwt_writer_check_rotation(&w3) == 0, "rotate");
    CHECK(access(pathcat(dir, PGWT_OPEN_CKPT_NAME), F_OK) != 0,
          "rotation must drop the checkpoint");
    const char *rarch = newest_archive(dir);
    if (rarch) {
        char side[800];
        snprintf(side, sizeof(side), "%s%s", rarch, PGWT_OPEN_CKPT_SUFFIX);
        CHECK(access(side, F_OK) != 0,
              "a rotated archive must have no checkpoint beside it");
    }
    pgwt_writer_close(&w3);
    pgwt_writer_destroy(&w3);
    rm_rf(dir);
}

static void test_uncertified_close_keeps_checkpoint(void)
{
    printf("\n[C3] a close that could NOT be certified keeps the checkpoint\n");
    const char *dir = fresh_dir("keepdrop");
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "init");
    push_n(&w, 40, 14000000000ULL);
    struct pgwt_open_ckpt_record r = mkrec(3003, 14100000000ULL, 1500000);
    CHECK(pgwt_writer_checkpoint_open(&w, 14100000000ULL, &r, 1) == 0, "ckpt");

    /* The shutdown flush's records may not have reached the disk, so the
     * checkpoint is the only surviving evidence and must NOT be dropped. */
    push_n(&w, 10, 14200000000ULL);
    w.compress_buf_size = 1;               /* every block write now fails */
    pgwt_writer_close(&w);
    enum pgwt_closure_status st;
    pgwt_trace_file_closure(pathcat(dir, "current.trace"), &st);
    CHECK(st != PGWT_CLOSURE_CLEAN,
          "the close must not read CLEAN (got %s)", pgwt_closure_name(st));
    CHECK(access(pathcat(dir, PGWT_OPEN_CKPT_NAME), F_OK) == 0,
          "an uncertified close must KEEP the checkpoint");
    pgwt_writer_destroy(&w);
    rm_rf(dir);
}

/* ── D. bypass suite ──────────────────────────────────────── */

static void test_stale_sidecar_refused(void)
{
    printf("\n[D1] a sidecar from another file is refused, not applied\n");
    const char *dir = fresh_dir("stale");

    /* A crashed capture, and a checkpoint whose identity names a DIFFERENT
     * trace file — exactly what a sidecar left over from an earlier file, an
     * earlier daemon, or a copied trace dir looks like. */
    CHECK(reap_killed(crash_with_ckpt(dir, 2, 15000000000ULL)), "child killed");

    void *buf = NULL;
    struct pgwt_open_ckpt_view v;
    CHECK(pgwt_open_ckpt_load(pathcat(dir, PGWT_OPEN_CKPT_NAME), NULL, &v,
                              &buf) == PGWT_OPEN_CKPT_OK, "load it back");
    struct pgwt_open_ckpt_header h = v.header;
    struct pgwt_open_ckpt_record recs[4];
    memcpy(recs, v.records, (size_t)v.count * sizeof(recs[0]));
    int n = v.count;
    free(buf);
    h.trace_start_mono_ns ^= 0x5555;        /* a different file */
    CHECK(write_ckpt_file(pathcat(dir, PGWT_OPEN_CKPT_NAME), h, recs, n, 1) == 0,
          "install the stale sidecar");

    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "restart");
    const char *arch = newest_archive(dir);
    CHECK(arch != NULL, "archived");
    if (arch) {
        char side[800];
        snprintf(side, sizeof(side), "%s%s", arch, PGWT_OPEN_CKPT_SUFFIX);
        CHECK(access(side, F_OK) != 0,
              "a stale sidecar must NOT be carried next to the archive");
    }
    CHECK(access(pathcat(dir, PGWT_OPEN_CKPT_NAME), F_OK) != 0,
          "and must not be left behind to be found again next restart");
    pgwt_writer_destroy(&w);
    rm_rf(dir);
}

static void test_corrupt_and_truncated_sidecars_refused(void)
{
    printf("\n[D2] damaged, truncated and insane sidecars are refused\n");

    const char *cases[] = { "corrupt", "trunc", "insane", "notackpt" };
    for (int c = 0; c < 4; c++) {
        const char *dir = fresh_dir(cases[c]);
        CHECK(reap_killed(crash_with_ckpt(dir, 2, 16000000000ULL + c)),
              "[%s] child killed", cases[c]);
        const char *live = pathcat(dir, PGWT_OPEN_CKPT_NAME);
        char livecopy[700];
        snprintf(livecopy, sizeof(livecopy), "%s", live);

        if (c == 0) {                    /* flip a bit inside a record */
            FILE *f = fopen(livecopy, "r+b");
            CHECK(f != NULL, "open sidecar");
            if (f) {
                fseek(f, (long)sizeof(struct pgwt_open_ckpt_header) + 2, SEEK_SET);
                int ch = fgetc(f);
                fseek(f, (long)sizeof(struct pgwt_open_ckpt_header) + 2, SEEK_SET);
                fputc(ch ^ 0x01, f);
                fclose(f);
            }
        } else if (c == 1) {             /* chop the record array in half */
            struct stat st;
            CHECK(stat(livecopy, &st) == 0, "stat sidecar");
            CHECK(truncate(livecopy, st.st_size -
                           (off_t)(sizeof(struct pgwt_open_ckpt_record) / 2)) == 0,
                  "truncate sidecar");
        } else if (c == 2) {             /* claim a preposterous count */
            FILE *f = fopen(livecopy, "r+b");
            CHECK(f != NULL, "open sidecar");
            if (f) {
                struct pgwt_open_ckpt_header h;
                if (fread(&h, sizeof(h), 1, f) == 1) {
                    h.num_records = 0x7FFFFFFF;
                    fseek(f, 0, SEEK_SET);
                    if (fwrite(&h, sizeof(h), 1, f) != 1) { /* ignore */ }
                }
                fclose(f);
            }
        } else {                         /* not a checkpoint at all */
            FILE *f = fopen(livecopy, "wb");
            CHECK(f != NULL, "overwrite sidecar");
            if (f) { fputs("this is not a checkpoint, it is a text file\n", f);
                     fclose(f); }
        }

        struct pgwt_event_writer w;
        CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "[%s] restart",
              cases[c]);
        const char *arch = newest_archive(dir);
        CHECK(arch != NULL, "[%s] archived", cases[c]);
        if (arch) {
            char side[800];
            snprintf(side, sizeof(side), "%s%s", arch, PGWT_OPEN_CKPT_SUFFIX);
            CHECK(access(side, F_OK) != 0,
                  "[%s] an unvalidatable sidecar must not be carried",
                  cases[c]);
        }
        CHECK(access(pathcat(dir, PGWT_OPEN_CKPT_NAME), F_OK) != 0,
              "[%s] and must be removed, not left to be retried", cases[c]);
        pgwt_writer_destroy(&w);
        rm_rf(dir);
    }
}

static void test_empty_checkpoint_supersedes(void)
{
    printf("\n[D3] an EMPTY checkpoint supersedes a non-empty one\n");
    const char *dir = fresh_dir("supersede");

    /* The hazard: intervals open at checkpoint 1 have all closed by
     * checkpoint 2, their closing records are in the trace, and the daemon is
     * then killed. If a zero-record checkpoint were treated as "nothing to
     * write", recovery would find checkpoint 1 and add time that the trace
     * already accounts for. */
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid == 0) {
        struct pgwt_event_writer w;
        if (pgwt_writer_init(&w, dir, 16, 0, NULL) != 0) _exit(2);
        push_n(&w, 60, 17000000000ULL);
        struct pgwt_open_ckpt_record r = mkrec(4001, 17100000000ULL, 5000000);
        if (pgwt_writer_checkpoint_open(&w, 17100000000ULL, &r, 1) != 0) _exit(4);
        if (pgwt_writer_checkpoint_open(&w, 17200000000ULL, NULL, 0) != 0) _exit(5);
        raise(SIGKILL);
        _exit(3);
    }
    CHECK(reap_killed(pid), "child killed after the empty checkpoint");

    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "restart");
    const char *arch = newest_archive(dir);
    CHECK(arch != NULL, "archived");
    if (arch) {
        char side[800];
        snprintf(side, sizeof(side), "%s%s", arch, PGWT_OPEN_CKPT_SUFFIX);
        void *buf = NULL;
        struct pgwt_open_ckpt_view v;
        enum pgwt_open_ckpt_rc rc = pgwt_open_ckpt_load(side, NULL, &v, &buf);
        CHECK(rc == PGWT_OPEN_CKPT_OK,
              "the empty checkpoint must still be carried (it is evidence "
              "that nothing was open), got %s", pgwt_open_ckpt_rc_name(rc));
        if (rc == PGWT_OPEN_CKPT_OK) {
            CHECK(v.count == 0,
                  "it must be the EMPTY one (seq %llu, %d records) — a stale "
                  "non-empty checkpoint here is a double count",
                  (unsigned long long)v.header.seq, v.count);
            CHECK(v.header.seq == 2, "and it must be the later seq, got %llu",
                  (unsigned long long)v.header.seq);
        }
        free(buf);
    }
    pgwt_writer_destroy(&w);
    rm_rf(dir);
}

static void test_absent_checkpoint_is_not_evidence(void)
{
    printf("\n[D4] absent / unreadable: refuse, never substitute\n");
    const char *dir = fresh_dir("absent");

    void *buf = (void *)0x1;
    struct pgwt_open_ckpt_view v;
    v.count = 5;
    enum pgwt_open_ckpt_rc rc =
        pgwt_open_ckpt_load(pathcat(dir, "nope.open"), NULL, &v, &buf);
    /* ABSENT, specifically: "nobody checkpointed" and "somebody did and it is
     * damaged" are different facts, and collapsing them is how a log stops
     * telling an operator which one happened. */
    CHECK(rc == PGWT_OPEN_CKPT_ABSENT && v.count == 0 && buf == NULL,
          "a missing sidecar must report ABSENT and clear its outputs (rc=%s)",
          pgwt_open_ckpt_rc_name(rc));
    CHECK(pgwt_open_ckpt_load(NULL, NULL, &v, &buf) == PGWT_OPEN_CKPT_ABSENT,
          "a NULL path must refuse as absent");
    CHECK(pgwt_open_ckpt_load(dir, NULL, &v, &buf) == PGWT_OPEN_CKPT_ABSENT,
          "a directory is not a checkpoint");

    /* An empty file is not an empty checkpoint: one is evidence, the other is
     * the absence of it. */
    FILE *f = fopen(pathcat(dir, "zero.open"), "wb");
    if (f) fclose(f);
    CHECK(pgwt_open_ckpt_load(pathcat(dir, "zero.open"), NULL, &v, &buf) ==
          PGWT_OPEN_CKPT_TOO_SHORT,
          "a zero-byte file must refuse as too short");

    /* A crash with NO checkpoint at all: recovery must not invent one. */
    const char *d2 = fresh_dir("nockpt");
    pid_t pid = fork();
    if (pid == 0) {
        struct pgwt_event_writer cw;
        if (pgwt_writer_init(&cw, d2, 16, 0, NULL) != 0) _exit(2);
        push_n(&cw, 60, 18000000000ULL);
        pgwt_writer_publish(&cw);
        raise(SIGKILL);
        _exit(3);
    }
    CHECK(reap_killed(pid), "child killed with no checkpoint");
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, d2, 16, 0, NULL) == 0, "restart");
    const char *arch = newest_archive(d2);
    CHECK(arch != NULL, "archived");
    if (arch) {
        char side[800];
        snprintf(side, sizeof(side), "%s%s", arch, PGWT_OPEN_CKPT_SUFFIX);
        CHECK(access(side, F_OK) != 0,
              "no checkpoint existed, so none may appear");
    }
    pgwt_writer_destroy(&w);
    rm_rf(dir);
    rm_rf(d2);
}

static void test_retention_takes_the_sidecar(void)
{
    printf("\n[D5] retention deletes a sidecar with its archive\n");
    const char *dir = fresh_dir("retain");

    CHECK(reap_killed(crash_with_ckpt(dir, 2, 19000000000ULL)), "child killed");
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "restart");
    const char *arch = newest_archive(dir);
    CHECK(arch != NULL, "archived");
    char side[800] = "";
    if (arch) {
        snprintf(side, sizeof(side), "%s%s", arch, PGWT_OPEN_CKPT_SUFFIX);
        CHECK(access(side, F_OK) == 0, "sidecar carried");
    }

    /* Drive the size cap (the age of an archive comes from its NAME, which is
     * today's hour, so a backdated mtime would not age it out). */
    char archcopy[800] = "";
    if (arch) snprintf(archcopy, sizeof(archcopy), "%s", arch);
    w.retention_bytes = 1;
    CHECK(pgwt_writer_cleanup_old_files(&w) == 0, "retention pass");
    if (archcopy[0]) {
        CHECK(access(archcopy, F_OK) != 0, "the archive is gone");
        CHECK(access(side, F_OK) != 0,
              "and its sidecar went with it (a sidecar without its trace is "
              "unattributable evidence that would otherwise never be deleted)");
    }
    pgwt_writer_destroy(&w);
    rm_rf(dir);

    /* An orphan sidecar — base file already gone, provably stale — is also
     * collected, so a half-cleaned trace dir does not accumulate them. */
    dir = fresh_dir("retain_orphan");
    struct pgwt_event_writer w2;
    CHECK(pgwt_writer_init(&w2, dir, 16, 0, NULL) == 0, "init");
    char orphan[800];
    snprintf(orphan, sizeof(orphan), "%s/2020-01-01_00.trace.lz4%s", dir,
             PGWT_OPEN_CKPT_SUFFIX);
    FILE *of = fopen(orphan, "wb");
    CHECK(of != NULL, "create the orphan sidecar");
    if (of) { fputs("x", of); fclose(of); }
    char cmd[900];
    snprintf(cmd, sizeof(cmd), "touch -t 202001010000 '%s'", orphan);
    if (system(cmd) != 0) { /* ignore */ }
    w2.retention_hours = 0;                /* age-based retention off */
    CHECK(pgwt_writer_cleanup_old_files(&w2) == 0, "retention pass");
    CHECK(access(orphan, F_OK) != 0,
          "an orphan sidecar whose trace file is gone must be collected");
    pgwt_writer_destroy(&w2);
    rm_rf(dir);
}

int main(void)
{
    printf("=== open-interval checkpoints (#424 deliverable 2) ===\n");
    snprintf(g_base, sizeof(g_base), "/tmp/pgwt_ckpt_XXXXXX");
    if (!mkdtemp(g_base)) {
        printf("FATAL: mkdtemp: %s\n", strerror(errno));
        return 1;
    }

    test_crc_is_the_standard_one();
    test_parse_refusals();
    test_suppression_rule();
    test_writer_identity_and_publication();
    test_recovery_carries_checkpoint();
    test_clean_close_drops_checkpoint();
    test_uncertified_close_keeps_checkpoint();
    test_stale_sidecar_refused();
    test_corrupt_and_truncated_sidecars_refused();
    test_empty_checkpoint_supersedes();
    test_absent_checkpoint_is_not_evidence();
    test_retention_takes_the_sidecar();

    rm_rf(g_base);
    printf("\n=== %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
