/* test_lock_chain_disclosure.c — #497: the lock_chains response must not
 * present its "blocker" as the lock holder.
 *
 * What the pick actually is (src/compute.c pgwt_compute_lock_chains): for each
 * Lock wait, the pid whose ON-CPU interval overlapped that wait the longest.
 * No lock identity is compared, and the candidate set is on-CPU intervals
 * only, so a holder that is itself waiting, sitting idle-in-transaction, or a
 * prepared transaction with no backend contributes no candidate and can never
 * be returned. That is not "a heuristic that is sometimes wrong"; it is
 * structurally blind, and the response used to carry the pid under the bare
 * key "blocker" with nothing saying so.
 *
 * Scope is wording and disclosure (the issue's own scope): this file asserts
 * nothing about the pick being CORRECT, only that the response says what the
 * pick is.
 *
 *   §1 the disclosure exists, is prose rather than a stub, names BOTH
 *      structural limits (no lock identity; off-CPU holders invisible), and
 *      does not re-assert likelihood ("likely") — all against literal text,
 *      never against the macro in server.c, which would make the assertion a
 *      tautology.
 *   §2 the disclosure travels with the ROW. A note only at the top of the
 *      response is one a per-row renderer never reads, so every chain carries
 *      blocker_basis, checked as a count against the chain count.
 *   §3 BYPASS SUITE: how this file's detection could stop seeing.
 *      §1 and §2 are both satisfiable by a response with zero chains ("every
 *      chain carries the field" is trivially true of none) and by a response
 *      that is an error or an `unavailable` refusal. §3 walks: a fixture with
 *      Lock waits but no overlapping candidate (chains legitimately empty —
 *      the note must still be there); an empty trace directory; and an
 *      explicit assertion that §1's response is neither an error nor a
 *      fidelity refusal, so a handler that stopped computing could not pass.
 *
 * Runs anywhere pgwt-server builds: no BPF, no PostgreSQL.
 */
#define main pgwt_server_main
#include "../src/server.c"
#undef main

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

#define MS      1000000ULL
#define ONE_SEC 1000000000ULL

#define EV_LOCK_TXN WEI(PG_WAIT_LOCK, 8)
#define EV_IO       WEI(PG_WAIT_IO, 21)
#define PID_WAITER  1001u
#define PID_ONCPU   1000u

typedef void (*handler_fn)(struct pgwt_server *, struct pgwt_request *);

static cJSON *call_handler(handler_fn fn, struct pgwt_server *srv,
                           struct pgwt_request *req)
{
    FILE *cap = tmpfile();
    if (!cap) { printf("  FATAL: tmpfile\n"); exit(2); }
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    if (saved < 0) { printf("  FATAL: dup\n"); exit(2); }
    if (dup2(fileno(cap), STDOUT_FILENO) < 0) { printf("  FATAL: dup2\n"); exit(2); }
    fn(srv, req);
    fflush(stdout);
    if (dup2(saved, STDOUT_FILENO) < 0) { printf("  FATAL: dup2 back\n"); exit(2); }
    close(saved);
    fseek(cap, 0, SEEK_END);
    long size = ftell(cap);
    if (size < 0) { printf("  FATAL: ftell\n"); exit(2); }
    rewind(cap);
    char *json = calloc((size_t)size + 1, 1);
    if (!json) { printf("  FATAL: oom\n"); exit(2); }
    size_t rd = fread(json, 1, (size_t)size, cap);
    (void)rd;
    fclose(cap);
    /* Some handlers print a diagnostic line first; the response is the last. */
    char *last = json;
    for (char *p = json; *p; p++)
        if (*p == '\n' && p[1]) last = p + 1;
    cJSON *out = cJSON_Parse(last);
    free(json);
    return out;
}

static const char *jstr(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

/* ── fixtures ─────────────────────────────────────────────────────────── */

/* A: pid 1000 on CPU across the whole second; pid 1001 blocked on
 * Lock:transactionid for 7 ms inside it. The overlap pick therefore has
 * exactly one candidate and returns 1000 — a chain the disclosure can be
 * attached to. */
static void build_fixture_overlap(const char *dir, uint64_t base)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 180000, 0, NULL) != 0) {
        printf("  FATAL: writer init (overlap)\n"); exit(2);
    }
    for (int s = 0; s < 10; s++) {
        uint64_t sec = base + (uint64_t)s * ONE_SEC;
        struct pgwt_trace_event e;

        memset(&e, 0, sizeof(e));
        e.timestamp_ns = sec + 50 * MS;
        e.pid = PID_ONCPU;
        e.old_event = 0;                 /* on CPU */
        e.new_event = EV_IO;
        e.duration_ns = 40 * MS;
        e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        pgwt_writer_push_event(&w, &e);

        memset(&e, 0, sizeof(e));
        e.timestamp_ns = sec + 40 * MS;
        e.pid = PID_WAITER;
        e.old_event = EV_LOCK_TXN;
        e.new_event = 0;
        e.duration_ns = 7 * MS;
        e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        pgwt_writer_push_event(&w, &e);
    }
    pgwt_writer_close(&w);
}

/* B: the SAME Lock waits, and on-CPU time that never overlaps them (it sits
 * 500 ms later in each second). Lock chains are legitimately empty, which is
 * what makes §1's "chains is non-empty" a real discriminator and what §3a
 * uses to show the disclosure does not vanish with the rows. */
static void build_fixture_no_overlap(const char *dir, uint64_t base)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 180000, 0, NULL) != 0) {
        printf("  FATAL: writer init (no-overlap)\n"); exit(2);
    }
    for (int s = 0; s < 10; s++) {
        uint64_t sec = base + (uint64_t)s * ONE_SEC;
        struct pgwt_trace_event e;

        memset(&e, 0, sizeof(e));
        e.timestamp_ns = sec + 40 * MS;
        e.pid = PID_WAITER;
        e.old_event = EV_LOCK_TXN;
        e.new_event = 0;
        e.duration_ns = 7 * MS;
        e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        pgwt_writer_push_event(&w, &e);

        memset(&e, 0, sizeof(e));
        e.timestamp_ns = sec + 560 * MS;
        e.pid = PID_ONCPU;
        e.old_event = 0;
        e.new_event = EV_IO;
        e.duration_ns = 40 * MS;
        e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        pgwt_writer_push_event(&w, &e);
    }
    pgwt_writer_close(&w);
}

static cJSON *ask_lock_chains(struct pgwt_server *srv)
{
    struct pgwt_request req = {0};
    req.from_ns = srv->earliest_wall_ns;
    req.to_ns   = srv->latest_wall_ns + 1;
    return call_handler(handle_lock_chains, srv, &req);
}

/* ── 1. the disclosure, as text ───────────────────────────────────────── */
static void t1_note(struct pgwt_server *srv)
{
    printf("--- 1. #497: the response says what the 'blocker' is ---\n");

    cJSON *out = ask_lock_chains(srv);
    CHECK(out != NULL, "1. parseable JSON");
    if (!out) return;

    /* A refusal would make every assertion below unreachable, and an
     * unreachable gate is the failure this project keeps finding. Say out
     * loud that the handler ANSWERED. */
    CHECK(cJSON_GetObjectItem(out, "error") == NULL, "1. no error key");
    CHECK(cJSON_GetObjectItem(out, "unavailable") == NULL,
          "1. not a fidelity refusal — the assertions below ran against a "
          "real answer, not an 'unavailable' envelope");

    cJSON *chains = cJSON_GetObjectItem(out, "chains");
    int n = cJSON_IsArray(chains) ? cJSON_GetArraySize(chains) : -1;
    CHECK(n > 0, "1. POSITIVE CONTROL: the fixture produced chains (%d) — a "
          "disclosure attached to an empty list proves nothing", n);

    const char *note = jstr(out, "blocker_note");
    CHECK(note != NULL, "1. blocker_note is present and a string");
    if (!note) { cJSON_Delete(out); return; }
    CHECK(strlen(note) > 80,
          "1. …and is prose, not a stub (%zu chars)", strlen(note));

    /* Literal phrases, not the macro: asserting against
     * PGWT_LOCK_BLOCKER_NOTE would compare the string to itself. */
    CHECK(strstr(note, "overlapping candidate") != NULL,
          "1. the note calls the pid an 'overlapping candidate'");
    CHECK(strstr(note, "unverified") != NULL,
          "1. …and says the blocker is 'unverified'");
    CHECK(strstr(note, "on-CPU overlap") != NULL,
          "1. …names the measurement that WAS made (on-CPU overlap)");
    CHECK(strstr(note, "No lock identity is compared") != NULL,
          "1. …names limit 1: no lock identity is compared");
    CHECK(strstr(note, "idle in") != NULL && strstr(note, "prepared") != NULL,
          "1. …names limit 2: an idle-in-transaction or prepared holder never "
          "appears (so the pid is not merely 'less likely' to be right)");
    /* The wording judgement, made machine-checkable: "likely holder" is a
     * probability claim this pick cannot support. */
    CHECK(strstr(note, "likely") == NULL,
          "1. the note does NOT soften into 'likely' anything");

    cJSON_Delete(out);
}

/* ── 2. the disclosure is on the row, not only at the top ─────────────── */
static void t2_per_row(struct pgwt_server *srv)
{
    printf("--- 2. every chain row carries its own basis ---\n");

    cJSON *out = ask_lock_chains(srv);
    if (!out) { CHECK(0, "2. parseable JSON"); return; }
    cJSON *chains = cJSON_GetObjectItem(out, "chains");
    int n = cJSON_IsArray(chains) ? cJSON_GetArraySize(chains) : 0;
    CHECK(n > 0, "2. POSITIVE CONTROL: %d rows to inspect", n);

    int with_basis = 0, right_value = 0, waiter_rows = 0, blocker_1000 = 0;
    cJSON *c;
    cJSON_ArrayForEach(c, chains) {
        const char *b = jstr(c, "blocker_basis");
        if (b) {
            with_basis++;
            if (strcmp(b, "cpu-overlap") == 0) right_value++;
        }
        cJSON *w = cJSON_GetObjectItem(c, "waiter");
        cJSON *bl = cJSON_GetObjectItem(c, "blocker");
        if (w && cJSON_IsNumber(w) && (uint32_t)w->valuedouble == PID_WAITER)
            waiter_rows++;
        if (bl && cJSON_IsNumber(bl) && (uint32_t)bl->valuedouble == PID_ONCPU)
            blocker_1000++;
    }
    CHECK(with_basis == n,
          "2. all %d rows carry blocker_basis (got %d) — a note that lives "
          "only at the top of the response is one a row renderer never reads",
          n, with_basis);
    CHECK(right_value == n,
          "2. …each reading \"cpu-overlap\" (got %d of %d)", right_value, n);
    /* The data is still the data: this change is disclosure only, so the
     * fixture's own pick must be unchanged. */
    CHECK(waiter_rows == n && blocker_1000 == n,
          "2. the pick itself is untouched: every row is waiter %u / "
          "candidate %u (%d, %d of %d)", PID_WAITER, PID_ONCPU,
          waiter_rows, blocker_1000, n);

    cJSON_Delete(out);
}

/* ── 3. bypass suite ──────────────────────────────────────────────────── */
static void t3_bypass(const char *dir_no_overlap)
{
    printf("--- 3. bypass suite ---\n");

    /* 3a. Lock waits present, no overlapping on-CPU interval: the view has
     * nothing to report. §1 and §2 are both VACUOUSLY true here ("all rows
     * carry the field" over zero rows), which is exactly why §1 asserts a
     * non-empty list. The disclosure must still ship: the basis of a view
     * does not depend on how many rows it happened to find. */
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir_no_overlap) != 0) {
        printf("  FATAL: server_init (no-overlap)\n"); exit(2);
    }
    cJSON *out = ask_lock_chains(srv);
    CHECK(out != NULL, "3a. parseable JSON");
    if (out) {
        cJSON *chains = cJSON_GetObjectItem(out, "chains");
        int n = cJSON_IsArray(chains) ? cJSON_GetArraySize(chains) : -1;
        CHECK(n == 0,
              "3a. no overlapping candidate -> zero chains (got %d), so §1's "
              "non-empty list really did discriminate", n);
        const char *note = jstr(out, "blocker_note");
        CHECK(note != NULL && strstr(note, "unverified") != NULL,
              "3a. the disclosure is present on an EMPTY result too — a field "
              "that comes and goes is a field clients stop handling");
        cJSON_Delete(out);
    }
    free(srv);

    /* 3b. An EMPTY trace directory: no events at all. Either server_init
     * refuses (correct) or the handler answers with no chains — what must NOT
     * happen is this file reporting a pass it did not earn, so the outcome is
     * named either way. */
    char empty[] = "/tmp/pgwt_lockdisc_empty_XXXXXX";
    if (!mkdtemp(empty)) { printf("  FATAL: mkdtemp empty\n"); exit(2); }
    struct pgwt_server *es = calloc(1, sizeof(*es));
    int erc = es ? server_init(es, empty) : -1;
    if (erc != 0) {
        CHECK(1, "3b. server_init REFUSED an empty trace dir (rc=%d) — it "
                 "cannot answer, and does not pretend to", erc);
    } else {
        cJSON *eo = ask_lock_chains(es);
        CHECK(eo != NULL, "3b. empty dir still produced parseable JSON");
        if (eo) {
            cJSON *ch = cJSON_GetObjectItem(eo, "chains");
            int n = cJSON_IsArray(ch) ? cJSON_GetArraySize(ch) : -1;
            int refused = cJSON_GetObjectItem(eo, "unavailable") != NULL ||
                          cJSON_GetObjectItem(eo, "error") != NULL;
            CHECK(refused || n == 0,
                  "3b. an empty capture yields a refusal or zero chains, "
                  "never a fabricated one (refused=%d, chains=%d)",
                  refused, n);
            cJSON_Delete(eo);
        }
    }
    free(es);
    rmdir(empty);
}

int main(void)
{
    printf("=== test_lock_chain_disclosure (#497) ===\n");

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t base = ((uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec);
    base = (base / ONE_SEC) * ONE_SEC;

    char dir_a[] = "/tmp/pgwt_lockdisc_a_XXXXXX";
    if (!mkdtemp(dir_a)) { printf("FATAL: mkdtemp a\n"); return 2; }
    build_fixture_overlap(dir_a, base);

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir_a) != 0) {
        printf("FATAL: server_init a\n"); return 2;
    }
    t1_note(srv);
    t2_per_row(srv);
    free(srv);

    char dir_b[] = "/tmp/pgwt_lockdisc_b_XXXXXX";
    if (!mkdtemp(dir_b)) { printf("FATAL: mkdtemp b\n"); return 2; }
    build_fixture_no_overlap(dir_b, base);
    t3_bypass(dir_b);

    printf("\n%d checks, %d failures\n", checks, failures);
    /* A suite whose sections silently stop running proved nothing. */
    if (checks < 18) {
        printf("FAIL: only %d assertions ran — the suite did not execute\n",
               checks);
        return 1;
    }
    return failures ? 1 : 0;
}
