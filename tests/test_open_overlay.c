/* test_open_overlay.c — the live open-interval overlay (#424, live half).
 *
 * Three halves, all hostile:
 *
 *  1. THE POSITIVE ORACLE. Every span length, CPU figure and classification is
 *     compared against a number written out by hand here from the fixture's
 *     own timestamps — never against a second call of the function under test.
 *     Additivity and self-consistency accept a uniformly missing overlay
 *     (including an all-zero one), so they are not the primary assertion
 *     anywhere in this file. The GOLDEN fixture at the end is the daemon
 *     half's own `sample_open_intervals.json`, verbatim: proof that the parser
 *     accepts the real producer's bytes rather than a shape invented here.
 *
 *  2. THE GATE-STATE TABLE. The command classification is three steps, two of
 *     which are RULES and not consequences of the majority rule. The daemon
 *     half's first draft omitted them and differed from the real classifier in
 *     59 of 96 gate states. Here all 144 combinations of
 *     (gate_present, seen, sweep_open, cmd_open) x (banked) x (anchor) are
 *     driven through the parser twice: once with the daemon's `cmd_in_window`
 *     set to the verdict an independently written expression in this file
 *     predicts (the reply must be ACCEPTED), and once with it flipped (the
 *     reply must be REFUSED). The second direction is what stops the table
 *     from being vacuous.
 *
 *  3. THE BYPASS SUITE. One case per way the overlay's refusal could be made
 *     unreachable, because the failure mode that matters is the FALSE
 *     NEGATIVE: an overlay that reports `included` while it is actually blind.
 *     Covered: empty/unparseable reply, each mandatory field missing in turn,
 *     a missing `pids` array under status included (absent is not "nothing
 *     open"), status refused/unavailable, an invariant-breaking reply
 *     (included + complete false), wall stamps that disagree with the
 *     daemon's own anchor, an open_count that disagrees with the rows that
 *     arrived, a CPU figure that disagrees with the shared arithmetic, a CPU
 *     figure present when the daemon measures none, an empty wait category, a
 *     daemon-side error, unusable entries, no socket at all, a socket that is
 *     not a listener, a daemon that accepts and never answers, one that drips
 *     a byte at a time, a truncated reply, a restarted daemon, an unpublished
 *     closed-record prefix, a missing admitted-record view, a window the
 *     snapshot never covered, a filtered request with no filter, the summary
 *     route, the two non-exact tiers, and an overlay nobody filled in.
 *     In every one of them the required outcome is the same: NOT `included`,
 *     and NO numeric in-flight field in the rendered JSON. Absent, never zero.
 *
 * Timing: the pure halves have none — every clock value is an explicit fixture
 * input. The socket half asserts only bounds (a 300 ms deadline against a 5 s
 * stall; a refusal that must never open the socket in under 100 ms).
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
#define SNAP_WALL   (SNAP_MONO + (uint64_t)OFFSET)
#define OPEN_MONO    990000000000ULL              /*  990 s — a 9 s stretch   */
#define OPEN_WALL   (OPEN_MONO + (uint64_t)OFFSET)
#define COMMITTED    995000000000ULL
#define COMMITTED_WALL (COMMITTED + (uint64_t)OFFSET)
#define WIN_FROM    (SNAP_WALL - 5000000000ULL)   /* 994 s                    */
#define WIN_TO      (SNAP_WALL - 1000000000ULL)   /* 998 s — the latest record */

#define EV_LOCK     0x01000001u

/* ── fixture builders, in the daemon half's exact wire shape ─── */

/* One row. Defaults describe the standard open Lock wait; a test overrides
 * only the field it is about. */
struct row_fx {
    uint32_t    pid;
    uint64_t    incarnation, generation;
    uint32_t    event_id, cat_flag, category_id;
    const char *category;          /* NULL renders no key at all */
    uint64_t    open_start_ns, cpu_ns_total, on_cpu_ts, last_cpu_ns;
    const char *cpu_ns;            /* NULL = omit (unmeasured) */
    uint64_t    query_id;
    uint16_t    query_quality;
    uint64_t    cmd_banked_ns, cmd_anchor_ns;
    int         cmd_open, cmd_sweep_open, cmd_seen, cmd_gate_present, cmd_in_window;
    const char *raw_extra;         /* appended verbatim, for type abuse */
    const char *drop_key;          /* omit this key entirely */
};

static struct row_fx base_row(void)
{
    struct row_fx r = {
        .pid = 4242, .incarnation = 7, .generation = 3,
        .event_id = EV_LOCK, .cat_flag = 0, .category_id = 5, .category = "Lock",
        .open_start_ns = OPEN_MONO,
        .cpu_ns_total = 3000000000ULL,
        .on_cpu_ts = SNAP_MONO - 1000000000ULL,
        .last_cpu_ns = 1000000000ULL,
        .cpu_ns = "3000000000",       /* 3e9 + 1e9 - 1e9, per the shared helper */
        .query_id = 987654321ULL, .query_quality = 1,
        .cmd_banked_ns = 0, .cmd_anchor_ns = SNAP_MONO - 2000000000ULL,
        .cmd_open = 1, .cmd_sweep_open = 1, .cmd_seen = 1, .cmd_gate_present = 1,
        .cmd_in_window = 0,           /* 2 s of command in a 9 s stretch */
        .raw_extra = NULL, .drop_key = NULL,
    };
    return r;
}

static int want_key(const struct row_fx *r, const char *k)
{
    return !(r->drop_key && strcmp(r->drop_key, k) == 0);
}

/* The daemon's `cmd_open_ns` for this row, written out here independently of
 * the module. The CLAMP lives in this quantity and nowhere else: it cannot
 * change the majority verdict (min(open, wall) >= wall/2 whenever
 * open >= wall/2), so a fixture that only carried the verdict could not see
 * the clamp removed. */
static uint64_t oracle_open_ns(const struct row_fx *r, uint64_t t0, uint64_t t1)
{
    if (!r->cmd_gate_present || !r->cmd_seen)
        return 0;
    uint64_t wall = t1 > t0 ? t1 - t0 : 0;
    uint64_t open_ns = r->cmd_banked_ns;
    if (r->cmd_sweep_open) {
        uint64_t a = r->cmd_anchor_ns > t0 ? r->cmd_anchor_ns : t0;
        if (t1 > a)
            open_ns += t1 - a;
    }
    return open_ns > wall ? wall : open_ns;
}

#define ADD_U64(k, v)                                                          \
    do {                                                                       \
        if (want_key(r, k))                                                    \
            off += (size_t)snprintf(buf + off, cap - off, "\"%s\":%llu,",      \
                                    k, (unsigned long long)(v));               \
    } while (0)
#define ADD_BOOL(k, v)                                                         \
    do {                                                                       \
        if (want_key(r, k))                                                    \
            off += (size_t)snprintf(buf + off, cap - off, "\"%s\":%s,",        \
                                    k, (v) ? "true" : "false");                \
    } while (0)

static size_t render_row(char *buf, size_t cap, const struct row_fx *r)
{
    size_t off = 0;
    off += (size_t)snprintf(buf + off, cap - off, "{");
    ADD_U64("pid", r->pid);
    ADD_U64("incarnation", r->incarnation);
    ADD_U64("generation", r->generation);
    ADD_U64("event_id", r->event_id);
    ADD_U64("cat_flag", r->cat_flag);
    if (r->category && want_key(r, "category"))
        off += (size_t)snprintf(buf + off, cap - off, "\"category\":\"%s\",",
                                r->category);
    ADD_U64("category_id", r->category_id);
    ADD_U64("open_start_ns", r->open_start_ns);
    ADD_U64("duration_ns", SNAP_MONO - r->open_start_ns);
    if (r->cpu_ns && want_key(r, "cpu_ns"))
        off += (size_t)snprintf(buf + off, cap - off, "\"cpu_ns\":%s,", r->cpu_ns);
    ADD_U64("cpu_ns_total", r->cpu_ns_total);
    ADD_U64("on_cpu_ts", r->on_cpu_ts);
    ADD_U64("last_cpu_ns", r->last_cpu_ns);
    ADD_U64("query_id", r->query_id);
    ADD_U64("query_quality", r->query_quality);
    ADD_U64("cmd_open_ns", oracle_open_ns(r, r->open_start_ns, SNAP_MONO));
    ADD_U64("cmd_banked_ns", r->cmd_banked_ns);
    ADD_U64("cmd_anchor_ns", r->cmd_anchor_ns);
    ADD_BOOL("cmd_open", r->cmd_open);
    ADD_BOOL("cmd_sweep_open", r->cmd_sweep_open);
    ADD_BOOL("cmd_seen", r->cmd_seen);
    ADD_BOOL("cmd_gate_present", r->cmd_gate_present);
    ADD_BOOL("cmd_in_window", r->cmd_in_window);
    ADD_BOOL("cmd_by_markers", r->cmd_gate_present && r->cmd_seen);
    ADD_BOOL("query_synth", r->query_quality == 2);
    if (r->raw_extra)
        off += (size_t)snprintf(buf + off, cap - off, "%s,", r->raw_extra);
    if (off > 1 && buf[off - 1] == ',')
        off--;                                  /* trailing comma */
    off += (size_t)snprintf(buf + off, cap - off, "}");
    return off;
}

/* The whole reply. Strings so a single field can be made wrong without
 * rebuilding the fixture; NULL means "omit the key". */
struct fx {
    const char *status, *complete, *reason;
    const char *snapshot_ns, *snapshot_wall_ns;
    const char *now_mono_ns, *now_wall_ns;
    const char *committed_through_ns, *committed_through_wall_ns;
    const char *daemon_generation, *cpu_accounting, *tier;
    const char *open_count;         /* NULL = auto from the row count */
    const char *entries;            /* NULL = one default row */
    int         omit_pids;
};

static char *num(uint64_t v)
{
    static char ring[8][32];
    static int i;
    char *b = ring[i++ & 7];
    snprintf(b, 32, "%llu", (unsigned long long)v);
    return b;
}
static char *str(uint64_t v)
{
    static char ring[8][32];
    static int i;
    char *b = ring[i++ & 7];
    snprintf(b, 32, "\"%llu\"", (unsigned long long)v);
    return b;
}

static struct fx base_fx(void)
{
    static char sn[32], sw[40], nm[32], nw[40], ct[32], cw[40];
    snprintf(sn, sizeof(sn), "%s", num(SNAP_MONO));
    snprintf(sw, sizeof(sw), "%s", str(SNAP_WALL));
    snprintf(nm, sizeof(nm), "%s", num(NOW_MONO));
    snprintf(nw, sizeof(nw), "%s", str(NOW_WALL));
    snprintf(ct, sizeof(ct), "%s", num(COMMITTED));
    snprintf(cw, sizeof(cw), "%s", str(COMMITTED_WALL));
    struct fx f = {
        .status = "\"included\"", .complete = "true", .reason = "\"\"",
        .snapshot_ns = sn, .snapshot_wall_ns = sw,
        .now_mono_ns = nm, .now_wall_ns = nw,
        .committed_through_ns = ct, .committed_through_wall_ns = cw,
        .daemon_generation = "11", .cpu_accounting = "true",
        .tier = "\"escalated\"", .open_count = NULL, .entries = NULL,
        .omit_pids = 0,
    };
    return f;
}

static char fx_buf[512 * 1024];

static void kv(size_t *off, const char *k, const char *v)
{
    if (v)
        *off += (size_t)snprintf(fx_buf + *off, sizeof(fx_buf) - *off,
                                 "\"%s\":%s,", k, v);
}

static char *render(const struct fx *f)
{
    static char rowbuf[2048];
    const char *entries = f->entries;
    int n_rows = 0;
    if (!entries) {
        struct row_fx r = base_row();
        render_row(rowbuf, sizeof(rowbuf), &r);
        entries = rowbuf;
        n_rows = 1;
    } else if (*entries) {
        n_rows = 1;
        for (const char *p = entries; *p; p++)
            if (*p == '{' && p != entries && p[-1] == ',')
                n_rows++;
    }

    size_t off = 0;
    off += (size_t)snprintf(fx_buf, sizeof(fx_buf), "{\"ok\":true,\"cmd\":\"open_intervals\",");
    kv(&off, "status", f->status);
    kv(&off, "reason", f->reason);
    kv(&off, "complete", f->complete);
    kv(&off, "snapshot_ns", f->snapshot_ns);
    kv(&off, "snapshot_wall_ns", f->snapshot_wall_ns);
    kv(&off, "now_mono_ns", f->now_mono_ns);
    kv(&off, "now_wall_ns", f->now_wall_ns);
    kv(&off, "committed_through_ns", f->committed_through_ns);
    kv(&off, "committed_through_wall_ns", f->committed_through_wall_ns);
    kv(&off, "daemon_generation", f->daemon_generation);
    kv(&off, "daemon_start_wall_ns", str(NOW_WALL - 600000000000ULL));
    kv(&off, "daemon_pid", "51234");
    kv(&off, "tier", f->tier);
    kv(&off, "cpu_accounting", f->cpu_accounting);
    kv(&off, "cmd_gate_active", "true");
    kv(&off, "open_count", f->open_count ? f->open_count : num((uint64_t)n_rows));
    if (!f->omit_pids)
        off += (size_t)snprintf(fx_buf + off, sizeof(fx_buf) - off,
                                "\"pids\":[%s],", entries);
    if (off > 1 && fx_buf[off - 1] == ',')
        off--;
    off += (size_t)snprintf(fx_buf + off, sizeof(fx_buf) - off, "}");
    /* A truncated fixture is invalid JSON, and every "the overlay refused it"
     * assertion would then pass for the wrong reason. */
    CHECK(off < sizeof(fx_buf) - 1, "fixture JSON truncated at %zu bytes", off);
    return fx_buf;
}

static char *render_one(const struct row_fx *r)
{
    static char rowbuf[2048];
    render_row(rowbuf, sizeof(rowbuf), r);
    struct fx f = base_fx();
    f.entries = rowbuf;
    return render(&f);
}

static struct pgwt_overlay_request base_req(void)
{
    struct pgwt_overlay_request r = {
        .from_wall_ns = WIN_FROM, .to_wall_ns = WIN_TO,
        .route = PGWT_OVERLAY_ROUTE_RAW, .tier = PGWT_OVERLAY_TIER_EXACT,
        .expect_generation = 0, .filters_active = 0, .accept = NULL,
        .accept_user = NULL
    };
    return r;
}

static struct pgwt_overlay_records no_records(void)
{
    struct pgwt_overlay_records r = { NULL, 0, OFFSET, NULL, NULL };
    return r;
}

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
        const struct pgwt_open_interval *it = &snap.items[0];
        CHECK(it->pid == 4242, "pid %u", it->pid);
        CHECK(it->incarnation == 7, "incarnation");
        CHECK(it->open_start_ns == OPEN_MONO, "open_start_ns");
        CHECK(it->query_id == 987654321ULL, "query_id");
        CHECK(it->cat_flag == 0, "cat_flag");
        CHECK(strcmp(it->category, "Lock") == 0, "category '%s'", it->category);
        CHECK(it->category_id == 5, "category_id");
        /* cpu_ns_total 3 s, running for the last second, base 1 s → 3 s. */
        CHECK(it->cpu_ns == 3000000000ULL, "stretch CPU %llu",
              (unsigned long long)it->cpu_ns);
        /* 2 s of command in a 9 s stretch is not a majority. */
        CHECK(it->in_command == 0, "in_command");
        CHECK(it->cmd_sweep_open == 1 && it->cmd_seen == 1 && it->cmd_gate_present == 1,
              "gate flags parsed as booleans");
    }
    /* A wall-clock nanosecond is ~1.76e18 and a double holds 53 bits, so the
     * decimal-string form must survive EXACTLY — not to the nearest 256 ns. */
    CHECK(snap.now_wall_ns == NOW_WALL, "anchor wall lost precision: %llu != %llu",
          (unsigned long long)snap.now_wall_ns, (unsigned long long)NOW_WALL);
    CHECK(snap.snapshot_wall_ns == SNAP_WALL, "snapshot wall lost precision");
    CHECK(snap.committed_through_wall_ns == COMMITTED_WALL, "committed wall");
    CHECK(snap.anchor_offset_ns == OFFSET, "anchor offset wrong");
    CHECK(snap.cpu_accounting == 1, "cpu_accounting");
    CHECK(strcmp(snap.tier, "escalated") == 0, "tier '%s'", snap.tier);
    pgwt_open_snapshot_free(&snap);

    /* Empty but well-formed: legitimately nothing open. An empty ARRAY is a
     * measurement; an absent array is not (see the bypass suite). */
    f = base_fx();
    f.entries = "";
    f.open_count = "0";
    CHECK(pgwt_overlay_parse(render(&f), &snap) == 0, "empty pids array rejected: %s",
          snap.reason);
    CHECK(snap.status == PGWT_OVERLAY_INCLUDED && snap.count == 0, "empty pids array");
    pgwt_open_snapshot_free(&snap);

    /* Monotonic fields may arrive as strings instead of numbers (the producer
     * is moving them, because a number is exact only below 2^53 ns ≈ 104 days
     * of uptime). The parser must not assume either form. */
    f = base_fx();
    f.snapshot_ns = str(SNAP_MONO);
    f.now_mono_ns = str(NOW_MONO);
    f.committed_through_ns = str(COMMITTED);
    CHECK(pgwt_overlay_parse(render(&f), &snap) == 0,
          "string-typed monotonic fields rejected: %s", snap.reason);
    CHECK(snap.snapshot_ns == SNAP_MONO && snap.now_mono_ns == NOW_MONO,
          "string-typed monotonic fields");
    pgwt_open_snapshot_free(&snap);

    /* …and a mono value past 2^53, which is where the number form breaks. */
    f = base_fx();
    uint64_t big_mono = 9007199254740993ULL;        /* 2^53 + 1 */
    uint64_t big_wall = NOW_WALL;
    static char bm[32], bw[40], bs[40], bc[32], bcw[40];
    snprintf(bm, sizeof(bm), "\"%llu\"", (unsigned long long)big_mono);
    snprintf(bw, sizeof(bw), "\"%llu\"", (unsigned long long)big_wall);
    snprintf(bs, sizeof(bs), "\"%llu\"", (unsigned long long)big_mono);
    snprintf(bc, sizeof(bc), "\"%llu\"", (unsigned long long)(big_mono - 1000));
    snprintf(bcw, sizeof(bcw), "\"%llu\"", (unsigned long long)(big_wall - 1000));
    f.now_mono_ns = bm; f.now_wall_ns = bw;
    f.snapshot_ns = bs; f.snapshot_wall_ns = bw;
    f.committed_through_ns = bc; f.committed_through_wall_ns = bcw;
    f.entries = "";
    f.open_count = "0";
    CHECK(pgwt_overlay_parse(render(&f), &snap) == 0,
          "a mono stamp past 2^53 was rejected: %s", snap.reason);
    CHECK(snap.snapshot_ns == big_mono, "2^53+1 did not survive: %llu",
          (unsigned long long)snap.snapshot_ns);
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

    /* Each mandatory top-level field, removed one at a time. */
    static const char *const fields[] = {
        "status", "complete", "snapshot_ns", "snapshot_wall_ns", "now_mono_ns",
        "now_wall_ns", "committed_through_ns", "committed_through_wall_ns",
        "daemon_generation", "cpu_accounting"
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        struct fx f = base_fx();
        const char **slot[] = {
            &f.status, &f.complete, &f.snapshot_ns, &f.snapshot_wall_ns,
            &f.now_mono_ns, &f.now_wall_ns, &f.committed_through_ns,
            &f.committed_through_wall_ns, &f.daemon_generation, &f.cpu_accounting
        };
        *slot[i] = NULL;                       /* omit exactly this one */
        CHECK(pgwt_overlay_parse(render(&f), &snap) != 0 &&
              snap.status != PGWT_OVERLAY_INCLUDED,
              "reply missing %s was accepted", fields[i]);
        CHECK(strstr(snap.reason, fields[i]) != NULL,
              "reason for missing %s does not name it: %s", fields[i], snap.reason);
        pgwt_open_snapshot_free(&snap);
    }

    /* The absent pids array under status included. The single most dangerous
     * false negative here: reading "no array" as "nothing open" would report
     * `included` with a zero contribution on every reply-shape change. */
    struct fx f = base_fx();
    f.omit_pids = 1;
    CHECK(pgwt_overlay_parse(render(&f), &snap) != 0, "a reply with no pids array accepted");
    CHECK(snap.status != PGWT_OVERLAY_INCLUDED && snap.count == 0,
          "a reply with no pids array became an empty included overlay");
    CHECK(strstr(snap.reason, "pids") != NULL, "reason: %s", snap.reason);
    pgwt_open_snapshot_free(&snap);

    /* The producer's statuses, and the invariant that ties them together. */
    f = base_fx();
    f.status = "\"refused\"";
    f.complete = "false";
    f.reason = "\"sampled tier: no watchpoints armed\"";
    f.omit_pids = 1;
    CHECK(pgwt_overlay_parse(render(&f), &snap) != 0, "status refused accepted");
    CHECK(snap.status == PGWT_OVERLAY_REFUSED, "status refused is not a refusal");
    CHECK(strstr(snap.reason, "watchpoints") != NULL, "the daemon's reason is dropped: %s",
          snap.reason);
    pgwt_open_snapshot_free(&snap);

    f = base_fx();
    f.status = "\"unavailable\"";
    f.complete = "false";
    f.reason = "\"state_map fd unavailable\"";
    f.omit_pids = 1;
    CHECK(pgwt_overlay_parse(render(&f), &snap) != 0, "status unavailable accepted");
    CHECK(snap.status == PGWT_OVERLAY_UNAVAILABLE, "status unavailable");
    pgwt_open_snapshot_free(&snap);

    /* included + complete:false breaks the producer's own invariant. The
     * fail-safe answer to a producer bug is the degraded one. */
    f = base_fx();
    f.complete = "false";
    CHECK(pgwt_overlay_parse(render(&f), &snap) != 0, "included+incomplete accepted");
    CHECK(snap.status == PGWT_OVERLAY_UNAVAILABLE && snap.count == 0, "invariant break");
    pgwt_open_snapshot_free(&snap);

    /* Wall stamps that do not sit on the daemon's own anchor: no coherent
     * clock model, so every span placed from it would be arbitrary. */
    f = base_fx();
    f.snapshot_wall_ns = str(SNAP_WALL + 1000);
    CHECK(pgwt_overlay_parse(render(&f), &snap) != 0, "an off-anchor wall stamp accepted");
    CHECK(strstr(snap.reason, "anchor") != NULL, "reason: %s", snap.reason);
    pgwt_open_snapshot_free(&snap);

    f = base_fx();
    f.committed_through_wall_ns = str(COMMITTED_WALL - 7);
    CHECK(pgwt_overlay_parse(render(&f), &snap) != 0,
          "an off-anchor committed wall stamp accepted");
    pgwt_open_snapshot_free(&snap);

    /* open_count is the producer's own row counter: if it disagrees with the
     * rows that actually arrived, rows were lost on the way out. */
    f = base_fx();
    f.open_count = "2";
    CHECK(pgwt_overlay_parse(render(&f), &snap) != 0, "open_count mismatch accepted");
    CHECK(strstr(snap.reason, "open_count") != NULL, "reason: %s", snap.reason);
    pgwt_open_snapshot_free(&snap);

    /* A daemon too old for the command. Unavailable (version skew), not
     * refused (which means "this tier cannot have open intervals"). */
    CHECK(pgwt_overlay_parse("{\"ok\":false,\"error\":\"unknown command\"}", &snap) != 0,
          "an error reply accepted");
    CHECK(snap.status == PGWT_OVERLAY_UNAVAILABLE, "an old daemon is not 'unavailable'");
    CHECK(strstr(snap.reason, "unknown command") != NULL, "reason: %s", snap.reason);
    pgwt_open_snapshot_free(&snap);

    /* Per-row fields: each required one removed in turn. */
    static const char *const row_fields[] = {
        "pid", "incarnation", "generation", "event_id", "cat_flag", "category",
        "category_id", "open_start_ns", "cpu_ns_total", "on_cpu_ts", "last_cpu_ns",
        "cmd_open", "cmd_sweep_open", "cmd_seen", "cmd_gate_present",
        "cmd_in_window", "cmd_open_ns", "cmd_banked_ns", "cmd_anchor_ns",
        "query_id", "query_quality"
    };
    for (size_t i = 0; i < sizeof(row_fields) / sizeof(row_fields[0]); i++) {
        struct row_fx r = base_row();
        r.drop_key = row_fields[i];
        CHECK(pgwt_overlay_parse(render_one(&r), &snap) != 0 &&
              snap.status != PGWT_OVERLAY_INCLUDED,
              "a row missing %s was accepted", row_fields[i]);
        pgwt_open_snapshot_free(&snap);
    }

    /* A gate flag sent as a string is a missing field, not a truthy value. */
    struct row_fx r = base_row();
    r.drop_key = "cmd_sweep_open";
    r.raw_extra = "\"cmd_sweep_open\":\"true\"";
    CHECK(pgwt_overlay_parse(render_one(&r), &snap) != 0,
          "a string-typed gate flag was accepted as true");
    pgwt_open_snapshot_free(&snap);

    /* The daemon's own command-open ns must agree with our reduction of the
     * same gate state — the quantity the verdict is computed from, not just
     * the verdict. */
    r = base_row();
    r.drop_key = "cmd_open_ns";
    r.raw_extra = "\"cmd_open_ns\":123";
    CHECK(pgwt_overlay_parse(render_one(&r), &snap) != 0,
          "a cmd_open_ns disagreeing with our gate reduction was accepted");
    CHECK(strstr(snap.reason, "cmd_open_ns") != NULL, "reason: %s", snap.reason);
    pgwt_open_snapshot_free(&snap);

    /* An empty wait category: the producer refuses a row it could not
     * resolve, so a consumer that accepted "" would undo that contract. */
    r = base_row();
    r.category = "";
    r.drop_key = "category";
    r.raw_extra = "\"category\":\"\"";
    CHECK(pgwt_overlay_parse(render_one(&r), &snap) != 0, "an empty category accepted");
    pgwt_open_snapshot_free(&snap);

    /* Unusable row values. */
    r = base_row(); r.pid = 0;
    CHECK(pgwt_overlay_parse(render_one(&r), &snap) != 0, "pid 0 accepted");
    pgwt_open_snapshot_free(&snap);

    r = base_row(); r.open_start_ns = SNAP_MONO + 1; r.cpu_ns = NULL;
    CHECK(pgwt_overlay_parse(render_one(&r), &snap) != 0,
          "an open_start after the snapshot accepted");
    pgwt_open_snapshot_free(&snap);

    r = base_row(); r.event_id = PGWT_MARKER_EXEC_START;
    CHECK(pgwt_overlay_parse(render_one(&r), &snap) != 0, "a marker as the open event");
    pgwt_open_snapshot_free(&snap);

    r = base_row(); r.event_id = PGWT_EVENT_EXIT;
    CHECK(pgwt_overlay_parse(render_one(&r), &snap) != 0, "the exit sentinel as the event");
    pgwt_open_snapshot_free(&snap);

    r = base_row(); r.on_cpu_ts = SNAP_MONO + 1;
    CHECK(pgwt_overlay_parse(render_one(&r), &snap) != 0, "on_cpu_ts after the snapshot");
    pgwt_open_snapshot_free(&snap);

    /* CPU: the daemon's figure must agree with the shared arithmetic, and
     * must be absent exactly when the daemon measures no CPU. */
    r = base_row(); r.cpu_ns = "2999999999";
    CHECK(pgwt_overlay_parse(render_one(&r), &snap) != 0,
          "a CPU figure disagreeing with the shared helper was accepted");
    CHECK(strstr(snap.reason, "cpu_ns") != NULL, "reason: %s", snap.reason);
    pgwt_open_snapshot_free(&snap);

    struct fx nf = base_fx();
    static char rowbuf[2048];
    r = base_row();
    render_row(rowbuf, sizeof(rowbuf), &r);
    nf.cpu_accounting = "false";
    nf.entries = rowbuf;
    CHECK(pgwt_overlay_parse(render(&nf), &snap) != 0,
          "a CPU figure present while the daemon measures none was accepted");
    pgwt_open_snapshot_free(&snap);

    /* cpu_accounting TRUE but the row carries no figure: absence stays
     * absence. Reading the computed number here would turn "the daemon did
     * not report one" into a measurement. */
    r = base_row(); r.cpu_ns = NULL;
    CHECK(pgwt_overlay_parse(render_one(&r), &snap) == 0,
          "a row with no cpu_ns was rejected: %s", snap.reason);
    CHECK(snap.count == 1 && snap.items[0].cpu_ns == PGWT_CPU_NS_UNKNOWN,
          "an absent cpu_ns became a measured number");
    pgwt_open_snapshot_free(&snap);

    /* …and with cpu_accounting false too. */
    r = base_row(); r.cpu_ns = NULL;
    render_row(rowbuf, sizeof(rowbuf), &r);
    nf = base_fx();
    nf.cpu_accounting = "false";
    nf.entries = rowbuf;
    CHECK(pgwt_overlay_parse(render(&nf), &snap) == 0, "cpu_accounting false rejected: %s",
          snap.reason);
    CHECK(snap.count == 1 && snap.items[0].cpu_ns == PGWT_CPU_NS_UNKNOWN,
          "unmeasured CPU became a number");
    pgwt_open_snapshot_free(&snap);

    /* One bad row fails the WHOLE reply: a partially parsed snapshot is a
     * silently short total. */
    static char two[4096];
    struct row_fx good = base_row(), bad = base_row();
    bad.pid = 4243;
    bad.drop_key = "category";
    size_t o = render_row(two, sizeof(two), &good);
    o += (size_t)snprintf(two + o, sizeof(two) - o, ",");
    render_row(two + o, sizeof(two) - o, &bad);
    struct fx mf = base_fx();
    mf.entries = two;
    CHECK(pgwt_overlay_parse(render(&mf), &snap) != 0 && snap.count == 0,
          "one bad row among good ones produced a partial snapshot");
    pgwt_open_snapshot_free(&snap);
}

/* ── 2. the gate-state table ───────────────────────────────── */

/* An independently written transcription of the rule, used ONLY to predict
 * what the parser should conclude. Its authority is not that it is the same
 * code (it is not called by the module) but that the fixture also carries the
 * daemon's own `cmd_in_window`, and the parser refuses when the two disagree —
 * so a wrong prediction here shows up as a refused fixture, not as a silent
 * pass. */
static int oracle_in_command(int gate_present, int seen, int sweep_open, int cmd_open,
                             uint64_t banked, uint64_t anchor, uint64_t t0, uint64_t t1)
{
    if (!gate_present)
        return cmd_open != 0;
    if (!seen)
        return 1;
    uint64_t wall = t1 > t0 ? t1 - t0 : 0;
    uint64_t open_ns = banked;
    if (sweep_open) {
        uint64_t a = anchor > t0 ? anchor : t0;
        if (t1 > a)
            open_ns += t1 - a;
    }
    if (open_ns > wall)
        open_ns = wall;
    if (t1 == t0)
        return sweep_open != 0;
    return open_ns * 2 >= wall;
}

static void test_gate_state_table(void)
{
    const uint64_t t0 = OPEN_MONO, t1 = SNAP_MONO;        /* a 9 s stretch */
    const uint64_t bankeds[] = { 0, 1000000000ULL, 100000000000ULL };  /* 0, 1 s, >wall */
    const uint64_t anchors[] = { t0 - 5000000000ULL, t1 - 2000000000ULL, t1 };
    int states = 0, flipped_caught = 0;
    struct pgwt_open_snapshot snap;

    for (int gp = 0; gp < 2; gp++)
    for (int sn = 0; sn < 2; sn++)
    for (int sw = 0; sw < 2; sw++)
    for (int co = 0; co < 2; co++)
    for (size_t bi = 0; bi < 3; bi++)
    for (size_t ai = 0; ai < 3; ai++) {
        int want = oracle_in_command(gp, sn, sw, co, bankeds[bi], anchors[ai], t0, t1);
        struct row_fx r = base_row();
        r.cmd_gate_present = gp;
        r.cmd_seen = sn;
        r.cmd_sweep_open = sw;
        r.cmd_open = co;
        r.cmd_banked_ns = bankeds[bi];
        r.cmd_anchor_ns = anchors[ai];
        r.cmd_in_window = want;
        states++;

        if (pgwt_overlay_parse(render_one(&r), &snap) != 0) {
            CHECK(0, "gate state gp=%d seen=%d sweep=%d open=%d banked=%llu anchor=%llu "
                     "was refused: %s", gp, sn, sw, co,
                  (unsigned long long)bankeds[bi], (unsigned long long)anchors[ai],
                  snap.reason);
        } else {
            CHECK(snap.count == 1 && snap.items[0].in_command == want,
                  "gate state gp=%d seen=%d sweep=%d open=%d banked=%llu anchor=%llu: "
                  "parser said %d, expected %d", gp, sn, sw, co,
                  (unsigned long long)bankeds[bi], (unsigned long long)anchors[ai],
                  snap.count ? snap.items[0].in_command : -1, want);
        }
        pgwt_open_snapshot_free(&snap);

        /* The same state with the daemon's verdict FLIPPED must be refused.
         * Without this direction the table above would pass just as happily
         * against a parser that copied cmd_in_window instead of reducing. */
        r.cmd_in_window = !want;
        if (pgwt_overlay_parse(render_one(&r), &snap) == 0) {
            CHECK(0, "a flipped cmd_in_window was accepted at gp=%d seen=%d sweep=%d "
                     "open=%d banked=%llu anchor=%llu", gp, sn, sw, co,
                  (unsigned long long)bankeds[bi], (unsigned long long)anchors[ai]);
        } else {
            flipped_caught++;
        }
        pgwt_open_snapshot_free(&snap);
    }

    CHECK(states == 144, "the gate table covered %d states, expected 144", states);
    CHECK(flipped_caught == states,
          "only %d of %d flipped verdicts were caught", flipped_caught, states);
}

/* ── 3. build: the positive oracle ─────────────────────────── */

static void build_from(const struct fx *f, struct pgwt_overlay_request *req,
                       struct pgwt_overlay_records *rec, struct pgwt_overlay *ov)
{
    struct pgwt_open_snapshot snap;
    if (pgwt_overlay_parse(render(f), &snap) != 0) {
        CHECK(0, "fixture failed to parse: %s", snap.reason);
        pgwt_overlay_init(ov);
        pgwt_open_snapshot_free(&snap);
        return;
    }
    pgwt_overlay_init(ov);
    pgwt_overlay_build(ov, &snap, req, rec);
    pgwt_open_snapshot_free(&snap);
}

static void build_row(const struct row_fx *r, struct pgwt_overlay_request *req,
                      struct pgwt_overlay_records *rec, struct pgwt_overlay *ov)
{
    static char rowbuf[2048];
    render_row(rowbuf, sizeof(rowbuf), r);
    struct fx f = base_fx();
    f.entries = rowbuf;
    build_from(&f, req, rec, ov);
}

static void test_build_oracle(void)
{
    struct fx f = base_fx();
    struct pgwt_overlay_request req = base_req();
    struct pgwt_overlay_records rec = no_records();
    struct pgwt_overlay ov;

    build_from(&f, &req, &rec, &ov);
    CHECK(ov.status == PGWT_OVERLAY_INCLUDED, "build: %s", ov.reason);
    CHECK(ov.count == 1, "expected one span, got %zu", ov.count);
    if (ov.count == 1) {
        const struct pgwt_overlay_span *s = &ov.spans[0];
        /* window [994 s, 998 s] ∩ open [990 s, 999 s] = [994 s, 998 s] = 4 s */
        CHECK(s->from_wall_ns == WIN_FROM, "span start");
        CHECK(s->to_wall_ns == WIN_TO, "span end");
        CHECK(s->wall_ns == 4000000000ULL, "span wall %llu != 4000000000",
              (unsigned long long)s->wall_ns);
        CHECK(s->pid == 4242 && s->incarnation == 7, "identity carried through");
        CHECK(s->event_id == EV_LOCK, "event id carried through");
        CHECK(s->query_id == 987654321ULL, "query id carried through");
        CHECK(strcmp(s->category, "Lock") == 0 && s->category_id == 5,
              "the resolved wait class is carried to the reducers");
        /* PROVISIONAL, asserted only as arithmetic: a 3 s stretch CPU over a
         * 9 s stretch gives the 4 s span 3 s * 4/9 = 1333333333 ns. */
        CHECK(s->cpu_ns == 3000000000ULL * 4000000000ULL / 9000000000ULL,
              "span CPU %llu != 1333333333", (unsigned long long)s->cpu_ns);
        CHECK(s->in_command == 0, "majority rule said in-command on 2 s of 9 s");
        CHECK(s->stretch_from_wall_ns == OPEN_WALL && s->stretch_to_wall_ns == SNAP_WALL,
              "provenance stretch");
    }
    CHECK(pgwt_overlay_total_wall_ns(&ov) == 4000000000ULL, "total");

    char status[32] = "";
    CHECK(json_has(&ov, "in_flight_ns", status) && strcmp(status, "included") == 0,
          "included overlay does not render in_flight_ns (status=%s)", status);
    CHECK(json_has(&ov, "pids", NULL), "included overlay does not render a pid count");

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

    /* The CONSUMER-side clamp. The shared helper is deliberately unclamped,
     * so a stretch can report more CPU than wall (a long-running backend
     * whose seeded base is stale); the span must never claim more CPU than
     * the wall time it occupies. */
    struct row_fx r = base_row();
    r.cpu_ns_total = 900000000000ULL;      /* 900 s of CPU in a 9 s stretch */
    r.on_cpu_ts = 0;
    r.last_cpu_ns = 0;
    r.cpu_ns = "900000000000";
    build_row(&r, &req, &rec, &ov);
    CHECK(ov.count == 1 && ov.spans[0].cpu_ns == ov.spans[0].wall_ns,
          "an over-wall CPU figure was not clamped to the span (%llu vs %llu)",
          ov.count ? (unsigned long long)ov.spans[0].cpu_ns : 0,
          ov.count ? (unsigned long long)ov.spans[0].wall_ns : 0);
    if (ov.count == 1)
        CHECK(pgwt_overlay_span_cpu_ns(&ov.spans[0], WIN_FROM,
                                       WIN_FROM + 1000000000ULL) == 1000000000ULL,
              "the sub-window CPU was not clamped either");
    pgwt_overlay_free(&ov);
}

static void test_build_clipping(void)
{
    struct fx f = base_fx();
    struct pgwt_overlay_records rec = no_records();
    struct pgwt_overlay ov;

    /* Never extrapolate past the observation. */
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

    /* THE PROPERTY THAT REMOVES EVERY NEAR-NOW HEURISTIC: a window that ends
     * before the open stamp produces an empty overlay by the clip alone. No
     * window-position test exists anywhere in the module. */
    req = base_req();
    req.from_wall_ns = OPEN_WALL - 500000000000ULL;
    req.to_wall_ns   = OPEN_WALL - 100000000000ULL;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.status == PGWT_OVERLAY_INCLUDED,
          "a historical window degraded instead of being empty: %s", ov.reason);
    CHECK(ov.count == 0, "a historical window synthesized %zu spans", ov.count);
    CHECK(pgwt_overlay_total_wall_ns(&ov) == 0, "a historical window contributed time");
    pgwt_overlay_free(&ov);

    /* A window entirely after the snapshot is UNKNOWN, not empty. */
    req = base_req();
    req.from_wall_ns = SNAP_WALL + 1;
    req.to_wall_ns   = SNAP_WALL + 60000000000ULL;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.status != PGWT_OVERLAY_INCLUDED,
          "a window past the snapshot was answered as an empty overlay");
    CHECK(strstr(ov.reason, "predates") != NULL, "reason: %s", ov.reason);
    assert_absent_not_zero(&ov, "window past the snapshot");
    pgwt_overlay_free(&ov);

    /* The boundary. */
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

    /* The close of the same interval WAS admitted (A's overlap admission) —
     * the span must go, or the window counts the stretch twice. */
    evs[0] = mk_ev(4242, OPEN_MONO + 1, EV_LOCK, 0, 0);
    rec.events = evs; rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.status == PGWT_OVERLAY_INCLUDED, "precedence case degraded: %s", ov.reason);
    CHECK(ov.count == 0, "a closing record past the open stamp did not drop the span");
    pgwt_overlay_free(&ov);

    /* A record exactly AT the open stamp is the record that OPENED it. */
    evs[0] = mk_ev(4242, OPEN_MONO, 0, EV_LOCK, 0);
    rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1, "the opening record itself suppressed the span");
    pgwt_overlay_free(&ov);

    evs[0] = mk_ev(4242, OPEN_MONO - 5000000000ULL, 0, EV_LOCK, 0);
    rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1, "an earlier record for the same pid suppressed the span");
    pgwt_overlay_free(&ov);

    evs[0] = mk_ev(9999, OPEN_MONO + 5000000000ULL, EV_LOCK, 0, 0);
    rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1, "another pid's record suppressed the span");
    pgwt_overlay_free(&ov);

    /* A marker carries no duration and closes no interval. */
    evs[0] = mk_ev(4242, OPEN_MONO + 2000000000ULL,
                   PGWT_MARKER_EXEC_START, PGWT_MARKER_EXEC_START, 0);
    rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1, "a marker after the open stamp suppressed the span");
    pgwt_overlay_free(&ov);

    /* A point sample is an observation, not an interval close. */
    evs[0] = mk_ev(4242, OPEN_MONO + 2000000000ULL, 0, EV_LOCK, PGWT_EVENT_FLAG_SAMPLE);
    rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 1, "a point sample after the open stamp suppressed the span");
    pgwt_overlay_free(&ov);

    /* An exit DOES end the interval, and we have no duration for it. */
    evs[0] = mk_ev(4242, OPEN_MONO + 2000000000ULL, EV_LOCK, PGWT_EVENT_EXIT, 0);
    rec.n = 1;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 0, "an exit record after the open stamp left the span standing");
    pgwt_overlay_free(&ov);

    /* The close is found wherever it sits in the array. */
    evs[0] = mk_ev(4242, OPEN_MONO + 1, EV_LOCK, 0, 0);
    evs[1] = mk_ev(9999, OPEN_MONO - 1, 0, 1, 0);
    evs[2] = mk_ev(4242, OPEN_MONO - 2, 0, 1, 0);
    rec.n = 3;
    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 0, "the closing record was missed when it was not last");
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
    struct pgwt_trace_event ev = mk_ev(4242, 0, EV_LOCK, 0, 0);
    uint64_t late = OPEN_WALL + 1;
    struct pgwt_overlay_records rec = { &ev, 1, 0, wall_of_late, &late };

    build_from(&f, &req, &rec, &ov);
    CHECK(ov.count == 0, "the per-segment wall_of callback was ignored");
    pgwt_overlay_free(&ov);
}

/* ── 4. the bypass suite on build ──────────────────────────── */

static void test_build_bypass(void)
{
    struct fx f = base_fx();
    struct pgwt_overlay_records rec = no_records();
    struct pgwt_open_snapshot snap;
    struct pgwt_overlay ov;

    pgwt_overlay_init(&ov);
    assert_absent_not_zero(&ov, "uninitialised overlay");
    CHECK(pgwt_overlay_total_wall_ns(&ov) == 0, "uninitialised overlay has a total");
    pgwt_overlay_free(&ov);

    CHECK(pgwt_overlay_parse(render(&f), &snap) == 0, "fixture parse: %s", snap.reason);

    /* The summary route, handed a perfectly good snapshot. */
    struct pgwt_overlay_request req = base_req();
    req.route = PGWT_OVERLAY_ROUTE_SUMMARY;
    pgwt_overlay_init(&ov);
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) != 0, "summary route built an overlay");
    CHECK(ov.status == PGWT_OVERLAY_REFUSED, "summary route is not a refusal");
    assert_absent_not_zero(&ov, "summary route");
    pgwt_overlay_free(&ov);
    CHECK(pgwt_overlay_wanted(&req, NULL, 0) == 0, "wanted() allows the summary route");

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

    /* No admitted-record view: the precedence rule cannot run. */
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
    bf.status = "\"unavailable\"";
    bf.complete = "false";
    bf.reason = "\"ring backlog not drained\"";
    bf.omit_pids = 1;
    pgwt_overlay_parse(render(&bf), &snap);
    req = base_req();
    pgwt_overlay_init(&ov);
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) != 0, "a degraded snapshot built");
    CHECK(strstr(ov.reason, "ring backlog") != NULL, "the daemon's reason was lost: %s",
          ov.reason);
    assert_absent_not_zero(&ov, "degraded snapshot");
    pgwt_overlay_free(&ov);
    pgwt_open_snapshot_free(&snap);

    /* committed_through behind the open start. */
    bf = base_fx();
    bf.committed_through_ns = num(900000000000ULL);     /* 900 s < the 990 s open */
    bf.committed_through_wall_ns = str(900000000000ULL + (uint64_t)OFFSET);
    pgwt_overlay_parse(render(&bf), &snap);
    pgwt_overlay_init(&ov);
    req = base_req();
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) != 0,
          "an unpublished closed-record prefix was accepted");
    CHECK(strstr(ov.reason, "published") != NULL, "reason: %s", ov.reason);
    assert_absent_not_zero(&ov, "unpublished prefix");
    pgwt_overlay_free(&ov);
    pgwt_open_snapshot_free(&snap);

    /* …but a historical window must NOT be blanked by that same condition. */
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

    /* Two live intervals for one pid that overlap in time. */
    static char two[4096];
    struct row_fx a = base_row(), b = base_row();
    b.incarnation = 8;
    b.event_id = EV_LOCK + 1;
    b.open_start_ns = OPEN_MONO + 1000000000ULL;
    b.cmd_anchor_ns = SNAP_MONO - 2000000000ULL;
    b.cpu_ns = "3000000000";
    size_t o = render_row(two, sizeof(two), &a);
    o += (size_t)snprintf(two + o, sizeof(two) - o, ",");
    render_row(two + o, sizeof(two) - o, &b);
    bf = base_fx();
    bf.entries = two;
    CHECK(pgwt_overlay_parse(render(&bf), &snap) == 0,
          "the overlapping-pid fixture does not parse: %s", snap.reason);
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

    /* io_workers are excluded from AAS/DB Time everywhere else; the overlay
     * must not be the door they come back through. */
    struct row_fx r = base_row();
    r.cat_flag = PGWT_EVENT_FLAG_IO_WORKER;
    r.category = "Activity";
    r.category_id = 9;
    build_row(&r, &req, &rec, &ov);
    CHECK(ov.status == PGWT_OVERLAY_INCLUDED, "io_worker case degraded: %s", ov.reason);
    CHECK(ov.count == 0, "an io_worker open interval was credited");
    pgwt_overlay_free(&ov);

    /* A maintenance backend is categorised, not excluded. */
    r = base_row();
    r.cat_flag = PGWT_EVENT_FLAG_MAINT;
    build_row(&r, &req, &rec, &ov);
    CHECK(ov.count == 1 && ov.spans[0].cat_flag == PGWT_EVENT_FLAG_MAINT,
          "an autovacuum open interval lost its category");
    pgwt_overlay_free(&ov);

    /* CPU that was never measured stays unmeasured, in the span and in every
     * sub-window of it. */
    static char rowbuf[2048];
    r = base_row();
    r.cpu_ns = NULL;
    render_row(rowbuf, sizeof(rowbuf), &r);
    struct fx nf = base_fx();
    nf.cpu_accounting = "false";
    nf.entries = rowbuf;
    build_from(&nf, &req, &rec, &ov);
    CHECK(ov.count == 1 && ov.spans[0].cpu_ns == PGWT_CPU_NS_UNKNOWN,
          "unmeasured CPU became a measured number");
    if (ov.count == 1)
        CHECK(pgwt_overlay_span_cpu_ns(&ov.spans[0], WIN_FROM, WIN_TO) == PGWT_CPU_NS_UNKNOWN,
              "unmeasured CPU became measured in a sub-window");
    pgwt_overlay_free(&ov);

    /* The command majority flips when most of the stretch is in-command.
     * banked 3 s + 2 s still open = 5 s of a 9 s stretch → in-command. */
    r = base_row();
    r.cmd_banked_ns = 3000000000ULL;
    r.cmd_in_window = 1;
    build_row(&r, &req, &rec, &ov);
    CHECK(ov.count == 1 && ov.spans[0].in_command == 1,
          "5 s of command inside a 9 s stretch was not a majority");
    pgwt_overlay_free(&ov);

    /* The #98 distinction: with the sweep flag clear the stretch is out of
     * command even though the INSTANTANEOUS gate is set. Classifying on
     * cmd_open would get this backwards. */
    r = base_row();
    r.cmd_open = 1;
    r.cmd_sweep_open = 0;
    r.cmd_banked_ns = 0;
    r.cmd_in_window = 0;
    build_row(&r, &req, &rec, &ov);
    CHECK(ov.count == 1 && ov.spans[0].in_command == 0,
          "the instantaneous gate overrode the marker sweep");
    pgwt_overlay_free(&ov);

    /* …and with no sweep state at all, cmd_open IS the answer. */
    r = base_row();
    r.cmd_gate_present = 0;
    r.cmd_open = 1;
    r.cmd_sweep_open = 0;
    r.cmd_seen = 0;
    r.cmd_in_window = 1;
    build_row(&r, &req, &rec, &ov);
    CHECK(ov.count == 1 && ov.spans[0].in_command == 1,
          "a pid with no sweep state did not fall back to cmd_open");
    pgwt_overlay_free(&ov);
}

/* ── 5. the golden fixture: the daemon half's own sample ───── */

/* tests/results/issue424-daemon/sample_open_intervals.json at
 * agent/424-daemon 13524e0, verbatim (whitespace collapsed). If the producer
 * changes shape, this is what notices. */
static const char GOLDEN[] =
"{\"ok\":true,\"cmd\":\"open_intervals\",\"status\":\"included\",\"reason\":\"\","
"\"complete\":true,\"snapshot_ns\":1832456789012345,"
"\"snapshot_wall_ns\":\"1792081234567902468\",\"now_mono_ns\":1832456789000000,"
"\"now_wall_ns\":\"1792081234567890123\",\"committed_through_ns\":1832456791500000,"
"\"committed_through_wall_ns\":\"1792081234570390123\",\"daemon_generation\":3,"
"\"daemon_start_wall_ns\":\"1792080634567890123\",\"daemon_pid\":51234,"
"\"tier\":\"escalated\",\"cpu_accounting\":true,\"cmd_gate_active\":true,"
"\"open_count\":3,\"pids\":[{"
"\"pid\":51890,\"incarnation\":1832400000051890,\"generation\":3,"
"\"event_id\":83886080,\"cat_flag\":0,\"eff_event\":83886080,\"flags\":2,"
"\"open_start_ns\":1832454489012345,\"duration_ns\":2300000000,\"cpu_ns\":0,"
"\"cpu_ns_total\":180000000,\"on_cpu_ts\":0,\"last_cpu_ns\":180000000,"
"\"query_id\":10313739733847959536,\"query_quality\":1,\"category\":\"Lock\","
"\"category_id\":5,\"cmd_open_ns\":2300000000,\"cmd_banked_ns\":2300000000,"
"\"cmd_anchor_ns\":1832454489012345,\"cmd_open\":true,\"cmd_sweep_open\":true,"
"\"cmd_seen\":true,\"cmd_gate_present\":true,\"cmd_in_window\":true,"
"\"cmd_by_markers\":true,\"query_synth\":false},{"
"\"pid\":51891,\"incarnation\":1832400000051891,\"generation\":3,"
"\"event_id\":0,\"cat_flag\":0,\"eff_event\":0,\"flags\":2,"
"\"open_start_ns\":1832456149012345,\"duration_ns\":640000000,"
"\"cpu_ns\":612000000,\"cpu_ns_total\":1000000000,"
"\"on_cpu_ts\":1832456177012345,\"last_cpu_ns\":1000000000,"
"\"query_id\":1234605616436508552,\"query_quality\":1,\"category\":\"CPU\","
"\"category_id\":0,\"cmd_open_ns\":640000000,\"cmd_banked_ns\":640000000,"
"\"cmd_anchor_ns\":1832456149012345,\"cmd_open\":true,\"cmd_sweep_open\":true,"
"\"cmd_seen\":true,\"cmd_gate_present\":true,\"cmd_in_window\":true,"
"\"cmd_by_markers\":true,\"query_synth\":false},{"
"\"pid\":51077,\"incarnation\":1832400000051077,\"generation\":3,"
"\"event_id\":150994944,\"cat_flag\":4,\"eff_event\":150994944,\"flags\":0,"
"\"open_start_ns\":1832415789012345,\"duration_ns\":41000000000,\"cpu_ns\":0,"
"\"cpu_ns_total\":5000000,\"on_cpu_ts\":0,\"last_cpu_ns\":5000000,"
"\"query_id\":0,\"query_quality\":0,\"category\":\"Activity\","
"\"category_id\":9,\"cmd_open_ns\":0,\"cmd_banked_ns\":0,"
"\"cmd_anchor_ns\":1832415789012345,\"cmd_open\":false,\"cmd_sweep_open\":false,"
"\"cmd_seen\":false,\"cmd_gate_present\":false,\"cmd_in_window\":false,"
"\"cmd_by_markers\":false,\"query_synth\":false}]}";

static void test_golden(void)
{
    struct pgwt_open_snapshot snap;
    CHECK(pgwt_overlay_parse(GOLDEN, &snap) == 0,
          "the daemon half's own sample reply was REJECTED: %s", snap.reason);
    CHECK(snap.status == PGWT_OVERLAY_INCLUDED, "golden status");
    CHECK(snap.count == 3, "golden rows: %zu", snap.count);
    if (snap.count != 3) {
        pgwt_open_snapshot_free(&snap);
        return;
    }
    CHECK(snap.snapshot_wall_ns == 1792081234567902468ULL, "golden snapshot wall");
    CHECK(snap.items[0].pid == 51890 && strcmp(snap.items[0].category, "Lock") == 0,
          "golden row 0");
    /* 1e9 + (snapshot - on_cpu_ts = 612 ms) - 1e9, via the shared helper. */
    CHECK(snap.items[1].cpu_ns == 612000000ULL, "golden row 1 CPU %llu",
          (unsigned long long)snap.items[1].cpu_ns);
    CHECK(snap.items[2].cat_flag == PGWT_EVENT_FLAG_IO_WORKER, "golden row 2 is an io_worker");
    CHECK(snap.items[0].in_command == 1 && snap.items[1].in_command == 1 &&
          snap.items[2].in_command == 0,
          "golden classifications %d/%d/%d", snap.items[0].in_command,
          snap.items[1].in_command, snap.items[2].in_command);

    /* Build over a window that covers all three stretches. Row 0 contributes
     * 2.3 s, row 1 0.64 s, row 2 is an io_worker and contributes nothing. */
    uint64_t snap_wall = snap.snapshot_wall_ns;
    struct pgwt_overlay_request req = base_req();
    req.from_wall_ns = snap_wall - 60000000000ULL;
    req.to_wall_ns   = snap_wall;
    struct pgwt_overlay_records rec = no_records();
    struct pgwt_overlay ov;
    pgwt_overlay_init(&ov);
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) == 0, "golden build: %s", ov.reason);
    CHECK(ov.count == 2, "golden spans: %zu (the io_worker must be excluded)", ov.count);
    CHECK(pgwt_overlay_total_wall_ns(&ov) == 2300000000ULL + 640000000ULL,
          "golden in-flight total %llu != 2940000000",
          (unsigned long long)pgwt_overlay_total_wall_ns(&ov));
    pgwt_overlay_free(&ov);
    pgwt_open_snapshot_free(&snap);
}

/* ── 6. scale ──────────────────────────────────────────────── */

static char *render_many(int n)
{
    static char *buf;
    size_t cap = 1024 + (size_t)n * 700;
    free(buf);
    buf = malloc(cap);
    size_t off = 0;
    char *rows = malloc((size_t)n * 700 + 16);
    size_t ro = 0;
    for (int i = 0; i < n; i++) {
        struct row_fx r = base_row();
        r.pid = (uint32_t)(1000 + i);
        r.incarnation = (uint64_t)(i + 1);
        r.query_id = (uint64_t)i;
        if (i)
            ro += (size_t)snprintf(rows + ro, (size_t)n * 700 + 16 - ro, ",");
        ro += render_row(rows + ro, (size_t)n * 700 + 16 - ro, &r);
    }
    struct fx f = base_fx();
    f.entries = rows;
    off = (size_t)snprintf(buf, cap, "%s", render(&f));
    (void)off;
    free(rows);
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
    CHECK(pgwt_overlay_build(&ov, &snap, &req, &rec) == 0, "1024-backend build failed: %s",
          ov.reason);
    CHECK(ov.count == 1024, "1024 open intervals produced %zu spans", ov.count);
    /* Every one is open from 990 s and the window is [994 s, 998 s]: 4 s each. */
    CHECK(pgwt_overlay_total_wall_ns(&ov) == 1024ULL * 4000000000ULL,
          "1024 x 4 s != %llu", (unsigned long long)pgwt_overlay_total_wall_ns(&ov));
    pgwt_overlay_free(&ov);
    pgwt_open_snapshot_free(&snap);
}

/* ── 7. transport ──────────────────────────────────────────── */

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
    switch (mode) {
    case REPLY_OK: {
        struct fx f = base_fx();
        char *line = render(&f);
        write_all(c, line, strlen(line));
        write_all(c, "\n", 1);
        break;
    }
    case REPLY_BIG:
    case REPLY_FRAGMENTED: {
        char *body = render_many(1024);
        size_t n = strlen(body);
        if (mode == REPLY_BIG) {
            write_all(c, body, n);
            write_all(c, "\n", 1);
        } else {
            /* Deliver in paced chunks, so the client has to re-poll and
             * re-read many times for one reply. The chunk is 32 KB rather
             * than 4 KB for a reason worth stating: a 1024-backend reply is
             * ~500 KB, and 4 KB x 2 ms would spend 250 ms of the test's own
             * 300 ms deadline in the FIXTURE's sleeps — the test would then
             * be measuring how fast this function writes, not whether the
             * client survives fragmentation. ~18 chunks still proves the
             * re-poll loop; the drip case below is what tests the deadline. */
            for (size_t off = 0; off < n; off += 32768) {
                size_t chunk = n - off < 32768 ? n - off : 32768;
                write_all(c, body + off, chunk);
                nanosleep(&(struct timespec){ 0, 2 * 1000 * 1000 }, NULL);
            }
            write_all(c, "\n", 1);
        }
        break;
    }
    case REPLY_SILENT:
        nanosleep(&(struct timespec){ 5, 0 }, NULL);
        break;
    case REPLY_DRIP:
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

    struct pgwt_open_snapshot snap;
    CHECK(pgwt_overlay_fetch(tmpdir, 300, &snap) != 0, "a missing socket was accepted");
    CHECK(snap.status == PGWT_OVERLAY_UNAVAILABLE, "missing socket status");
    CHECK(strstr(snap.reason, "absent") != NULL, "missing socket reason: %s", snap.reason);
    pgwt_open_snapshot_free(&snap);

    int fd = open(sockpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        close(fd);
        CHECK(pgwt_overlay_fetch(tmpdir, 300, &snap) != 0, "a stale socket file was accepted");
        CHECK(snap.status != PGWT_OVERLAY_INCLUDED, "stale socket file status");
        pgwt_open_snapshot_free(&snap);
        unlink(sockpath);
    }

    run_transport_case(REPLY_OK,        "a normal reply",            1, 0,    1);
    /* 1024 backends is ~500 KB: the case a 64 KB buffer fails. */
    run_transport_case(REPLY_BIG,       "a 1024-backend reply",      1, 0,    1024);
    run_transport_case(REPLY_FRAGMENTED,"a fragmented 1024-backend reply", 1, 0, 1024);
    run_transport_case(REPLY_SILENT,    "a silent daemon",           0, 2000, 0);
    run_transport_case(REPLY_DRIP,      "a drip-feeding daemon",     0, 2000, 0);
    run_transport_case(REPLY_TRUNCATED, "a truncated reply",         0, 2000, 0);
    run_transport_case(REPLY_GARBAGE,   "a garbage reply",           0, 2000, 0);

    /* pgwt_overlay_attach(): the ONE call the loader makes, so that every
     * raw-route consumer inherits the same overlay. */
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

    /* A refused route must not touch the socket at all: the fixture daemon
     * here accepts and then says nothing for five seconds. */
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
    test_gate_state_table();
    test_parse_1024();
    test_build_oracle();
    test_build_clipping();
    test_precedence();
    test_precedence_callback();
    test_build_bypass();
    test_filters();
    test_classification();
    test_golden();
    test_transport();

    printf("test_open_overlay: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
