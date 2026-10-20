/* test_exec_sites.c — #464: canonical USDT execution-probe site selection.
 *
 * The defect: PostgreSQL emits query__execute__start/done from TWO code
 * sites for plannable non-SELECT work (around PortalRun, and again inside
 * the static helper PortalRunMulti calls), libbpf attaches USDT probes by
 * NAME, and so every DML statement produced two nested EXEC_START/EXEC_END
 * pairs. Measured on gate-1 against the trees in this repo:
 *
 *     pre-fix / admit-all mutant   UPDATE exec 40  plan 20   (N = 20)
 *     fixed                        UPDATE exec 20  plan 20
 *
 * This file covers the userspace half — the ELF note parser, the pure
 * selector and, above all, every way the selector could be made to APPROVE
 * without having looked at anything. The live half is
 * tests/test_execution_cardinality.py.
 *
 * Two test axes:
 *
 *  1. Pure selector (pgwt_exec_sites_select) driven with hand-written
 *     plans, including the REAL addresses read off gate-1's PG18 package on
 *     2026-10-19 (readelf -n / readelf -sW; see src/discovery.h for the
 *     table). That fixture is the regression anchor: it is the exact input
 *     shape that used to admit both pairs.
 *
 *  2. Real ELF parsing against THIS binary, which carries four hand-rolled
 *     `.note.stapsdt` entries built the same way sys/sdt.h builds
 *     PostgreSQL's — two well-formed ones (one inside the canonical
 *     function, one outside it) and, placed FIRST in the note table, two
 *     deliberately unusable ones (a note whose name is not "stapsdt", and
 *     one whose descsz cannot hold three addresses). A parser that aborts
 *     or resynchronises on the bad notes finds zero good sites and this
 *     test goes red, which is the false-negative the bypass suite below is
 *     mostly about: a gate that cannot see must refuse, never approve.
 *
 * Built twice (-pie / -no-pie) exactly like test_discovery, because the
 * link-time-to-runtime translation these sites need is the #24 axis.
 */
#include "discovery.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <elf.h>

static int run = 0, failed = 0;
#define CHECK(c, ...) do { \
        run++; \
        if (!(c)) { failed++; printf("  FAIL: "); printf(__VA_ARGS__); \
                    printf("\n"); } \
    } while (0)

/* ── The probe notes this binary carries ──────────────────────────────── */

/* sys/sdt.h's base anchor, verbatim in effect: a weak hidden symbol every
 * note records its link-time view of, so a prelinked image can be corrected
 * by the difference against the section's current address. */
__asm__(".ifndef _.stapsdt.base\n"
        ".pushsection .stapsdt.base,\"aG\",\"progbits\",.stapsdt.base,comdat\n"
        ".weak _.stapsdt.base\n"
        ".hidden _.stapsdt.base\n"
        "_.stapsdt.base: .space 1\n"
        ".size _.stapsdt.base,1\n"
        ".popsection\n"
        ".endif\n");

/* A well-formed note, as sdt.h emits it: nop at the probe site, note in
 * .note.stapsdt recording loc / base / semaphore then provider, name, args. */
#define PGWT_TEST_NOTE(prov, nm) \
    __asm__ volatile ( \
        "990:\n" \
        "   nop\n" \
        ".pushsection .note.stapsdt,\"?\",\"note\"\n" \
        "   .balign 4\n" \
        "   .4byte 992f-991f\n" \
        "   .4byte 994f-993f\n" \
        "   .4byte 3\n" \
        "991: .asciz \"stapsdt\"\n" \
        "992: .balign 4\n" \
        "993: .8byte 990b\n" \
        "   .8byte _.stapsdt.base\n" \
        "   .8byte 0\n" \
        "   .asciz \"" prov "\"\n" \
        "   .asciz \"" nm "\"\n" \
        "   .asciz \"\"\n" \
        "994: .balign 4\n" \
        ".popsection\n")

/* Note #1 in the table: NT_STAPSDT type, but the note name is not
 * "stapsdt". A parser that trusts the type alone reads the next 24 bytes as
 * three addresses and invents a site. */
__asm__(".pushsection .note.stapsdt,\"?\",\"note\"\n"
        ".balign 4\n"
        ".4byte 2f-1f\n"
        ".4byte 4f-3f\n"
        ".4byte 3\n"
        "1: .asciz \"notstap\"\n"
        "2: .balign 4\n"
        "3: .8byte 0xdeadbeef\n"
        "   .8byte 0\n"
        "   .8byte 0\n"
        "   .asciz \"pgwttest\"\n"
        "   .asciz \"probe_exec\"\n"
        "   .asciz \"\"\n"
        "4: .balign 4\n"
        ".popsection\n");

/* Note #2: correct name, but descsz holds only four bytes — it cannot carry
 * loc/base/semaphore at all. Reading it anyway walks off the end of the
 * descriptor into the next note's header. */
__asm__(".pushsection .note.stapsdt,\"?\",\"note\"\n"
        ".balign 4\n"
        ".4byte 12f-11f\n"
        ".4byte 14f-13f\n"
        ".4byte 3\n"
        "11: .asciz \"stapsdt\"\n"
        "12: .balign 4\n"
        "13: .4byte 0x41414141\n"
        "14: .balign 4\n"
        ".popsection\n");

/* The canonical function: its probe site is the one that must be admitted.
 * noinline + a side effect keep it a real, sized, non-merged FUNC symbol. */
volatile int pgwt_test_sink;

__attribute__((noinline)) void pgwt_test_canonical_fn(void)
{
    PGWT_TEST_NOTE("pgwttest", "probe_exec");
    pgwt_test_sink += 1;
}

/* The other site: a different function entirely, standing in for the inner
 * PortalRunMulti pair. On the shipped PostgreSQL packages that one is a
 * STATIC function in a binary with zero LOCAL FUNC symbols, so it resolves
 * to no name at all — which is why the selector keys on "inside the
 * canonical range" and never on the excluded site being identifiable. */
__attribute__((noinline)) void pgwt_test_other_fn(void)
{
    PGWT_TEST_NOTE("pgwttest", "probe_exec");
    pgwt_test_sink += 2;
}

/* A FUNC symbol with NO .size directive, hence st_size == 0: it cannot
 * bound a range, and pgwt_elf_func_range must refuse rather than return a
 * degenerate [v, v) (admits nothing, reads as success) or treat it as
 * unbounded (admits everything). */
__asm__(".globl pgwt_test_sizeless_fn\n"
        ".type pgwt_test_sizeless_fn, @function\n"
        "pgwt_test_sizeless_fn:\n"
        "   ret\n");

/* ── Section 1: the real PG18 fixture (regression anchor) ─────────────── */

/* readelf -n / readelf -sW, /usr/lib/postgresql/18/bin/postgres on gate-1,
 * 2026-10-19. PortalRun = 0x5a6e70 + 1167 bytes. */
#define PG18_PORTALRUN_LO   0x5a6e70ULL
#define PG18_PORTALRUN_HI   (0x5a6e70ULL + 1167)
#define PG18_START_INNER    0x5a6bc0ULL   /* static helper — excluded */
#define PG18_DONE_INNER     0x5a6c2fULL   /* static helper — excluded */
#define PG18_START_PORTALRUN 0x5a6eb8ULL  /* PortalRun — admitted */
#define PG18_DONE_PORTALRUN  0x5a711eULL  /* PortalRun — admitted */

static void add_site(struct pgwt_exec_site_plan *p, uint64_t addr, int marker)
{
    p->sites[p->n_sites].elf_addr = addr;
    p->sites[p->n_sites].marker = marker;
    p->sites[p->n_sites].canonical = 0;
    p->sites[p->n_sites].runtime_addr = 0;
    p->n_sites++;
}

static void pg18_plan(struct pgwt_exec_site_plan *p)
{
    memset(p, 0, sizeof(*p));
    add_site(p, PG18_START_INNER,     PGWT_SITE_EXEC_START);
    add_site(p, PG18_START_PORTALRUN, PGWT_SITE_EXEC_START);
    add_site(p, PG18_DONE_INNER,      PGWT_SITE_EXEC_DONE);
    add_site(p, PG18_DONE_PORTALRUN,  PGWT_SITE_EXEC_DONE);
    p->fn_lo = PG18_PORTALRUN_LO;
    p->fn_hi = PG18_PORTALRUN_HI;
}

static uint64_t admitted_addr(const struct pgwt_exec_site_plan *p, int marker)
{
    for (int i = 0; i < p->n_sites; i++)
        if (p->sites[i].canonical && p->sites[i].marker == marker)
            return p->sites[i].elf_addr;
    return 0;
}

static void test_pg18_fixture(void)
{
    struct pgwt_exec_site_plan p;
    pg18_plan(&p);
    CHECK(pgwt_exec_sites_select(&p) == 0, "PG18 fixture must be admitted");
    CHECK(p.refusal == PGWT_EXEC_SITES_OK, "PG18 fixture refusal == OK (got %d)",
          (int)p.refusal);
    CHECK(p.n_canonical == 2, "PG18: exactly 2 admitted sites (got %d)",
          p.n_canonical);
    CHECK(p.n_excluded == 2, "PG18: exactly 2 excluded sites (got %d)",
          p.n_excluded);
    CHECK(p.n_start_canonical == 1 && p.n_done_canonical == 1,
          "PG18: one start + one done admitted (got %d/%d)",
          p.n_start_canonical, p.n_done_canonical);
    /* The whole point: the admitted pair is PortalRun's, not the inner
     * helper's. Swapping these two constants is the defect. */
    CHECK(admitted_addr(&p, PGWT_SITE_EXEC_START) == PG18_START_PORTALRUN,
          "PG18: admitted start is PortalRun's 0x%llx (got 0x%llx)",
          (unsigned long long)PG18_START_PORTALRUN,
          (unsigned long long)admitted_addr(&p, PGWT_SITE_EXEC_START));
    CHECK(admitted_addr(&p, PGWT_SITE_EXEC_DONE) == PG18_DONE_PORTALRUN,
          "PG18: admitted done is PortalRun's 0x%llx (got 0x%llx)",
          (unsigned long long)PG18_DONE_PORTALRUN,
          (unsigned long long)admitted_addr(&p, PGWT_SITE_EXEC_DONE));
    /* The inner pair must be flagged non-canonical, not merely left out of
     * a count: daemon.c publishes per-site and the mutant hook flips on
     * exactly this flag. */
    for (int i = 0; i < p.n_sites; i++)
        if (p.sites[i].elf_addr == PG18_START_INNER ||
            p.sites[i].elf_addr == PG18_DONE_INNER)
            CHECK(p.sites[i].canonical == 0,
                  "PG18: inner site 0x%llx must be flagged excluded",
                  (unsigned long long)p.sites[i].elf_addr);
}

/* A packaged build that declares only the PortalRun pair must still be
 * admitted: "there was nothing to exclude" is not a failure. */
static void test_single_site_build_is_admitted(void)
{
    struct pgwt_exec_site_plan p;
    memset(&p, 0, sizeof(p));
    add_site(&p, PG18_START_PORTALRUN, PGWT_SITE_EXEC_START);
    add_site(&p, PG18_DONE_PORTALRUN,  PGWT_SITE_EXEC_DONE);
    p.fn_lo = PG18_PORTALRUN_LO;
    p.fn_hi = PG18_PORTALRUN_HI;
    CHECK(pgwt_exec_sites_select(&p) == 0,
          "a build with only the canonical pair is admitted");
    CHECK(p.n_canonical == 2 && p.n_excluded == 0,
          "single-pair build: 2 admitted, 0 excluded (got %d/%d)",
          p.n_canonical, p.n_excluded);
}

/* ── Section 2: the bypass suite ──────────────────────────────────────────
 *
 * Every entry is a way the selector could be reached without being able to
 * see what it is checking. Each must REFUSE with its own reason — not
 * approve, and not approve-with-a-warning. A refusal makes daemon.c skip
 * the attach entirely, so execution telemetry goes absent (and loud)
 * instead of silently doubled. */

static void test_bypass_empty_input(void)
{
    struct pgwt_exec_site_plan p;
    memset(&p, 0, sizeof(p));
    p.fn_lo = PG18_PORTALRUN_LO;
    p.fn_hi = PG18_PORTALRUN_HI;
    CHECK(pgwt_exec_sites_select(&p) == -1, "no sites at all must refuse");
    CHECK(p.refusal == PGWT_EXEC_SITES_NO_NOTES,
          "no sites -> NO_NOTES (got %d)", (int)p.refusal);
    CHECK(p.n_canonical == 0, "no sites -> 0 admitted");
}

static void test_bypass_unresolvable_base(void)
{
    /* The canonical function could not be resolved at all. Classifying
     * against [0, 0) would mark every site non-canonical and report
     * NO_CANONICAL — the same words a genuinely odd binary produces. The
     * cause must be distinguishable, and it must refuse either way. */
    struct pgwt_exec_site_plan p;
    pg18_plan(&p);
    p.fn_lo = 0;
    p.fn_hi = 0;
    CHECK(pgwt_exec_sites_select(&p) == -1,
          "unresolvable canonical function must refuse");
    CHECK(p.refusal == PGWT_EXEC_SITES_NO_FUNCTION,
          "unresolved function -> NO_FUNCTION (got %d)", (int)p.refusal);

    /* And an inverted/degenerate range is the same refusal, not an accident
     * that admits everything. */
    pg18_plan(&p);
    p.fn_hi = p.fn_lo;
    CHECK(pgwt_exec_sites_select(&p) == -1, "empty [lo,lo) range must refuse");
    CHECK(p.refusal == PGWT_EXEC_SITES_NO_FUNCTION,
          "empty range -> NO_FUNCTION (got %d)", (int)p.refusal);

    pg18_plan(&p);
    p.fn_hi = p.fn_lo - 1;
    CHECK(pgwt_exec_sites_select(&p) == -1, "inverted range must refuse");
}

static void test_bypass_thing_checked_is_absent(void)
{
    /* Notes exist, the function resolves — but no site is inside it. This is
     * "the thing being checked is absent rather than wrong": a future PG
     * that moves the boundary out of PortalRun. Refuse; do not fall back to
     * the inner pair just because it is the only one left. */
    struct pgwt_exec_site_plan p;
    memset(&p, 0, sizeof(p));
    add_site(&p, PG18_START_INNER, PGWT_SITE_EXEC_START);
    add_site(&p, PG18_DONE_INNER,  PGWT_SITE_EXEC_DONE);
    p.fn_lo = PG18_PORTALRUN_LO;
    p.fn_hi = PG18_PORTALRUN_HI;
    CHECK(pgwt_exec_sites_select(&p) == -1,
          "sites all outside the canonical function must refuse");
    CHECK(p.refusal == PGWT_EXEC_SITES_NO_CANONICAL,
          "outside-only -> NO_CANONICAL (got %d)", (int)p.refusal);
    CHECK(p.n_canonical == 0, "outside-only -> 0 admitted");
}

static void test_bypass_partial_pair(void)
{
    /* A start inside PortalRun with its done outside it (or missing) would
     * produce open-forever executions, which read as "slow queries", not as
     * a capture fault. Refuse. */
    struct pgwt_exec_site_plan p;
    memset(&p, 0, sizeof(p));
    add_site(&p, PG18_START_PORTALRUN, PGWT_SITE_EXEC_START);
    add_site(&p, PG18_DONE_INNER,      PGWT_SITE_EXEC_DONE);
    p.fn_lo = PG18_PORTALRUN_LO;
    p.fn_hi = PG18_PORTALRUN_HI;
    CHECK(pgwt_exec_sites_select(&p) == -1,
          "canonical start without a canonical done must refuse");
    CHECK(p.refusal == PGWT_EXEC_SITES_AMBIGUOUS,
          "half a pair -> AMBIGUOUS (got %d)", (int)p.refusal);

    memset(&p, 0, sizeof(p));
    add_site(&p, PG18_START_INNER,    PGWT_SITE_EXEC_START);
    add_site(&p, PG18_DONE_PORTALRUN, PGWT_SITE_EXEC_DONE);
    p.fn_lo = PG18_PORTALRUN_LO;
    p.fn_hi = PG18_PORTALRUN_HI;
    CHECK(pgwt_exec_sites_select(&p) == -1,
          "canonical done without a canonical start must refuse");
}

static void test_bypass_two_canonical_starts(void)
{
    /* Two starts INSIDE the canonical range: either a second
     * instrumentation pair moved into PortalRun, or an inlined clone
     * sharing its symbol range. Admitting both is exactly the double count.
     * Picking one arbitrarily would be worse: it would look fixed. */
    struct pgwt_exec_site_plan p;
    pg18_plan(&p);
    p.sites[0].elf_addr = PG18_PORTALRUN_LO + 8;   /* inner start moved in */
    CHECK(pgwt_exec_sites_select(&p) == -1,
          "two canonical starts must refuse");
    CHECK(p.refusal == PGWT_EXEC_SITES_AMBIGUOUS,
          "two canonical starts -> AMBIGUOUS (got %d)", (int)p.refusal);
}

static void test_bypass_preexisting_refusal_is_not_cleared(void)
{
    /* pgwt_elf_usdt_sites sets OVERFLOW and returns -1. If the selector
     * then ran on the truncated list and found a plausible pair, it would
     * approve a PARTIAL site range — the "partial commit range" case. A
     * refusal already recorded upstream must survive. */
    struct pgwt_exec_site_plan p;
    pg18_plan(&p);
    p.refusal = PGWT_EXEC_SITES_OVERFLOW;
    CHECK(pgwt_exec_sites_select(&p) == -1,
          "an upstream refusal must not be overwritten by a clean select");
    CHECK(p.refusal == PGWT_EXEC_SITES_OVERFLOW,
          "upstream refusal preserved (got %d)", (int)p.refusal);
    CHECK(p.n_canonical == 0,
          "upstream refusal -> 0 admitted (got %d)", p.n_canonical);
}

static void test_bypass_null_plan(void)
{
    CHECK(pgwt_exec_sites_select(NULL) == -1, "NULL plan must refuse");
}

/* ── Section 3: runtime address translation (the #24 axis) ────────────── */

static void test_runtime_addr(void)
{
    /* PIE: elf_base 0, so runtime = elf + load_base. */
    CHECK(pgwt_site_runtime_addr(0x5a6eb8, 0, 0x7f0000000000ULL)
              == 0x7f00005a6eb8ULL, "PIE translation");
    /* non-PIE: elf_base == load_base, so the address is unchanged. */
    CHECK(pgwt_site_runtime_addr(0x4a02df, 0x400000, 0x400000) == 0x4a02df,
          "non-PIE translation is identity");
    /* A missing load base must NOT produce a plausible-looking address:
     * publishing one would make the allowlist match nothing (silent zero
     * executions) or, worse, something unrelated. */
    CHECK(pgwt_site_runtime_addr(0x5a6eb8, 0, 0) == 0,
          "load_base 0 -> 0, never a wild address");
    CHECK(pgwt_site_runtime_addr(0, 0, 0x400000) == 0, "elf_addr 0 -> 0");
    /* Underflow (elf_addr below the ELF base) is a resolution bug, not an
     * address to wrap around into. */
    CHECK(pgwt_site_runtime_addr(0x1000, 0x400000, 0x400000) == 0,
          "elf_addr < elf_base -> 0, never a wrapped address");
}

/* ── Section 4: real ELF parsing against this binary ─────────────────── */

static int elf_type_of(const char *path)
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

static void test_elf_notes_in_this_binary(void)
{
    const char *self = "/proc/self/exe";

    /* The build really is the ELF type it claims to cover — otherwise a
     * toolchain default silently collapses both builds into one. */
    int want = PGWT_TEST_EXPECT_PIE ? ET_DYN : ET_EXEC;
    CHECK(elf_type_of(self) == want, "this build is %s (e_type %d)",
          PGWT_TEST_EXPECT_PIE ? "PIE" : "non-PIE", elf_type_of(self));

    struct pgwt_exec_site_plan p;
    memset(&p, 0, sizeof(p));
    int n = pgwt_elf_usdt_sites(self, "pgwttest", "probe_exec",
                                PGWT_SITE_EXEC_START, &p);
    /* Exactly the two WELL-FORMED notes. Three or four means a malformed
     * note was read as real; fewer than two means the parser gave up on
     * one, which is the false negative that would make the whole fix
     * silently refuse on a real binary. */
    CHECK(n == 2, "parser finds exactly the 2 well-formed notes (got %d)", n);
    CHECK(p.n_sites == 2, "plan holds 2 sites (got %d)", p.n_sites);

    uint64_t lo = 0, hi = 0;
    CHECK(pgwt_elf_func_range(self, "pgwt_test_canonical_fn", &lo, &hi) == 0,
          "canonical function range resolves");
    CHECK(hi > lo, "canonical function has a non-empty range");
    p.fn_lo = lo;
    p.fn_hi = hi;

    /* Both notes carry the same provider/name and the same marker tag, so
     * the only thing separating them is containment. Tag the one outside as
     * the DONE marker so the selector's one-of-each rule is satisfiable;
     * what is under test here is WHICH address lands inside the range. */
    uint64_t other_lo = 0, other_hi = 0;
    CHECK(pgwt_elf_func_range(self, "pgwt_test_other_fn",
                              &other_lo, &other_hi) == 0,
          "non-canonical function range resolves");

    int inside = 0, outside = 0;
    for (int i = 0; i < p.n_sites; i++) {
        uint64_t a = p.sites[i].elf_addr;
        if (a >= lo && a < hi) inside++;
        else if (a >= other_lo && a < other_hi) outside++;
    }
    CHECK(inside == 1, "exactly one note is inside the canonical function "
          "(got %d)", inside);
    CHECK(outside == 1, "exactly one note is inside the other function "
          "(got %d)", outside);

    /* Provider and name must both match. A parser that keyed on the name
     * alone would pick up any provider's probe of the same name. */
    struct pgwt_exec_site_plan q;
    memset(&q, 0, sizeof(q));
    CHECK(pgwt_elf_usdt_sites(self, "postgresql", "probe_exec",
                              PGWT_SITE_EXEC_START, &q) == 0,
          "wrong provider matches nothing");
    memset(&q, 0, sizeof(q));
    CHECK(pgwt_elf_usdt_sites(self, "pgwttest", "query__execute__start",
                              PGWT_SITE_EXEC_START, &q) == 0,
          "wrong probe name matches nothing");
}

static void test_elf_refusals(void)
{
    struct pgwt_exec_site_plan p;
    uint64_t lo = 1, hi = 1;

    /* Missing tool/file: a path that does not exist. -1, never 0 — "no
     * notes in a file I could not open" is a different answer from "no
     * notes in this binary", and only the second is a fact about the
     * binary. */
    memset(&p, 0, sizeof(p));
    CHECK(pgwt_elf_usdt_sites("/nonexistent/pgwt/postgres", "pgwttest",
                              "probe_exec", PGWT_SITE_EXEC_START, &p) == -1,
          "missing binary -> -1");
    CHECK(pgwt_elf_func_range("/nonexistent/pgwt/postgres", "PortalRun",
                              &lo, &hi) == -1, "missing binary -> range -1");
    CHECK(lo == 0 && hi == 0, "failed range resolution zeroes its outputs");

    /* A readable file that is not an ELF object. */
    char path[] = "/tmp/pgwt_exec_sites_notelfXXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "create non-ELF scratch file");
    if (fd >= 0) {
        const char junk[] = "this is definitely not an ELF binary\n";
        ssize_t w = write(fd, junk, sizeof(junk) - 1);
        CHECK(w == (ssize_t)(sizeof(junk) - 1), "write scratch file");
        close(fd);
        memset(&p, 0, sizeof(p));
        CHECK(pgwt_elf_usdt_sites(path, "pgwttest", "probe_exec",
                                  PGWT_SITE_EXEC_START, &p) == -1,
              "non-ELF file -> -1");
        CHECK(pgwt_elf_func_range(path, "PortalRun", &lo, &hi) == -1,
              "non-ELF file -> range -1");
        unlink(path);
    }

    /* An absent symbol, and a FUNC symbol with st_size == 0: both are
     * "cannot bound the canonical function", both must refuse. The sizeless
     * one is the subtle case — the symbol EXISTS, so a resolver that only
     * checked for presence would hand back a range that admits nothing and
     * call it success. */
    CHECK(pgwt_elf_func_range("/proc/self/exe", "pgwt_no_such_symbol_zz",
                              &lo, &hi) == -1, "absent symbol -> -1");
    CHECK(pgwt_elf_func_range("/proc/self/exe", "pgwt_test_sizeless_fn",
                              &lo, &hi) == -1,
          "FUNC symbol with st_size 0 -> -1 (must not be treated as a range)");

    /* pgwt_resolve_exec_sites end to end on a binary that has no
     * query__execute__* notes at all (this one): refuse, and say it was the
     * notes that were missing. */
    memset(&p, 0, sizeof(p));
    CHECK(pgwt_resolve_exec_sites("/proc/self/exe", getpid(), &p) == -1,
          "a binary with no postgresql notes must refuse");
    CHECK(p.refusal == PGWT_EXEC_SITES_NO_NOTES,
          "no postgresql notes -> NO_NOTES (got %d: %s)", (int)p.refusal,
          pgwt_exec_site_refusal_str(p.refusal));
    CHECK(pgwt_resolve_exec_sites("", getpid(), &p) == -1,
          "empty binary path must refuse");
    CHECK(p.refusal == PGWT_EXEC_SITES_NO_BINARY,
          "empty path -> NO_BINARY (got %d)", (int)p.refusal);

    /* Every refusal code has a distinct, non-empty string: a diagnosis
     * nobody can read is how a refusal gets mistaken for a hiccup. */
    for (int r = PGWT_EXEC_SITES_OK; r <= PGWT_EXEC_SITES_OVERFLOW; r++) {
        const char *s = pgwt_exec_site_refusal_str((enum pgwt_exec_site_refusal)r);
        CHECK(s && s[0] && strcmp(s, "unknown") != 0,
              "refusal %d has a real description", r);
    }
}

int main(void)
{
    printf("test_exec_sites: #464 canonical execution probe sites\n");

    /* Keep the instrumented functions reachable so the compiler cannot
     * discard them (and with them their notes). */
    pgwt_test_canonical_fn();
    pgwt_test_other_fn();
    CHECK(pgwt_test_sink == 3, "instrumented functions ran (sink=%d)",
          pgwt_test_sink);

    test_pg18_fixture();
    test_single_site_build_is_admitted();
    test_bypass_empty_input();
    test_bypass_unresolvable_base();
    test_bypass_thing_checked_is_absent();
    test_bypass_partial_pair();
    test_bypass_two_canonical_starts();
    test_bypass_preexisting_refusal_is_not_cleared();
    test_bypass_null_plan();
    test_runtime_addr();
    test_elf_notes_in_this_binary();
    test_elf_refusals();

    printf("test_exec_sites: %d/%d checks passed\n", run - failed, run);
    if (failed) {
        printf("test_exec_sites: FAILED (%d)\n", failed);
        return 1;
    }
    printf("test_exec_sites: PASSED\n");
    return 0;
}
