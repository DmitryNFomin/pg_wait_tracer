/* test_trace_closure.c — #424 deliverable 1: can a reader tell that a capture
 * ended uncleanly?
 *
 * The defect being gated: a footer proves nothing. Rotation writes one, crash
 * recovery writes one, a clean close writes one — so before this change a
 * trace whose daemon was SIGKILLed (losing every interval that was open at
 * that instant) was byte-indistinguishable from a complete capture.
 *
 * Three groups of cases, in this order:
 *
 *   A. the pure bit→verdict mapping, including every way the mapping could be
 *      talked into saying "clean" when it must not;
 *   B. the real writer lifecycle on real files: close, rotation, mid-capture,
 *      SIGKILL + recovery, SIGKILL twice, and a clean close whose label must
 *      SURVIVE being archived by recovery;
 *   C. the bypass suite — every way this gate could be made unable to SEE.
 *      A gate that cannot see must refuse (UNKNOWN / rc<0), never approve.
 *      These are the false negatives: an empty file, a truncated header, a
 *      missing file, a file that is absent rather than wrong, and a legacy
 *      file whose stray bits happen to look like a clean label.
 *
 * Server build (-DPGWT_SERVER): writer only, no BPF.
 */
#include "event_writer.h"
#include "trace_closure.h"
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

static char g_base_dir[300];

static void rm_rf(const char *dir)
{
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* ignore */ }
}

/* A fresh capture directory per subtest: a SIGKILLed subtest must never leave
 * state another subtest reads (issue #125's lesson). */
static const char *fresh_dir(const char *name)
{
    static char buf[600];
    snprintf(buf, sizeof(buf), "%s/%s", g_base_dir, name);
    rm_rf(buf);
    mkdir(buf, 0750);
    return buf;
}

static void push_n(struct pgwt_event_writer *w, int n, uint64_t t0)
{
    for (int i = 0; i < n; i++) {
        struct pgwt_trace_event e = {
            .timestamp_ns = t0 + (uint64_t)i * 1000000ULL,
            .pid = 1000 + (uint32_t)(i % 7),
            .old_event = 0,
            .new_event = 0x01000004,
            .duration_ns = 500000,
            .query_id = 42,
            .cpu_ns = 100000,
        };
        pgwt_writer_push_event(w, &e);
    }
}

/* Newest *.trace.lz4 in dir, or NULL. */
static const char *newest_archive(const char *dir)
{
    static char best[700];
    best[0] = '\0';
    time_t best_mtime = 0;
    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t n = strlen(de->d_name);
        if (n < 10 || strcmp(de->d_name + n - 10, ".trace.lz4") != 0)
            continue;
        char p[700];
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (best[0] == '\0' || st.st_mtime >= best_mtime) {
            best_mtime = st.st_mtime;
            snprintf(best, sizeof(best), "%s", p);
        }
    }
    closedir(d);
    return best[0] ? best : NULL;
}

static int count_archives(const char *dir)
{
    int n = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l >= 10 && strcmp(de->d_name + l - 10, ".trace.lz4") == 0)
            n++;
    }
    closedir(d);
    return n;
}

static enum pgwt_closure_status status_of(const char *path)
{
    enum pgwt_closure_status s;
    pgwt_trace_file_closure(path, &s);
    return s;
}

static const char *cur_path(const char *dir)
{
    static char buf[700];
    snprintf(buf, sizeof(buf), "%s/current.trace", dir);
    return buf;
}

/* Raw header flags of a file (no interpretation). -1 if unreadable. */
static long raw_flags(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    struct pgwt_trace_file_header h;
    long rc = (fread(&h, sizeof(h), 1, f) == 1) ? (long)h.flags : -1;
    fclose(f);
    return rc;
}

static int set_raw_flags(const char *path, uint16_t flags)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return -1;
    if (fseek(f, 6, SEEK_SET) != 0) { fclose(f); return -1; }
    int ok = fwrite(&flags, sizeof(flags), 1, f) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

/* ── A. the pure mapping ──────────────────────────────────── */

static void test_mapping(void)
{
    printf("\n[A] bits -> verdict\n");

    const uint16_t L = PGWT_FLAG_LZ4;
    const uint16_t T = PGWT_FLAG_CLOSURE_TRACKED;
    const uint16_t C = PGWT_FLAG_CLOSED_CLEAN;
    const uint16_t R = PGWT_FLAG_ROTATED;
    const uint16_t V = PGWT_FLAG_RECOVERED;

    /* A legacy file carries only the LZ4 bit. It is NOT clean — it is
     * unknown. This is the rule the brief names explicitly. */
    CHECK(pgwt_closure_from_flags(L) == PGWT_CLOSURE_UNKNOWN,
          "legacy flags must read UNKNOWN, got %s",
          pgwt_closure_name(pgwt_closure_from_flags(L)));
    CHECK(pgwt_closure_open_intervals_may_be_missing(PGWT_CLOSURE_UNKNOWN, false),
          "UNKNOWN must warn about open intervals");

    /* BYPASS: a pre-#424 file (or a bit-flip) whose flags happen to contain
     * the value we now call CLEAN, WITHOUT the capability bit. If the
     * capability bit were not checked first, this file would read clean. */
    CHECK(pgwt_closure_from_flags((uint16_t)(L | C)) == PGWT_CLOSURE_UNKNOWN,
          "clean bit without the capability bit must NOT read clean (got %s)",
          pgwt_closure_name(pgwt_closure_from_flags((uint16_t)(L | C))));
    CHECK(pgwt_closure_from_flags((uint16_t)(L | C | R | V)) ==
          PGWT_CLOSURE_UNKNOWN,
          "no capability bit: every other bit is noise, must read UNKNOWN");

    CHECK(pgwt_closure_from_flags((uint16_t)(L | T)) == PGWT_CLOSURE_IN_PROGRESS,
          "tracked, unfinalized -> IN_PROGRESS");
    CHECK(!pgwt_closure_open_intervals_may_be_missing(PGWT_CLOSURE_IN_PROGRESS, true),
          "a LIVE unfinalized file is a running capture, not a loss");
    CHECK(pgwt_closure_open_intervals_may_be_missing(PGWT_CLOSURE_IN_PROGRESS, false),
          "an ARCHIVED unfinalized file must warn");

    CHECK(pgwt_closure_from_flags((uint16_t)(L | T | C)) == PGWT_CLOSURE_CLEAN,
          "tracked + clean -> CLEAN");
    CHECK(!pgwt_closure_open_intervals_may_be_missing(PGWT_CLOSURE_CLEAN, false),
          "CLEAN must not warn");

    CHECK(pgwt_closure_from_flags((uint16_t)(L | T | R)) == PGWT_CLOSURE_ROTATED,
          "tracked + rotated -> ROTATED");
    CHECK(!pgwt_closure_open_intervals_may_be_missing(PGWT_CLOSURE_ROTATED, false),
          "ROTATED must not warn: the capture continues in the next file");

    CHECK(pgwt_closure_from_flags((uint16_t)(L | T | V)) == PGWT_CLOSURE_UNCLEAN,
          "tracked + recovered -> UNCLEAN");
    CHECK(pgwt_closure_open_intervals_may_be_missing(PGWT_CLOSURE_UNCLEAN, false),
          "UNCLEAN must warn");

    /* Contradictions resolve AWAY from clean, never towards it. */
    CHECK(pgwt_closure_from_flags((uint16_t)(L | T | C | V)) == PGWT_CLOSURE_UNCLEAN,
          "clean+recovered is a close that did not take effect -> UNCLEAN");
    CHECK(pgwt_closure_from_flags((uint16_t)(L | T | C | R)) == PGWT_CLOSURE_UNCLEAN,
          "clean+rotated are mutually exclusive -> UNCLEAN, not clean");
    CHECK(pgwt_closure_from_flags(0xFFFF) == PGWT_CLOSURE_UNCLEAN,
          "an all-ones flags word must never read clean");

    /* Forward compatibility: a bit a future writer adds must not disturb a
     * verdict this build can read. */
    CHECK(pgwt_closure_from_flags((uint16_t)(L | T | C | 0x8000)) ==
          PGWT_CLOSURE_CLEAN,
          "an unknown spare bit must not change a CLEAN verdict");

    /* The name surface the API/UI will serialize. */
    CHECK(strcmp(pgwt_closure_name(PGWT_CLOSURE_UNKNOWN), "unknown") == 0 &&
          strcmp(pgwt_closure_name(PGWT_CLOSURE_UNCLEAN), "unclean") == 0 &&
          strcmp(pgwt_closure_name(PGWT_CLOSURE_CLEAN), "clean") == 0 &&
          strcmp(pgwt_closure_name(PGWT_CLOSURE_ROTATED), "rotated") == 0 &&
          strcmp(pgwt_closure_name(PGWT_CLOSURE_IN_PROGRESS), "in_progress") == 0,
          "status tokens are part of the interface and must not drift");
}

/* ── B. the writer lifecycle on real files ────────────────── */

static void test_clean_close(void)
{
    printf("\n[B1] a clean close certifies the file\n");
    const char *dir = fresh_dir("clean");
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "init");
    push_n(&w, 100, 1000000000ULL);

    /* Mid-capture: tracked but not finalized, and a LIVE file must not be
     * reported as a loss. */
    pgwt_writer_publish(&w);
    CHECK(status_of(cur_path(dir)) == PGWT_CLOSURE_IN_PROGRESS,
          "mid-capture file must read IN_PROGRESS, got %s",
          pgwt_closure_name(status_of(cur_path(dir))));

    pgwt_writer_close(&w);
    CHECK(status_of(cur_path(dir)) == PGWT_CLOSURE_CLEAN,
          "after a clean close the file must read CLEAN, got %s",
          pgwt_closure_name(status_of(cur_path(dir))));
    CHECK((raw_flags(cur_path(dir)) & PGWT_FLAG_LZ4) != 0,
          "the LZ4 codec bit must survive the flags rewrite");
    pgwt_writer_destroy(&w);
    rm_rf(dir);
}

static void test_rotation_is_not_a_clean_close(void)
{
    printf("\n[B2] rotation says ROTATED, not CLEAN\n");
    const char *dir = fresh_dir("rot");
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "init");
    push_n(&w, 100, 1000000000ULL);
    pgwt_writer_publish(&w);

    /* Force the hour check to fire without waiting for an hour: the field the
     * rotation compares against is the writer's own recorded hour. No sleep,
     * no wall-clock dependency. */
    w.current_hour = -99;
    CHECK(pgwt_writer_check_rotation(&w) == 0, "rotation");

    const char *arch = newest_archive(dir);
    CHECK(arch != NULL, "rotation must produce an archive");
    if (arch) {
        CHECK(status_of(arch) == PGWT_CLOSURE_ROTATED,
              "a rotated archive must read ROTATED, got %s",
              pgwt_closure_name(status_of(arch)));
        CHECK(!pgwt_closure_open_intervals_may_be_missing(status_of(arch), false),
              "rotation loses nothing: the capture continues in the next file");
    }
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    rm_rf(dir);
}

/* Child: write `n` events into `dir`, publish them, then die the way a
 * SIGKILLed daemon dies — no close, no footer, no flags. */
static pid_t crash_child(const char *dir, int n, uint64_t t0)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 16, 0, NULL) != 0)
        _exit(2);
    push_n(&w, n, t0);
    pgwt_writer_publish(&w);
    raise(SIGKILL);
    _exit(3);   /* unreachable */
}

static int reap_killed(pid_t pid)
{
    int st = 0;
    if (waitpid(pid, &st, 0) != pid)
        return 0;
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL;
}

static void test_crash_reads_unclean(void)
{
    printf("\n[B3] SIGKILL -> the archive says UNCLEAN\n");
    const char *dir = fresh_dir("crash");

    pid_t pid = crash_child(dir, 200, 2000000000ULL);
    CHECK(pid > 0, "fork");
    CHECK(reap_killed(pid), "the child must actually have died by SIGKILL "
          "(otherwise this test proves nothing)");

    /* The leftover live file: tracked, never finalized. */
    CHECK(status_of(cur_path(dir)) == PGWT_CLOSURE_IN_PROGRESS,
          "the killed daemon's current.trace must read IN_PROGRESS, got %s",
          pgwt_closure_name(status_of(cur_path(dir))));

    /* The next daemon start recovers it. */
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "restart");
    const char *arch = newest_archive(dir);
    CHECK(arch != NULL, "recovery must archive the crashed file");
    if (arch) {
        CHECK(status_of(arch) == PGWT_CLOSURE_UNCLEAN,
              "a recovered crash archive must read UNCLEAN, got %s",
              pgwt_closure_name(status_of(arch)));
        CHECK(pgwt_closure_open_intervals_may_be_missing(status_of(arch), false),
              "UNCLEAN must tell the reader open intervals may be missing");
    }
    pgwt_writer_destroy(&w);
    rm_rf(dir);
}

static void test_repeated_restart(void)
{
    printf("\n[B4] crash, restart, crash again\n");
    const char *dir = fresh_dir("repeat");

    for (int round = 0; round < 2; round++) {
        pid_t pid = crash_child(dir, 120, 3000000000ULL + round * 1000000000ULL);
        CHECK(reap_killed(pid), "round %d child killed", round);
        struct pgwt_event_writer w;
        CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "round %d restart", round);
        pgwt_writer_destroy(&w);
    }
    CHECK(count_archives(dir) == 2,
          "two crashes must leave two archives, found %d", count_archives(dir));

    /* Every archive — not just the newest — must read UNCLEAN. A guard that
     * only inspects the last file would pass while an earlier crash was
     * silently relabelled. */
    DIR *d = opendir(dir);
    int unclean = 0, seen = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t l = strlen(de->d_name);
            if (l < 10 || strcmp(de->d_name + l - 10, ".trace.lz4") != 0)
                continue;
            char p[700];
            snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
            seen++;
            if (status_of(p) == PGWT_CLOSURE_UNCLEAN) unclean++;
        }
        closedir(d);
    }
    CHECK(seen == 2 && unclean == 2,
          "both crash archives must read UNCLEAN (%d of %d did)", unclean, seen);
    rm_rf(dir);
}

static void test_clean_label_survives_recovery(void)
{
    printf("\n[B5] a clean close stays CLEAN after recovery archives it\n");
    const char *dir = fresh_dir("preserve");

    /* A graceful shutdown leaves current.trace in place with its footer; the
     * NEXT daemon start is what archives it. If recovery stamped every file
     * it touched as crashed, every graceful shutdown would be libelled. */
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "init");
    push_n(&w, 80, 4000000000ULL);
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);

    struct pgwt_event_writer w2;
    CHECK(pgwt_writer_init(&w2, dir, 16, 0, NULL) == 0, "restart");
    pgwt_writer_destroy(&w2);

    const char *arch = newest_archive(dir);
    CHECK(arch != NULL, "recovery must archive the cleanly closed file");
    if (arch)
        CHECK(status_of(arch) == PGWT_CLOSURE_CLEAN,
              "a cleanly closed file must still read CLEAN after recovery "
              "archives it, got %s", pgwt_closure_name(status_of(arch)));
    rm_rf(dir);
}

static void test_clean_contradicted_by_watermark(void)
{
    printf("\n[B6] CLEAN over missing committed blocks -> UNCLEAN\n");
    const char *dir = fresh_dir("liar");

    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "init");
    push_n(&w, 100, 5000000000ULL);
    pgwt_writer_publish(&w);
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    CHECK(status_of(cur_path(dir)) == PGWT_CLOSURE_CLEAN, "closed clean");

    /* Now claim more committed blocks than the file contains — the shape of a
     * close that reported success over data that never reached the disk. */
    char meta[700];
    snprintf(meta, sizeof(meta), "%s/current.trace.meta", dir);
    FILE *mf = fopen(meta, "w");
    CHECK(mf != NULL, "write meta");
    if (mf) { fprintf(mf, "99\n"); fclose(mf); }

    struct pgwt_event_writer w2;
    CHECK(pgwt_writer_init(&w2, dir, 16, 0, NULL) == 0, "restart");
    pgwt_writer_destroy(&w2);

    const char *arch = newest_archive(dir);
    CHECK(arch != NULL, "archived");
    if (arch)
        CHECK(status_of(arch) == PGWT_CLOSURE_UNCLEAN,
              "a CLEAN label contradicted by the committed watermark must "
              "read UNCLEAN, got %s", pgwt_closure_name(status_of(arch)));
    rm_rf(dir);
}

static void test_failed_flush_is_not_certified(void)
{
    printf("\n[B7] a close whose flush failed must NOT be certified\n");
    const char *dir = fresh_dir("badflush");

    /* The bit means "every buffer flushed, footer written, fsync returned 0".
     * Without an injected failure nothing in this suite can tell that promise
     * apart from "the bit is always set at close" — which is how the pre-#424
     * code behaved, discarding all four return values.
     *
     * Injection 1: starve the compression output buffer, so the pending
     * block's LZ4_compress_default fails and flush_block returns -1. No
     * filesystem trickery, no timing: a deterministic failure of one step. */
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "init");
    push_n(&w, 50, 8000000000ULL);          /* pending, not yet written */
    CHECK(access(cur_path(dir), F_OK) == 0, "the file exists (lazy open ran)");
    w.compress_buf_size = 1;                 /* every block write now fails */
    pgwt_writer_close(&w);
    CHECK(status_of(cur_path(dir)) != PGWT_CLOSURE_CLEAN,
          "a close whose flush failed must not read CLEAN (got %s)",
          pgwt_closure_name(status_of(cur_path(dir))));
    CHECK(status_of(cur_path(dir)) == PGWT_CLOSURE_IN_PROGRESS,
          "it must read IN_PROGRESS — uncertified, not mislabelled (got %s)",
          pgwt_closure_name(status_of(cur_path(dir))));
    pgwt_writer_destroy(&w);
    rm_rf(dir);

    /* Injection 2: the file descriptor is gone under the stream, so the
     * footer write / fflush / fsync chain fails instead. Same promise, a
     * different step broken. */
    dir = fresh_dir("badfsync");
    struct pgwt_event_writer w2;
    CHECK(pgwt_writer_init(&w2, dir, 16, 0, NULL) == 0, "init");
    push_n(&w2, 50, 9000000000ULL);
    pgwt_writer_publish(&w2);
    CHECK(close(fileno(w2.fp)) == 0, "pull the fd out from under the stream");
    pgwt_writer_close(&w2);
    CHECK(status_of(cur_path(dir)) != PGWT_CLOSURE_CLEAN,
          "a close whose fsync failed must not read CLEAN (got %s)",
          pgwt_closure_name(status_of(cur_path(dir))));
    pgwt_writer_destroy(&w2);
    rm_rf(dir);
}

/* ── C. bypass suite: every way this gate could fail to SEE ── */

static void test_legacy_file_reads_unknown(void)
{
    printf("\n[C1] a legacy file reads UNKNOWN, before and after recovery\n");
    const char *dir = fresh_dir("legacy");

    /* Build a real, complete trace file, then strip the capability bit from
     * its header — byte-for-byte what a pre-#424 daemon left behind. */
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "init");
    push_n(&w, 60, 6000000000ULL);
    pgwt_writer_publish(&w);
    pgwt_writer_destroy(&w);     /* no close: as a pre-#424 crash would leave it */
    CHECK(set_raw_flags(cur_path(dir), PGWT_FLAG_LZ4) == 0, "strip to legacy");
    CHECK(status_of(cur_path(dir)) == PGWT_CLOSURE_UNKNOWN,
          "a legacy header must read UNKNOWN, got %s",
          pgwt_closure_name(status_of(cur_path(dir))));

    struct pgwt_event_writer w2;
    CHECK(pgwt_writer_init(&w2, dir, 16, 0, NULL) == 0, "restart");
    pgwt_writer_destroy(&w2);

    const char *arch = newest_archive(dir);
    CHECK(arch != NULL, "legacy file archived by recovery");
    if (arch) {
        /* Not CLEAN (we never verified a close) and not UNCLEAN either (we
         * have no evidence of a crash). Unknown is the honest answer, and it
         * still warns. */
        CHECK(status_of(arch) == PGWT_CLOSURE_UNKNOWN,
              "a recovered legacy file must stay UNKNOWN — never fabricated "
              "in either direction — got %s",
              pgwt_closure_name(status_of(arch)));
        CHECK(pgwt_closure_open_intervals_may_be_missing(status_of(arch), false),
              "UNKNOWN still warns the reader");
    }
    rm_rf(dir);
}

static void test_unreadable_inputs_refuse(void)
{
    printf("\n[C2] no file / empty / truncated / garbage -> refuse\n");
    const char *dir = fresh_dir("bypass");
    char p[700];
    enum pgwt_closure_status s;

    /* Absent rather than wrong: the file this gate is asked about does not
     * exist at all. It must refuse, not approve. */
    snprintf(p, sizeof(p), "%s/does-not-exist.trace", dir);
    CHECK(pgwt_trace_file_closure(p, &s) == -1 && s == PGWT_CLOSURE_UNKNOWN,
          "a missing file must refuse (rc<0, UNKNOWN), got rc/%s",
          pgwt_closure_name(s));
    CHECK(access(p, F_OK) != 0,
          "the missing-file case must really be missing, or it proves nothing");

    /* NULL path: a caller that lost the path must not get a verdict. */
    s = PGWT_CLOSURE_CLEAN;
    CHECK(pgwt_trace_file_closure(NULL, &s) == -1 && s == PGWT_CLOSURE_UNKNOWN,
          "a NULL path must refuse and must OVERWRITE the caller's variable");

    /* Empty file. */
    snprintf(p, sizeof(p), "%s/empty.trace", dir);
    FILE *f = fopen(p, "wb"); if (f) fclose(f);
    CHECK(pgwt_trace_file_closure(p, &s) == -1 && s == PGWT_CLOSURE_UNKNOWN,
          "an empty file must refuse, got %s", pgwt_closure_name(s));

    /* Truncated header — fewer bytes than the struct. */
    snprintf(p, sizeof(p), "%s/short.trace", dir);
    f = fopen(p, "wb");
    if (f) { uint32_t m = PGWT_TRACE_MAGIC; fwrite(&m, 4, 1, f); fclose(f); }
    CHECK(pgwt_trace_file_closure(p, &s) == -1 && s == PGWT_CLOSURE_UNKNOWN,
          "a truncated header must refuse, got %s", pgwt_closure_name(s));

    /* Right size, wrong magic, every flag bit set: the adversarial input for
     * a gate that reads flags before checking it is looking at a trace. */
    snprintf(p, sizeof(p), "%s/garbage.trace", dir);
    f = fopen(p, "wb");
    if (f) {
        struct pgwt_trace_file_header h;
        memset(&h, 0xFF, sizeof(h));
        fwrite(&h, sizeof(h), 1, f);
        fclose(f);
    }
    CHECK(pgwt_trace_file_closure(p, &s) == -1 && s == PGWT_CLOSURE_UNKNOWN,
          "a non-trace file with all bits set must refuse, got %s",
          pgwt_closure_name(s));

    /* A directory where a file was expected. */
    CHECK(pgwt_trace_file_closure(dir, &s) == -1 && s == PGWT_CLOSURE_UNKNOWN,
          "a directory must refuse, got %s", pgwt_closure_name(s));

    rm_rf(dir);
}

static void test_nothing_captured_is_not_clean(void)
{
    printf("\n[C3] a capture that never opened a file is not a clean capture\n");
    const char *dir = fresh_dir("nofile");

    /* The writer opens lazily. init + close with no events writes nothing —
     * the gate must report "no file" rather than inheriting a verdict. */
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "init");
    CHECK(pgwt_writer_close(&w) == 0, "close with no events");
    pgwt_writer_destroy(&w);

    CHECK(access(cur_path(dir), F_OK) != 0,
          "no events must mean no current.trace at all");
    enum pgwt_closure_status s;
    CHECK(pgwt_trace_file_closure(cur_path(dir), &s) == -1 &&
          s == PGWT_CLOSURE_UNKNOWN,
          "an absent capture must refuse, not read CLEAN (got %s)",
          pgwt_closure_name(s));
    CHECK(count_archives(dir) == 0, "nothing was captured, nothing archived");
    rm_rf(dir);
}

static void test_crash_during_publication(void)
{
    printf("\n[C4] killed mid-publication: partial data, honest label\n");
    const char *dir = fresh_dir("midpub");

    /* A child that dies with a FULL event buffer that was never flushed:
     * blocks on disk, more events pending, meta out of date — the state a
     * kill during block/meta publication leaves. The file must not read
     * clean, and recovery must archive what did land. */
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid == 0) {
        struct pgwt_event_writer w;
        if (pgwt_writer_init(&w, dir, 16, 0, NULL) != 0)
            _exit(2);
        push_n(&w, PGWT_BLOCK_EVENTS + 500, 7000000000ULL);  /* 1 block out, rest pending */
        raise(SIGKILL);
        _exit(3);
    }
    CHECK(reap_killed(pid), "child killed mid-publication");

    CHECK(status_of(cur_path(dir)) == PGWT_CLOSURE_IN_PROGRESS,
          "the half-published file must read IN_PROGRESS, got %s",
          pgwt_closure_name(status_of(cur_path(dir))));

    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, dir, 16, 0, NULL) == 0, "restart");
    pgwt_writer_destroy(&w);
    const char *arch = newest_archive(dir);
    CHECK(arch != NULL, "recovery archived the partial file");
    if (arch)
        CHECK(status_of(arch) == PGWT_CLOSURE_UNCLEAN,
              "a file killed mid-publication must read UNCLEAN, got %s",
              pgwt_closure_name(status_of(arch)));
    rm_rf(dir);
}

int main(void)
{
    printf("=== trace closure status (#424 deliverable 1) ===\n");

    snprintf(g_base_dir, sizeof(g_base_dir), "/tmp/pgwt_closure_XXXXXX");
    if (!mkdtemp(g_base_dir)) {
        printf("FATAL: mkdtemp: %s\n", strerror(errno));
        return 1;
    }

    test_mapping();
    test_clean_close();
    test_rotation_is_not_a_clean_close();
    test_crash_reads_unclean();
    test_repeated_restart();
    test_clean_label_survives_recovery();
    test_clean_contradicted_by_watermark();
    test_failed_flush_is_not_certified();
    test_legacy_file_reads_unknown();
    test_unreadable_inputs_refuse();
    test_nothing_captured_is_not_clean();
    test_crash_during_publication();

    rm_rf(g_base_dir);

    printf("\n=== %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
