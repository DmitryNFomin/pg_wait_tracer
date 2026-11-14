/* Exercise real /proc descriptor lifetime on Linux (including EL8 4.18).
 * The portable injected suite covers numeric PID reuse deterministically. */
#define _GNU_SOURCE
#include "backend_status_layout.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
static int checks, failures;
#define CHECK(c, msg) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL: %s (errno=%d)\n", msg, errno); } } while (0)
static void put(uint8_t *row, const struct pgwt_pgbs_field *field, uint64_t value)
{
    memcpy(row + field->offset, &value, field->width);
}
static void test_version(int major)
{
    struct PgBackendStatusLayout layout;
    CHECK(pgwt_pgbs_hard_table_lookup(major, PGWT_PGBS_ARCH_X86_64, 8,
          PGWT_PGBS_ABI_SYSV_LP64, &layout) == 0, "fixture layout");
    layout.validation = PGWT_PGBS_VALIDATION_VALIDATED;
    struct pgwt_pgbs_field *fields[] = {&layout.st_changecount, &layout.st_procpid,
        &layout.st_databaseid, &layout.st_userid, &layout.st_state,
        &layout.st_activity_raw, &layout.st_query_id};
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++)
        if (fields[i]->present) fields[i]->validation = PGWT_PGBS_FIELD_VALIDATED;
    uint8_t row[440] = {0};
    char activity[64] = "SELECT 42";
    layout.activity_buffer_size = sizeof(activity);
    layout.status_anchor = (uintptr_t)row;
    put(row, &layout.st_changecount, 20);
    put(row, &layout.st_databaseid, 16384);
    put(row, &layout.st_userid, 10);
    put(row, &layout.st_state, layout.state_running);
    put(row, &layout.st_activity_raw, (uintptr_t)activity);
    if (major != 13) put(row, &layout.st_query_id, 1234);
    uint64_t entry = (uintptr_t)row;
    int ready[2], release[2];
    if (pipe(ready) || pipe(release)) { perror("pipe"); exit(2); }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }
    if (!pid) {
        close(ready[0]); close(release[1]);
        put(row, &layout.st_procpid, getpid());
        if (write(ready[1], "x", 1) != 1) _exit(2);
        char ch;
        ssize_t n = read(release[0], &ch, 1);
        _exit(n == 1 ? 0 : 2);
    }
    close(ready[1]); close(release[0]);
    char ch;
    CHECK(read(ready[0], &ch, 1) == 1, "child row initialized");
    close(ready[0]);
    struct pgwt_pgbs_reader reader = {0};
    CHECK(pgwt_pgbs_reader_open(&reader, pid) == 0, "open original child's mm");
    CHECK((fcntl(reader.mem_fd, F_GETFD) & FD_CLOEXEC) != 0, "cached mem is close-on-exec");
    struct pgwt_pgbs_sampled_attr out;
    CHECK(pgwt_pgbs_reader_attr(&reader, pid, (uintptr_t)&entry,
          major == 13 ? entry : 0, &layout, &out) == 0 && out.cmd_open &&
          out.databaseid == 16384, "original child attribution readable");
    CHECK(write(release[1], "x", 1) == 1, "release child");
    close(release[1]);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status),
          "child fully retired before next read");
    uint32_t value;
    ssize_t n = pread(reader.mem_fd, &value, sizeof(value), (off_t)(uintptr_t)row);
    CHECK(n == 0, "kept original mem descriptor returns EOF after exit");
    CHECK(pgwt_pgbs_reader_attr(&reader, pid, (uintptr_t)&entry, entry,
          &layout, &out) == -1 && reader.dead && !out.cmd_open,
          "exit between ticks fails closed and marks original mm dead");
    CHECK(pgwt_pgbs_reader_open(&reader, pid) == -1, "dead mm cannot reopen by numeric PID");
    int fd = reader.mem_fd;
    pgwt_pgbs_reader_close(&reader);
    CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "retirement closes descriptor");
    pgwt_pgbs_reader_close(&reader);
    printf("PG%d real proc lifetime: original read OK, exit -> EOF, retired fd closed\n", major);
}
#endif
int main(void)
{
#ifdef __linux__
    test_version(18); test_version(17); test_version(13);
    printf("sampled_attr_lifetime: %d/%d checks passed\n", checks-failures, checks);
    return failures ? 1 : 0;
#else
    puts("SKIP: real proc memory lifetime requires Linux; use test_sampled_attr_io on macOS");
    return 0;
#endif
}
