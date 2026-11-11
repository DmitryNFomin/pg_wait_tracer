#include "phase_capability.h"
#include "compute.h" /* reader must coexist with the future server caller */
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

static int checks, failures;
static char dir[] = "/tmp/pgwt_phase_cap.XXXXXX", path[512];
static int deny_read;
static int fail_write;
ssize_t write(int fd, const void *buf, size_t n)
{
    if (fail_write) { errno = EIO; return -1; }
    struct iovec v = {.iov_base = (void *)buf, .iov_len = n};
    return writev(fd, &v, 1);
}
/* Deterministic EACCES, including root on a gate box. All other opens use
 * the real filesystem. Also proves a missing-file-only reader cannot pass. */
FILE *fopen(const char *p, const char *mode)
{
    if (deny_read && !strcmp(p, path) && mode[0] == 'r') {
        errno = EACCES; return NULL;
    }
    int flags = mode[0] == 'r' ? O_RDONLY :
        O_WRONLY | O_CREAT | (mode[0] == 'a' ? O_APPEND : O_TRUNC);
    int fd = open(p, flags, 0600);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, mode);
    if (!f) close(fd);
    return f;
}
#define CHECK(c, m) do { checks++; if (!(c)) { failures++; printf("FAIL: %s\n", m); } } while (0)
static void put(const char *s, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (!f || fputs(s, f) < 0 || fclose(f)) exit(2);
}
static struct pgwt_phase_record rec(uint64_t start, int kernel, int exec, int plan)
{
    struct pgwt_phase_record r = {.start_wall_ns = start};
    snprintf(r.kernel_release, sizeof(r.kernel_release), "fixture-\"kernel\\release");
    r.phase[0] = pgwt_phase_classify(kernel, exec);
    r.phase[1] = pgwt_phase_classify(kernel, plan);
    return r;
}
static struct pgwt_phase_window readwin(uint64_t a, uint64_t b)
{
    struct pgwt_phase_window w;
    pgwt_phase_read_window(dir, a, b, &w);
    return w;
}
static void unknown(uint64_t a, uint64_t b, const char *why)
{
    struct pgwt_phase_window w = readwin(a, b);
    CHECK(w.phase[0].value.state == PGWT_PHASE_UNKNOWN &&
          w.phase[1].value.state == PGWT_PHASE_UNKNOWN, why);
    pgwt_phase_window_free(&w);
}
/* Check both phases, including the aggregate and the exact returned range. */
static void one_range(uint64_t lo, uint64_t hi, enum pgwt_phase_state state,
                      const char *reason, const char *message)
{
    struct pgwt_phase_window w = readwin(lo, hi);
    for (int p = 0; p < PGWT_CAP_PHASE_COUNT; p++) {
        struct pgwt_phase_result *r = &w.phase[p];
        CHECK(r->value.state == state && !strcmp(r->value.reason, reason) &&
              r->n_ranges == 1 && r->ranges[0].from_ns == lo &&
              r->ranges[0].to_ns == hi && r->ranges[0].value.state == state &&
              !strcmp(r->ranges[0].value.reason, reason), message);
    }
    pgwt_phase_window_free(&w);
}
static void corrupt_records(void)
{
    const char *available100 =
        "{\"version\":1,\"start_wall_ns\":\"100\",\"kernel_release\":\"fixture\","
        "\"execute\":{\"state\":\"available\",\"reason\":\"none\"},"
        "\"plan\":{\"state\":\"available\",\"reason\":\"none\"}}\n";
    const char *corrupt200 =
        "{\"version\":1,\"start_wall_ns\":\"200\",\"kernel_release\":\"fixture\","
        "\"execute\":{\"state\":\"unavailable\",\"reason\":\"kernel_no_ref_ctr\"},\"pl#corrupt\n";
    const char *available300 =
        "{\"version\":1,\"start_wall_ns\":\"300\",\"kernel_release\":\"fixture\","
        "\"execute\":{\"state\":\"available\",\"reason\":\"none\"},"
        "\"plan\":{\"state\":\"available\",\"reason\":\"none\"}}\n";
    put(available100, "w"); put(corrupt200, "a"); put(available300, "a");
    one_range(100, 200, PGWT_PHASE_UNKNOWN, "invalid_record",
              "corrupt middle line invalidates previous valid start [100,200)");
    one_range(200, 300, PGWT_PHASE_UNKNOWN, "invalid_record",
              "corrupt middle line cannot extend available over [200,300)");
    one_range(300, 400, PGWT_PHASE_AVAILABLE, "none",
              "corrupt middle line preserves next valid start [300,400)");
    struct pgwt_phase_window w = readwin(150, 350);
    for (int p = 0; p < PGWT_CAP_PHASE_COUNT; p++) {
        struct pgwt_phase_result *r = &w.phase[p];
        CHECK(r->value.state == PGWT_PHASE_MIXED && r->n_ranges == 2 &&
              r->ranges[0].from_ns == 150 && r->ranges[0].to_ns == 300 &&
              r->ranges[0].value.state == PGWT_PHASE_UNKNOWN &&
              !strcmp(r->ranges[0].value.reason, "invalid_record") &&
              r->ranges[1].from_ns == 300 && r->ranges[1].to_ns == 350 &&
              r->ranges[1].value.state == PGWT_PHASE_AVAILABLE &&
              !strcmp(r->ranges[1].value.reason, "none"),
              "unknown and available coverage produces mixed with clipped ranges");
    }
    pgwt_phase_window_free(&w);
    put(available100, "w"); put(corrupt200, "a");
    one_range(100, 200, PGWT_PHASE_UNKNOWN, "invalid_record",
              "corrupt last complete line invalidates previous start [100,200)");
    one_range(200, 400, PGWT_PHASE_UNKNOWN, "invalid_record",
              "corrupt last complete line stays unknown through requested end");
    /* A record before the immediately preceding start remains trustworthy. */
    put(available100, "w"); put(available300, "a"); put(corrupt200, "a");
    one_range(100, 300, PGWT_PHASE_AVAILABLE, "none",
              "corrupt last complete line preserves earlier closed interval");
    one_range(300, 400, PGWT_PHASE_UNKNOWN, "invalid_record",
              "corrupt last complete line invalidates last start through window end");
    put(corrupt200, "w"); put(available300, "a");
    one_range(100, 300, PGWT_PHASE_UNKNOWN, "invalid_record",
              "corrupt first line makes prefix unknown with invalid_record");
    one_range(300, 400, PGWT_PHASE_AVAILABLE, "none",
              "corrupt first line preserves next valid record");
    put(corrupt200, "w");
    w = readwin(100, 400);
    CHECK(w.phase[0].value.state == PGWT_PHASE_UNKNOWN &&
          w.phase[1].value.state == PGWT_PHASE_UNKNOWN &&
          !strcmp(w.phase[0].value.reason, "invalid_record") &&
          !strcmp(w.phase[1].value.reason, "invalid_record"),
          "only corrupt complete record is unknown with invalid_record");
    pgwt_phase_window_free(&w);
    put(available100, "w"); put("\n", "a"); put(available300, "a");
    one_range(100, 300, PGWT_PHASE_UNKNOWN, "invalid_record",
              "empty complete line is an invalid record");
    one_range(300, 400, PGWT_PHASE_AVAILABLE, "none",
              "valid record after empty line keeps its state");
    /* Same bytes without LF are a torn tail, not a committed corrupt line. */
    put(available100, "w");
    FILE *f = fopen(path, "a");
    size_t n = strlen(corrupt200) - 1;
    if (!f || fwrite(corrupt200, 1, n, f) != n || fclose(f)) exit(2);
    one_range(100, 400, PGWT_PHASE_AVAILABLE, "none",
              "torn corrupt tail does not invalidate earlier valid record");
}
static void no_allocations(void *p) { (void)p; }
static void *fail_allocation(size_t n) { (void)n; return NULL; }
int main(void)
{
    if (!mkdtemp(dir)) return 2;
    snprintf(path, sizeof(path), "%s/%s", dir, PGWT_PHASE_CAPABILITY_FILE);
    unknown(100, 200, "no sidecar is unknown");
    put("", "w"); unknown(100, 200, "empty sidecar is unknown");
    if (unlink(path)) return 2; /* writer must CREATE under the restrictive umask */
    uint64_t errors = 0;
    struct pgwt_phase_record a = rec(100, 1, 1, 0), b = rec(200, 0, 1, 1);
    mode_t old_mask = umask(0077);
    CHECK(pgwt_phase_append(dir, getgid(), &a, &errors) == 0, "writer appends first start");
    umask(old_mask);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640 && st.st_gid == getgid(),
          "writer enforces trace mode and group despite restrictive umask");
    CHECK(errors == 0, "successful writes do not count failures");
    FILE *on_disk = fopen(path, "r");
    char first[2048];
    if (!on_disk || !fgets(first, sizeof(first), on_disk)) return 2;
    fclose(on_disk);
    cJSON *json = cJSON_Parse(first);
    cJSON *version = cJSON_GetObjectItem(json, "version");
    cJSON *kernel = cJSON_GetObjectItem(json, "kernel_release");
    CHECK(cJSON_IsNumber(version) && version->valueint == 1 && cJSON_IsString(kernel) &&
          !strcmp(kernel->valuestring, a.kernel_release),
          "one JSON record per start preserves version and escaped kernel release");
    cJSON_Delete(json);
    struct pgwt_phase_window w = readwin(100, 200);
    CHECK(w.phase[0].value.state == PGWT_PHASE_AVAILABLE &&
          w.phase[1].value.state == PGWT_PHASE_UNAVAILABLE &&
          !strcmp(w.phase[1].value.reason, "binary_no_sdt"), "round trip preserves independent phases and reason");
    CHECK(w.phase[0].n_ranges == 1 && w.phase[0].ranges[0].from_ns == 100 &&
          w.phase[0].ranges[0].to_ns == 200, "last start extends to window end");
    pgwt_phase_window_free(&w);
    deny_read = 1; unknown(100, 200, "EACCES sidecar is unknown"); deny_read = 0;
    w = readwin(50, 150);
    CHECK(w.phase[0].value.state == PGWT_PHASE_MIXED &&
          w.phase[1].value.state == PGWT_PHASE_MIXED &&
          w.phase[0].n_ranges == 2 &&
          !strcmp(w.phase[0].ranges[0].value.reason, "before_first_record"),
          "window spanning before-first unknown and known states is mixed");
    pgwt_phase_window_free(&w);
    unknown(1, 100, "window entirely before first record is unknown");
    unknown(100, 100, "empty window is unknown");
    unknown(200, 100, "reversed window is unknown");
    CHECK(pgwt_phase_append(dir, getgid(), &b, &errors) == 0, "writer appends second start");
    w = readwin(150, 250);
    CHECK(w.phase[0].value.state == PGWT_PHASE_MIXED &&
          w.phase[1].value.state == PGWT_PHASE_MIXED, "state or unavailable reason change produces mixed");
    CHECK(w.phase[0].n_ranges == 2 && w.phase[0].ranges[0].from_ns == 150 &&
          w.phase[0].ranges[0].to_ns == 200 && w.phase[0].ranges[1].from_ns == 200 &&
          w.phase[0].ranges[1].to_ns == 250 &&
          !strcmp(w.phase[0].ranges[1].value.reason, "kernel_no_ref_ctr"),
          "mixed ranges are window clipped with exact boundaries and reasons");
    pgwt_phase_window_free(&w);
    w = readwin(200, 201);
    CHECK(w.phase[0].value.state == PGWT_PHASE_UNAVAILABLE, "half-open boundary belongs to next start");
    pgwt_phase_window_free(&w);
    put("{\"version\":1,\"start_wall_ns\":\"300\"", "a");
    w = readwin(150, 400);
    CHECK(w.phase[0].value.state == PGWT_PHASE_MIXED && w.phase[0].n_ranges == 2 &&
          w.phase[0].ranges[1].to_ns == 400, "torn last line ignored, earlier records retained");
    pgwt_phase_window_free(&w);
    /* A restart must discard only the uncommitted fragment, preserving
     * both earlier observations and the new observation's boundary. */
    struct pgwt_phase_record restart = rec(300, 1, 1, 1);
    FILE *recovery_log = tmpfile();
    int recovery_stderr = dup(STDERR_FILENO);
    if (!recovery_log || recovery_stderr < 0 ||
        dup2(fileno(recovery_log), STDERR_FILENO) < 0) return 2;
    CHECK(pgwt_phase_append(dir, getgid(), &restart, &errors) == 0,
          "restart appends after recovering torn tail");
    fflush(stderr);
    if (dup2(recovery_stderr, STDERR_FILENO) < 0) return 2;
    close(recovery_stderr); rewind(recovery_log);
    char diagnostic[512] = {0};
    (void)fgets(diagnostic, sizeof(diagnostic), recovery_log);
    fclose(recovery_log);
    CHECK(errors == 1 && strstr(diagnostic, "recovered torn tail") &&
          strstr(diagnostic, "truncated_bytes=") && strstr(diagnostic, "write_failures=1"),
          "torn-tail recovery is counted and logged");
    errors = 0;
    w = readwin(150, 400);
    CHECK(w.phase[0].value.state == PGWT_PHASE_MIXED && w.phase[0].n_ranges == 3 &&
          w.phase[0].ranges[0].value.state == PGWT_PHASE_AVAILABLE &&
          w.phase[0].ranges[1].value.state == PGWT_PHASE_UNAVAILABLE &&
          w.phase[0].ranges[1].from_ns == 200 && w.phase[0].ranges[1].to_ns == 300 &&
          w.phase[0].ranges[2].value.state == PGWT_PHASE_AVAILABLE &&
          w.phase[0].ranges[2].from_ns == 300 && w.phase[0].ranges[2].to_ns == 400 &&
          w.phase[1].value.state == PGWT_PHASE_MIXED && w.phase[1].n_ranges == 3 &&
          !strcmp(w.phase[1].ranges[0].value.reason, "binary_no_sdt") &&
          !strcmp(w.phase[1].ranges[1].value.reason, "kernel_no_ref_ctr") &&
          w.phase[1].ranges[2].value.state == PGWT_PHASE_AVAILABLE,
          "valid records survive torn fragment and new start for both phases");
    pgwt_phase_window_free(&w);
    on_disk = fopen(path, "r");
    int lines = 0, malformed = 0;
    if (!on_disk) return 2;
    while (fgets(diagnostic, sizeof(diagnostic), on_disk)) {
        lines++;
        json = cJSON_Parse(diagnostic);
        if (!json) malformed++;
        cJSON_Delete(json);
    }
    fclose(on_disk);
    CHECK(lines == 3 && malformed == 0,
          "recovery removes fragment without adding a bad or blank line");
    put("bad complete line\n", "a");
    w = readwin(300, 400);
    CHECK(w.phase[0].value.state == PGWT_PHASE_UNKNOWN &&
          w.phase[1].value.state == PGWT_PHASE_UNKNOWN &&
          !strcmp(w.phase[0].value.reason, "invalid_record") &&
          !strcmp(w.phase[1].value.reason, "invalid_record"),
          "bad complete line invalidates preceding start");
    pgwt_phase_window_free(&w);
    put("bad complete line\n", "w"); put(first, "a");
    w = readwin(100, 200);
    CHECK(w.phase[0].value.state == PGWT_PHASE_AVAILABLE &&
          w.phase[1].value.state == PGWT_PHASE_UNAVAILABLE,
          "bad complete line does not hide later valid records");
    pgwt_phase_window_free(&w);
    put("", "w");
    char large_tail[9001];
    memset(large_tail, 'x', sizeof(large_tail) - 1);
    large_tail[sizeof(large_tail) - 1] = 0;
    put(large_tail, "a");
    CHECK(pgwt_phase_append(dir, getgid(), &restart, &errors) == 0 && errors == 1,
          "recovery handles a file with no complete records");
    errors = 0;
    w = readwin(300, 400);
    CHECK(w.phase[0].value.state == PGWT_PHASE_AVAILABLE &&
          w.phase[1].value.state == PGWT_PHASE_AVAILABLE,
          "new start is readable after discarding an entirely torn file");
    pgwt_phase_window_free(&w);
    put(first, "w"); put(large_tail, "a");
    CHECK(pgwt_phase_append(dir, getgid(), &restart, &errors) == 0 && errors == 1,
          "multi-buffer recovery preserves complete prefix");
    errors = 0;
    w = readwin(100, 400);
    CHECK(w.phase[0].value.state == PGWT_PHASE_AVAILABLE &&
          w.phase[1].value.state == PGWT_PHASE_MIXED && w.phase[1].n_ranges == 2 &&
          w.phase[1].ranges[0].from_ns == 100 && w.phase[1].ranges[0].to_ns == 300,
          "multi-buffer recovery keeps earlier phase states and restart boundary");
    pgwt_phase_window_free(&w);
    put(first, "w");
    FILE *binary_line = fopen(path, "a");
    const char nul_record[] = "\0junk\n";
    if (!binary_line || fwrite(nul_record, 1, sizeof(nul_record) - 1, binary_line) != sizeof(nul_record) - 1 ||
        fclose(binary_line)) return 2;
    w = readwin(100, 200);
    CHECK(w.phase[0].value.state == PGWT_PHASE_UNKNOWN &&
          w.phase[1].value.state == PGWT_PHASE_UNKNOWN &&
          !strcmp(w.phase[0].value.reason, "invalid_record") &&
          !strcmp(w.phase[1].value.reason, "invalid_record"),
          "embedded NUL complete line invalidates preceding start");
    pgwt_phase_window_free(&w);
    put(large_tail, "w");
    put("\n", "a"); put(first, "a");
    w = readwin(100, 200);
    CHECK(w.phase[0].value.state == PGWT_PHASE_AVAILABLE &&
          w.phase[1].value.state == PGWT_PHASE_UNAVAILABLE,
          "oversized bad line is skipped without hiding later valid records");
    pgwt_phase_window_free(&w);
    char *ver = strstr(first, "\"version\":1");
    if (!ver) return 2;
    ver[strlen("\"version\":")] = '9';
    put(first, "w"); unknown(100, 200, "unsupported version of otherwise valid record is unknown");
    put("{\"version\":1}\n", "w"); unknown(100, 200, "missing required fields are unknown");
    put("", "w");
    a = rec(100, 1, 1, 1); b = rec(200, 1, 1, -1);
    CHECK(pgwt_phase_append(dir, getgid(), &a, &errors) == 0 &&
          pgwt_phase_append(dir, getgid(), &b, &errors) == 0, "append asymmetric unknown restart");
    w = readwin(100, 300);
    CHECK(w.phase[0].value.state == PGWT_PHASE_AVAILABLE && w.phase[0].n_ranges == 1 &&
          w.phase[1].value.state == PGWT_PHASE_MIXED, "same states coalesce per phase; known and unknown states are mixed");
    pgwt_phase_window_free(&w);
    struct pgwt_phase_record c = rec(200, 1, 0, 1);
    CHECK(pgwt_phase_append(dir, getgid(), &c, &errors) == 0, "append equal start time");
    w = readwin(200, 201);
    CHECK(w.phase[0].value.state == PGWT_PHASE_UNAVAILABLE && w.phase[1].value.state == PGWT_PHASE_AVAILABLE,
          "equal start times use the last observation");
    pgwt_phase_window_free(&w);
    CHECK(pgwt_phase_append(dir, getgid(), &a, &errors) == 0, "writer preserves append history even for wall-clock regression");
    unknown(100, 300, "out-of-order wall times are unknown");
    put("", "w");
    a = rec(18446744073709551000ULL, 1, 1, 1);
    CHECK(pgwt_phase_append(dir, getgid(), &a, &errors) == 0, "writer encodes full uint64 wall time");
    w = readwin(a.start_wall_ns, a.start_wall_ns + 1);
    CHECK(w.phase[0].value.state == PGWT_PHASE_AVAILABLE && w.phase[0].n_ranges == 1 &&
          w.phase[0].ranges[0].from_ns == a.start_wall_ns, "timestamp survives JSON without double precision loss");
    pgwt_phase_window_free(&w);
    w = readwin(a.start_wall_ns - 1, a.start_wall_ns + 1);
    CHECK(w.phase[0].value.state == PGWT_PHASE_MIXED && w.phase[0].n_ranges == 2 &&
          w.phase[0].ranges[1].from_ns == a.start_wall_ns,
          "one-nanosecond prefix exposes any timestamp rounding");
    pgwt_phase_window_free(&w);
    cJSON_Hooks hooks = {.malloc_fn = fail_allocation, .free_fn = no_allocations};
    FILE *warnings = tmpfile();
    int saved_stderr = dup(STDERR_FILENO);
    if (!warnings || saved_stderr < 0 || dup2(fileno(warnings), STDERR_FILENO) < 0) return 2;
    cJSON_InitHooks(&hooks);
    unknown(a.start_wall_ns, a.start_wall_ns + 1, "JSON allocation failure is unknown");
    CHECK(pgwt_phase_append(dir, getgid(), &a, &errors) == -1 && errors == 1,
          "writer allocation failure counted and nonfatal");
    cJSON_InitHooks(NULL);
    fail_write = 1;
    CHECK(pgwt_phase_append(dir, getgid(), &a, &errors) == -1 && errors == 2,
          "writer IO failure counted and nonfatal");
    fail_write = 0;
    /* A directory at the sidecar name fails even as root; no chmod-based skip. */
    unlink(path); if (mkdir(path, 0700)) return 2;
    CHECK(pgwt_phase_append(dir, getgid(), &a, &errors) == -1 && errors == 3,
          "writer open failure counted and nonfatal");
    fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0) return 2;
    close(saved_stderr); rewind(warnings);
    int logged = 0;
    while (fgets(first, sizeof(first), warnings))
        if (strstr(first, "WARN: phase_capabilities.jsonl append failed:") &&
            strstr(first, "write_failures=")) logged++;
    fclose(warnings);
    CHECK(logged == 3, "every failed write emits one diagnostic with its counter");
    rmdir(path);
    corrupt_records();
    unlink(path); rmdir(dir);
    printf("phase_capability: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
