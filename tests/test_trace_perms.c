/* test_trace_perms.c — #499 and #500: who can read a trace, and whether the
 * name sidecar is there at all.
 *
 * #499 (startup ORDER): src/discovery.c writes wait_event_names.json at the
 * end of pgwt_discover(); the mkdir that creates the trace directory used to
 * live only in pgwt_writer_init(), which runs later (pgwt_daemon_init). So on
 * every run against a not-yet-existing trace dir the sidecar write failed and
 * the capture carried no wait-event name provenance. test_missing_dir below
 * reproduces that exact failure, test_prepare_then_sidecar the fix.
 *
 * #500 (who can READ it): the default trace group was the literal string
 * "dba". On a host without that group getgrnam() failed, trace_gid stayed
 * (gid_t)-1, and every file was written root:root 0640 — root-only. Asserting
 * the mode bits does NOT catch that (0640 is the mode in both the broken and
 * the fixed case), so the check that matters is an actual open() by an actual
 * non-root process, plus its mirror image: a process that is NOT in the trace
 * group must still be refused. Both halves are needed — a one-sided check
 * passes if the child silently kept root's credentials, and a mode-bit check
 * passes while nobody but root can read anything.
 *
 * The reader half needs root (chown to a foreign group, then setuid away from
 * it). Non-root, this test says so and runs the rest; the live tier's
 * tests/test_trace_sidecar_perms.sh does the same check against the real
 * daemon under sudo, where --require-live turns a skip into a failure. */
#include "event_writer.h"
#include "pg_wait_tracer.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

/* A path under /tmp that does NOT exist yet. */
static void scratch_path(char *out, size_t sz, const char *tag)
{
    snprintf(out, sz, "/tmp/pgwt_perms_%s_%d", tag, (int)getpid());
}

static void rm_rf(const char *dir)
{
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* best effort cleanup */ }
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

/* Push a handful of events and close, so current.trace exists on disk. */
static int write_a_trace(const char *dir, const char *group)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 18, 24, group) != 0)
        return -1;
    for (int i = 0; i < 64; i++) {
        struct pgwt_trace_event evt = {
            .timestamp_ns = 1000000000ULL + (uint64_t)i * 10000,
            .pid = 4242,
            .old_event = (i % 2) ? 0 : 0x05000015,
            .new_event = (i % 2) ? 0x05000015 : 0,
            .duration_ns = 5000,
            .query_id = 7ULL,
        };
        if (pgwt_writer_push_event(&w, &evt) != 0) {
            pgwt_writer_destroy(&w);
            return -1;
        }
    }
    int rc = pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    return rc;
}

/* ── #499: the sidecar write fails when the directory is not there yet ── */

static void test_missing_dir(void)
{
    printf("--- #499: sidecar into a not-yet-created trace dir ---\n");
    char dir[256], names[320];
    scratch_path(dir, sizeof(dir), "missing");
    rm_rf(dir);
    snprintf(names, sizeof(names), "%s/wait_event_names.json", dir);

    /* This is the product's old startup order, reproduced: discovery writing
     * the sidecar before anything created the directory. */
    int rc = pgwt_write_names_json(dir);
    CHECK(rc != 0, "write_names_json into a missing dir must fail, got %d", rc);
    CHECK(!file_exists(names), "no sidecar can exist at %s", names);
}

static void test_prepare_then_sidecar(void)
{
    printf("--- #499: prepare first, then the sidecar lands ---\n");
    char dir[256], names[320];
    scratch_path(dir, sizeof(dir), "prepared");
    rm_rf(dir);
    snprintf(names, sizeof(names), "%s/wait_event_names.json", dir);

    gid_t gid = (gid_t)0;
    int rc = pgwt_trace_dir_prepare(dir, NULL, &gid);
    CHECK(rc == 0, "trace_dir_prepare returned %d (%s)", rc, strerror(errno));
    CHECK(gid == (gid_t)-1, "no group requested -> gid unresolved, got %ld",
          (long)gid);

    struct stat st;
    CHECK(stat(dir, &st) == 0 && S_ISDIR(st.st_mode),
          "%s exists and is a directory", dir);
    CHECK((st.st_mode & 07777) == 0750, "trace dir mode 0750, got 0%o",
          (unsigned)(st.st_mode & 07777));

    rc = pgwt_write_names_json(dir);
    CHECK(rc == 0, "write_names_json after prepare returned %d", rc);
    CHECK(file_exists(names), "sidecar written at %s", names);

    /* It must be the real mapping, not an empty shell: pgwt-server reads
     * pg_version and names_source out of it. */
    FILE *f = fopen(names, "r");
    CHECK(f != NULL, "sidecar readable");
    if (f) {
        char buf[65536] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(n > 0 && strstr(buf, "\"pg_version\"") != NULL,
              "sidecar carries pg_version");
        CHECK(strstr(buf, "\"names_source\"") != NULL,
              "sidecar carries names_source provenance");
        CHECK(strstr(buf, "\"Timeout\"") != NULL,
              "sidecar carries the Timeout name table");
    }
    rm_rf(dir);
}

/* ── #500: group resolution ───────────────────────────────── */

static void test_default_group_contract(void)
{
    printf("--- #500: the default trace group exists on this host ---\n");
    const char *name = pgwt_default_trace_group();
    if (name) {
        CHECK(name[0] != '\0', "default group name is not empty");
        CHECK(getgrnam(name) != NULL,
              "default group '%s' must resolve (that was the whole bug)", name);
        printf("  note: default trace group on this host: %s\n", name);
    } else {
        /* Legitimate on a host with neither a dba group nor a postgres
         * account. The contract is only that it never hands back a name that
         * does not resolve — which is what "dba" did everywhere. */
        printf("  note: no candidate trace group exists on this host\n");
        CHECK(getgrnam("dba") == NULL && getpwnam("postgres") == NULL,
              "NULL default is only allowed when no candidate exists");
    }
}

static void test_explicit_missing_group_not_substituted(void)
{
    printf("--- #500: an explicit missing group is refused, not swapped ---\n");
    char dir[256];
    scratch_path(dir, sizeof(dir), "nogroup");
    rm_rf(dir);

    gid_t gid = (gid_t)0;
    int rc = pgwt_trace_dir_prepare(dir, "pgwt_no_such_group_zz", &gid);
    CHECK(rc == 0, "prepare still creates the dir, got %d", rc);
    CHECK(gid == (gid_t)-1,
          "an unresolvable explicit group must leave the gid unset, got %ld",
          (long)gid);
    rm_rf(dir);
}

static void test_writer_applies_group_and_mode(void)
{
    printf("--- #500: trace file and sidecar get the trace group ---\n");
    char dir[256], trace[320], names[320];
    scratch_path(dir, sizeof(dir), "mode");
    rm_rf(dir);
    snprintf(trace, sizeof(trace), "%s/current.trace", dir);
    snprintf(names, sizeof(names), "%s/wait_event_names.json", dir);

    /* As root, use an unprivileged account's group, so the gid assertion has
     * teeth: with our own (root) group a freshly created file already has
     * gid 0 and "the writer set the group" would be vacuously true.
     * Unprivileged, our own primary group is the only one we may chown to —
     * and there the gid is not the default for a file created under a
     * root-owned directory, so it still asserts something. */
    struct group *mine = NULL;
    if (geteuid() == 0) {
        struct passwd *pw = getpwnam("postgres");
        if (!pw) pw = getpwnam("nobody");
        if (pw && pw->pw_gid != 0) mine = getgrgid(pw->pw_gid);
    }
    if (!mine) mine = getgrgid(getgid());
    if (!mine) {
        printf("  FAIL: cannot resolve a group to write the trace with\n");
        tests_run++;
        return;
    }
    char gname[64];
    snprintf(gname, sizeof(gname), "%s", mine->gr_name);
    gid_t want = mine->gr_gid;

    gid_t gid = (gid_t)-1;
    CHECK(pgwt_trace_dir_prepare(dir, gname, &gid) == 0, "prepare %s", dir);
    CHECK(gid == want, "prepare resolved group '%s' to %ld, wanted %ld",
          gname, (long)gid, (long)want);

    /* Sidecar first, exactly like the daemon: discovery writes it, then the
     * writer initialises. The writer is what gives it the trace group. */
    CHECK(pgwt_write_names_json(dir) == 0, "sidecar written");
    CHECK(chmod(names, 0600) == 0, "sidecar starts group-unreadable (0600)");

    CHECK(write_a_trace(dir, gname) == 0, "trace written with group '%s'", gname);

    struct stat st;
    CHECK(stat(trace, &st) == 0, "current.trace exists");
    if (stat(trace, &st) == 0) {
        CHECK((st.st_mode & 07777) == 0640, "trace file mode 0640, got 0%o",
              (unsigned)(st.st_mode & 07777));
        CHECK(st.st_gid == want, "trace file group %ld, wanted %ld",
              (long)st.st_gid, (long)want);
    }
    CHECK(stat(names, &st) == 0, "sidecar still there");
    if (stat(names, &st) == 0) {
        CHECK((st.st_mode & 07777) == 0640,
              "sidecar re-moded to 0640 by writer_init, got 0%o",
              (unsigned)(st.st_mode & 07777));
        CHECK(st.st_gid == want, "sidecar group %ld, wanted %ld",
              (long)st.st_gid, (long)want);
    }
    rm_rf(dir);
}

/* ── #500: the check that actually matters — a non-root open() ── */

#define READER_OK_TRACE   0x1   /* the group-owned trace opened and read */
#define READER_NOT_ROOT   0x2   /* the child really dropped root */
#define READER_DENIED_ROOT 0x4  /* a root-only file was still refused */

/* Runs in the child, after dropping to uid/gid. Writes a result mask. */
static int reader_child(uid_t uid, gid_t gid, const char *trace,
                        const char *rootonly)
{
    int mask = 0;
    if (setgroups(1, &gid) != 0) { perror("setgroups"); _exit(60); }
    if (setgid(gid) != 0)        { perror("setgid");    _exit(61); }
    if (setuid(uid) != 0)        { perror("setuid");    _exit(62); }
    if (getuid() != 0 && geteuid() != 0 && setuid(0) != 0)
        mask |= READER_NOT_ROOT;   /* credentials really are gone */

    int fd = open(trace, O_RDONLY);
    if (fd >= 0) {
        char b[64];
        if (read(fd, b, sizeof(b)) > 0)
            mask |= READER_OK_TRACE;
        close(fd);
    } else {
        fprintf(stderr, "  child: open(%s) failed: %s\n", trace,
                strerror(errno));
    }

    int fd2 = open(rootonly, O_RDONLY);
    if (fd2 < 0 && errno == EACCES)
        mask |= READER_DENIED_ROOT;
    else if (fd2 >= 0)
        close(fd2);

    _exit(mask);
}

static void test_non_root_reader(void)
{
    printf("--- #500: a non-root process opens the trace ---\n");
    if (geteuid() != 0) {
        printf("  note: needs root (chown to a foreign group + setuid away "
               "from it). The same check runs against the real daemon in "
               "tests/test_trace_sidecar_perms.sh, where run_all.sh "
               "--require-live counts a skip as a failure.\n");
        return;
    }

    /* The account a non-root replay or demo service actually runs as. */
    struct passwd *pw = getpwnam("postgres");
    if (!pw) pw = getpwnam("nobody");
    CHECK(pw != NULL, "an unprivileged account (postgres/nobody) must exist "
                      "for this check to mean anything");
    if (!pw) return;
    uid_t ruid = pw->pw_uid;
    gid_t rgid = pw->pw_gid;
    CHECK(ruid != 0 && rgid != 0, "reader account is not root (uid %ld gid %ld)",
          (long)ruid, (long)rgid);
    if (ruid == 0 || rgid == 0) return;

    struct group *gr = getgrgid(rgid);
    CHECK(gr != NULL, "reader's primary group %ld resolves", (long)rgid);
    if (!gr) return;
    char gname[64];
    snprintf(gname, sizeof(gname), "%s", gr->gr_name);

    char dir[256], trace[320], rootonly[320];
    scratch_path(dir, sizeof(dir), "reader");
    rm_rf(dir);
    snprintf(trace, sizeof(trace), "%s/current.trace", dir);
    snprintf(rootonly, sizeof(rootonly), "%s/rootonly.trace", dir);

    CHECK(write_a_trace(dir, gname) == 0, "trace written with group '%s'",
          gname);

    /* The NEGATIVE CONTROL, in-process: a file left the way the old default
     * left every trace (root:root 0640) in the very same directory. If the
     * child can read this one, the reader check has no teeth. */
    int fd = open(rootonly, O_CREAT | O_WRONLY | O_TRUNC, 0640);
    CHECK(fd >= 0, "created %s", rootonly);
    if (fd >= 0) {
        CHECK(write(fd, "root-only payload\n", 18) == 18, "wrote control file");
        close(fd);
    }
    CHECK(chown(rootonly, 0, 0) == 0, "control file is root:root");
    CHECK(chmod(rootonly, 0640) == 0, "control file is 0640");

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0)
        reader_child(ruid, rgid, trace, rootonly);
    CHECK(pid > 0, "forked reader child");
    if (pid <= 0) { rm_rf(dir); return; }

    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid, "reaped reader child");
    CHECK(WIFEXITED(status), "reader child exited normally (status %d)", status);
    int mask = WIFEXITED(status) ? WEXITSTATUS(status) : 0;

    CHECK((mask & READER_NOT_ROOT) != 0,
          "reader child really dropped root (mask 0x%x)", mask);
    CHECK((mask & READER_OK_TRACE) != 0,
          "uid %ld / group '%s' opened and read %s (mask 0x%x)",
          (long)ruid, gname, trace, mask);
    CHECK((mask & READER_DENIED_ROOT) != 0,
          "the root:root 0640 control file stayed unreadable (mask 0x%x)",
          mask);

    rm_rf(dir);
}

int main(void)
{
    printf("=== test_trace_perms (#499 sidecar order, #500 reader access) ===\n");
    pgwt_init_event_names(18);

    test_missing_dir();
    test_prepare_then_sidecar();
    test_default_group_contract();
    test_explicit_missing_group_not_substituted();
    test_writer_applies_group_and_mode();
    test_non_root_reader();

    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    if (tests_run == 0) {
        printf("FAILED: no checks ran at all\n");
        return 1;
    }
    return tests_passed == tests_run ? 0 : 1;
}
