/* test_system_event_offcpu.c — #498: the system_event view's %DB column must
 * be a COMPLETE decomposition of DB Time, which means it needs the Off-CPU*
 * row that until now only the two time_model printers emitted.
 *
 * Off-CPU* is not a wait event and so appears in no acc->system_events slot:
 * an on-CPU interval's wall splits into the measured cpu ns (CPU row) and the
 * off-CPU remainder (runqueue / throttle / unaccounted), and map_reader.c
 * files the remainder straight into the time model. A reader who only looked
 * at system_event therefore saw percentages that stopped short of 100% with
 * nothing on screen explaining the gap, while time_model — same trace, same
 * numbers — added up.
 *
 * WHAT THIS FILE ASSERTS, and why in this shape:
 *
 *   §1 the row exists, with the RIGHT duration and the RIGHT %DB, in the
 *      single-window printer, and the whole %DB column sums to 100%. The sum
 *      is scraped from the printed text and compared against the literal
 *      100.0 — never against another sum of the same inputs, which is how a
 *      conservation check in this project once became unfailable.
 *   §2 it takes its own SORT POSITION rather than being tacked on the end
 *      (two fixtures: residual smallest, residual largest).
 *   §3 ABSENCE: a legacy/sampled shape has a structurally zero residual and
 *      must print NO row — a "0.0%" line would assert a measurement nobody
 *      made. This is the "the thing is absent rather than wrong" case.
 *   §4 the multi-window printer, which is a SECOND function with its own
 *      column layout; a single-window-only test would be green on a
 *      half-applied fix.
 *   §5 BYPASS SUITE: the ways this file's own detection could stop seeing.
 *      Every assertion in §1 is satisfiable by a view that printed nothing —
 *      "no wrong Off-CPU* row" is trivially true of an empty table, and a
 *      %DB sum over zero rows is 0, not 100. §5 walks an empty accumulator, a
 *      zero DB Time, a capture that returns nothing, and the 20-row cap (a
 *      residual printed only from inside the row loop would be invisible in
 *      any busy capture, which is every real one).
 *
 * Runs anywhere the daemon objects link; no BPF calls, no PostgreSQL. Every
 * setup failure is fatal with a non-zero exit, never a skip.
 */
#include "daemon.h"
#include "output.h"
#include "map_reader.h"
#include "snapshot.h"
#include "wait_event.h"
#include "idle_rule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

static int checks = 0;
static int failures = 0;

#define CHECK(cond, fmt, ...) do {                                        \
    checks++;                                                             \
    if (cond) printf("  ok: " fmt "\n", ##__VA_ARGS__);                   \
    else { failures++;                                                    \
           printf("  FAIL(%s:%d): " fmt "\n", __FILE__, __LINE__,         \
                  ##__VA_ARGS__); }                                       \
} while (0)

#define MS 1000000ULL

/* The on-CPU pseudo-event. pgwt_event_full_name renders id 0 as "CPU*"
 * (src/wait_event.c) -- the star marks a derived row, exactly as Off-CPU*
 * does. The first version of this file looked for a row named "CPU", and
 * its own positive control is what caught that: every "no wrong row"
 * assertion below would otherwise have passed against a table the parser
 * was not actually reading. */
#define EV_CPU  0u
#define EV_IO   WEI(PG_WAIT_IO, 21)     /* IO:DataFileRead */
#define EV_LOCK WEI(PG_WAIT_LOCK, 0)

/* The cell the view already uses for "does not apply" (fmt_pct_db). The
 * Off-CPU* row has no occurrence count, average or maximum, and must not
 * print 0 for them: 0 would be a measured zero. */
#define DASH_CELL "—"

/* ── printed-table capture and parsing ────────────────────────────────── */

#define MAX_ROWS 64

struct prow {
    char   name[64];
    char   count_cell[32];   /* verbatim: digits, or the em dash */
    double total_ms;
    double pct;              /* from the trailing "...%" cell */
    int    order;            /* 0-based print order among parsed rows */
};

struct ptable {
    struct prow rows[MAX_ROWS];
    int n;
    int lines;               /* total captured lines, so "nothing printed"
                              * is distinguishable from "nothing matched" */
};

/* Capture one printer's stdout. Returns a malloc'd NUL-terminated buffer.
 * A capture that cannot be set up is FATAL: a test that silently measures
 * nothing is worse than one that fails. */
static char *capture(void (*fn)(struct pgwt_daemon *), struct pgwt_daemon *d)
{
    FILE *cap = tmpfile();
    if (!cap) { printf("  FATAL: tmpfile\n"); exit(2); }
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    if (saved < 0) { printf("  FATAL: dup\n"); exit(2); }
    if (dup2(fileno(cap), STDOUT_FILENO) < 0) {
        printf("  FATAL: dup2\n"); exit(2);
    }
    fn(d);
    fflush(stdout);
    if (dup2(saved, STDOUT_FILENO) < 0) { printf("  FATAL: dup2 back\n"); exit(2); }
    close(saved);
    fseek(cap, 0, SEEK_END);
    long size = ftell(cap);
    if (size < 0) { printf("  FATAL: ftell\n"); exit(2); }
    rewind(cap);
    char *buf = calloc((size_t)size + 1, 1);
    if (!buf) { printf("  FATAL: oom\n"); exit(2); }
    size_t rd = fread(buf, 1, (size_t)size, cap);
    (void)rd;
    fclose(cap);
    return buf;
}

/* A data row of either system_event layout:
 *   "  <name> <count|dash> <total_ms> <avg> [<max>] <pct>%"
 * Accepted only when the name is a single token, the total parses as a
 * number, and the LAST token ends in '%'. That rejects the column header
 * (ends in "DB"), the rule line, and every banner. */
static void parse_table(const char *text, struct ptable *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = eol ? eol + 1 : p + strlen(p);
        out->lines++;

        if (line[0] != ' ' || line[1] != ' ') continue;

        char *tok[12];
        int nt = 0;
        for (char *s = strtok(line, " \t"); s && nt < 12; s = strtok(NULL, " \t"))
            tok[nt++] = s;
        if (nt < 4) continue;

        char *last = tok[nt - 1];
        size_t ll = strlen(last);
        if (ll < 2 || last[ll - 1] != '%') continue;

        char *endp = NULL;
        double total = strtod(tok[2], &endp);
        if (!endp || endp == tok[2] || *endp != '\0') continue;

        char pctbuf[32];
        snprintf(pctbuf, sizeof(pctbuf), "%.*s", (int)(ll - 1), last);
        endp = NULL;
        double pct = strtod(pctbuf, &endp);
        if (!endp || endp == pctbuf || *endp != '\0') continue;

        if (out->n >= MAX_ROWS) continue;
        struct prow *r = &out->rows[out->n];
        snprintf(r->name, sizeof(r->name), "%s", tok[0]);
        snprintf(r->count_cell, sizeof(r->count_cell), "%s", tok[1]);
        r->total_ms = total;
        r->pct = pct;
        r->order = out->n;
        out->n++;
    }
}

static const struct prow *find_row(const struct ptable *t, const char *name)
{
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->rows[i].name, name) == 0)
            return &t->rows[i];
    return NULL;
}

/* The conservation verdict, as three states rather than a bool: a sum that
 * saw no rows must REFUSE, never approve. (sum_pct is returned so a failure
 * message can say what it actually read.) */
enum sum_verdict { SUM_REFUSED_NO_ROWS, SUM_CLOSES, SUM_SHORT };

static enum sum_verdict pct_sum(const struct ptable *t, double *sum_out)
{
    double sum = 0.0;
    for (int i = 0; i < t->n; i++)
        sum += t->rows[i].pct;
    if (sum_out) *sum_out = sum;
    if (t->n == 0) return SUM_REFUSED_NO_ROWS;
    return fabs(sum - 100.0) < 0.15 ? SUM_CLOSES : SUM_SHORT;
}

/* ── fixtures ─────────────────────────────────────────────────────────── */

static struct pgwt_daemon *new_daemon(void)
{
    struct pgwt_daemon *d = calloc(1, sizeof(*d));
    if (!d) { printf("  FATAL: oom (daemon)\n"); exit(2); }
    d->interval = 1;
    d->format = PGWT_FMT_TUI;   /* no per-interval timestamp line */
    return d;
}

static void add_event(struct pgwt_accumulator *acc, uint32_t we,
                      uint64_t count, uint64_t total_ns)
{
    struct pgwt_event_stats *se = pgwt_get_or_create_system_event(acc, we);
    if (!se) { printf("  FATAL: system_event slot\n"); exit(2); }
    se->count = count;
    se->total_ns = total_ns;
    se->max_ns = count ? total_ns / count : 0;
    se->min_ns = se->max_ns;
}

/* db = cpu + offcpu + waits exactly, so the printed column has 100% to find.
 * No idle event is in the fixture on purpose: an idle row's %DB prints as the
 * em dash ("not applicable"), so including one would make the column's sum a
 * statement about the idle rule rather than about this fix. */
static struct pgwt_daemon *fixture(uint64_t cpu_ms, uint64_t offcpu_ms,
                                   uint64_t io_ms, uint64_t lock_ms)
{
    struct pgwt_daemon *d = new_daemon();
    struct pgwt_accumulator *acc = &d->accum;
    pgwt_accum_init(acc);
    acc->tm.cpu_time_ns    = cpu_ms * MS;
    acc->tm.offcpu_time_ns = offcpu_ms * MS;
    acc->tm.io_time_ns     = io_ms * MS;
    acc->tm.lock_time_ns   = lock_ms * MS;
    acc->tm.db_time_ns     = (cpu_ms + offcpu_ms + io_ms + lock_ms) * MS;
    if (cpu_ms)  add_event(acc, EV_CPU, 10, cpu_ms * MS);
    if (io_ms)   add_event(acc, EV_IO, 5, io_ms * MS);
    if (lock_ms) add_event(acc, EV_LOCK, 2, lock_ms * MS);
    return d;
}

/* ── 1. the row, its numbers, and the column that now closes ──────────── */
static void t1_single_window(void)
{
    printf("--- 1. #498: single-window system_event carries Off-CPU* ---\n");

    /* 600 CPU + 150 off-CPU + 250 IO = 1000 ms DB Time */
    struct pgwt_daemon *d = fixture(600, 150, 250, 0);
    char *text = capture(pgwt_print_system_event, d);
    struct ptable t;
    parse_table(text, &t);

    /* POSITIVE CONTROL first: the parser read the real table. Without this
     * every assertion below is also true of an empty capture. */
    const struct prow *cpu = find_row(&t, "CPU*");
    const struct prow *io  = find_row(&t, "IO:DataFileRead");
    CHECK(cpu != NULL && io != NULL,
          "the parser found the CPU* and IO:DataFileRead rows "
          "(%d rows over %d lines) — the rest of this section is about a "
          "table that exists", t.n, t.lines);
    if (cpu) CHECK(fabs(cpu->total_ms - 600.0) < 0.05,
                   "CPU* row reads 600.0 ms (got %.1f)", cpu->total_ms);
    if (io)  CHECK(fabs(io->total_ms - 250.0) < 0.05,
                   "IO:DataFileRead row reads 250.0 ms (got %.1f)", io->total_ms);

    const struct prow *off = find_row(&t, "Off-CPU*");
    CHECK(off != NULL, "an Off-CPU* row is printed");
    if (off) {
        CHECK(fabs(off->total_ms - 150.0) < 0.05,
              "Off-CPU* reads the time model's residual, 150.0 ms (got %.1f)",
              off->total_ms);
        CHECK(fabs(off->pct - 15.0) < 0.05,
              "Off-CPU* %%DB is 150/1000 = 15.0%% (got %.1f%%)", off->pct);
        CHECK(strcmp(off->count_cell, DASH_CELL) == 0,
              "Off-CPU* count cell is the inapplicable dash, not a measured "
              "0 (got \"%s\")", off->count_cell);
    }

    double sum = 0.0;
    enum sum_verdict v = pct_sum(&t, &sum);
    CHECK(v == SUM_CLOSES,
          "the %%DB column sums to 100%% (got %.1f%% over %d rows; verdict %d)",
          sum, t.n, (int)v);

    free(text);
    free(d);
}

/* ── 2. own sort position, not appended ───────────────────────────────── */
static void t2_sort_position(void)
{
    printf("--- 2. Off-CPU* sits in the view's own size order ---\n");

    /* Residual SMALLEST: 600 CPU, 250 IO, 150 off-CPU -> last of three. */
    struct pgwt_daemon *d1 = fixture(600, 150, 250, 0);
    char *x1 = capture(pgwt_print_system_event, d1);
    struct ptable t1;
    parse_table(x1, &t1);
    const struct prow *o1 = find_row(&t1, "Off-CPU*");
    CHECK(t1.n == 3, "2a. three rows printed (got %d)", t1.n);
    CHECK(o1 != NULL && o1->order == 2,
          "2a. smallest residual prints LAST (order %d)", o1 ? o1->order : -1);
    free(x1);
    free(d1);

    /* Residual LARGEST: 400 off-CPU, 350 CPU, 250 IO -> first of three.
     * A row appended after the loop would be last here too, so this is the
     * assertion that distinguishes "sorted" from "tacked on". */
    struct pgwt_daemon *d2 = fixture(350, 400, 250, 0);
    char *x2 = capture(pgwt_print_system_event, d2);
    struct ptable t2;
    parse_table(x2, &t2);
    const struct prow *o2 = find_row(&t2, "Off-CPU*");
    CHECK(t2.n == 3, "2b. three rows printed (got %d)", t2.n);
    CHECK(o2 != NULL && o2->order == 0,
          "2b. largest residual prints FIRST (order %d)", o2 ? o2->order : -1);
    if (o2) CHECK(fabs(o2->total_ms - 400.0) < 0.05,
                  "2b. Off-CPU* reads 400.0 ms (got %.1f)", o2->total_ms);
    double sum2 = 0.0;
    CHECK(pct_sum(&t2, &sum2) == SUM_CLOSES,
          "2b. column still sums to 100%% (got %.1f%%)", sum2);
    free(x2);
    free(d2);
}

/* ── 3. absence: a zero residual prints NO row ────────────────────────── */
static void t3_zero_residual(void)
{
    printf("--- 3. the legacy/sampled shape has no residual and no row ---\n");

    /* 750 CPU + 250 IO = 1000 ms; offcpu structurally 0 (gap-as-CPU). */
    struct pgwt_daemon *d = fixture(750, 0, 250, 0);
    char *text = capture(pgwt_print_system_event, d);
    struct ptable t;
    parse_table(text, &t);

    CHECK(find_row(&t, "CPU*") != NULL,
          "3. positive control: the CPU* row is there, so the absence below is "
          "about the residual and not about an empty view (%d rows)", t.n);
    CHECK(find_row(&t, "Off-CPU*") == NULL,
          "3. NO Off-CPU* row when the residual is 0 — a 0.0%% line would "
          "claim a measurement this tier never made");
    double sum = 0.0;
    CHECK(pct_sum(&t, &sum) == SUM_CLOSES,
          "3. the column already closed without it (got %.1f%%)", sum);

    free(text);
    free(d);
}

/* ── 4. the multi-window printer is a second function ─────────────────── */
static void t4_multi_window(void)
{
    printf("--- 4. --window system_event carries it too ---\n");

    struct pgwt_daemon *d = fixture(600, 150, 250, 0);
    if (pgwt_ring_init(&d->ring, 8) != 0) {
        printf("  FATAL: ring_init\n"); exit(2);
    }
    /* Window = one tick, so the delta is (populated - empty) = the fixture. */
    struct pgwt_accumulator *zero = calloc(1, sizeof(*zero));
    if (!zero) { printf("  FATAL: oom (zero accum)\n"); exit(2); }
    pgwt_accum_init(zero);
    pgwt_ring_push(&d->ring, zero);
    pgwt_ring_push(&d->ring, &d->accum);
    free(zero);
    d->num_windows = 1;
    d->windows[0] = 1;

    char *text = capture(pgwt_print_system_event, d);
    struct ptable t;
    parse_table(text, &t);

    CHECK(find_row(&t, "CPU*") != NULL && find_row(&t, "IO:DataFileRead") != NULL,
          "4. positive control: the windowed section printed its rows "
          "(%d rows over %d lines)", t.n, t.lines);
    const struct prow *off = find_row(&t, "Off-CPU*");
    CHECK(off != NULL, "4. the multi-window printer emits Off-CPU* as well");
    if (off) {
        CHECK(fabs(off->total_ms - 150.0) < 0.05,
              "4. windowed Off-CPU* reads 150.0 ms (got %.1f)", off->total_ms);
        CHECK(fabs(off->pct - 15.0) < 0.05,
              "4. windowed Off-CPU* %%DB is 15.0%% (got %.1f%%)", off->pct);
    }
    double sum = 0.0;
    CHECK(pct_sum(&t, &sum) == SUM_CLOSES,
          "4. the windowed %%DB column sums to 100%% (got %.1f%%)", sum);

    free(text);
    pgwt_ring_free(&d->ring);
    free(d);
}

/* ── 5. bypass suite: how this file could stop seeing ─────────────────── */
static void t5_bypass(void)
{
    printf("--- 5. bypass suite ---\n");

    /* 5a. EMPTY accumulator. Every §1 assertion of the form "no wrong row"
     * is satisfied by a view that printed no rows at all, and a %DB sum over
     * zero rows is 0. The verdict must be REFUSED, never "closes". */
    struct pgwt_daemon *de = new_daemon();
    pgwt_accum_init(&de->accum);
    char *xe = capture(pgwt_print_system_event, de);
    struct ptable te;
    parse_table(xe, &te);
    CHECK(te.lines > 0, "5a. the empty view still printed something (%d lines)",
          te.lines);
    CHECK(te.n == 0, "5a. and no data rows (got %d)", te.n);
    double se = 0.0;
    CHECK(pct_sum(&te, &se) == SUM_REFUSED_NO_ROWS,
          "5a. the conservation verdict REFUSES on zero rows instead of "
          "approving (sum %.1f%%)", se);
    free(xe);
    free(de);

    /* 5b. Rows present but DB Time 0 — the denominator is unusable, so every
     * printed share is 0. The verdict must be SHORT (red), not "closes", and
     * nothing may print as nan/inf. */
    struct pgwt_daemon *dz = new_daemon();
    pgwt_accum_init(&dz->accum);
    dz->accum.tm.cpu_time_ns = 600 * MS;
    dz->accum.tm.offcpu_time_ns = 150 * MS;
    dz->accum.tm.db_time_ns = 0;
    add_event(&dz->accum, EV_CPU, 10, 600 * MS);
    char *xz = capture(pgwt_print_system_event, dz);
    struct ptable tz;
    parse_table(xz, &tz);
    CHECK(tz.n > 0, "5b. positive control: rows printed with DB Time 0 (%d)",
          tz.n);
    double sz = 0.0;
    CHECK(pct_sum(&tz, &sz) == SUM_SHORT,
          "5b. an unusable denominator reads SHORT, not 100%% (got %.1f%%)",
          sz);
    CHECK(strstr(xz, "nan") == NULL && strstr(xz, "inf") == NULL,
          "5b. no nan/inf in the printed table");
    free(xz);
    free(dz);

    /* 5c. The capture path itself. If capture() ever returned an empty
     * buffer, §1–§4 would all pass vacuously, so prove it carries bytes for
     * a printer whose output is not in question. */
    struct pgwt_daemon *dc = fixture(600, 150, 250, 0);
    char *xc = capture(pgwt_print_time_model, dc);
    CHECK(strlen(xc) > 0 && strstr(xc, "DB Time") != NULL,
          "5c. capture() returns the printer's real bytes (%zu, 'DB Time' "
          "present)", strlen(xc));
    free(xc);
    free(dc);

    /* 5d. The 20-row cap. The view shows at most 20 wait events; a residual
     * emitted only from inside that loop, or counted against the cap, would
     * be missing from exactly the busy captures where it matters. 25 wait
     * events, each smaller than the residual's neighbours, plus a residual
     * that sorts past the cap. */
    struct pgwt_daemon *dm = new_daemon();
    pgwt_accum_init(&dm->accum);
    uint64_t waits_ns = 0;
    for (int i = 0; i < 25; i++) {
        uint64_t ns = (uint64_t)(100 - i) * MS;      /* 100 ms down to 76 ms */
        add_event(&dm->accum, WEI(PG_WAIT_IO, 21 + i), 3, ns);
        waits_ns += ns;
    }
    dm->accum.tm.io_time_ns = waits_ns;
    dm->accum.tm.cpu_time_ns = 0;
    dm->accum.tm.offcpu_time_ns = 5 * MS;            /* sorts dead last */
    dm->accum.tm.db_time_ns = waits_ns + 5 * MS;
    char *xm = capture(pgwt_print_system_event, dm);
    struct ptable tm_;
    parse_table(xm, &tm_);
    CHECK(tm_.n == 21,
          "5d. 20 capped wait rows plus the residual = 21 (got %d) — the cap "
          "on wait events is unchanged and the residual is outside it",
          tm_.n);
    CHECK(find_row(&tm_, "Off-CPU*") != NULL,
          "5d. the residual survives the 20-row cap");
    free(xm);
    free(dm);

    /* 5e. A reader could believe this file tests system_event while actually
     * reading time_model's table (both carry an Off-CPU* row). Pin the view
     * by a string only system_event prints. */
    struct pgwt_daemon *dv = fixture(600, 150, 250, 0);
    char *xv = capture(pgwt_print_system_event, dv);
    CHECK(strstr(xv, "System Events") != NULL && strstr(xv, "Total Waits") != NULL,
          "5e. the captured table IS the system_event view, not time_model");
    free(xv);
    free(dv);
}

int main(void)
{
    printf("=== test_system_event_offcpu (#498) ===\n");
    t1_single_window();
    t2_sort_position();
    t3_zero_residual();
    t4_multi_window();
    t5_bypass();
    printf("\n%d checks, %d failures\n", checks, failures);
    /* A suite whose section count silently drops to zero is the failure this
     * guard exists for. */
    if (checks < 30) {          /* 32 today */
        printf("FAIL: only %d checks ran; sections went missing\n", checks);
        return 1;
    }
    return failures ? 1 : 0;
}
