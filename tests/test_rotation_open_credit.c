/* test_rotation_open_credit.c — #430: an hourly rotation must not pause
 * open-interval crediting, and the pause's cost is NOT attribution-only.
 *
 * WHAT THE DEFECT WAS, MEASURED RATHER THAN DESCRIBED
 * ─────────────────────────────────────────────────────────────────────────
 * pgwt_summary_check_rotation archives the file and leaves w->fp NULL on
 * purpose, so open_summary_file fills it in LAZILY on the next PUSHED record.
 * writer_seal_through refused outright on a NULL fp, so every daemon tick
 * between the rotation and the next pushed record sealed NOTHING: the
 * per-second credit of every open interval simply stopped.
 *
 * Issue #430 called that "attribution-only -- totals over a window
 * containing the close are unaffected". THAT IS FALSE, and §1 is the
 * measurement: pgwt_summary_close() also returns early on a NULL fp, so a
 * capture that STOPS inside the pause never writes those seconds at all. On
 * this fixture 11 s of a continuously-waiting backend elapse and the parent
 * tree puts 5000 ms of it on disk. The demo capture is 35 minutes
 * (tests/demo_rehearsal.sh) and rotation is on the wall-clock hour, so
 * whether that happens is decided by the MINUTE the capture starts.
 *
 * THE CONSERVATION STATEMENT, and why it cannot pass vacuously
 * ─────────────────────────────────────────────────────────────────────────
 * For a backend that waits continuously from T0 to T1 across a rotation,
 *
 *     Sigma over every record on disk of that pid's db_time_ns
 *         == the wall of the completed seconds the ticks could seal
 *
 * EXACTLY -- no tolerance, no floor. The two sides come from different
 * places: the left is summed off DISK through the shipping reader, the right
 * is derived from the fixture's literal tick schedule
 * (pgwt_summary_flush_completed's own rule: a tick at T0 + k s + 200 ms
 * seals every second strictly below T0 + k s). Neither side is a writer
 * counter, so the identity cannot be satisfied by comparing one sum with
 * itself. A floor ("at least 2000 ms") would have passed on the parent tree.
 *
 * It is also placement-sensitive, not just total-sensitive: §1 asserts the
 * PER-FILE split (5 s before the rotation, 6 s after) and that every record
 * carries exactly 1000 ms, so a "fix" that re-credited the interval from its
 * start into the new file, double-counted the rotation's own second, or
 * dumped the whole pause into one record fails here rather than passing on
 * the total.
 *
 * THE ONE INPUT THAT GREENS A CARELESS GATE (§2)
 * ─────────────────────────────────────────────────────────────────────────
 * Push a record immediately after the rotation and the lazy reopen happens on
 * the PUSH path, the owed seconds are all sealed in one catch-up pass, and
 * the total is exact with the defect fully present. That is why
 * tests/test_open_cpu_accounting.c's rotation leg (§3c) could not see #430:
 * it pushes the closing record right after. §2 runs exactly that fixture and
 * asserts it is 11 s on BOTH trees -- so it is the reachability probe for
 * §1: if §2 were red too, §1's red would be the harness, not the pause.
 *
 * THE BYPASS SUITE (§3-§9) — one leg per way this gate's DETECTION could be
 * made unreachable, which is the failure mode that actually ships. A gate
 * that cannot see must REFUSE, never approve.
 *   §3 NEVER ANCHORED. A writer that has never opened a file has no clock
 *      anchor, so mono_to_wall would stamp garbage. Notes and ticks must
 *      invent NOTHING and create no file. This is the leg that goes red if
 *      the fix reopens unconditionally instead of only once it knows it has
 *      a second to write.
 *   §4 NOTHING OPEN. Rotate with no open interval and tick for seconds: no
 *      file may be created and no empty record invented. Red input: an
 *      eager reopen on the rotation path or at every tick.
 *   §5 THE REOPEN FAILS (a dependency exiting non-zero). A directory planted
 *      at <dir>/current.summary makes open_summary_file fail. Nothing may be
 *      credited, the failure must be COUNTED, and the seconds must stay OWED
 *      -- removing the obstacle must place all of them. A gate that cannot
 *      write must not pretend it did.
 *   §6 THE NOTE GOES STALE during the pause (a pid that died without an EXIT
 *      record). Crediting must stop within PGWT_SUMMARY_OPEN_STALE_NS and
 *      the expiry must be COUNTED -- the fix must not fabricate time for a
 *      process that is gone.
 *   §7 THE ROTATION IS ABSENT rather than wrong. The same fixture without
 *      the forced rotation must give the same 11 s. Without this, §1's
 *      number could come from anything.
 *   §8 EMPTY INPUT. A missing trace dir and an empty one must make the
 *      read-back REFUSE. A gate that sums zero records agrees with
 *      everything.
 *   §9 A WRONG VERSION ON DISK / A DISABLED WRITER. Evidence gathered under
 *      a different accounting rule must be refused, and a writer that wrote
 *      nothing must FAIL the harness rather than read as agreement.
 *
 * Server build (no BPF, no live PG), ~0.1 s. Rotation is forced by setting
 * w->current_hour = -1, the same deterministic seam
 * tests/test_open_cpu_accounting.c uses: pgwt_summary_check_rotation
 * compares the hour it last saw against localtime(), so a mismatched cached
 * value drives the REAL rotation path without waiting for a wall-clock hour.
 * The clock anchor is PINNED and every timestamp is a literal offset from
 * it, so no assertion depends on a real clock or on scheduling. */
#define _GNU_SOURCE
#include "summary_writer.h"
#include "summary_reader.h"
#include "wait_event.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures;
static int checks;

#define CHECK(cond, fmt, ...) do {                                        \
    checks++;                                                             \
    if (!(cond)) {                                                        \
        failures++;                                                       \
        printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                       \
    } else {                                                              \
        printf("  ok:   " fmt "\n", ##__VA_ARGS__);                       \
    }                                                                     \
} while (0)

#define S  1000000000ULL
#define MS 1000000ULL

#define PID_A        2001u      /* the backend that waits across the boundary */
#define PID_SEED     2002u      /* opens the file, and nothing else           */
#define BASE_MONO    (20000ULL * S)      /* second-aligned synthetic clock */

static const uint32_t EV_LOCK = (PG_WAIT_LOCK << 24) | 0;

/* ── fixture plumbing ─────────────────────────────────────────────────── */

static const char *fresh_dir(const char *name)
{
    static char buf[512];
    snprintf(buf, sizeof(buf), "/tmp/pgwt_rotcredit_%s_%d", name, (int)getpid());
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", buf, buf);
    if (system(cmd) != 0)
        return NULL;
    return buf;
}

struct wr {
    struct pgwt_summary_writer *w;
    char dir[512];
};

static int wr_open(struct wr *x, const char *name)
{
    const char *d = fresh_dir(name);
    if (!d) return -1;
    snprintf(x->dir, sizeof(x->dir), "%s", d);
    x->w = calloc(1, sizeof(*x->w));
    if (!x->w) return -1;
    if (pgwt_summary_writer_init(x->w, x->dir, 24, NULL) != 0)
        return -1;
    pgwt_summary_pin_clock(x->w, BASE_MONO, BASE_MONO);
    return 0;
}

static void wr_free(struct wr *x)
{
    pgwt_summary_destroy(x->w);
    free(x->w);
    x->w = NULL;
}

static struct pgwt_trace_event mkev(uint32_t pid, uint64_t ts, uint64_t dur,
                                    uint32_t old_ev, uint32_t new_ev)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.pid = pid;
    e.timestamp_ns = ts;
    e.duration_ns = dur;
    e.old_event = old_ev;
    e.new_event = new_ev;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

/* ── read-back ────────────────────────────────────────────────────────── */

#define RB_MAX_ROWS 128

struct sec_row {
    uint64_t wall;
    uint64_t lock_ns;        /* class_ns[PGWT_CLASS_LOCK] of this record */
    uint64_t sess_db_ns;     /* want_pid's db_time_ns in this record     */
};

struct readback {
    struct sec_row rows[RB_MAX_ROWS];
    int n;
    int overflow;
    uint32_t want_pid;
};

static int rb_visit(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct readback *rb = vctx;
    if (rb->n >= RB_MAX_ROWS) { rb->overflow = 1; return 1; }
    struct sec_row *r = &rb->rows[rb->n++];
    memset(r, 0, sizeof(*r));
    r->wall    = rec->second_wall_ns;
    r->lock_ns = rec->class_ns[PGWT_CLASS_LOCK];
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
        if (rec->sessions[i].pid == rb->want_pid)
            r->sess_db_ns = rec->sessions[i].db_time_ns;
    return 0;
}

/* Read every record under `dir`. Returns the count, or -1 -- NEVER
 * 0-as-success: "no records" must FAIL the caller, not read as agreement
 * (bypass §8). */
static int read_back(const char *dir, struct readback *rb)
{
    rb->n = 0;
    rb->overflow = 0;
    int rc = pgwt_visit_summaries(dir, 0, 0, rb_visit, rb);
    if (rc <= 0 || rb->overflow)
        return -1;
    return rb->n;
}

static uint64_t rb_sum_sess(const struct readback *rb)
{
    uint64_t t = 0;
    for (int i = 0; i < rb->n; i++) t += rb->rows[i].sess_db_ns;
    return t;
}

/* How many records credit this pid something OTHER than exactly one second?
 * A catch-up pass that dumped the whole pause into one record, or a re-credit
 * from the interval's start, shows up here and not in the total. */
static int rb_rows_not_one_second(const struct readback *rb)
{
    int n = 0;
    for (int i = 0; i < rb->n; i++)
        if (rb->rows[i].sess_db_ns != 0 && rb->rows[i].sess_db_ns != S)
            n++;
    return n;
}

/* Per-FILE sum of want_pid's db_time, over the files the shipping scanner
 * lists, in ascending time order. Returns the file count or -1. */
struct per_file {
    int      nfiles;
    uint64_t sess_ns[8];
    int      nrecs[8];
    char     name[8][128];
};

static int read_per_file(const char *dir, uint32_t want_pid,
                         struct per_file *out)
{
    memset(out, 0, sizeof(*out));
    struct pgwt_summary_file_entry files[16];
    int nf = pgwt_scan_summary_files(dir, files, 16);
    if (nf <= 0)
        return -1;
    if (nf > 8)
        return -1;
    for (int f = 0; f < nf; f++) {
        struct pgwt_summary_reader r;
        if (pgwt_summary_reader_open(&r, files[f].path) != 0)
            return -1;
        const char *slash = strrchr(files[f].path, '/');
        snprintf(out->name[f], sizeof(out->name[f]), "%s",
                 slash ? slash + 1 : files[f].path);
        struct pgwt_summary_accum *rec = calloc(1, sizeof(*rec));
        if (!rec) { pgwt_summary_reader_close(&r); return -1; }
        for (int b = 0; b < r.num_blocks; b++) {
            if (pgwt_summary_reader_decode_block(&r, b, rec) != 0) {
                free(rec);
                pgwt_summary_reader_close(&r);
                return -1;
            }
            out->nrecs[f]++;
            for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
                if (rec->sessions[i].pid == want_pid)
                    out->sess_ns[f] += rec->sessions[i].db_time_ns;
        }
        free(rec);
        pgwt_summary_reader_close(&r);
    }
    out->nfiles = nf;
    return nf;
}

/* The index of the one file whose name ENDS with `suffix`, or -1 if there is
 * not exactly one. Ambiguity REFUSES rather than picking the first match. */
static int pf_index_of(const struct per_file *pf, const char *suffix)
{
    int found = -1;
    size_t sl = strlen(suffix);
    for (int f = 0; f < pf->nfiles; f++) {
        size_t nl = strlen(pf->name[f]);
        if (nl < sl || strcmp(pf->name[f] + nl - sl, suffix) != 0)
            continue;
        if (found >= 0)
            return -1;
        found = f;
    }
    return found;
}

static int count_summary_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)))
        if (strstr(e->d_name, ".summary"))
            n++;
    closedir(d);
    return n;
}

/* ARCHIVED files only (*.summary.lz4): the rotation's own witness, which must
 * not depend on whether the new file was reopened. */
static int count_archived(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        const char *s = strstr(e->d_name, ".summary.lz4");
        if (s && s[12] == '\0')
            n++;
    }
    closedir(d);
    return n;
}

/* ── the fixture every leg shares ─────────────────────────────────────── */

/* One daemon tick, in the daemon's own order (src/daemon.c: note the opens,
 * pgwt_summary_flush_completed, then pgwt_summary_check_rotation). */
static void tick(struct wr *x, int k, bool note, bool rotate)
{
    uint64_t now = BASE_MONO + (uint64_t)k * S + 200 * MS;
    if (note)
        pgwt_summary_note_open(x->w, PID_A, EV_LOCK, BASE_MONO + 1 * S, 0,
                               now);
    pgwt_summary_flush_completed(x->w, now);
    if (rotate) {
        x->w->current_hour = -1;              /* force the hour to differ */
        pgwt_summary_check_rotation(x->w);
    }
}

/* The seed PUSH exists only to open the file and anchor the writer, the way
 * the first real event does in the daemon. It belongs to a DIFFERENT pid, so
 * every assertion below -- which sums PID_A's sessions[] row -- is isolated
 * from it. */
static void seed(struct wr *x)
{
    struct pgwt_trace_event e =
        mkev(PID_SEED, BASE_MONO + 1 * S, 1 * S, EV_LOCK, 0);
    pgwt_summary_push_event(x->w, &e, 0, e.old_event);
}

/* Ticks 2..LAST_TICK, rotating at ROT_TICK. flush_completed's rule (a tick at
 * BASE + k s + 200 ms seals every second strictly below BASE + k s) makes the
 * sealable span seconds 1..LAST_TICK-1 -- the interval starts at BASE + 1 s
 * -- so the conservation target is a literal consequence of the schedule and
 * not of any writer counter. */
#define ROT_TICK   6
#define LAST_TICK  12
#define WANT_NS    ((uint64_t)(LAST_TICK - 1) * S)          /* 11 s */
#define WANT_BEFORE ((uint64_t)(ROT_TICK - 1) * S)          /*  5 s */
#define WANT_AFTER  (WANT_NS - WANT_BEFORE)                 /*  6 s */

static void run_ticks(struct wr *x, bool rotate)
{
    for (int k = 2; k <= LAST_TICK; k++)
        tick(x, k, true, rotate && k == ROT_TICK);
}

/* ── §1 CONSERVATION ACROSS A ROTATION, WITH NO RECORD PUSHED AFTER ───── */

static void test_conservation_no_push(void)
{
    struct wr x;
    if (wr_open(&x, "cons") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    seed(&x);
    int files_before = count_summary_files(x.dir);
    CHECK(files_before == 1 || files_before == 2,
          "SETUP: the seed opened a file (%d summary file(s) incl. meta)",
          files_before);

    /* THE ROTATION MUST HAVE HAPPENED, and that has to be witnessed
     * INDEPENDENTLY of whether the fix works. pgwt_summary_check_rotation
     * returns 0 and does nothing when w->fp is already NULL, so a leg that
     * silently exercised no rotation would GREEN this whole file. The witness
     * is taken the instant it fires: the file handle is gone and an archived
     * .summary.lz4 exists. Counting files at the END would instead be a
     * restatement of the fix. */
    for (int k = 2; k <= ROT_TICK; k++)
        tick(&x, k, true, k == ROT_TICK);
    CHECK(x.w->fp == NULL,
          "SETUP: the rotation really fired — it closed the file handle");
    CHECK(count_archived(x.dir) == 1,
          "SETUP: the rotation really fired — %d archived .summary.lz4 on "
          "disk", count_archived(x.dir));
    for (int k = ROT_TICK + 1; k <= LAST_TICK; k++)
        tick(&x, k, true, false);

    /* NOTHING is pushed after the rotation and the writer is NOT closed
     * first: this is the state a daemon is in while it ticks. */
    struct readback rb = { .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "conservation: records read back (%d) — a refusal here would "
          "mean the gate measured nothing", n);
    if (n > 0) {
        uint64_t got = rb_sum_sess(&rb);
        CHECK(got == WANT_NS,
              "CONSERVATION: %llu ms of PID_A on disk == the %llu ms of "
              "completed seconds the ticks could seal, EXACTLY. The parent "
              "tree reads %llu ms here: the ticks after the rotation sealed "
              "nothing and pgwt_summary_close could not either",
              (unsigned long long)(got / MS),
              (unsigned long long)(WANT_NS / MS),
              (unsigned long long)(WANT_BEFORE / MS));
        CHECK(rb_rows_not_one_second(&rb) == 0,
              "PLACEMENT: every one of the %d records crediting PID_A carries "
              "exactly 1000 ms (%d do not) — a catch-up pass that dumped the "
              "whole pause into one second fails here, not on the total",
              n, rb_rows_not_one_second(&rb));
    }

    /* Close, then split the same total by FILE. The seconds that completed
     * before the rotation belong to the archived hour and the ones after to
     * the file the seal reopened; a fix that re-credited the interval from
     * its start, or re-sealed the rotation's own second into the new file,
     * moves these two numbers without moving the total. */
    pgwt_summary_close(x.w);
    struct per_file pf;
    int pn = read_per_file(x.dir, PID_A, &pf);
    CHECK(pn == 2, "per-file: both files opened and decoded (%d)", pn);
    if (pn == 2) {
        /* Identified by NAME, not by scan order: this fixture pins the clock
         * anchor, so both headers carry the same start_time_ns and
         * pgwt_scan_summary_files' ascending sort cannot separate them. The
         * archived *.summary.lz4 is the hour the rotation closed;
         * current.summary is the one the seal reopened. */
        int arch = pf_index_of(&pf, ".summary.lz4");
        int cur  = pf_index_of(&pf, "current.summary");
        CHECK(arch >= 0 && cur >= 0 && arch != cur,
              "per-file: the archived hour and the reopened file are both "
              "present and distinct (%d, %d)", arch, cur);
        if (arch >= 0 && cur >= 0 && arch != cur)
            CHECK(pf.sess_ns[arch] == WANT_BEFORE &&
                  pf.sess_ns[cur] == WANT_AFTER,
                  "SPLIT: %llu ms in %s and %llu ms in %s — the %llu s that "
                  "completed before the rotation stayed in the hour they "
                  "belong to, the %llu s after went to the file the seal "
                  "reopened, nothing counted twice and nothing skipped",
                  (unsigned long long)(pf.sess_ns[arch] / MS), pf.name[arch],
                  (unsigned long long)(pf.sess_ns[cur] / MS), pf.name[cur],
                  (unsigned long long)(WANT_BEFORE / S),
                  (unsigned long long)(WANT_AFTER / S));
    }

    CHECK(x.w->open_seal_reopens_total == 1,
          "COUNTED: the seal reopened the rotated file exactly once "
          "(open_seal_reopens_total = %llu) — zero would mean the pause is "
          "back and this gate stopped looking",
          (unsigned long long)x.w->open_seal_reopens_total);
    CHECK(x.w->open_seal_reopen_failures_total == 0,
          "COUNTED: no reopen failed (%llu)",
          (unsigned long long)x.w->open_seal_reopen_failures_total);
    CHECK(x.w->open_credited_ns_total == WANT_NS,
          "COUNTED: open_credited_ns_total = %llu ms, agreeing with the disk",
          (unsigned long long)(x.w->open_credited_ns_total / MS));
    wr_free(&x);
}

/* ── §2 THE INPUT THAT GREENS A CARELESS GATE: a push after the pause ─── */

static void test_push_after_rotation_is_not_the_gate(void)
{
    struct wr x;
    if (wr_open(&x, "push") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    seed(&x);
    run_ticks(&x, true);
    /* The closing record for PID_A's 11 s wait, exactly as
     * tests/test_open_cpu_accounting.c's rotation leg does. On the PUSH path
     * the file reopens lazily and writer_seal_through catches every owed
     * second up in ONE pass, so the TOTAL is exact even with #430 fully
     * present -- which is why that leg never saw it. */
    struct pgwt_trace_event cl =
        mkev(PID_A, BASE_MONO + 12 * S, 11 * S, EV_LOCK, 0);
    pgwt_summary_push_event(x.w, &cl, 0, cl.old_event);
    pgwt_summary_flush(x.w);
    pgwt_summary_close(x.w);

    struct readback rb = { .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "push-after: records read back (%d)", n);
    if (n > 0) {
        uint64_t got = rb_sum_sess(&rb);
        CHECK(got == WANT_NS,
              "REACHABILITY PROBE: with a record pushed after the pause the "
              "total is %llu ms == %llu ms on BOTH trees. So §1's red is the "
              "PAUSE and not this harness, and a fixture of this shape "
              "cannot gate #430",
              (unsigned long long)(got / MS),
              (unsigned long long)(WANT_NS / MS));
    }
    wr_free(&x);
}

/* ── §3 BYPASS: never anchored — invent nothing ───────────────────────── */

static void test_never_anchored_invents_nothing(void)
{
    struct wr x;
    if (wr_open(&x, "anchor") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    /* NO seed push: no file has ever been opened. This is the state the
     * daemon is in between pgwt_summary_writer_init and its first event,
     * while its timer handler is already noting opens and calling
     * pgwt_summary_flush_completed every second. */
    CHECK(!x.w->file_ever_opened,
          "SETUP: no file has ever been opened (file_ever_opened = %d)",
          (int)x.w->file_ever_opened);
    run_ticks(&x, false);

    CHECK(count_summary_files(x.dir) == 0,
          "BYPASS §3 (never anchored): %d files created — the seal must not "
          "open a file for a writer that has never had one, however much is "
          "open. A fix that reopened unconditionally fails here",
          count_summary_files(x.dir));
    CHECK(x.w->open_credited_ns_total == 0,
          "BYPASS §3: nothing credited (%llu ns)",
          (unsigned long long)x.w->open_credited_ns_total);
    CHECK(x.w->open_seal_reopens_total == 0 &&
          x.w->open_seal_reopen_failures_total == 0,
          "BYPASS §3: the reopen was not even attempted (%llu/%llu)",
          (unsigned long long)x.w->open_seal_reopens_total,
          (unsigned long long)x.w->open_seal_reopen_failures_total);
    struct readback rb = { .want_pid = PID_A };
    CHECK(read_back(x.dir, &rb) == -1,
          "BYPASS §3: the read-back REFUSES an empty directory rather than "
          "returning an agreeing zero");
    wr_free(&x);
}

/* ── §4 BYPASS: nothing open after the rotation — no empty record ─────── */

static void test_nothing_open_creates_no_file(void)
{
    struct wr x;
    if (wr_open(&x, "quiet") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    seed(&x);
    /* Seal the seed's own second first, so the accumulator is closed and the
     * rotation has nothing pending. */
    tick(&x, 2, false, false);
    tick(&x, 3, false, true);                 /* rotate: fp -> NULL        */
    int after_rot = count_summary_files(x.dir);
    for (int k = 4; k <= 10; k++)             /* tick on, NOTHING open     */
        tick(&x, k, false, false);

    CHECK(count_summary_files(x.dir) == after_rot,
          "BYPASS §4 (nothing open): seven quiet ticks after the rotation "
          "created no file (%d, was %d) — the seal reopens only once it KNOWS "
          "it has a second to write, so an idle daemon does not churn an "
          "empty file per hour",
          count_summary_files(x.dir), after_rot);
    CHECK(x.w->open_seal_reopens_total == 0,
          "BYPASS §4: no reopen attempted (%llu)",
          (unsigned long long)x.w->open_seal_reopens_total);
    CHECK(x.w->open_credit_records_total == 0,
          "BYPASS §4: no open-credit-only record invented (%llu)",
          (unsigned long long)x.w->open_credit_records_total);
    wr_free(&x);
}

/* ── §5 BYPASS: the reopen FAILS — refuse, count, and stay owed ───────── */

static void test_reopen_failure_is_counted_and_recoverable(void)
{
    struct wr x;
    if (wr_open(&x, "noopen") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    seed(&x);
    for (int k = 2; k <= ROT_TICK; k++)
        tick(&x, k, true, k == ROT_TICK);

    /* A DIRECTORY where current.summary must be created. fopen(..,"wbxe")
     * fails with EEXIST, recover_current_summary cannot open a directory
     * either, and the retry fails the same way -- so open_summary_file
     * returns -1 deterministically, as root as well as unprivileged (a chmod
     * would not). This is the "dependency exits non-zero" leg. */
    char blocker[600];
    snprintf(blocker, sizeof(blocker), "%s/current.summary", x.dir);
    CHECK(mkdir(blocker, 0755) == 0,
          "SETUP: current.summary replaced by a directory, so the reopen "
          "cannot succeed");

    uint64_t credited_at_rotation = x.w->open_credited_ns_total;
    for (int k = ROT_TICK + 1; k <= LAST_TICK; k++)
        tick(&x, k, true, false);

    CHECK(x.w->open_credited_ns_total == credited_at_rotation,
          "BYPASS §5 (reopen fails): nothing was credited while the file "
          "could not be created (%llu ms, unchanged) — a gate that cannot "
          "write must not pretend it did",
          (unsigned long long)(x.w->open_credited_ns_total / MS));
    CHECK(x.w->open_seal_reopen_failures_total ==
              (uint64_t)(LAST_TICK - ROT_TICK),
          "BYPASS §5: every failed reopen is COUNTED "
          "(open_seal_reopen_failures_total = %llu over %d ticks) — the gap "
          "is a number, not a belief",
          (unsigned long long)x.w->open_seal_reopen_failures_total,
          LAST_TICK - ROT_TICK);

    /* THE SECONDS ARE STILL OWED, not written off: credited_until_ns was
     * never advanced, so one successful tick after the obstacle is removed
     * places all of them and conservation is restored. */
    CHECK(rmdir(blocker) == 0, "SETUP: obstacle removed");
    tick(&x, LAST_TICK, true, false);
    pgwt_summary_close(x.w);
    struct readback rb = { .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "reopen-fail: records read back (%d)", n);
    if (n > 0)
        CHECK(rb_sum_sess(&rb) == WANT_NS,
              "BYPASS §5: the owed seconds survived the failure — %llu ms on "
              "disk == %llu ms, so the failure DELAYED the credit and did not "
              "destroy it",
              (unsigned long long)(rb_sum_sess(&rb) / MS),
              (unsigned long long)(WANT_NS / MS));
    wr_free(&x);
}

/* ── §6 BYPASS: the note goes stale during the pause ──────────────────── */

static void test_stale_note_during_pause_stops(void)
{
    struct wr x;
    if (wr_open(&x, "stale") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    seed(&x);
    for (int k = 2; k <= ROT_TICK; k++)
        tick(&x, k, true, k == ROT_TICK);
    uint64_t at_rotation = x.w->open_credited_ns_total;

    /* The producer stops refreshing: PID_A died without an EXIT record. The
     * ticks continue. PGWT_SUMMARY_OPEN_STALE_NS is 3 s, so at most that much
     * more may be credited and then crediting must STOP -- the fix must not
     * turn the restored seal into a fabrication engine. */
    for (int k = ROT_TICK + 1; k <= ROT_TICK + 20; k++)
        tick(&x, k, false, false);

    uint64_t extra = x.w->open_credited_ns_total - at_rotation;
    CHECK(extra <= PGWT_SUMMARY_OPEN_STALE_NS,
          "BYPASS §6 (stale note): only %llu ms more was credited after the "
          "producer stopped refreshing, within the %llu ms freshness window, "
          "and then crediting STOPPED over 20 further ticks",
          (unsigned long long)(extra / MS),
          (unsigned long long)(PGWT_SUMMARY_OPEN_STALE_NS / MS));
    CHECK(x.w->open_notes_expired_total > 0,
          "BYPASS §6: the expiry is COUNTED (open_notes_expired_total = "
          "%llu) — a fail-safe that works invisibly is one nobody notices "
          "stop working",
          (unsigned long long)x.w->open_notes_expired_total);
    wr_free(&x);
}

/* ── §7 BYPASS: the rotation ABSENT rather than wrong ─────────────────── */

static void test_no_rotation_same_total(void)
{
    struct wr x;
    if (wr_open(&x, "norot") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    seed(&x);
    run_ticks(&x, false);                     /* no forced rotation */
    pgwt_summary_close(x.w);

    CHECK(count_summary_files(x.dir) <= 2,
          "SETUP: no rotation happened (%d file(s) incl. meta)",
          count_summary_files(x.dir));
    struct readback rb = { .want_pid = PID_A };
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "no-rotation: records read back (%d)", n);
    if (n > 0)
        CHECK(rb_sum_sess(&rb) == WANT_NS,
              "BYPASS §7 (rotation absent): the identical tick schedule "
              "WITHOUT a rotation also reads %llu ms == %llu ms. So §1's "
              "number is the schedule's, and the rotation is the only "
              "variable between them",
              (unsigned long long)(rb_sum_sess(&rb) / MS),
              (unsigned long long)(WANT_NS / MS));
    CHECK(x.w->open_seal_reopens_total == 0,
          "BYPASS §7: no reopen without a rotation (%llu)",
          (unsigned long long)x.w->open_seal_reopens_total);
    wr_free(&x);
}

/* ── §8/§9 BYPASS: empty input, wrong version, disabled writer ────────── */

static int set_version(const char *dir, uint32_t version)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (!strstr(e->d_name, ".summary"))
            continue;
        if (strstr(e->d_name, ".meta"))
            continue;
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE *fp = fopen(path, "r+b");
        if (!fp) continue;
        struct pgwt_trace_file_header hdr;
        if (fread(&hdr, sizeof(hdr), 1, fp) == 1 &&
            hdr.magic == PGWT_SUMMARY_MAGIC) {
            hdr.version = version;
            if (fseek(fp, 0, SEEK_SET) == 0 &&
                fwrite(&hdr, sizeof(hdr), 1, fp) == 1)
                n++;
        }
        fclose(fp);
    }
    closedir(d);
    return n;
}

static void test_reader_refusals(void)
{
    struct readback rb = { .want_pid = PID_A };
    CHECK(read_back("/tmp/pgwt_rotcredit_does_not_exist", &rb) == -1,
          "BYPASS §8 (no such trace dir): the read-back REFUSES rather than "
          "agreeing over nothing");

    const char *empty = fresh_dir("empty");
    if (empty) {
        rb.want_pid = PID_A;
        CHECK(read_back(empty, &rb) == -1,
              "BYPASS §8 (empty trace dir): the read-back REFUSES");
    }

    /* A file written under a DIFFERENT accounting rule must not green this
     * run. It has to read back FIRST, or the refusal below would be about an
     * unreadable fixture instead of about the version. */
    struct wr x;
    if (wr_open(&x, "version") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    seed(&x);
    run_ticks(&x, true);
    pgwt_summary_close(x.w);
    rb.want_pid = PID_A;
    int n = read_back(x.dir, &rb);
    CHECK(n > 0, "wrong version: the fixture reads back first (%d records), "
          "so the refusal below is about the VERSION", n);
    CHECK(set_version(x.dir, PGWT_SUMMARY_VERSION - 1) > 0,
          "SETUP: every header rewritten to the previous accounting version");
    rb.want_pid = PID_A;
    CHECK(read_back(x.dir, &rb) == -1,
          "BYPASS §9 (previous version on disk): the reader REFUSES evidence "
          "gathered under another accounting rule instead of summing it");
    wr_free(&x);

    /* A DISABLED writer writes nothing, and the harness must FAIL on that
     * rather than report agreement over an empty file. */
    struct wr y;
    if (wr_open(&y, "disabled") != 0) { printf("  FAIL: setup\n"); failures++; return; }
    y.w->enabled = false;
    seed(&y);
    run_ticks(&y, true);
    CHECK(y.w->open_credited_ns_total == 0 &&
          y.w->open_seal_reopens_total == 0,
          "BYPASS §9 (writer disabled): nothing credited and no file "
          "reopened");
    rb.want_pid = PID_A;
    CHECK(read_back(y.dir, &rb) == -1,
          "BYPASS §9 (writer disabled): the read-back REFUSES rather than "
          "agreeing over an empty file");
    wr_free(&y);
}

int main(void)
{
    printf("=== test_rotation_open_credit: #430 rotation must not pause "
           "open-interval crediting ===\n");
    printf("-- 1: conservation across a rotation, nothing pushed after --\n");
    test_conservation_no_push();
    printf("-- 2: the input that greens a careless gate (a push after) --\n");
    test_push_after_rotation_is_not_the_gate();
    printf("-- 3: bypass — never anchored, invent nothing --\n");
    test_never_anchored_invents_nothing();
    printf("-- 4: bypass — nothing open, create no file --\n");
    test_nothing_open_creates_no_file();
    printf("-- 5: bypass — the reopen fails: refuse, count, stay owed --\n");
    test_reopen_failure_is_counted_and_recoverable();
    printf("-- 6: bypass — the note goes stale during the pause --\n");
    test_stale_note_during_pause_stops();
    printf("-- 7: bypass — the rotation absent rather than wrong --\n");
    test_no_rotation_same_total();
    printf("-- 8/9: bypass — empty input, wrong version, disabled writer --\n");
    test_reader_refusals();

    printf("\n%d checks, %d failures\n", checks, failures);
    printf("=== test_rotation_open_credit: %s ===\n",
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
