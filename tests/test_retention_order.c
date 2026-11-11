/* #779: exercise the real retention pass, including its failure path.
 * Archive contents are opaque to cleanup; exact byte sizes trip the cap. */
#include "event_writer.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;
static const char *refuse_unlink;
static uint64_t live_summary_ns;
static char trace_dir[] = "/tmp/pgwt_retention_order.XXXXXX";

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: %s\n", msg); } \
} while (0)

/* A failed summary removal must not be followed by raw removal. unlinkat
 * performs the real operation otherwise, even when run as root on Linux. */
int unlink(const char *path)
{
    if (refuse_unlink && strstr(path, refuse_unlink)) {
        errno = EACCES;
        return -1;
    }
    return unlinkat(AT_FDCWD, path, 0);
}

static void reset_dir(void)
{
    refuse_unlink = NULL;
    live_summary_ns = 0;
    DIR *d = opendir(trace_dir);
    if (!d) exit(2);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (unlinkat(dirfd(d), e->d_name, 0) != 0) exit(2);
    }
    closedir(d);
}

static void put(const char *name, size_t bytes)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", trace_dir, name);
    FILE *f = fopen(path, "w");
    if (!f) exit(2);
    for (size_t i = 0; i < bytes; i++)
        if (fputc('x', f) == EOF) exit(2);
    if (fclose(f) != 0) exit(2);
}

static int exists(const char *name)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", trace_dir, name);
    return access(path, F_OK) == 0;
}

static uint64_t dir_bytes(void)
{
    uint64_t bytes = 0;
    DIR *d = opendir(trace_dir);
    if (!d) exit(2);
    struct dirent *e;
    while ((e = readdir(d))) {
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, 0) != 0) exit(2);
        if (S_ISREG(st.st_mode)) bytes += st.st_size;
    }
    closedir(d);
    return bytes;
}

static void cleanup(unsigned hours, uint64_t cap, char *warn, size_t warn_size)
{
    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    if (!w) exit(2);
    snprintf(w->trace_dir, sizeof(w->trace_dir), "%s", trace_dir);
    w->retention_hours = hours;
    w->retention_bytes = cap; /* --retention-gb deliberately enabled by caller */
    w->retention_live_summary_ns = live_summary_ns;
    FILE *capture = tmpfile();
    int saved = dup(STDERR_FILENO);
    if (!capture || saved < 0 || dup2(fileno(capture), STDERR_FILENO) < 0)
        exit(2);
    CHECK(pgwt_writer_cleanup_old_files(w) == 0, "real cleanup succeeds");
    fflush(stderr);
    if (dup2(saved, STDERR_FILENO) < 0) exit(2);
    close(saved);
    rewind(capture);
    size_t n = fread(warn, 1, warn_size - 1, capture);
    warn[n] = '\0';
    fclose(capture);
    free(w);
}

static void test_single_eviction(void)
{
    char warn[1024];
    reset_dir();
    put("2025-01-01_10.trace.lz4", 100);
    put("2025-01-01_10.summary.lz4", 100);
    put("current.trace", 10);
    CHECK(dir_bytes() == 210, "pair fixture really exceeds the 110-byte cap");
    cleanup(0, 110, warn, sizeof(warn));
    CHECK(!exists("2025-01-01_10.summary.lz4") ||
          exists("2025-01-01_10.trace.lz4"),
          "one eviction must not leave an hour summary-only");
    CHECK(dir_bytes() == 110, "exactly one 100-byte archive evicted");
    CHECK(exists("2025-01-01_10.trace.lz4"), "raw-only hour remains answerable");
    CHECK(warn[0] == '\0', "no over-cap warning after reaching cap");
}

static void test_suffixes_and_age_order(void)
{
    char warn[1024];
    reset_dir();
    put("2025-01-01_10.trace.lz4", 100);
    put("2025-01-01_10.1.trace.lz4", 100);
    put("2025-01-01_10.summary.lz4", 100);
    put("2025-01-01_10.2.summary.lz4", 100);
    put("2025-01-01_11.summary.lz4", 100);
    put("2025-01-01_11.trace.lz4", 100);
    CHECK(dir_bytes() == 600, "suffix fixture really exceeds cap");
    cleanup(0, 300, warn, sizeof(warn));
    CHECK(!exists("2025-01-01_10.summary.lz4") &&
          !exists("2025-01-01_10.2.summary.lz4"),
          "all same-hour summaries go before any same-hour raw");
    CHECK(exists("2025-01-01_10.trace.lz4") ||
          exists("2025-01-01_10.1.trace.lz4"), "one old raw archive remains");
    CHECK(exists("2025-01-01_11.summary.lz4") &&
          exists("2025-01-01_11.trace.lz4"), "older hour goes before newer summary");
    CHECK(dir_bytes() == 300, "suffix fixture reaches cap");
}

static void test_raw_only_control(void)
{
    char warn[1024];
    reset_dir();
    put("2025-01-01_10.trace.lz4", 100);
    put("2025-01-01_11.trace.lz4", 100);
    put("current.trace", 10);
    CHECK(dir_bytes() == 210, "raw-only control really exceeds cap");
    cleanup(0, 110, warn, sizeof(warn));
    CHECK(!exists("2025-01-01_10.trace.lz4") &&
          exists("2025-01-01_11.trace.lz4"), "unpaired raw still evicts oldest first");
    CHECK(dir_bytes() == 110, "raw-only control reaches cap");
}

static void test_hours_and_default_off(void)
{
    char warn[1024];
    char recent_trace[80], recent_summary[80];
    time_t future = time(NULL) + 24 * 3600;
    struct tm tm;
    localtime_r(&future, &tm);
    strftime(recent_trace, sizeof(recent_trace), "%Y-%m-%d_%H.trace.lz4", &tm);
    strftime(recent_summary, sizeof(recent_summary), "%Y-%m-%d_%H.summary.lz4", &tm);
    reset_dir();
    put("2025-01-01_10.trace.lz4", 100);
    put("2025-01-01_10.summary.lz4", 100);
    put("current.trace", 10);
    cleanup(0, 0, warn, sizeof(warn));
    CHECK(dir_bytes() == 210, "byte retention stays default-off");
    put(recent_trace, 100);
    put(recent_summary, 100);
    cleanup(1, 0, warn, sizeof(warn));
    CHECK(!exists("2025-01-01_10.trace.lz4") &&
          !exists("2025-01-01_10.summary.lz4"), "-R still removes both old archives");
    CHECK(exists("current.trace"), "-R keeps live file");
    CHECK(exists(recent_trace) && exists(recent_summary), "-R keeps both recent archives");
}

static void test_warnings_and_failed_unlink(void)
{
    char warn[1024];
    reset_dir();
    put("2025-01-01_10.trace.lz4", 100);
    put("2025-01-01_10.summary.lz4", 100);
    put("current.trace", 10);
    refuse_unlink = ".summary.lz4";
    cleanup(0, 110, warn, sizeof(warn));
    CHECK(exists("2025-01-01_10.trace.lz4") &&
          exists("2025-01-01_10.summary.lz4"), "failed summary unlink preserves raw");
    CHECK(strstr(warn, "100 bytes over") && strstr(warn, "could not remove"),
          "failed unlink WARN explains actual excess and removal failure");
    reset_dir();
    put("2025-01-01_10.trace.lz4", 100);
    put("2025-01-01_10.summary.lz4", 100);
    put("current.trace", 200);
    cleanup(0, 100, warn, sizeof(warn));
    CHECK(dir_bytes() == 200 && exists("current.trace"), "live files can exceed cap");
    CHECK(strstr(warn, "100 bytes over") && strstr(warn, "protected files"),
          "live-files-over-cap WARN stays reachable and true");
}

static void test_failed_unlink_other_hours(void)
{
    char warn[1024];
    reset_dir();
    put("2025-01-01_09.trace.lz4", 100);
    put("2025-01-01_10.2.summary.lz4", 100);
    put("2025-01-01_10.summary.lz4", 100);
    put("2025-01-01_10.trace.lz4", 100);
    put("2025-01-01_10.1.trace.lz4", 100);
    put("2025-01-01_11.summary.lz4", 100);
    put("2025-01-01_11.trace.lz4", 100);
    put("2025-01-01_12.trace.lz4", 100);
    put("current.trace", 10);
    /* The failure is neither oldest nor adjacent to its raw pieces in the
     * sorted array: the other hour-10 summary sorts between them. */
    refuse_unlink = "2025-01-01_10.2.summary.lz4";
    CHECK(dir_bytes() == 810, "failed-summary fixture really exceeds cap");
    cleanup(0, 110, warn, sizeof(warn));
    CHECK(!exists("2025-01-01_09.trace.lz4"),
          "oldest archive evicts before the later summary unlink failure");
    CHECK(exists("2025-01-01_10.2.summary.lz4") &&
          exists("2025-01-01_10.trace.lz4") &&
          exists("2025-01-01_10.1.trace.lz4"),
          "failed summary preserves every same-ftime raw piece");
    CHECK(!exists("2025-01-01_10.summary.lz4"),
          "summary failure still permits another summary of that hour to evict");
    CHECK(!exists("2025-01-01_11.summary.lz4") &&
          !exists("2025-01-01_11.trace.lz4") &&
          !exists("2025-01-01_12.trace.lz4"),
          "summary unlink failure does not block eviction from later hours");
    CHECK(dir_bytes() == 310 && strstr(warn, "200 bytes over") &&
          strstr(warn, "could not remove"),
          "failed-summary WARN reports excess after all other hours evict");
    put("2025-01-01_13.trace.lz4", 100);
    cleanup(0, 110, warn, sizeof(warn));
    CHECK(!exists("2025-01-01_13.trace.lz4") && dir_bytes() == 310 &&
          exists("2025-01-01_10.trace.lz4") &&
          exists("2025-01-01_10.1.trace.lz4") &&
          strstr(warn, "200 bytes over"),
          "persistent summary failure still permits later-hour eviction next pass");

    reset_dir();
    put("2025-01-01_09.trace.lz4", 100);
    put("2025-01-01_10.summary.lz4", 100);
    put("2025-01-01_10.trace.lz4", 100);
    put("2025-01-01_11.summary.lz4", 100);
    put("2025-01-01_11.trace.lz4", 100);
    put("2025-01-01_12.trace.lz4", 100);
    put("current.trace", 10);
    refuse_unlink = "2025-01-01_10.trace.lz4";
    cleanup(0, 110, warn, sizeof(warn));
    CHECK(!exists("2025-01-01_09.trace.lz4") &&
          !exists("2025-01-01_10.summary.lz4") &&
          exists("2025-01-01_10.trace.lz4"),
          "raw unlink failure leaves a valid raw-only hour after older eviction");
    CHECK(!exists("2025-01-01_11.summary.lz4") &&
          !exists("2025-01-01_11.trace.lz4") &&
          !exists("2025-01-01_12.trace.lz4"),
          "raw unlink failure does not block eviction from later hours");
    CHECK(dir_bytes() == 110 && strstr(warn, "could not remove") &&
          !strstr(warn, "trace dir still"),
          "raw failure can still reach cap without an excess WARN");
}

static void test_sidecar_accounting(void)
{
    char warn[1024];
    reset_dir();
    put("2025-01-01_10.trace.lz4", 100);
    put("2025-01-01_10.trace.lz4" PGWT_OPEN_CKPT_SUFFIX, 100);
    put("2025-01-01_11.trace.lz4", 100);
    put("current.trace", 10);
    cleanup(0, 110, warn, sizeof(warn));
    CHECK(!exists("2025-01-01_10.trace.lz4" PGWT_OPEN_CKPT_SUFFIX),
          "eviction removes archive sidecar");
    CHECK(exists("2025-01-01_11.trace.lz4") && dir_bytes() == 110,
          "freed sidecar bytes prevent needless extra eviction");
    CHECK(warn[0] == '\0', "removed sidecars do not produce a false WARN");
}

static void test_pending_summary_rotation(void)
{
    char warn[1024], live[600], archive[600];
    struct tm tm = { .tm_year = 125, .tm_mon = 0, .tm_mday = 1,
                     .tm_hour = 10, .tm_isdst = -1 };
    reset_dir();
    put("2025-01-01_09.trace.lz4", 100);
    put("2025-01-01_10.trace.lz4", 100);
    put("2025-01-01_10.1.trace.lz4", 100);
    put("current.summary", 100);
    put("current.trace", 10);
    /* The same naming anchor the daemon supplies for a live summary.
     * Raw can rotate early at its block cap while summary stays live. */
    live_summary_ns = (uint64_t)mktime(&tm) * 1000000000ULL;
    CHECK(dir_bytes() == 410, "pending rotation fixture really exceeds cap");
    cleanup(0, 110, warn, sizeof(warn));
    CHECK(!exists("2025-01-01_09.trace.lz4"), "older unprotected raw still evicts");
    CHECK(exists("2025-01-01_10.trace.lz4") &&
          exists("2025-01-01_10.1.trace.lz4"),
          "live summary protects all same-hour raw archives");
    CHECK(dir_bytes() == 310 && strstr(warn, "200 bytes over") &&
          strstr(warn, "live summary"), "pinned-raw WARN gives actual excess and reason");
    snprintf(live, sizeof(live), "%s/current.summary", trace_dir);
    snprintf(archive, sizeof(archive), "%s/2025-01-01_10.summary.lz4", trace_dir);
    CHECK(rename(live, archive) == 0, "summary rotation publishes the pending hour");
    live_summary_ns = 0;
    cleanup(0, 110, warn, sizeof(warn));
    CHECK(!exists("2025-01-01_10.summary.lz4") ||
          exists("2025-01-01_10.trace.lz4") ||
          exists("2025-01-01_10.1.trace.lz4"),
          "later summary publication cannot create a summary-only hour");
    CHECK(dir_bytes() == 110 && warn[0] == '\0', "rotation releases raw for cap eviction");
}

static void test_phase_capability_sidecar(void)
{
    char warn[1024];
    reset_dir();
    put("phase_capabilities.jsonl", 50);
    put("current.trace", 10);
    put("2025-01-01_10.trace.lz4", 100);
    put("2025-01-01_11.trace.lz4", 100);
    cleanup(0, 210, warn, sizeof(warn));
    CHECK(exists("phase_capabilities.jsonl"), "phase capability history is protected from size eviction");
    CHECK(!exists("2025-01-01_10.trace.lz4") && exists("2025-01-01_11.trace.lz4") && dir_bytes() == 160,
          "phase capability bytes count toward the cap");
    char sidecar[600];
    struct timespec old[2] = {{1, 0}, {1, 0}};
    snprintf(sidecar, sizeof(sidecar), "%s/phase_capabilities.jsonl", trace_dir);
    if (utimensat(AT_FDCWD, sidecar, old, 0)) exit(2);
    cleanup(1, 0, warn, sizeof(warn));
    CHECK(exists("phase_capabilities.jsonl") && dir_bytes() == 60,
          "phase capability history survives hours retention");
    cleanup(0, 1, warn, sizeof(warn));
    CHECK(exists("phase_capabilities.jsonl") && strstr(warn, "59 bytes over"),
          "protected capability bytes are counted in over-cap warning");
}

int main(void)
{
    if (!mkdtemp(trace_dir)) return 2;
    test_single_eviction();
    test_suffixes_and_age_order();
    test_raw_only_control();
    test_hours_and_default_off();
    test_warnings_and_failed_unlink();
    test_failed_unlink_other_hours();
    test_sidecar_accounting();
    test_pending_summary_rotation();
    test_phase_capability_sidecar();
    reset_dir();
    rmdir(trace_dir);
    printf("retention_order: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
