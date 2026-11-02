/* test_discovery.c — unit tests for the #24 (load-base / symbol resolution)
 * regression class: pgwt_find_load_base_in_maps, pgwt_find_symbol_offset,
 * pgwt_resolve_symbol.
 *
 * This code broke in the field twice (PR #24: non-PIE EL8 load base; the
 * earlier "first r--p" bug) and had zero tests. Two test axes:
 *
 *  1. Committed /proc/<pid>/maps fixtures (tests/fixtures/maps_*.txt,
 *     provenance in tests/fixtures/README.md): EL8 non-PIE, EL9 PIE,
 *     Ubuntu PIE, and the adversarial CAP-4 case (extension .so whose path
 *     contains "postgres" mapped below the binary).
 *
 *  2. Self-resolution: this binary is built TWICE by tests/Makefile — once
 *     -pie (test_discovery_pie), once -no-pie (test_discovery_nopie), the
 *     exact axis #24 broke on. Each build resolves a known global symbol in
 *     its own image via /proc/self/exe + /proc/self/maps and asserts the
 *     computed runtime VA equals the symbol's actual address. A load-base
 *     regression on either ELF type fails here in milliseconds, no
 *     PostgreSQL needed.
 *
 * PGWT_TEST_EXPECT_PIE (1/0) is set by the Makefile per build so the test
 * also verifies it really IS the ELF type it claims to cover (guards
 * against toolchain defaults silently overriding the -pie/-no-pie flags).
 */
#include "discovery.h"
#include "daemon.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int run = 0, ok = 0;
#define CHECK(c, ...) do { \
        run++; \
        if (c) { ok++; } \
        else { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

/* A global with external linkage in .data: the self-resolution target.
 * volatile + non-zero init keep it present and unmergeable. */
volatile uint32_t pgwt_test_fixture_symbol = 0xC0FFEE42u;

/* pgwt_discover() resolves these globals from a PG17+ postmaster.  They make
 * this test binary a sufficient discovery fixture without a live database. */
uint32_t *my_wait_event_info;
void *MyBEEntry;

/* Locate tests/fixtures/ relative to this binary (cwd-independent). */
static void fixture_path(char *out, size_t outsz, const char *name)
{
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) { snprintf(out, outsz, "fixtures/%s", name); return; }
    exe[n] = '\0';
    snprintf(out, outsz, "%s/fixtures/%s", dirname(exe), name);
}

static uint64_t load_base_fixture(const char *name, const char *basename_)
{
    char path[600];
    fixture_path(path, sizeof(path), name);
    return pgwt_find_load_base_in_maps(path, basename_);
}

/* ELF type (ET_EXEC / ET_DYN) of a binary, for the PIE/non-PIE sanity check. */
static int elf_type(const char *path)
{
    Elf64_Ehdr eh;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t r = fread(&eh, 1, sizeof(eh), f);
    fclose(f);
    if (r != sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0)
        return -1;
    return eh.e_type;
}

static void test_validation_exclusions_reset_per_discovery_cycle(void)
{
    struct pgwt_daemon *d = calloc(1, sizeof(*d));
    pid_t reused_pid = (pid_t)2147483647;

    CHECK(d != NULL, "allocate discovery fixture");
    if (!d)
        return;
    CHECK(pgwt_pgbs_exclusion_add(&d->pgbs_validation_exclusions,
                                  reused_pid) == 0 &&
          d->pgbs_validation_exclusions.count == 1 &&
          d->pgbs_validation_exclusions.entries[0].start_time == 0 &&
          pgwt_pgbs_exclusion_contains(&d->pgbs_validation_exclusions,
                                       reused_pid),
          "prior discovery cycle retains a conservative numeric-PID exclusion");

    d->postmaster_pid = getpid();
    CHECK(setenv("PGWT_TEST_PGBS_VALIDATION_FAIL", "1", 1) == 0,
          "set forced validation failure hook");
    int rc = pgwt_discover(d);
    CHECK(unsetenv("PGWT_TEST_PGBS_VALIDATION_FAIL") == 0,
          "clear forced validation failure hook");

    CHECK(rc == 0 && d->pgbs_validation_exclusions.count == 0 &&
          !pgwt_pgbs_exclusion_contains(&d->pgbs_validation_exclusions,
                                        reused_pid),
          "new discovery cycle clears stale numeric-PID exclusions before validation");
    free(d->pg_binary_saved);
    free(d);
}

/* #584: exact_probes.query_offset / .activity_offset are FILE offsets into
 * the postgres binary, lazily computed ONCE by exact_probe.c's attach_link
 * and never reset by pgwt_exact_probe_bundle_forget_skeleton (exact_probe.h
 * says so explicitly: they "do not move when a process restarts" -- true
 * for a SAME-binary restart, false across a re-attach onto a DIFFERENT
 * binary, where the comment itself calls this "pre-existing and not
 * addressed"). Before this fix nothing ever cleared them, so a second
 * pgwt_discover() cycle left whatever a prior cycle had computed completely
 * untouched -- a stale file offset handed to the new binary's uprobe
 * attach, not detected, not refused: a uprobe fires on whatever instruction
 * happens to sit at that byte offset in the NEW binary, producing
 * plausible-looking garbage rather than an error.
 *
 * Non-vacuous without a second PostgreSQL binary or a live database: a
 * sentinel that cannot possibly be a real computed offset (0xdeadbeef) is
 * set BEFORE the discovery cycle; the assertion is that it is gone AFTER,
 * regardless of what pgwt_discover() recomputes it to. Re-attaching to the
 * SAME binary cannot distinguish "recomputed to the same value" from
 * "never touched" -- which is exactly the trap a same-binary re-attach test
 * would fall into -- but a changed sentinel proves the cache was cleared
 * independent of what it was cleared TO. The stronger assertions below
 * pin both offsets to 0: pgwt_discover() never calls exact_probe.c, so it
 * cannot resolve them regardless of the fixture's symbol table. Mistaking
 * missing symbols for the reason could lead a reader to weaken == 0 back
 * to != 0xdeadbeef, re-opening the bypass #584 closed. */
static void test_exact_probe_offsets_reset_per_discovery_cycle(void)
{
    struct pgwt_daemon *d = calloc(1, sizeof(*d));
    CHECK(d != NULL, "allocate discovery fixture");
    if (!d)
        return;

    d->exact_probes.query_offset = 0xdeadbeefULL;
    d->exact_probes.activity_offset = 0xdeadbeefULL;
    d->postmaster_pid = getpid();
    int rc = pgwt_discover(d);

    CHECK(rc == 0, "discovery cycle succeeds against this test binary");
    CHECK(d->exact_probes.query_offset != 0xdeadbeefULL,
          "a new discovery cycle does not leave a prior cycle's "
          "query_offset untouched");
    CHECK(d->exact_probes.activity_offset != 0xdeadbeefULL,
          "a new discovery cycle does not leave a prior cycle's "
          "activity_offset untouched");

    /* "Changed" alone is too weak to be evidence: an implementation that
     * overwrote the cache with any non-sentinel junk (say 1) would satisfy
     * the two assertions above and still hand a wrong FILE offset to
     * bpf_program__attach_uprobe_opts(), which is the entire bug. Pin the
     * value, not just the change. pgwt_discover() itself never resolves
     * these two symbols (no exact_probe.c call anywhere in discovery.c --
     * attach_link() does the lazy lookup later), so the only correct state
     * on return is the cleared one: 0, the "not resolved yet" value that
     * makes attach_link() re-derive later. The fixture's symbol table is
     * irrelevant to this state. Attributing it to missing symbols could
     * invite weakening == 0 back to != 0xdeadbeef, re-opening the bypass
     * #584 closed. */
    CHECK(d->exact_probes.query_offset == 0,
          "query_offset is cleared to the re-derive/refuse value, not to junk");
    CHECK(d->exact_probes.activity_offset == 0,
          "activity_offset is cleared to the re-derive/refuse value, not to junk");

    free(d->pg_binary_saved);
    free(d);
}

/* #664: a PG13 -> PG14+ re-attach must discard the prior PG13 attribution
 * state. This binary reports PG17 via --version. Check allocation and
 * discovery success, then pin the non-PG13 version so a changed fixture
 * cannot accidentally exercise legitimate PG13 re-initialization instead.
 * The cleared flag selects pgstat_report_query_id and preserves the
 * st_query_id refusal; each offset must be exactly 0 because daemon.c
 * copies it verbatim into BPF rodata. pgss_loaded must also be re-derived.
 * Check the sentinel precondition before discovery: an already-zeroed
 * struct would satisfy every reset assertion vacuously without the fix. */
static void test_pg13_query_attr_reset_per_discovery_cycle(void)
{
    struct pgwt_daemon *d = calloc(1, sizeof(*d));
    CHECK(d != NULL, "PG13 re-attach: allocate discovery fixture");
    if (!d)
        return;

    d->use_myproc = true;
    d->pgss_loaded = true;
    d->use_pg13_query_attr = true;
    d->pg13_qd_plannedstmt_off = 0x5101;
    d->pg13_ps_queryid_off = 0x5102;
    d->pg13_qd_sourcetext_off = 0x5103;
    CHECK(d->use_pg13_query_attr && d->pg13_qd_plannedstmt_off != 0 &&
          d->pg13_ps_queryid_off != 0 && d->pg13_qd_sourcetext_off != 0,
          "PG13 re-attach: prior-cycle sentinel state is present");

    d->postmaster_pid = getpid();
    int rc = pgwt_discover(d);

    CHECK(rc == 0, "PG13 re-attach: discovery succeeds");
    CHECK(d->pg_major_version != 13,
          "PG13 re-attach: fixture reports a different major version");
    CHECK(!d->use_pg13_query_attr,
          "PG13 re-attach: clear use_pg13_query_attr for symbol selection");
    CHECK(d->pg13_qd_plannedstmt_off == 0,
          "PG13 re-attach: pg13_qd_plannedstmt_off is zero");
    CHECK(d->pg13_ps_queryid_off == 0,
          "PG13 re-attach: pg13_ps_queryid_off is zero");
    CHECK(d->pg13_qd_sourcetext_off == 0,
          "PG13 re-attach: pg13_qd_sourcetext_off is zero");
    CHECK(!d->pgss_loaded, "PG13 re-attach: clear pgss_loaded");

    free(d->pg_binary_saved);
    free(d);
}

static int discover_capture_stderr(struct pgwt_daemon *d, char *text,
                                   size_t textsz, const char *step)
{
    text[0] = '\0';
    FILE *capture = tmpfile();
    CHECK(capture != NULL, "refusal transition %s: create stderr capture", step);
    if (!capture)
        return -2;

    int saved_stderr = dup(STDERR_FILENO);
    CHECK(saved_stderr >= 0, "refusal transition %s: save real stderr", step);
    if (saved_stderr < 0) {
        fclose(capture);
        return -2;
    }

    fflush(stderr);
    int redirected = dup2(fileno(capture), STDERR_FILENO);
    CHECK(redirected >= 0, "refusal transition %s: redirect stderr", step);
    if (redirected < 0) {
        close(saved_stderr);
        fclose(capture);
        return -2;
    }

    int rc = pgwt_discover(d);
    fflush(stderr);
    int restored = dup2(saved_stderr, STDERR_FILENO);
    CHECK(restored >= 0, "refusal transition %s: restore real stderr", step);
    close(saved_stderr);

    rewind(capture);
    size_t n = fread(text, 1, textsz - 1, capture);
    text[n] = '\0';
    CHECK(!ferror(capture) && feof(capture),
          "refusal transition %s: read complete stderr capture", step);
    fclose(capture);
    return rc;
}

static void stop_stripped_discovery_fixture(pid_t pid, const char *copy)
{
    if (pid > 0) {
        CHECK(kill(pid, SIGKILL) == 0,
              "refusal transition: kill stripped fixture");
        pid_t reaped;
        do {
            reaped = waitpid(pid, NULL, 0);
        } while (reaped < 0 && errno == EINTR);
        CHECK(reaped == pid, "refusal transition: reap stripped fixture");
    }
    if (copy[0] != '\0')
        CHECK(unlink(copy) == 0, "refusal transition: unlink stripped fixture");
}

static pid_t start_stripped_discovery_fixture(char *copy, size_t copysz)
{
    char exe[4096];
    int source = -1, target = -1;
    pid_t pid = -1;
    copy[0] = '\0';

    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    CHECK(n > 0 && n < (ssize_t)sizeof(exe) - 1,
          "refusal transition: locate binary for stripped fixture");
    if (n <= 0 || n >= (ssize_t)sizeof(exe) - 1)
        return -1;
    exe[n] = '\0';
    source = open(exe, O_RDONLY);
    CHECK(source >= 0, "refusal transition: open binary for stripped fixture");
    if (source < 0)
        return -1;

    int len = snprintf(copy, copysz, "/tmp/pgwt_test_discovery_stripped.XXXXXX");
    CHECK(len >= 0 && (size_t)len < copysz,
          "refusal transition: stripped fixture path fits");
    if (len < 0 || (size_t)len >= copysz) {
        copy[0] = '\0';
        goto fail;
    }
    target = mkstemp(copy);
    CHECK(target >= 0, "refusal transition: create stripped fixture copy");
    if (target < 0) {
        copy[0] = '\0';
        goto fail;
    }
    int chmod_rc = fchmod(target, 0755);
    CHECK(chmod_rc == 0, "refusal transition: make stripped fixture executable");
    if (chmod_rc != 0)
        goto fail;

    char bytes[16384];
    int copied = 1;
    for (;;) {
        n = read(source, bytes, sizeof(bytes));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            copied = (n == 0);
            break;
        }
        ssize_t offset = 0;
        while (offset < n) {
            ssize_t written = write(target, bytes + offset, (size_t)(n - offset));
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                copied = 0;
                break;
            }
            offset += written;
        }
        if (!copied)
            break;
    }
    CHECK(copied, "refusal transition: copy complete fixture binary");
    int close_rc = close(target);
    target = -1;
    CHECK(close_rc == 0, "refusal transition: close fixture copy before stripping");
    close(source);
    source = -1;
    if (!copied || close_rc != 0)
        goto fail;

    /* Keep .symtab: discovery still needs my_wait_event_info and MyBEEntry. */
    char command[4096];
    snprintf(command, sizeof(command), "objcopy --strip-debug %s 2>/dev/null", copy);
    int strip_rc = system(command);
    if (strip_rc != 0) {
        snprintf(command, sizeof(command), "strip --strip-debug %s 2>/dev/null", copy);
        strip_rc = system(command);
    }
    CHECK(strip_rc == 0,
          "refusal transition: objcopy/strip tool missing or --strip-debug failed");
    if (strip_rc != 0)
        goto fail;

    int ready[2];
    int pipe_rc = pipe(ready);
    CHECK(pipe_rc == 0, "refusal transition: create stripped fixture handshake pipe");
    if (pipe_rc != 0)
        goto fail;
    pid = fork();
    if (pid == 0) {
        close(ready[0]);
        char fdstr[32];
        snprintf(fdstr, sizeof(fdstr), "%d", ready[1]);
        execl(copy, copy, "--pgwt-test-stay-alive", fdstr, (char *)NULL);
        _exit(127);
    }
    close(ready[1]);
    CHECK(pid > 0, "refusal transition: fork stripped fixture");
    if (pid < 0) {
        close(ready[0]);
        goto fail;
    }
    char byte;
    do {
        n = read(ready[0], &byte, 1);
    } while (n < 0 && errno == EINTR);
    close(ready[0]);
    CHECK(n == 1, "refusal transition: stripped fixture exec handshake succeeds");
    if (n != 1)
        goto fail;
    return pid;

fail:
    if (source >= 0)
        close(source);
    if (target >= 0)
        close(target);
    stop_stripped_discovery_fixture(pid, copy);
    copy[0] = '\0';
    return -1;
}

/* The debug build's own PgBackendStatus DWARF supplies st_query_id=104;
 * the known-offset table supplies 424 for PG17/18 on x86_64; and the
 * PGPROC wait offset is known only for PG13, so PG14/15/16 fail earlier.
 * PG13 is exempt from this refusal. Thus PG19+ with no DWARF is the only
 * reachable configuration: a PG13 daemon re-attaching to PG19 installed
 * without a -dbgsym/-debuginfo package. Do not simplify this fixture back
 * into a discovery that never reaches the refusal under test. */
static void test_repeated_refusal_logs_once_per_transition(void)
{
    char copy[512] = "";
    pid_t pid = -1;
    struct pgwt_daemon *reset = calloc(1, sizeof(*reset));
    struct pgwt_daemon *a = calloc(1, sizeof(*a));
    struct pgwt_daemon *b = calloc(1, sizeof(*b));
    CHECK(reset != NULL, "refusal transition: allocate initial successful daemon");
    CHECK(a != NULL, "refusal transition: allocate refusing daemon A");
    CHECK(b != NULL, "refusal transition: allocate successful daemon B");
    if (!reset || !a || !b)
        goto cleanup;

    int env_rc = setenv("PGWT_TEST_FIXTURE_PG_VERSION", "19.0", 1);
    CHECK(env_rc == 0, "refusal transition: configure PG19 fixture version");
    if (env_rc != 0)
        goto cleanup;
    pid = start_stripped_discovery_fixture(copy, sizeof(copy));
    if (pid < 0)
        goto cleanup;

    /* The memo is file-static. A successful default-view discovery first
     * clears it so this test does not depend on which tests ran before it. */
    reset->postmaster_pid = pid;
    CHECK(pgwt_discover(reset) == 0,
          "refusal transition: initial default-view discovery clears memo");

    a->view = PGWT_VIEW_QUERY_EVENT;
    a->postmaster_pid = pid;
    a->verbose = false;
    char text[16384];
    int rc1 = discover_capture_stderr(a, text, sizeof(text), "first call");
    CHECK(rc1 == -1, "refusal transition: the refusal is reachable at all");
    CHECK(text[0] != '\0', "refusal transition: first stderr capture is non-empty");
    CHECK(strstr(text, "st_query_id offset not found for PG19") != NULL,
          "refusal transition: first call logs exact PG19 refusal reason");

    int rc2 = discover_capture_stderr(a, text, sizeof(text), "repeat call");
    CHECK(rc2 == -1, "refusal transition: suppressed repeat still refuses discovery");
    CHECK(strstr(text, "st_query_id offset not found") == NULL,
          "refusal transition: immediate repeat suppresses reason");

    b->postmaster_pid = pid;
    CHECK(pgwt_discover(b) == 0,
          "refusal transition: daemon B default-view success clears memo");

    int rc3 = discover_capture_stderr(a, text, sizeof(text), "recurring call");
    CHECK(rc3 == -1, "refusal transition: recurrence after success refuses discovery");
    CHECK(strstr(text, "st_query_id offset not found for PG19") != NULL,
          "refusal transition: recurrence after success logs exact PG19 reason again");

cleanup:
    stop_stripped_discovery_fixture(pid, copy);
    CHECK(unsetenv("PGWT_TEST_FIXTURE_PG_VERSION") == 0,
          "refusal transition: clear fixture version override");
    if (reset)
        free(reset->pg_binary_saved);
    free(reset);
    if (a)
        free(a->pg_binary_saved);
    free(a);
    if (b)
        free(b->pg_binary_saved);
    free(b);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--pgwt-test-stay-alive") == 0) {
        int fd = atoi(argv[2]);
        ssize_t n;
        do {
            n = write(fd, "x", 1);
        } while (n < 0 && errno == EINTR);
        close(fd);
        if (n != 1)
            return 127;
        for (;;)
            pause();
    }

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        const char *version = getenv("PGWT_TEST_FIXTURE_PG_VERSION");
        printf("postgres (PostgreSQL) %s\n", version && version[0] ? version : "17.0");
        return 0;
    }

    printf("=== test_discovery (%s build) ===\n",
           PGWT_TEST_EXPECT_PIE ? "PIE" : "non-PIE");

    /* ── 1. maps fixtures: pgwt_find_load_base_in_maps ─────────────── */

    /* EL8 non-PIE PGDG: first mapping of the binary is the r-xp text
     * segment at 0x400000 (no r--p below it). The old "first r--p" match
     * returned the rodata base 0xf5d000 here → wrong VAs → zero events
     * (the #24 symptom). */
    CHECK(load_base_fixture("maps_el8_nonpie.txt", "postgres") == 0x400000ULL,
          "EL8 non-PIE load base != 0x400000");

    /* EL9 PIE PGDG: first mapping is the r--p ELF-header page. */
    CHECK(load_base_fixture("maps_el9_pie.txt", "postgres") == 0x55d4c1000000ULL,
          "EL9 PIE load base != 0x55d4c1000000");

    /* Ubuntu PGDG deb, PIE. */
    CHECK(load_base_fixture("maps_ubuntu_pie.txt", "postgres") == 0x5560d1a00000ULL,
          "Ubuntu PIE load base != 0x5560d1a00000");

    /* Binary not mapped at all → 0 (caller fails loudly, never guesses). */
    CHECK(load_base_fixture("maps_ubuntu_pie.txt", "no_such_binary") == 0,
          "missing basename should return 0");

    /* ── CAP-4 adversarial case (fixed in Phase T4) ───────────────────
     * pg_stat_statements.so mapped BELOW the binary; its path
     * "/usr/lib/postgresql/17/lib/..." contains the substring "postgres".
     * The old whole-line strstr() match picked the .so's base
     * (0x4f2a80000000) → every symbol VA garbage → zero events, silently
     * (the #24 class). The exact pathname-field match must resolve the
     * BINARY's base. Hard assert — this was T0's pinned expected-failure. */
    CHECK(load_base_fixture("maps_ext_below_binary.txt", "postgres")
              == 0x5560d1a00000ULL,
          "CAP-4: extension .so below binary must not win (exact-basename "
          "match)");

    /* Same fixture through the FULL-path match — the path the daemon
     * actually uses (pgwt_resolve_symbol passes /proc/<pid>/exe). */
    CHECK(load_base_fixture("maps_ext_below_binary.txt",
                            "/usr/lib/postgresql/17/bin/postgres")
              == 0x5560d1a00000ULL,
          "CAP-4: full-path match resolves the binary's base");

    /* A full path that names the EXTENSION resolves the extension —
     * proving the comparison is exact-pathname, not basename-of-path. */
    CHECK(load_base_fixture("maps_ext_below_binary.txt",
                            "/usr/lib/postgresql/17/lib/pg_stat_statements.so")
              == 0x4f2a80000000ULL,
          "full-path match of the .so itself resolves the .so");

    /* Substring of a basename must NOT match: "postgres" is a substring of
     * "pg_stat_statements.so"'s path but of no basename in the EL9 fixture
     * except the binary's own. A name that is a strict substring of the
     * binary basename must find nothing. */
    CHECK(load_base_fixture("maps_el9_pie.txt", "postgre") == 0,
          "strict-substring basename must not match");

    /* ── 2. self-resolution: the #24 regression test proper ───────────
     * Resolve a known global in THIS binary through the real code path
     * (ELF symbol table + /proc/self/maps + PIE/non-PIE base arithmetic)
     * and compare against the symbol's actual address. */

    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    CHECK(n > 0, "readlink(/proc/self/exe) failed");
    if (n > 0) {
        exe[n] = '\0';
        const char *base = strrchr(exe, '/');
        base = base ? base + 1 : exe;

        /* The build flag must have produced the ELF type it claims. */
        int et = elf_type(exe);
#if PGWT_TEST_EXPECT_PIE
        CHECK(et == ET_DYN, "expected PIE (ET_DYN), got e_type=%d — "
              "-pie flag did not take effect", et);
#else
        CHECK(et == ET_EXEC, "expected non-PIE (ET_EXEC), got e_type=%d — "
              "-no-pie flag did not take effect", et);
#endif

        uint64_t sym_val = pgwt_find_symbol_offset(exe, "pgwt_test_fixture_symbol");
        CHECK(sym_val != 0, "pgwt_find_symbol_offset found nothing");

        /* Both match forms must find the load base: exact basename (fixture
         * form) and the full /proc/self/exe path (the daemon's form). */
        uint64_t load_base = pgwt_find_load_base(getpid(), base);
        CHECK(load_base != 0, "pgwt_find_load_base(self, basename) returned 0");
        CHECK(pgwt_find_load_base(getpid(), exe) == load_base,
              "full-path and basename load base disagree");

        uint64_t va = pgwt_resolve_symbol(exe, "pgwt_test_fixture_symbol",
                                          getpid());
        uint64_t actual = (uint64_t)(uintptr_t)&pgwt_test_fixture_symbol;
        CHECK(va == actual,
              "resolved VA 0x%llx != actual &symbol 0x%llx (load_base=0x%llx, "
              "sym_val=0x%llx) — the #24 load-base bug class",
              (unsigned long long)va, (unsigned long long)actual,
              (unsigned long long)load_base, (unsigned long long)sym_val);

        /* And the resolved address must read back the magic value through
         * /proc/<pid>/mem — exactly how the daemon consumes it. */
        uint64_t readback = pgwt_read_pointer(getpid(), va);
        CHECK((uint32_t)readback == 0xC0FFEE42u,
              "read-through-/proc/mem at resolved VA gave 0x%llx, not the magic",
              (unsigned long long)readback);

        /* ── 3. vaddr -> uprobe FILE offset (the T2-study defect-1 class) ──
         * The daemon computes uprobe file offsets from a symbol's st_value.
         * The old `va - 0x400000` heuristic was a non-PIE assumption: on PIE
         * builds the uprobe attached to a dead byte and NEVER fired,
         * silently. pgwt_vaddr_to_file_offset must translate through the
         * PT_LOAD program headers for BOTH ELF types (this test runs in the
         * -pie and -no-pie builds).
         *
         * Proof "the offset lands on the symbol": the bytes in the FILE at
         * the computed offset must equal the bytes in MEMORY at the symbol's
         * runtime address — for a data object (the magic value) and for a
         * function (the daemon probes functions). x86_64 text/data are not
         * relocated in place, so file bytes == memory bytes. */
        uint64_t data_off = pgwt_vaddr_to_file_offset(exe, sym_val);
        CHECK(data_off != 0, "vaddr_to_file_offset(data symbol) returned 0");

        FILE *ef = fopen(exe, "rb");
        CHECK(ef != NULL, "cannot open own binary");
        if (ef) {
            /* Offset must land inside the file. */
            fseek(ef, 0, SEEK_END);
            long fsz = ftell(ef);
            CHECK(data_off + 4 <= (uint64_t)fsz,
                  "data offset 0x%llx beyond file size %ld",
                  (unsigned long long)data_off, fsz);

            uint32_t file_val = 0;
            fseek(ef, (long)data_off, SEEK_SET);
            CHECK(fread(&file_val, 1, 4, ef) == 4 && file_val == 0xC0FFEE42u,
                  "file bytes at translated offset 0x%llx are 0x%x, not the "
                  "magic — offset does not land on the symbol",
                  (unsigned long long)data_off, file_val);

            /* Function symbol — the actual uprobe use case. */
            uint64_t fn_val = pgwt_find_symbol_offset(exe, "main");
            CHECK(fn_val != 0, "cannot resolve 'main' in own binary");
            uint64_t fn_off = pgwt_vaddr_to_file_offset(exe, fn_val);
            CHECK(fn_off != 0, "vaddr_to_file_offset(function) returned 0");
            CHECK(fn_off + 16 <= (uint64_t)fsz,
                  "function offset 0x%llx beyond file size %ld",
                  (unsigned long long)fn_off, fsz);

            uint8_t file_bytes[16] = {0};
            fseek(ef, (long)fn_off, SEEK_SET);
            CHECK(fread(file_bytes, 1, 16, ef) == 16 &&
                  memcmp(file_bytes, (const void *)(uintptr_t)main, 16) == 0,
                  "file bytes at fn offset 0x%llx != memory bytes of main() — "
                  "a uprobe at this offset would land on a dead/wrong byte "
                  "(the PIE defect class)",
                  (unsigned long long)fn_off);

            /* The old heuristic must be provably wrong on at least one of
             * the two builds: on PIE, st_value - 0x400000 (when applicable)
             * must NOT equal the correct translation. On non-PIE, where
             * p_offset == p_vaddr - image_base for the classic layout, the
             * two may coincide — that coincidence is exactly why the bug
             * shipped. Only assert the divergence where it must exist. */
#if PGWT_TEST_EXPECT_PIE
            uint64_t heuristic = fn_val > 0x400000 ? fn_val - 0x400000 : fn_val;
            CHECK(heuristic != fn_off || fn_val <= 0x400000,
                  "PIE: heuristic accidentally equals the real offset — "
                  "fixture no longer covers the defect");
#endif

            /* A vaddr in no PT_LOAD (way past the image) must return 0. */
            CHECK(pgwt_vaddr_to_file_offset(exe, 0x7FFFFFFFFFFFULL) == 0,
                  "unmapped vaddr must translate to 0");

            fclose(ef);
        }
    }

    /* A daemon re-enters pgwt_discover() after each PostgreSQL restart. */
    test_validation_exclusions_reset_per_discovery_cycle();
    test_exact_probe_offsets_reset_per_discovery_cycle();
    test_pg13_query_attr_reset_per_discovery_cycle();
    test_repeated_refusal_logs_once_per_transition();

    printf("\n%d/%d tests passed\n", ok, run);
    return (ok == run) ? 0 : 1;
}
