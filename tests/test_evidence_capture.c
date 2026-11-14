#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "evidence_capture.h"
#include "core/evidence_codec.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUIRE(x, name) do { if (!(x)) { \
    fprintf(stderr, "FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__); exit(1); \
} } while (0)

#ifdef __APPLE__
/* Synthetic host cases have a synthetic boot domain. The macOS sandbox denies
 * kern.bootsessionuuid; production still refuses if its OS read fails. */
int sysctlbyname(const char *name, void *out, size_t *size, void *in, size_t in_size)
{
    static const char boot[] = "synthetic-capture-unit-boot";
    REQUIRE(!strcmp(name, "kern.bootsessionuuid") && !in && !in_size &&
            *size >= sizeof(boot), "boot identity read shape");
    memcpy(out, boot, sizeof(boot));
    *size = sizeof(boot);
    return 0;
}
#endif

static struct pgwt_counter_vector counters(void)
{
    struct pgwt_counter_vector v = {.valid = true};
    for (size_t i = 0; i < PGWT_COUNTER_COMPONENTS; ++i)
        v.components[i] = (struct pgwt_counter_value){UINT64_MAX - i, true};
    return v;
}

static void temporary_directory(char *directory, size_t size, const char *label)
{
    const char *base = getenv("TMPDIR");
    if (!base)
        base = "/tmp";
    REQUIRE(snprintf(directory, size, "%s/pgwt-evidence-%s-XXXXXX", base, label) < (int)size,
            "temporary path fits");
    REQUIRE(mkdtemp(directory), "temporary directory");
}

static void observe(struct pgwt_evidence_capture *c, unsigned type,
                    uint32_t pid, uint64_t t)
{
    struct pgwt_lifecycle_event ev = {.type = type, .pid = pid, .timestamp = t};
    pgwt_evidence_capture_observe(c, &ev, sizeof(ev));
}

static size_t files(const char *directory, char *path, size_t capacity)
{
    DIR *dir = opendir(directory);
    REQUIRE(dir, "open test directory");
    size_t n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (path)
            snprintf(path, capacity, "%s/%s", directory, entry->d_name);
        ++n;
    }
    REQUIRE(!closedir(dir), "close test directory");
    return n;
}

static unsigned char *decode(const char *path, pgwt_codec_decoded *decoded)
{
    FILE *f = fopen(path, "rb");
    REQUIRE(f && !fseek(f, 0, SEEK_END), "read capture");
    long length = ftell(f);
    REQUIRE(length > 0 && !fseek(f, 0, SEEK_SET), "capture has bytes");
    unsigned char *bytes = malloc((size_t)length);
    REQUIRE(bytes && fread(bytes, 1, (size_t)length, f) == (size_t)length,
            "whole capture readable");
    REQUIRE(!fclose(f), "close input");
    pgwt_codec_buffer buffer = {bytes, (size_t)length};
    REQUIRE(pgwt_codec_decode(&buffer, 1, decoded) == 0, "decode PASS");
    REQUIRE(!decoded->failed && decoded->stop_count == 0 &&
            decoded->diagnostic_count == 0, "zero codec stops/diagnostics");
    REQUIRE(decoded->file_count == 1 && decoded->capture_count == 1,
            "one file and capture");
    REQUIRE(decoded->files[0].version == 5, "version 5");
    REQUIRE(!decoded->files[0].qualified, "qualified=false");
    REQUIRE(!decoded->captures[0].replay.stop.stopped, "zero replay stops");
    unsigned counts[PGWT_EVIDENCE_VOID + 1] = {0};
    for (size_t i = 0; i < decoded->files[0].record_count; ++i) {
        pgwt_evidence_record r = decoded->files[0].records[i];
        REQUIRE(r.kind <= PGWT_EVIDENCE_VOID, "bounded kind");
        ++counts[r.kind];
    }
    REQUIRE(counts[PGWT_EVIDENCE_VOID] == 1, "exactly one closing VOID");
    REQUIRE(counts[PGWT_EVIDENCE_ATTACH_OK] == 0 &&
            counts[PGWT_EVIDENCE_FRONTIER] == 0 &&
            counts[PGWT_EVIDENCE_END] == 0 && counts[PGWT_EVIDENCE_TRANS] == 0 &&
            counts[PGWT_EVIDENCE_OTHER] == 0, "no unsupported evidence kinds");
    return bytes;
}

static void replay(const pgwt_codec_decoded *d, uint32_t pid, uint64_t fork,
                   uint64_t from, uint64_t to, const char *expected)
{
    pgwt_evidence_input input = {
        .files = d->files, .file_count = d->file_count,
        .capture = d->files[0].capture, .pid = pid, .has_fork = true, .fork = fork,
    };
    pgwt_core_query query = {.event = {"PgSleep", 7}, .from = from, .to = to};
    pgwt_evidence_output out;
    REQUIRE(pgwt_evidence_replay(&input, &query, &out, NULL) == PGWT_REPLAY_OK,
            "replay OK");
    const pgwt_core_result *results[] = {&out.wait_time, &out.completed_waits};
    for (size_t i = 0; i < 2; ++i) {
        REQUIRE(!results[i]->value.present &&
                !strcmp(results[i]->completeness, "unavailable"), "unavailable value");
        REQUIRE(results[i]->reason_count == 1 &&
                !strcmp(results[i]->reasons[0], expected), expected);
    }
    pgwt_evidence_output_free(&out);
}

static void lifecycle_case(unsigned invalid)
{
    char directory[1024], path[1200];
    temporary_directory(directory, sizeof(directory), "unit");
    struct pgwt_evidence_capture *c = pgwt_evidence_capture_init(directory, 17);
    REQUIRE(c, "opt-in initialised");
    observe(c, PGWT_LIFECYCLE_INIT, 999, 1);
    observe(c, PGWT_LIFECYCLE_EXIT, 999, 2);
    observe(c, PGWT_LIFECYCLE_FORK, 101, 100);
    observe(c, PGWT_LIFECYCLE_FORK, 202, 50);
    observe(c, PGWT_LIFECYCLE_FORK, 101, 300);
    observe(c, PGWT_LIFECYCLE_EXIT, 101, 200); /* delayed old generation */
    observe(c, PGWT_LIFECYCLE_EXIT, 101, 400);
    observe(c, PGWT_LIFECYCLE_EXIT, 101, 400); /* idempotent */
    observe(c, PGWT_LIFECYCLE_FORK, 101, 100); /* idempotent */
    observe(c, PGWT_LIFECYCLE_FORK, 303, UINT64_MAX - 100);
    observe(c, PGWT_LIFECYCLE_EXIT, 303, UINT64_MAX - 50);
    REQUIRE(files(directory, NULL, 0) == 0, "buffered only before close");
    struct pgwt_counter_vector v = counters();
    if (invalid == 1)
        v.components[7].valid = false;
    if (invalid == 2)
        v.valid = false;
    REQUIRE(!pgwt_evidence_capture_close(&c, v) && !c, "close consumes capture");
    REQUIRE(!pgwt_evidence_capture_close(&c, v), "double close no-op");
    REQUIRE(files(directory, path, sizeof(path)) == 1, "one file, no temporary file");
    pgwt_codec_decoded d;
    unsigned char *bytes = decode(path, &d);
    REQUIRE(d.files[0].record_count == 5, "four generations plus loss");
    const uint32_t pids[] = {101, 202, 101, 303};
    const uint64_t forks[] = {100, 50, 300, UINT64_MAX - 100};
    const uint64_t exits[] = {200, 0, 400, UINT64_MAX - 50};
    for (size_t i = 0; i < 4; ++i) {
        pgwt_evidence_record r = d.files[0].records[i];
        REQUIRE(r.kind == PGWT_EVIDENCE_LIFE && r.pid == pids[i] &&
                r.has_fork && r.fork == forks[i] && r.start_known && r.start == forks[i],
                "LIFE exactly preserves observed fork");
        REQUIRE(r.has_exit == (i != 1) && (!r.has_exit ||
                (r.exit == exits[i] && r.fork <= r.exit)), "LIFE exact exit or null");
        REQUIRE(r.t == (r.has_exit ? r.exit : r.fork), "LIFE observation instant");
    }
    pgwt_evidence_record closing = d.files[0].records[4];
    REQUIRE(closing.kind == PGWT_EVIDENCE_VOID && !closing.has_invalid_from &&
            closing.void_reason.length > 0, "closing loss has no invented cutoff");
    REQUIRE(closing.counters.known == (invalid == 0), "unknown vector is null");
    if (!invalid)
        for (size_t i = 0; i < 15; ++i)
            REQUIRE(closing.counters.value[i] == UINT64_MAX - i, "all final counter slots exact");
    replay(&d, 101, 100, 110, 190, "coverage_incomplete");
    replay(&d, 101, 300, 310, 390, "coverage_incomplete");
    replay(&d, 202, 50, 60, 70, "coverage_incomplete");
    replay(&d, 999, 0, 110, 190, "evidence_invalid");
    pgwt_codec_decoded_free(&d);
    free(bytes);
    REQUIRE(!unlink(path) && !rmdir(directory), "remove fixture");
    printf("capture LIFE/pid reuse/open lifetime/replay, counters=%s: PASS\n",
           invalid ? "null" : "known");
}

static void refusals(void)
{
    char directory[1024];
    temporary_directory(directory, sizeof(directory), "refusal");
    FILE *log = tmpfile();
    REQUIRE(log, "refusal log");
    int saved = dup(STDERR_FILENO);
    REQUIRE(saved >= 0 && dup2(fileno(log), STDERR_FILENO) >= 0, "capture diagnostics");
    REQUIRE(!setenv("PGWT_EVIDENCE_CAPTURE_MAX_BYTES", "1", 1), "tiny cap env");
    REQUIRE(!pgwt_evidence_capture_init(directory, 17), "tiny cap refused");
    REQUIRE(!setenv("PGWT_EVIDENCE_CAPTURE_MAX_BYTES", "2048", 1), "bounded cap env");
    struct pgwt_evidence_capture *c = pgwt_evidence_capture_init(directory, 17);
    REQUIRE(c, "bounded capture initialised");
    for (unsigned i = 1; i < 1000; ++i)
        observe(c, PGWT_LIFECYCLE_FORK, i, i);
    REQUIRE(pgwt_evidence_capture_close(&c, counters()) == -1, "overflow refuses whole capture");
    REQUIRE(!unsetenv("PGWT_EVIDENCE_CAPTURE_MAX_BYTES"), "clear cap");
    c = pgwt_evidence_capture_init(directory, 17);
    REQUIRE(c, "unwritable capture initialised");
    REQUIRE(!rmdir(directory), "withdraw directory before publication");
    REQUIRE(pgwt_evidence_capture_close(&c, counters()) == -1, "unwritable refuses cleanly");
    REQUIRE(mkdir(directory, 0700) == 0, "restore test directory");
    c = pgwt_evidence_capture_init(directory, 17);
    REQUIRE(c, "contradiction capture initialised");
    observe(c, PGWT_LIFECYCLE_FORK, 1, 100);
    observe(c, PGWT_LIFECYCLE_EXIT, 1, 200);
    observe(c, PGWT_LIFECYCLE_EXIT, 1, 201);
    REQUIRE(pgwt_evidence_capture_close(&c, counters()) == -1, "contradictory exit refuses");
    fflush(stderr);
    REQUIRE(dup2(saved, STDERR_FILENO) >= 0 && !close(saved), "restore stderr");
    REQUIRE(!fseek(log, 0, SEEK_SET), "read refusals");
    char text[4096] = {0};
    REQUIRE(fread(text, 1, sizeof(text)-1, log) > 0, "explicit diagnostics");
    REQUIRE(strstr(text, "EVIDENCE CAPTURE REFUSED: size cap reached") &&
            strstr(text, "EVIDENCE CAPTURE REFUSED: trace directory unwritable") &&
            strstr(text, "EVIDENCE CAPTURE REFUSED: contradictory lifecycle exit"), "named refusals");
    REQUIRE(!fclose(log), "close refusal log");
    REQUIRE(files(directory, NULL, 0) == 0 && !rmdir(directory), "refusal leaves no partial file");
    puts("capture tiny cap/buffer limit/unwritable/contradiction: explicit refusal, no partial file PASS");
}

/* Box driver: inspect actual daemon bytes with the production decoder/replay.
 * Output identities/times as strings so the route can use them losslessly. */
static void inspect(const char *path, uint32_t pid, uint32_t unknown)
{
    pgwt_codec_decoded d;
    unsigned char *bytes = decode(path, &d);
    const pgwt_evidence_record *life = NULL;
    size_t lives = 0;
    for (size_t i = 0; i < d.files[0].record_count; ++i) {
        const pgwt_evidence_record *r = &d.files[0].records[i];
        if (r->kind != PGWT_EVIDENCE_LIFE)
            continue;
        ++lives;
        REQUIRE(r->has_fork && r->start_known && r->start == r->fork &&
                (!r->has_exit || r->fork <= r->exit), "live LIFE valid");
        if (r->pid == pid)
            life = r;
        REQUIRE(r->pid != unknown, "pre-existing checkpointer gets no LIFE");
    }
    REQUIRE(life && life->has_exit && life->exit - life->fork > 2, "live pid fork and exit observed");
    replay(&d, pid, life->fork, life->fork + 1, life->exit - 1, "coverage_incomplete");
    replay(&d, unknown, 0, life->fork + 1, life->exit - 1, "evidence_invalid");
    printf("{\"version\":5,\"qualified\":false,\"codec_stops\":0,\"replay_stops\":0,"
           "\"LIFE\":%zu,\"VOID\":1,\"ATTACH_OK\":0,\"FRONTIER\":0,\"END\":0,\"TRANS\":0,"
           "\"capture\":\"%.*s\",\"pid\":%u,\"fork\":\"%" PRIu64 "\","
           "\"exit\":\"%" PRIu64 "\"}\n", lives,
           (int)d.files[0].capture.length, d.files[0].capture.data, pid, life->fork, life->exit);
    pgwt_codec_decoded_free(&d);
    free(bytes);
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "--inspect")) {
        inspect(argv[2], (uint32_t)strtoul(argv[3], NULL, 10),
                (uint32_t)strtoul(argv[4], NULL, 10));
        return 0;
    }
    REQUIRE(argc == 1, "usage");
    REQUIRE(!unsetenv("PGWT_EVIDENCE_CAPTURE"), "opt out env");
    struct pgwt_evidence_capture *c = pgwt_evidence_capture_init("/does/not/exist", 17);
    REQUIRE(!c, "unset is no-op");
    pgwt_evidence_capture_observe(NULL, NULL, 0);
    REQUIRE(!pgwt_evidence_capture_close(&c, counters()), "disabled close no-op");
    REQUIRE(!setenv("PGWT_EVIDENCE_CAPTURE", "1", 1) &&
            !unsetenv("PGWT_EVIDENCE_CAPTURE_MAX_BYTES"), "opt in env");
    lifecycle_case(0);
    lifecycle_case(1);
    lifecycle_case(2);
    refusals();
    puts("test_evidence_capture: PASS");
    return 0;
}
