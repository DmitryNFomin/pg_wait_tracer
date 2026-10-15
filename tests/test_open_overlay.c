/* test_open_overlay.c — the live open-interval overlay (#424, live half).
 *
 * Two halves, both hostile:
 *
 *  1. THE POSITIVE ORACLE. Every span length, CPU figure and classification is
 *     compared against a number written out by hand here from the fixture's
 *     own timestamps — never against a second call of the function under test.
 *     Additivity and self-consistency accept a uniformly missing overlay
 *     (including an all-zero one), so they are not used as the primary
 *     assertion anywhere in this file.
 *
 *  2. THE BYPASS SUITE. One case per way the overlay's refusal could be made
 *     unreachable, because the failure mode that matters is the FALSE NEGATIVE:
 *     an overlay that reports `included` while it is actually blind. Covered:
 *     empty reply, unparseable reply, missing mandatory field (each one in
 *     turn), absent `pids` array (absent is not "nothing open"), complete:false,
 *     a daemon-side error, an entry whose fields are unusable, no daemon socket
 *     at all, a socket that is not a listener, a daemon that accepts and never
 *     answers, a daemon that drip-feeds one byte at a time, a reply truncated
 *     mid-flight, a restarted daemon, an unpublished closed-record prefix, a
 *     missing admitted-record view, the summary route, the sampled and
 *     lightweight tiers, and an overlay struct nobody ever filled in.
 *     In every one of them the required outcome is the same: NOT `included`,
 *     and NO numeric in-flight field in the rendered JSON. Absent, never zero.
 *
 * Timing: the pure half has none — every clock value is an explicit fixture
 * input, so the arithmetic is deterministic. The socket half asserts only that
 * a wedged daemon is abandoned far inside its own stall (a 300 ms deadline
 * against a 5 s stall), which is a bound, not a duration.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "open_overlay.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* ── the fixture clock ─────────────────────────────────────────
 * One anchor, one snapshot, one open interval. Every expected value below is
 * derived from THESE constants with pencil and paper. */
#define NOW_MONO    1000000000000ULL              /* 1000 s of uptime        */
/* Deliberately NOT a multiple of 256 ns. At 1.76e18 a double's ulp is 256 ns,
 * so a wall stamp ending in ...000 would survive a round trip through a double
 * by luck and the exactness assertion below would prove nothing. */
#define NOW_WALL    1760000001000000123ULL        /* the daemon's anchor pair */
#define OFFSET      ((int64_t)(NOW_WALL - NOW_MONO))
#define SNAP_MONO    999000000000ULL              /*  999 s — the observation */
#define SNAP_WALL   (SNAP_MONO + (uint64_t)OFFSET)        /* ...999000000     */
#define OPEN_MONO    990000000000ULL              /*  990 s — 9 s open        */
#define OPEN_WALL   (OPEN_MONO + (uint64_t)OFFSET)
#define COMMITTED    995000000000ULL
#define WIN_FROM    (SNAP_WALL - 5000000000ULL)   /* 994 s                    */
#define WIN_TO      (SNAP_WALL - 1000000000ULL)   /* 998 s — the latest record */

/* Build one daemon reply. Fields are overridable so a single bad value can be
 * injected without rewriting the whole fixture. */
struct fx {
    const char *snapshot_ns, *now_mono_ns, *now_wall_ns;
    const char *committed_through_ns, *daemon_generation;
    const char *complete;
    const char *entries;      /* the raw "pids" array body, or NULL for one default */
    int         omit_pids;
};

static const char *default_entry(void)
{
    static char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"pid\":4242,\"incarnation\":7,\"event_id\":%u,\"category\":0,"
        "\"open_start_ns\":\"%llu\",\"cpu_ns_total\":\"3000000000\","
        "\"on_cpu_ts\":\"%llu\",\"last_cpu_ns\":\"1000000000\","
        "\"cmd_open\":1,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":\"%llu\","
        "\"query_id\":\"987654321\",\"query_quality\":1}",
        0x01000001u,
        (unsigned long long)OPEN_MONO,
        (unsigned long long)(SNAP_MONO - 1000000000ULL),
        (unsigned long long)(SNAP_MONO - 2000000000ULL));
    return buf;
}

static char *render(const struct fx *f)
{
    static char buf[2048];
    int n = snprintf(buf, sizeof(buf),
        "{\"snapshot_ns\":%s,\"now_mono_ns\":%s,\"now_wall_ns\":%s,"
        "\"committed_through_ns\":%s,\"daemon_generation\":%s,\"complete\":%s%s%s%s}",
        f->snapshot_ns, f->now_mono_ns, f->now_wall_ns,
        f->committed_through_ns, f->daemon_generation, f->complete,
        f->omit_pids ? "" : ",\"pids\":[",
        f->omit_pids ? "" : (f->entries ? f->entries : default_entry()),
        f->omit_pids ? "" : "]");
    /* A truncated fixture is invalid JSON, and every "the overlay refused it"
     * assertion would then pass for the wrong reason. */
    CHECK(n > 0 && (size_t)n < sizeof(buf), "fixture JSON truncated at %d bytes", n);
    return buf;
}

static struct fx base_fx(void)
{
    static char s[32], nm[32], nw[32], ct[32];
    snprintf(s,  sizeof(s),  "\"%llu\"", (unsigned long long)SNAP_MONO);
    snprintf(nm, sizeof(nm), "\"%llu\"", (unsigned long long)NOW_MONO);
    snprintf(nw, sizeof(nw), "\"%llu\"", (unsigned long long)NOW_WALL);
    snprintf(ct, sizeof(ct), "\"%llu\"", (unsigned long long)COMMITTED);
    struct fx f = { s, nm, nw, ct, "11", "true", NULL, 0 };
    return f;
}

static struct pgwt_overlay_request base_req(void)
{
    struct pgwt_overlay_request r = {
        .from_wall_ns = WIN_FROM, .to_wall_ns = WIN_TO,
        .route = PGWT_OVERLAY_ROUTE_RAW, .tier = PGWT_OVERLAY_TIER_EXACT,
        .expect_generation = 0
    };
    return r;
}

static struct pgwt_overlay_records no_records(void)
{
    struct pgwt_overlay_records r = { NULL, 0, OFFSET, NULL, NULL };
    return r;
}

/* Does the rendered JSON advertise any in-flight number? */
static int json_has(const struct pgwt_overlay *ov, const char *key, char *status_out)
{
    cJSON *root = cJSON_CreateObject();
    pgwt_overlay_add_json(ov, root);
    cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "open_intervals");
    int has = 0;
    if (o) {
        has = cJSON_GetObjectItemCaseSensitive(o, key) != NULL;
        cJSON *st = cJSON_GetObjectItemCaseSensitive(o, "status");
        if (status_out)
            snprintf(status_out, 32, "%s", (st && st->valuestring) ? st->valuestring : "?");
    }
    cJSON_Delete(root);
    return has;
}

/* The one assertion every bypass case shares: not included, and nothing
 * numeric about in-flight time anywhere in the response. */
static void assert_absent_not_zero(const struct pgwt_overlay *ov, const char *what)
{
    char status[32] = "";
    CHECK(ov->status != PGWT_OVERLAY_INCLUDED, "%s: status is included", what);
    CHECK(ov->count == 0, "%s: %zu spans survived a degraded overlay", what, ov->count);
    CHECK(ov->reason[0] != '\0', "%s: degraded overlay carries no reason", what);
    CHECK(!json_has(ov, "in_flight_ns", status),
          "%s: JSON advertises in_flight_ns while degraded (status=%s)", what, status);
    CHECK(!json_has(ov, "pids", NULL), "%s: JSON advertises a pid count while degraded", what);
    CHECK(json_has(ov, "excludes_in_flight", NULL),
          "%s: JSON does not mark the total as excluding in-flight time", what);
    CHECK(json_has(ov, "reason", NULL), "%s: JSON carries no reason", what);
}

/* ── 1. parse + validate ───────────────────────────────────── */

static void test_parse_valid(void)
{
    struct fx f = base_fx();
    struct pgwt_open_snapshot snap;
    CHECK(pgwt_overlay_parse(render(&f), &snap) == 0, "valid reply rejected: %s", snap.reason);
    CHECK(snap.status == PGWT_OVERLAY_INCLUDED, "valid reply not included");
    CHECK(snap.count == 1, "expected 1 open interval, got %zu", snap.count);
    if (snap.count == 1) {
        CHECK(snap.items[0].pid == 4242, "pid %u", snap.items[0].pid);
        CHECK(snap.items[0].incarnation == 7, "incarnation %llu",
              (unsigned long long)snap.items[0].incarnation);
        CHECK(snap.items[0].open_start_ns == OPEN_MONO, "open_start_ns");
        CHECK(snap.items[0].query_id == 987654321ULL, "query_id");
    }
    /* A wall-clock nanosecond is ~1.76e18 and a double holds 53 bits, so the
     * decimal-string form must survive EXACTLY — not to the nearest 256 ns. */
    CHECK(snap.now_wall_ns == NOW_WALL, "anchor wall lost precision: %llu != %llu",
          (unsigned long long)snap.now_wall_ns, (unsigned long long)NOW_WALL);
    CHECK(snap.anchor_offset_ns == OFFSET, "anchor offset wrong");
    pgwt_open_snapshot_free(&snap);

    /* Empty but well-formed: legitimately nothing open. */
    f = base_fx();
    f.entries = "";
    struct fx g = f;
    char body[1024];
    snprintf(body, sizeof(body),
        "{\"snapshot_ns\":%s,\"now_mono_ns\":%s,\"now_wall_ns\":%s,"
        "\"committed_through_ns\":%s,\"daemon_generation\":%s,\"complete\":true,\"pids\":[]}",
        g.snapshot_ns, g.now_mono_ns, g.now_wall_ns, g.committed_through_ns,
        g.daemon_generation);
    CHECK(pgwt_overlay_parse(body, &snap) == 0, "empty pids array rejected: %s", snap.reason);
    CHECK(snap.status == PGWT_OVERLAY_INCLUDED && snap.count == 0, "empty pids array");
    pgwt_open_snapshot_free(&snap);
}

static void test_parse_bypass(void)
{
    struct pgwt_open_snapshot snap;

    CHECK(pgwt_overlay_parse(NULL, &snap) != 0 && snap.status != PGWT_OVERLAY_INCLUDED,
          "NULL reply accepted");
    CHECK(pgwt_overlay_parse("", &snap) != 0 && snap.status != PGWT_OVERLAY_INCLUDED,
          "empty reply accepted");
    CHECK(pgwt_overlay_parse("{\"snapshot_ns\":", &snap) != 0 &&
          snap.status != PGWT_OVERLAY_INCLUDED, "truncated JSON accepted");
    CHECK(pgwt_overlay_parse("[]", &snap) != 0, "a JSON array accepted as a reply");
    CHECK(pgwt_overlay_parse("\"nope\"", &snap) != 0, "a JSON string accepted as a reply");

    /* Each mandatory top-level field, removed one at a time. A reply missing
     * any of them must be refused — not defaulted, not inferred. */
    static const char *fields[] = { "snapshot_ns", "now_mono_ns", "now_wall_ns",
                                    "committed_through_ns", "daemon_generation",
                                    "complete" };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        struct fx f = base_fx();
        char body[1024], tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", render(&f));
        /* rebuild without field i */
        char *p = body;
        size_t left = sizeof(body);
        int n = snprintf(p, left, "{");
        p += n; left -= (size_t)n;
        const char *vals[] = { f.snapshot_ns, f.now_mono_ns, f.now_wall_ns,
                               f.committed_through_ns, f.daemon_generation, f.complete };
        for (size_t j = 0; j < 6; j++) {
            if (j == i)
                continue;
            n = snprintf(p, left, "\"%s\":%s,", fields[j], vals[j]);
            p += n; left -= (size_t)n;
        }
        snprintf(p, left, "\"pids\":[%s]}", default_entry());
        CHECK(pgwt_overlay_parse(body, &snap) != 0 && snap.status != PGWT_OVERLAY_INCLUDED,
              "reply missing %s was accepted", fields[i]);
        CHECK(strstr(snap.reason, fields[i]) != NULL,
              "reason for missing %s does not name it: %s", fields[i], snap.reason);
        pgwt_open_snapshot_free(&snap);
    }

    /* The absent pids array. This is the single most dangerous false negative
     * in the whole module: treating "no array" as "nothing open" would report
     * `included` with a zero contribution on every reply shape change. */
    struct fx f = base_fx();
    f.omit_pids = 1;
    CHECK(pgwt_overlay_parse(render(&f), &snap) != 0, "a reply with no pids array was accepted");
    CHECK(snap.status != PGWT_OVERLAY_INCLUDED && snap.count == 0,
          "a reply with no pids array became an empty included overlay");
    CHECK(strstr(snap.reason, "pids") != NULL, "reason: %s", snap.reason);
    pgwt_open_snapshot_free(&snap);

    /* complete:false, with and without a reason. */
    f = base_fx();
    f.complete = "false";
    CHECK(pgwt_overlay_parse(render(&f), &snap) != 0, "complete:false accepted");
    CHECK(snap.status == PGWT_OVERLAY_UNAVAILABLE, "complete:false status");
    pgwt_open_snapshot_free(&snap);

    char body[1024];
    snprintf(body, sizeof(body),
        "{\"snapshot_ns\":\"%llu\",\"now_mono_ns\":\"%llu\",\"now_wall_ns\":\"%llu\","
        "\"committed_through_ns\":\"%llu\",\"daemon_generation\":11,\"complete\":false,"
        "\"reason\":\"ring backlog not drained\",\"pids\":[]}",
        (unsigned long long)SNAP_MONO, (unsigned long long)NOW_MONO,
        (unsigned long long)NOW_WALL, (unsigned long long)COMMITTED);
    CHECK(pgwt_overlay_parse(body, &snap) != 0, "complete:false with reason accepted");
    CHECK(strstr(snap.reason, "ring backlog") != NULL,
          "the daemon's own reason is dropped: %s", snap.reason);
    pgwt_open_snapshot_free(&snap);

    /* A daemon-side refusal (wrong mode) is a REFUSAL, reported as such. */
    CHECK(pgwt_overlay_parse("{\"error\":\"sampled tier has no watchpoints\"}", &snap) != 0,
          "daemon error accepted");
    CHECK(snap.status == PGWT_OVERLAY_REFUSED, "daemon error is not a refusal");
    CHECK(strstr(snap.reason, "sampled tier") != NULL, "reason: %s", snap.reason);
    pgwt_open_snapshot_free(&snap);

    /* Unusable entries: one bad entry fails the WHOLE reply, because a
     * partially parsed snapshot is a silently short total. */
    struct { const char *entry; const char *why; } bad[] = {
        { "{\"pid\":1}", "entry missing most fields" },
        { "{\"pid\":0,\"incarnation\":1,\"event_id\":1,\"category\":0,"
          "\"open_start_ns\":\"990000000000\",\"cpu_ns_total\":0,\"on_cpu_ts\":0,"
          "\"last_cpu_ns\":0,\"cmd_open\":0,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":0,"
          "\"query_id\":0,\"query_quality\":0}", "pid 0" },
        { "{\"pid\":5,\"incarnation\":1,\"event_id\":1,\"category\":0,"
          "\"open_start_ns\":\"1500000000000\",\"cpu_ns_total\":0,\"on_cpu_ts\":0,"
          "\"last_cpu_ns\":0,\"cmd_open\":0,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":0,"
          "\"query_id\":0,\"query_quality\":0}", "open_start after the snapshot" },
        { "{\"pid\":5,\"incarnation\":1,\"event_id\":4294967280,\"category\":0,"
          "\"open_start_ns\":\"990000000000\",\"cpu_ns_total\":0,\"on_cpu_ts\":0,"
          "\"last_cpu_ns\":0,\"cmd_open\":0,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":0,"
          "\"query_id\":0,\"query_quality\":0}", "a marker as the open event" },
        { "{\"pid\":5,\"incarnation\":1,\"event_id\":4294967295,\"category\":0,"
          "\"open_start_ns\":\"990000000000\",\"cpu_ns_total\":0,\"on_cpu_ts\":0,"
          "\"last_cpu_ns\":0,\"cmd_open\":0,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":0,"
          "\"query_id\":0,\"query_quality\":0}", "the exit sentinel as the open event" },
        { "{\"pid\":5,\"incarnation\":1,\"event_id\":1,\"category\":0,"
          "\"open_start_ns\":\"-5\",\"cpu_ns_total\":0,\"on_cpu_ts\":0,"
          "\"last_cpu_ns\":0,\"cmd_open\":0,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":0,"
          "\"query_id\":0,\"query_quality\":0}", "a negative timestamp" },
        { "{\"pid\":5,\"incarnation\":1,\"event_id\":1,\"category\":0,"
          "\"open_start_ns\":\"nine\",\"cpu_ns_total\":0,\"on_cpu_ts\":0,"
          "\"last_cpu_ns\":0,\"cmd_open\":0,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":0,"
          "\"query_id\":0,\"query_quality\":0}", "an unparseable numeric field" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct fx bf = base_fx();
        bf.entries = bad[i].entry;
        CHECK(pgwt_overlay_parse(render(&bf), &snap) != 0 &&
              snap.status != PGWT_OVERLAY_INCLUDED,
              "accepted an entry with %s", bad[i].why);
        pgwt_open_snapshot_free(&snap);
    }

    /* A valid entry mixed with a bad one: still the whole reply, not a
     * quietly shortened one. */
    char two[1200];
    snprintf(two, sizeof(two), "%s,{\"pid\":9}", default_entry());
    struct fx mf = base_fx();
    mf.entries = two;
    CHECK(pgwt_overlay_parse(render(&mf), &snap) != 0 && snap.count == 0,
          "one bad entry among good ones produced a partial snapshot");
    pgwt_open_snapshot_free(&snap);
}

/* 1024 backends is MAX_BACKENDS: the size the reply must actually survive. */
static char *render_many(int n)
{
    size_t cap = 512 + (size_t)n * 300;
    char *buf = malloc(cap);
    size_t off = (size_t)snprintf(buf, cap,
        "{\"snapshot_ns\":\"%llu\",\"now_mono_ns\":\"%llu\",\"now_wall_ns\":\"%llu\","
        "\"committed_through_ns\":\"%llu\",\"daemon_generation\":11,\"complete\":true,"
        "\"pids\":[",
        (unsigned long long)SNAP_MONO, (unsigned long long)NOW_MONO,
        (unsigned long long)NOW_WALL, (unsigned long long)COMMITTED);
    for (int i = 0; i < n; i++)
        off += (size_t)snprintf(buf + off, cap - off,
            "%s{\"pid\":%d,\"incarnation\":%d,\"event_id\":%u,\"category\":0,"
            "\"open_start_ns\":\"%llu\",\"cpu_ns_total\":\"%d\",\"on_cpu_ts\":0,"
            "\"last_cpu_ns\":0,\"cmd_open\":0,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":0,"
            "\"query_id\":%d,\"query_quality\":0}",
            i ? "," : "", 1000 + i, i + 1, 0x01000001u,
            (unsigned long long)OPEN_MONO, i * 1000, i);
    snprintf(buf + off, cap - off, "]}");
    return buf;
}

static void test_parse_1024(void)
{
    char *body = render_many(1024);
    struct pgwt_open_snapshot snap;
    CHECK(pgwt_overlay_parse(body, &snap) == 0, "1024-backend reply rejected: %s", snap.reason);
    CHECK(snap.count == 1024, "1024-backend reply yielded %zu entries", snap.count);
    CHECK(strlen(body) > 64u * 1024u,
          "the 1024-backend fixture is only %zu bytes — it no longer proves 64 KB is too small",
          strlen(body));

    struct pgwt_overlay ov;
    pgwt_overlay_init(&ov);
    struct pgwt_overlay_request req = base_req();
    struct pgwt_overlay_records rec = no_records();
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) == 0, "1024-backend build failed");
    CHECK(ov.count == 1024, "1024 open intervals produced %zu spans", ov.count);
    /* Independent oracle: every one of them is open from 990 s, the window is
     * [994 s, 998 s], so every span is exactly 4 s. */
    CHECK(pgwt_overlay_total_wall_ns(&ov) == 1024ULL * 4000000000ULL,
          "1024 x 4 s != %llu", (unsigned long long)pgwt_overlay_total_wall_ns(&ov));
    pgwt_overlay_free(&ov);
    pgwt_open_snapshot_free(&snap);
    free(body);
}

/* ── 2. build: the positive oracle ─────────────────────────── */

static void test_build_oracle(void)
{
    struct fx f = base_fx();
    struct pgwt_open_snapshot snap;
    CHECK(pgwt_overlay_parse(render(&f), &snap) == 0, "fixture parse: %s", snap.reason);

    struct pgwt_overlay ov;
    pgwt_overlay_init(&ov);
    struct pgwt_overlay_request req = base_req();
    struct pgwt_overlay_records rec = no_records();
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) == 0, "build: %s", ov.reason);
    CHECK(ov.status == PGWT_OVERLAY_INCLUDED, "status");
    CHECK(ov.count == 1, "expected one span, got %zu", ov.count);
    if (ov.count == 1) {
        const struct pgwt_overlay_span *s = &ov.spans[0];
        /* window [994 s, 998 s] ∩ open [990 s, 999 s] = [994 s, 998 s] = 4 s */
        CHECK(s->from_wall_ns == WIN_FROM, "span start %llu != %llu",
              (unsigned long long)s->from_wall_ns, (unsigned long long)WIN_FROM);
        CHECK(s->to_wall_ns == WIN_TO, "span end %llu != %llu",
              (unsigned long long)s->to_wall_ns, (unsigned long long)WIN_TO);
        CHECK(s->wall_ns == 4000000000ULL, "span wall %llu != 4000000000",
              (unsigned long long)s->wall_ns);
        CHECK(s->pid == 4242 && s->incarnation == 7, "identity carried through");
        CHECK(s->event_id == 0x01000001u, "event id carried through");
        CHECK(s->query_id == 987654321ULL, "query id carried through");
        /* PROVISIONAL, asserted only as arithmetic: cpu_ns_total 3 s, running
         * for the last second (on_cpu_ts = snap-1 s), base 1 s
         *   → stretch CPU = 3 + 1 - 1 = 3 s over a 9 s stretch
         *   → the 4 s span carries 3 s * 4/9 = 1333333333 ns. */
        CHECK(s->cpu_ns == 3000000000ULL * 4000000000ULL / 9000000000ULL,
              "span CPU %llu != 1333333333", (unsigned long long)s->cpu_ns);
        /* cmd_open with an anchor 2 s back and nothing banked: 2 s of 9 s is
         * not a majority. */
        CHECK(s->in_command == 0, "majority rule said in-command on 2 s of 9 s");
        CHECK(s->stretch_from_wall_ns == OPEN_WALL && s->stretch_to_wall_ns == SNAP_WALL,
              "provenance stretch");
    }
    CHECK(pgwt_overlay_total_wall_ns(&ov) == 4000000000ULL, "total");

    /* The rendered pane, on the happy path. */
    char status[32] = "";
    CHECK(json_has(&ov, "in_flight_ns", status) && strcmp(status, "included") == 0,
          "included overlay does not render in_flight_ns (status=%s)", status);
    CHECK(json_has(&ov, "pids", NULL), "included overlay does not render a pid count");

    /* Sub-window arithmetic, as the AAS bucket loop will use it. */
    const struct pgwt_overlay_span *s = &ov.spans[0];
    CHECK(pgwt_overlay_span_overlap_ns(s, WIN_FROM, WIN_FROM + 1000000000ULL) == 1000000000ULL,
          "1 s bucket overlap");
    CHECK(pgwt_overlay_span_overlap_ns(s, WIN_FROM - 10000000000ULL, WIN_FROM) == 0,
          "a bucket before the span is not zero");
    CHECK(pgwt_overlay_span_overlap_ns(s, WIN_TO, WIN_TO + 10000000000ULL) == 0,
          "a bucket after the span is not zero");
    CHECK(pgwt_overlay_span_cpu_ns(s, WIN_FROM, WIN_FROM + 2000000000ULL) == s->cpu_ns / 2,
          "half the span does not carry half the CPU");

    pgwt_overlay_free(&ov);
    pgwt_open_snapshot_free(&snap);
}

static void build_from(const struct fx *f, struct pgwt_overlay_request *req,
                       struct pgwt_overlay_records *rec, struct pgwt_overlay *ov)
{
    struct pgwt_open_snapshot snap;
    if (pgwt_overlay_parse(render(f), &snap) != 0) {
        CHECK(0, "fixture failed to parse: %s", snap.reason);
        pgwt_open_snapshot_free(&snap);
        return;
    }
    pgwt_overlay_init(ov);
    pgwt_overlay_build(ov, &snap, req, rec);
    pgwt_open_snapshot_free(&snap);
}

static void test_build_clipping(void)
{
    struct fx f = base_fx();
    struct pgwt_overlay_records rec = no_records();
    struct pgwt_overlay ov;

    /* Never extrapolate past the observation: a `to` beyond the snapshot is
     * clipped to the snapshot, not to `to`. */
    struct pgwt_overlay_request req = base_req();
    req.to_wall_ns = SNAP_WALL + 10000000000ULL;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1 && ov.spans[0].to_wall_ns == SNAP_WALL,
          "a window past the snapshot was not clipped to it");
    CHECK(ov.count == 1 && ov.spans[0].wall_ns == SNAP_WALL - WIN_FROM,
          "clipped span length");
    pgwt_overlay_free(&ov);

    /* A window that starts before the interval opened starts at the open. */
    req = base_req();
    req.from_wall_ns = OPEN_WALL - 20000000000ULL;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1 && ov.spans[0].from_wall_ns == OPEN_WALL,
          "a window opening before the interval did not start at the open stamp");
    pgwt_overlay_free(&ov);

    /* THE PROPERTY THAT REMOVES EVERY NEAR-NOW HEURISTIC: a historical window
     * produces an empty overlay by the clip alone. No window-position test
     * exists anywhere in the module, so this cannot regress into one. */
    req = base_req();
    req.from_wall_ns = OPEN_WALL - 500000000000ULL;
    req.to_wall_ns   = OPEN_WALL - 100000000000ULL;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.status == PGWT_OVERLAY_INCLUDED,
          "a historical window degraded instead of being empty: %s", ov.reason);
    CHECK(ov.count == 0, "a historical window synthesized %zu spans", ov.count);
    CHECK(pgwt_overlay_total_wall_ns(&ov) == 0, "a historical window contributed time");
    pgwt_overlay_free(&ov);

    /* A window entirely after the snapshot is UNKNOWN, not empty: the daemon
     * never looked at it. Returning an included overlay with no spans would
     * read as "nothing was in flight". */
    req = base_req();
    req.from_wall_ns = SNAP_WALL + 1;
    req.to_wall_ns   = SNAP_WALL + 60000000000ULL;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.status != PGWT_OVERLAY_INCLUDED,
          "a window past the snapshot was answered as an empty overlay");
    CHECK(strstr(ov.reason, "predates") != NULL, "reason: %s", ov.reason);
    assert_absent_not_zero(&ov, "window past the snapshot");
    pgwt_overlay_free(&ov);

    /* The boundary: a window starting exactly at the snapshot is still zero
     * coverage. One nanosecond earlier is covered. */
    req = base_req();
    req.from_wall_ns = SNAP_WALL;
    req.to_wall_ns   = SNAP_WALL + 1000;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.status != PGWT_OVERLAY_INCLUDED, "from == snapshot was accepted as covered");
    pgwt_overlay_free(&ov);

    req = base_req();
    req.from_wall_ns = SNAP_WALL - 1;
    req.to_wall_ns   = SNAP_WALL + 1000;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.status == PGWT_OVERLAY_INCLUDED && ov.count == 1 && ov.spans[0].wall_ns == 1,
          "a 1 ns covered sliver was lost");
    pgwt_overlay_free(&ov);
}

static struct pgwt_trace_event mk_ev(uint32_t pid, uint64_t mono_ts,
                                     uint32_t old_ev, uint32_t new_ev, uint32_t flags)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = mono_ts;
    e.pid = pid;
    e.old_event = old_ev;
    e.new_event = new_ev;
    e.flags = flags;
    return e;
}

static void test_precedence(void)
{
    struct fx f = base_fx();
    struct pgwt_overlay_request req = base_req();
    struct pgwt_overlay ov;
    struct pgwt_trace_event evs[4];
    struct pgwt_overlay_records rec = no_records();

    /* The close of the same interval WAS admitted (widened admission) — the
     * span must go, or the window counts the stretch twice. */
    evs[0] = mk_ev(4242, OPEN_MONO + 1, 0x01000001u, 0, 0);
    rec.events = evs; rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.status == PGWT_OVERLAY_INCLUDED, "precedence case degraded: %s", ov.reason);
    CHECK(ov.count == 0, "a closing record past the open stamp did not drop the span");
    pgwt_overlay_free(&ov);

    /* A record exactly AT the open stamp is the record that OPENED it. */
    evs[0] = mk_ev(4242, OPEN_MONO, 0, 0x01000001u, 0);
    rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1, "the opening record itself suppressed the span");
    pgwt_overlay_free(&ov);

    /* Earlier history for the same pid never suppresses. */
    evs[0] = mk_ev(4242, OPEN_MONO - 5000000000ULL, 0, 0x01000001u, 0);
    rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1, "an earlier record for the same pid suppressed the span");
    pgwt_overlay_free(&ov);

    /* Another backend's close is not this backend's close. */
    evs[0] = mk_ev(9999, OPEN_MONO + 5000000000ULL, 0x01000001u, 0, 0);
    rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1, "another pid's record suppressed the span");
    pgwt_overlay_free(&ov);

    /* A marker carries no duration and closes no interval: an EXEC_START
     * landing after the open stamp must not delete in-flight time. */
    evs[0] = mk_ev(4242, OPEN_MONO + 2000000000ULL,
                   PGWT_MARKER_EXEC_START, PGWT_MARKER_EXEC_START, 0);
    rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1, "a marker after the open stamp suppressed the span");
    pgwt_overlay_free(&ov);

    /* A point sample is an observation, not an interval close. */
    evs[0] = mk_ev(4242, OPEN_MONO + 2000000000ULL, 0, 0x01000001u,
                   PGWT_EVENT_FLAG_SAMPLE);
    rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1, "a point sample after the open stamp suppressed the span");
    pgwt_overlay_free(&ov);

    /* An exit DOES end the interval, and we have no duration for it: fail
     * closed and drop, rather than credit time the backend did not live. */
    evs[0] = mk_ev(4242, OPEN_MONO + 2000000000ULL, 0x01000001u, PGWT_EVENT_EXIT, 0);
    rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 0, "an exit record after the open stamp left the span standing");
    pgwt_overlay_free(&ov);

    /* The close is found wherever it sits in the array, not only at the end. */
    evs[0] = mk_ev(4242, OPEN_MONO + 1, 0x01000001u, 0, 0);
    evs[1] = mk_ev(9999, OPEN_MONO - 1, 0, 1, 0);
    evs[2] = mk_ev(4242, OPEN_MONO - 2, 0, 1, 0);
    rec.n = 3;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 0, "the closing record was missed when it was not last");
    pgwt_overlay_free(&ov);

    /* Multi-file windows convert through a callback instead of one offset. */
    evs[0] = mk_ev(4242, 42, 0x01000001u, 0, 0);   /* nonsense mono, fixed by wall_of */
    rec.n = 1;
    rec.offset_ns = 0;
    struct pgwt_overlay_records cb = rec;
    cb.wall_of = NULL;
    build_from(&f, &req, &cb, &ov);
    CHECK(ov.count == 1, "a stale offset should not have suppressed anything");
    pgwt_overlay_free(&ov);
}

static uint64_t wall_of_late(const struct pgwt_trace_event *ev, void *user)
{
    (void)ev;
    return *(uint64_t *)user;
}

static void test_precedence_callback(void)
{
    struct fx f = base_fx();
    struct pgwt_overlay_request req = base_req();
    struct pgwt_overlay ov;
    struct pgwt_trace_event ev = mk_ev(4242, 0, 0x01000001u, 0, 0);
    uint64_t late = OPEN_WALL + 1;
    struct pgwt_overlay_records rec = { &ev, 1, 0, wall_of_late, &late };

    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 0, "the per-segment wall_of callback was ignored");
    pgwt_overlay_free(&ov);
}

/* ── 3. the bypass suite on build ──────────────────────────── */

static void test_build_bypass(void)
{
    struct fx f = base_fx();
    struct pgwt_overlay_records rec = no_records();
    struct pgwt_open_snapshot snap;
    struct pgwt_overlay ov;

    /* An overlay nobody filled in. */
    pgwt_overlay_init(&ov);
    assert_absent_not_zero(&ov, "uninitialised overlay");
    CHECK(pgwt_overlay_total_wall_ns(&ov) == 0, "uninitialised overlay has a total");
    pgwt_overlay_free(&ov);

    CHECK(pgwt_overlay_parse(render(&f), &snap) == 0, "fixture parse");

    /* The summary route, handed a perfectly good snapshot. Summary v8 already
     * credits open waits per sealed second: synthesizing here double counts,
     * so the refusal cannot depend on the caller checking first. */
    struct pgwt_overlay_request req = base_req();
    req.route = PGWT_OVERLAY_ROUTE_SUMMARY;
    pgwt_overlay_init(&ov);
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) != 0, "summary route built an overlay");
    CHECK(ov.status == PGWT_OVERLAY_REFUSED, "summary route is not a refusal");
    assert_absent_not_zero(&ov, "summary route");
    pgwt_overlay_free(&ov);
    CHECK(pgwt_overlay_wanted(&req, NULL, 0) == 0, "wanted() allows the summary route");

    /* Sampled and lightweight tiers have no watchpoint intervals at all. */
    enum pgwt_overlay_tier tiers[] = { PGWT_OVERLAY_TIER_SAMPLED,
                                       PGWT_OVERLAY_TIER_LIGHTWEIGHT };
    for (int i = 0; i < 2; i++) {
        req = base_req();
        req.tier = tiers[i];
        pgwt_overlay_init(&ov);
        CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) != 0, "tier %d built an overlay", i);
        CHECK(ov.status == PGWT_OVERLAY_REFUSED, "tier %d is not a refusal", i);
        assert_absent_not_zero(&ov, "non-exact tier");
        CHECK(pgwt_overlay_wanted(&req, NULL, 0) == 0, "wanted() allows tier %d", i);
        pgwt_overlay_free(&ov);
    }

    /* No admitted-record view: the precedence rule cannot run, so the overlay
     * must refuse rather than credit a stretch a real record may already
     * cover. A gate that cannot see must never approve. */
    req = base_req();
    pgwt_overlay_init(&ov);
    CHECK(pgwt_overlay_build(&ov, &snap, &req, NULL) != 0, "a NULL record view was accepted");
    assert_absent_not_zero(&ov, "missing record view");
    pgwt_overlay_free(&ov);

    struct pgwt_overlay_records lying = { NULL, 17, OFFSET, NULL, NULL };
    pgwt_overlay_init(&ov);
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &lying) != 0,
          "a record view claiming 17 records with no array was accepted");
    assert_absent_not_zero(&ov, "inconsistent record view");
    pgwt_overlay_free(&ov);

    /* No snapshot at all. */
    pgwt_overlay_init(&ov);
    CHECK(pgwt_overlay_build(&ov, NULL, &req, &rec) != 0, "a NULL snapshot was accepted");
    assert_absent_not_zero(&ov, "missing snapshot");
    pgwt_overlay_free(&ov);

    /* A restarted daemon is not the daemon whose trace we are reading. */
    req = base_req();
    req.expect_generation = 12;     /* the fixture says 11 */
    pgwt_overlay_init(&ov);
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) != 0, "a restarted daemon was accepted");
    assert_absent_not_zero(&ov, "daemon restart");
    pgwt_overlay_free(&ov);

    req = base_req();
    req.expect_generation = 11;
    pgwt_overlay_init(&ov);
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) == 0,
          "the matching generation was rejected: %s", ov.reason);
    pgwt_overlay_free(&ov);
    pgwt_open_snapshot_free(&snap);

    /* A degraded snapshot stays degraded through build, reason intact. */
    struct fx bf = base_fx();
    bf.complete = "false";
    pgwt_overlay_parse(render(&bf), &snap);
    req = base_req();
    pgwt_overlay_init(&ov);
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) != 0, "an incomplete snapshot built");
    CHECK(strstr(ov.reason, "incomplete") != NULL, "the snapshot's reason was lost: %s", ov.reason);
    assert_absent_not_zero(&ov, "incomplete snapshot");
    pgwt_overlay_free(&ov);
    pgwt_open_snapshot_free(&snap);

    /* committed_through behind the open start: the closed prefix the server
     * can see does not reach this interval, so the total would be short by an
     * unknown amount. Absent, not partial. */
    bf = base_fx();
    bf.committed_through_ns = "900000000000";   /* 900 s < the 990 s open start */
    pgwt_overlay_parse(render(&bf), &snap);
    pgwt_overlay_init(&ov);
    req = base_req();
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) != 0,
          "an unpublished closed-record prefix was accepted");
    CHECK(strstr(ov.reason, "published") != NULL, "reason: %s", ov.reason);
    assert_absent_not_zero(&ov, "unpublished prefix");
    pgwt_overlay_free(&ov);
    pgwt_open_snapshot_free(&snap);

    /* …but a historical window must NOT be blanked by that same condition:
     * it emits no span, so there is nothing whose prefix could be missing. */
    pgwt_overlay_parse(render(&bf), &snap);
    req = base_req();
    req.from_wall_ns = OPEN_WALL - 500000000000ULL;
    req.to_wall_ns   = OPEN_WALL - 100000000000ULL;
    pgwt_overlay_init(&ov);
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) == 0,
          "a historical window was blanked by an irrelevant prefix gap: %s", ov.reason);
    CHECK(ov.count == 0, "historical window span count");
    pgwt_overlay_free(&ov);
    pgwt_open_snapshot_free(&snap);

    /* Two live intervals for one pid that overlap in time would count one
     * backend twice. PID reuse makes this reachable, so it is checked. */
    char twobody[1400];
    snprintf(twobody, sizeof(twobody),
        "{\"pid\":4242,\"incarnation\":7,\"event_id\":%u,\"category\":0,"
        "\"open_start_ns\":\"%llu\",\"cpu_ns_total\":0,\"on_cpu_ts\":0,\"last_cpu_ns\":0,"
        "\"cmd_open\":0,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":0,\"query_id\":0,"
        "\"query_quality\":0},"
        "{\"pid\":4242,\"incarnation\":8,\"event_id\":%u,\"category\":0,"
        "\"open_start_ns\":\"%llu\",\"cpu_ns_total\":0,\"on_cpu_ts\":0,\"last_cpu_ns\":0,"
        "\"cmd_open\":0,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":0,\"query_id\":0,"
        "\"query_quality\":0}",
        0x01000001u, (unsigned long long)OPEN_MONO,
        0x01000002u, (unsigned long long)(OPEN_MONO + 1000000000ULL));
    bf = base_fx();
    bf.entries = twobody;
    pgwt_overlay_parse(render(&bf), &snap);
    pgwt_overlay_init(&ov);
    req = base_req();
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) != 0,
          "two overlapping open intervals for one pid were both credited");
    assert_absent_not_zero(&ov, "overlapping same-pid intervals");
    pgwt_overlay_free(&ov);
    pgwt_open_snapshot_free(&snap);
}

static int reject_all(const struct pgwt_overlay_span *s, void *user)
{
    (void)s;
    (*(int *)user)++;
    return 0;
}

static int accept_pid(const struct pgwt_overlay_span *s, void *user)
{
    return s->pid == *(uint32_t *)user;
}

static void test_filters(void)
{
    struct fx f = base_fx();
    struct pgwt_overlay_records rec = no_records();
    struct pgwt_overlay ov;

    /* A filtered window that received unfiltered in-flight time would inflate
     * exactly the view the user narrowed down. Wiring the filter in is
     * therefore mandatory, and forgetting it degrades the pane visibly. */
    struct pgwt_overlay_request req = base_req();
    req.filters_active = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.status != PGWT_OVERLAY_INCLUDED,
          "a filtered request got unfiltered in-flight time");
    assert_absent_not_zero(&ov, "filtered request with no filter callback");
    pgwt_overlay_free(&ov);

    int calls = 0;
    req = base_req();
    req.filters_active = 1;
    req.accept = reject_all;
    req.accept_user = &calls;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.status == PGWT_OVERLAY_INCLUDED, "a filtered build degraded: %s", ov.reason);
    CHECK(calls == 1, "the filter was consulted %d times, expected 1", calls);
    CHECK(ov.count == 0, "a rejected span was credited anyway");
    pgwt_overlay_free(&ov);

    uint32_t want = 4242;
    req = base_req();
    req.filters_active = 1;
    req.accept = accept_pid;
    req.accept_user = &want;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1 && ov.spans[0].pid == 4242, "the matching span was dropped");
    pgwt_overlay_free(&ov);

    want = 1;      /* no such backend */
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 0, "a non-matching span survived the filter");
    pgwt_overlay_free(&ov);
}

static void test_classification(void)
{
    struct pgwt_overlay_records rec = no_records();
    struct pgwt_overlay_request req = base_req();
    struct pgwt_overlay ov;
    char entry[512];

    /* io_workers are excluded from AAS/DB Time everywhere else; the overlay
     * must not be the door they come back through. */
    snprintf(entry, sizeof(entry),
        "{\"pid\":4242,\"incarnation\":1,\"event_id\":%u,\"category\":%u,"
        "\"open_start_ns\":\"%llu\",\"cpu_ns_total\":0,\"on_cpu_ts\":0,\"last_cpu_ns\":0,"
        "\"cmd_open\":0,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":0,\"query_id\":0,"
        "\"query_quality\":0}",
        0x01000001u, PGWT_EVENT_FLAG_IO_WORKER, (unsigned long long)OPEN_MONO);
    struct fx f = base_fx();
    f.entries = entry;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.status == PGWT_OVERLAY_INCLUDED, "io_worker case degraded: %s", ov.reason);
    CHECK(ov.count == 0, "an io_worker open interval was credited");
    pgwt_overlay_free(&ov);

    /* A maintenance backend is categorised, not excluded. */
    snprintf(entry, sizeof(entry),
        "{\"pid\":4242,\"incarnation\":1,\"event_id\":%u,\"category\":%u,"
        "\"open_start_ns\":\"%llu\",\"cpu_ns_total\":0,\"on_cpu_ts\":0,\"last_cpu_ns\":0,"
        "\"cmd_open\":0,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":0,\"query_id\":0,"
        "\"query_quality\":0}",
        0x01000001u, PGWT_EVENT_FLAG_MAINT, (unsigned long long)OPEN_MONO);
    f.entries = entry;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1 && ov.spans[0].cat_flag == PGWT_EVENT_FLAG_MAINT,
          "an autovacuum open interval lost its category");
    pgwt_overlay_free(&ov);

    /* CPU that was never measured stays unmeasured — it must not render as a
     * measured zero, in the span or in any sub-window of it.
     *
     * The backend is RUNNING (on_cpu_ts set) and carries a seeded base. That
     * matters: with on_cpu_ts = 0 and last_cpu_ns = 0 the sentinel survives
     * the arithmetic by accident (UINT64_MAX + 0 - 0), so dropping the
     * unmeasured check entirely would still produce the right answer and this
     * assertion would be vacuous. With a running stretch and a base, the
     * arithmetic yields a plausible-looking number instead. */
    snprintf(entry, sizeof(entry),
        "{\"pid\":4242,\"incarnation\":1,\"event_id\":%u,\"category\":0,"
        "\"open_start_ns\":\"%llu\",\"cpu_ns_total\":\"%llu\",\"on_cpu_ts\":\"%llu\","
        "\"last_cpu_ns\":1000,\"cmd_open\":0,\"cmd_banked_ns\":0,\"cmd_anchor_ns\":0,"
        "\"query_id\":0,\"query_quality\":0}",
        0x01000001u, (unsigned long long)OPEN_MONO,
        (unsigned long long)PGWT_CPU_NS_UNKNOWN,
        (unsigned long long)(SNAP_MONO - 1000000000ULL));
    f.entries = entry;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1 && ov.spans[0].cpu_ns == PGWT_CPU_NS_UNKNOWN,
          "unmeasured CPU became a measured number");
    if (ov.count == 1)
        CHECK(pgwt_overlay_span_cpu_ns(&ov.spans[0], WIN_FROM, WIN_TO) == PGWT_CPU_NS_UNKNOWN,
              "unmeasured CPU became measured in a sub-window");
    pgwt_overlay_free(&ov);

    /* The command majority flips when most of the stretch is in-command —
     * PROVISIONAL by contract §C, asserted here only as arithmetic. */
    snprintf(entry, sizeof(entry),
        "{\"pid\":4242,\"incarnation\":1,\"event_id\":%u,\"category\":0,"
        "\"open_start_ns\":\"%llu\",\"cpu_ns_total\":0,\"on_cpu_ts\":0,\"last_cpu_ns\":0,"
        "\"cmd_open\":1,\"cmd_banked_ns\":\"3000000000\",\"cmd_anchor_ns\":\"%llu\","
        "\"query_id\":0,\"query_quality\":0}",
        0x01000001u, (unsigned long long)OPEN_MONO,
        (unsigned long long)(SNAP_MONO - 2000000000ULL));
    f.entries = entry;
    build_from(&f, &req, &rec, &ov);
    /* banked 3 s + 2 s still open = 5 s of a 9 s stretch → in-command. */
    CHECK(ov.count == 1 && ov.spans[0].in_command == 1,
          "5 s of command inside a 9 s stretch was not a majority");
    pgwt_overlay_free(&ov);
}

/* ── 4. transport ──────────────────────────────────────────── */

enum reply_mode {
    REPLY_OK, REPLY_BIG, REPLY_FRAGMENTED, REPLY_SILENT, REPLY_DRIP,
    REPLY_TRUNCATED, REPLY_GARBAGE
};

static char tmpdir[256];
static char sockpath[300];

static void write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

/* Serve exactly one connection in a forked child, then exit. */
static pid_t spawn_daemon(int lfd, enum reply_mode mode)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;

    int c = accept(lfd, NULL, NULL);
    if (c < 0)
        _exit(1);
    char junk[256];
    (void)read(c, junk, sizeof(junk));          /* the request line */

    struct timespec slow = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    char *body = NULL;
    switch (mode) {
    case REPLY_OK: {
        struct fx f = base_fx();
        char line[1200];
        snprintf(line, sizeof(line), "%s\n", render(&f));
        write_all(c, line, strlen(line));
        break;
    }
    case REPLY_BIG:
    case REPLY_FRAGMENTED: {
        body = render_many(1024);
        size_t n = strlen(body);
        if (mode == REPLY_BIG) {
            write_all(c, body, n);
            write_all(c, "\n", 1);
        } else {
            for (size_t off = 0; off < n; off += 4096) {
                size_t chunk = n - off < 4096 ? n - off : 4096;
                write_all(c, body + off, chunk);
                nanosleep(&(struct timespec){ 0, 2 * 1000 * 1000 }, NULL);
            }
            write_all(c, "\n", 1);
        }
        free(body);
        break;
    }
    case REPLY_SILENT:
        /* accepts, answers nothing, holds the connection open */
        nanosleep(&(struct timespec){ 5, 0 }, NULL);
        break;
    case REPLY_DRIP:
        /* one byte every 50 ms, forever: fatal to a per-I/O timeout, harmless
         * against an overall deadline */
        for (int i = 0; i < 200; i++) {
            write_all(c, "{", 1);
            nanosleep(&slow, NULL);
        }
        break;
    case REPLY_TRUNCATED: {
        struct fx f = base_fx();
        char *line = render(&f);
        write_all(c, line, strlen(line) / 2);   /* no newline, then hang up */
        break;
    }
    case REPLY_GARBAGE:
        write_all(c, "not json at all\n", 16);
        break;
    }
    close(c);
    _exit(0);
}

static long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int listen_sock(void)
{
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", sockpath);
    unlink(sockpath);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lfd, 4) != 0) {
        CHECK(0, "cannot bind the fixture socket %s: %s", sockpath, strerror(errno));
        close(lfd);
        return -1;
    }
    return lfd;
}

static void run_transport_case(enum reply_mode mode, const char *what,
                               int expect_included, long max_ms, size_t expect_spans)
{
    int lfd = listen_sock();
    if (lfd < 0)
        return;
    pid_t child = spawn_daemon(lfd, mode);
    if (child == 0)
        _exit(0);

    struct pgwt_open_snapshot snap;
    long t0 = ms_now();
    pgwt_overlay_fetch(tmpdir, 300, &snap);
    long dt = ms_now() - t0;

    kill(child, SIGKILL);
    int st;
    waitpid(child, &st, 0);
    close(lfd);
    unlink(sockpath);

    if (expect_included) {
        CHECK(snap.status == PGWT_OVERLAY_INCLUDED, "%s: not included (%s)", what, snap.reason);
        CHECK(snap.count == expect_spans, "%s: %zu entries, expected %zu",
              what, snap.count, expect_spans);
    } else {
        CHECK(snap.status != PGWT_OVERLAY_INCLUDED, "%s: a bad reply was accepted", what);
        CHECK(snap.reason[0] != '\0', "%s: no reason given", what);
        CHECK(snap.count == 0, "%s: %zu entries survived", what, snap.count);
    }
    if (max_ms > 0)
        CHECK(dt < max_ms, "%s: took %ld ms, which is not inside the %ld ms bound "
              "(a per-I/O timeout would take 5000+)", what, dt, max_ms);
    pgwt_open_snapshot_free(&snap);
}

static void test_transport(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/pgwt_overlay_test_XXXXXX");
    if (!mkdtemp(tmpdir)) {
        CHECK(0, "cannot create a temp dir: %s", strerror(errno));
        return;
    }
    snprintf(sockpath, sizeof(sockpath), "%s/pgwt.sock", tmpdir);

    /* No daemon at all: the standing case on a replayed trace. */
    struct pgwt_open_snapshot snap;
    CHECK(pgwt_overlay_fetch(tmpdir, 300, &snap) != 0, "a missing socket was accepted");
    CHECK(snap.status == PGWT_OVERLAY_UNAVAILABLE, "missing socket status");
    CHECK(strstr(snap.reason, "absent") != NULL, "missing socket reason: %s", snap.reason);
    pgwt_open_snapshot_free(&snap);

    /* A stale socket FILE with nothing listening. */
    int fd = open(sockpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        close(fd);
        CHECK(pgwt_overlay_fetch(tmpdir, 300, &snap) != 0, "a stale socket file was accepted");
        CHECK(snap.status != PGWT_OVERLAY_INCLUDED, "stale socket file status");
        pgwt_open_snapshot_free(&snap);
        unlink(sockpath);
    }

    run_transport_case(REPLY_OK,        "a normal reply",            1, 0,    1);
    /* 1024 backends is ~200 KB: this is the case a 64 KB buffer fails. */
    run_transport_case(REPLY_BIG,       "a 1024-backend reply",      1, 0,    1024);
    run_transport_case(REPLY_FRAGMENTED,"a fragmented 1024-backend reply", 1, 0, 1024);
    /* The daemon accepts and says nothing for 5 s. A 300 ms OVERALL deadline
     * must abandon it; a per-I/O timeout of 3 s would not. */
    run_transport_case(REPLY_SILENT,    "a silent daemon",           0, 2000, 0);
    /* One byte every 50 ms forever re-arms a per-I/O timeout indefinitely. */
    run_transport_case(REPLY_DRIP,      "a drip-feeding daemon",     0, 2000, 0);
    run_transport_case(REPLY_TRUNCATED, "a truncated reply",         0, 2000, 0);
    run_transport_case(REPLY_GARBAGE,   "a garbage reply",           0, 2000, 0);

    /* pgwt_overlay_attach(): the ONE call the loader makes, so that every
     * raw-route consumer inherits the same overlay. Per-endpoint attachment
     * would correct time_model while Sessions and Top Events under 120 s --
     * the same raw route -- stayed silently short. */
    {
        int lfd = listen_sock();
        pid_t child = spawn_daemon(lfd, REPLY_OK);
        struct pgwt_overlay ov;
        struct pgwt_overlay_request req = base_req();
        struct pgwt_overlay_records rec = no_records();
        int rc = pgwt_overlay_attach(&ov, tmpdir, 300, &req, &rec);
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        close(lfd);
        unlink(sockpath);
        CHECK(rc == 0 && ov.status == PGWT_OVERLAY_INCLUDED,
              "attach did not include the overlay (%s)", ov.reason);
        CHECK(ov.count == 1 && ov.spans[0].wall_ns == 4000000000ULL,
              "attach produced %zu spans", ov.count);
        pgwt_overlay_free(&ov);
    }

    /* A refused route must not touch the socket at all. The fixture daemon
     * here accepts and then says nothing for five seconds: if attach talked
     * to it, this case could not finish in single-digit milliseconds. */
    {
        int lfd = listen_sock();
        pid_t child = spawn_daemon(lfd, REPLY_SILENT);
        struct pgwt_overlay ov;
        struct pgwt_overlay_request req = base_req();
        req.route = PGWT_OVERLAY_ROUTE_SUMMARY;
        struct pgwt_overlay_records rec = no_records();
        long t0 = ms_now();
        pgwt_overlay_attach(&ov, tmpdir, 300, &req, &rec);
        long dt = ms_now() - t0;
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        close(lfd);
        unlink(sockpath);
        CHECK(ov.status == PGWT_OVERLAY_REFUSED, "attach on the summary route: %s", ov.reason);
        CHECK(dt < 100, "attach queried the daemon on a refused route (%ld ms)", dt);
        assert_absent_not_zero(&ov, "attach on the summary route");
        pgwt_overlay_free(&ov);
    }

    /* No daemon at all, through the same single call: absent and labelled. */
    {
        struct pgwt_overlay ov;
        struct pgwt_overlay_request req = base_req();
        struct pgwt_overlay_records rec = no_records();
        CHECK(pgwt_overlay_attach(&ov, tmpdir, 300, &req, &rec) != 0,
              "attach with no daemon reported success");
        assert_absent_not_zero(&ov, "attach with no daemon");
        pgwt_overlay_free(&ov);
    }

    rmdir(tmpdir);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    test_parse_valid();
    test_parse_bypass();
    test_parse_1024();
    test_build_oracle();
    test_build_clipping();
    test_precedence();
    test_precedence_callback();
    test_build_bypass();
    test_filters();
    test_classification();
    test_transport();

    printf("test_open_overlay: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
