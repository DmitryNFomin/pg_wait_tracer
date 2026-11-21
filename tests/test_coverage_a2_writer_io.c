/* Refs #933: observe the real coverage path's flush/fsync calls and inject
 * failures. Only event_writer.c is built with the two redirected symbols. */
#define _GNU_SOURCE
#include "event_writer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static struct pgwt_event_writer w;
static FILE *watched;
static int flushes, syncs, fail_flush, fail_sync, failures;
#define CHECK(ok, name) do { if (!(ok)) { \
    printf("FAIL %s\n", name); failures++; } } while (0)

int pgwt_a2_test_fflush(FILE *fp)
{
    if (fp == watched) {
        flushes++;
        if (fail_flush) { errno = EIO; return EOF; }
    }
    return fflush(fp);
}

int pgwt_a2_test_fsync(int fd)
{
    if (watched && fd == fileno(watched)) {
        syncs++;
        struct stat st;
        CHECK(flushes == 1 && fstat(fd, &st) == 0 && st.st_size == ftell(watched),
              "GAP_OPEN fsync follows fflush of the appended bytes");
        if (fail_sync) { errno = EIO; return -1; }
    }
    return fsync(fd);
}

static int append(int shape)
{
    if (shape == 2)
        return pgwt_writer_coverage_a2_bucket(&w, 900, 100, 200, 10, 1);
    return pgwt_writer_coverage_untraced_lifetime(&w, 900, shape == 0,
                                                  100, 42, 7, 800);
}

static void scenario(int shape, int bad_flush, int bad_sync)
{
    char dir[] = "/tmp/pgwt-a2-io-XXXXXX";
    if (!mkdtemp(dir)) { failures++; return; }
    CHECK(pgwt_writer_init(&w, dir, 18, 24, NULL) == 0, "writer init");
    pgwt_writer_coverage_configure(&w, 100, 0, 0);
    CHECK(pgwt_writer_coverage_watermark(&w, 200, 100, 100, 100) == 0,
          "open BOOT and WATERMARK before spying");
    watched = w.fp;
    flushes = syncs = 0;
    fail_flush = bad_flush;
    fail_sync = bad_sync;
    int rc = append(shape);
    CHECK(flushes == 1, "every A2 member and certificate flushes");
    CHECK(syncs == (shape == 0 && !bad_flush), "only GAP_OPEN fsyncs, after flush");
    if (bad_flush || bad_sync) {
        CHECK(rc < 0 && w.failed && !w.enabled, "I/O failure makes writer refuse");
        fail_flush = fail_sync = 0;
        for (int i = 0; i < 3; i++)
            CHECK(append(i) < 0, "failed writer refuses every A2 API");
        CHECK(pgwt_writer_coverage_gap(&w, 900, true, 8, 100, 2) < 0 &&
              pgwt_writer_coverage_watermark(&w, 900, 100, 100, 100) < 0 &&
              pgwt_writer_coverage_seal(&w, 900, 100) < 0,
              "failed writer also refuses existing coverage APIs");
        CHECK(flushes == 1 && syncs == (shape == 0 && !bad_flush),
              "refused calls append nothing after failure");
    } else {
        CHECK(rc == 0 && !w.failed && w.enabled, "durable append succeeds");
    }
    watched = NULL;
    fail_flush = fail_sync = 0;
    pgwt_writer_destroy(&w);
    char command[256];
    snprintf(command, sizeof(command), "rm -rf '%s'", dir);
    CHECK(system(command) == 0, "remove temporary trace");
}

int main(void)
{
    for (int shape = 0; shape < 3; shape++) {
        scenario(shape, 0, 0);
        scenario(shape, 1, 0);
    }
    scenario(0, 0, 1);
    if (!failures) puts("PASS A2 writer I/O: flush/fsync ordering, all append failures sticky");
    return failures ? 1 : 0;
}
