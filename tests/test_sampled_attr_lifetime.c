/* Linux kernel check: foreign-pid shared rows survive backend exit. Row
 * identity is not liveness; lifecycle delivery retires the enrollment. */
#define _GNU_SOURCE
#include "backend_status_layout.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
static int checks, failures;
#define CHECK(c, msg) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL: %s (errno=%d)\n", msg, errno); } } while (0)
static void put(uint8_t *row, const struct pgwt_pgbs_field *field, uint64_t value)
{ memcpy(row + field->offset, &value, field->width); }
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
    uint8_t *row = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (row == MAP_FAILED) { perror("mmap"); exit(2); }
    char *activity = (char *)row + 4096;
    strcpy(activity, "SELECT 42");
    layout.activity_buffer_size = 64;
    layout.status_anchor = (uintptr_t)row;
    put(row, &layout.st_changecount, 20);
    put(row, &layout.st_databaseid, 16384); put(row, &layout.st_userid, 10);
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
        _exit(read(release[0], &ch, 1) == 1 ? 0 : 2);
    }
    close(ready[1]); close(release[0]);
    char ch;
    CHECK(read(ready[0], &ch, 1) == 1, "child row initialized"); close(ready[0]);
    struct pgwt_pgbs_reader reader = {0};
    CHECK(pgwt_pgbs_reader_enroll(&reader, pid) == 0 && reader.start_time,
          "enrollment retains starttime without descriptor");
    CHECK(pgwt_pgbs_reader_resolve(&reader, getpid(), (uintptr_t)&entry, &layout, NULL) == 0,
          "own-pid pointer resolves to a row shared in target and reader");
    struct pgwt_pgbs_batch_target target = {.reader = &reader, .pid = pid};
    pgwt_pgbs_read_batch(getpid(), &layout, &target, 1, false, NULL);
    CHECK(target.rc == 0 && target.attr.cmd_open && target.attr.databaseid == 16384,
          "parent reads child's shared row through the parent reader");
    CHECK(write(release[1], "x", 1) == 1, "release child"); close(release[1]);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status),
          "child retired before next batch");
    pgwt_pgbs_read_batch(getpid(), &layout, &target, 1, false, NULL);
    CHECK(target.rc == 0, "coherent shared row does not prove liveness");
    CHECK(!pgwt_pgbs_pid_is_original(pid, reader.start_time),
          "fork-delivery starttime check rejects retired incarnation");
    reader.shared_reader_pid = pid;
    pgwt_pgbs_read_batch(pid, &layout, &target, 1, false, NULL);
    CHECK(target.rc == -1 && target.error == ESRCH && !target.attr.cmd_open,
          "reader ESRCH fails closed through the real syscall");
    pgwt_pgbs_reader_reset(&reader);
    pgwt_pgbs_read_batch(getpid(), &layout, &target, 1, false, NULL);
    CHECK(target.rc == -1 && !reader.pid && !reader.status_addr,
          "lifecycle retirement removes cached row proof");
    munmap(row, 8192);
    printf("PG%d real shared lifetime: identity != liveness; reader ESRCH closed\n", major);
}
#endif
int main(void)
{
#ifdef __linux__
    test_version(18); test_version(17); test_version(13);
    printf("sampled_attr_lifetime: %d/%d checks passed\n", checks-failures, checks);
    return failures ? 1 : 0;
#else
    puts("SKIP: real shared-row lifetime requires Linux; injected cases run on macOS");
    return 0;
#endif
}
