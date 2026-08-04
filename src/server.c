/* server.c — pgwt-server: trace file replay server for web client.
 *
 * Reads JSON lines from stdin, dispatches commands, writes JSON to stdout.
 * Designed to run over SSH: ssh user@host pgwt-server /path/to/traces
 */
#include "pg_wait_tracer.h"
#include "event_reader.h"
#include "summary_reader.h"
#include "compute.h"
#include "wait_event.h"
#include "cmdline.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ── Utility ──────────────────────────────────────────────── */

/* Count lines in a file. Returns 0 if file doesn't exist. */
static int count_lines(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int n = 0;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp))
        n++;
    fclose(fp);
    return n;
}

/* Next power-of-2 >= n, minimum 256, clamped to 1<<30 */
static int next_pow2(int n)
{
    if (n < 256) return 256;
    if (n > (1 << 30)) return 1 << 30;
    int p = 256;
    while (p < n) p <<= 1;
    return p;
}

/* ── cJSON helpers ────────────────────────────────────────── */

/* Add uint64 as raw JSON number (avoids double precision loss for ns timestamps) */
static void cjson_add_uint64(cJSON *obj, const char *name, uint64_t val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)val);
    cJSON_AddRawToObject(obj, name, buf);
}

/* Add int64 as raw JSON number (preserves sign for query_id etc.) */
static void cjson_add_int64(cJSON *obj, const char *name, int64_t val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)val);
    cJSON_AddRawToObject(obj, name, buf);
}

/* Nanosecond timestamps/durations in the B6 contracts are strings: browser
 * JSON numbers cannot exactly represent the trace clock once it exceeds 2^53. */
static void cjson_add_uint64_string(cJSON *obj, const char *name, uint64_t val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)val);
    cJSON_AddStringToObject(obj, name, buf);
}

/* PostgreSQL exposes query_id through signed bigint-facing surfaces. Match
 * top_queries/query_texts so high-bit IDs join and filter consistently. */
static void cjson_add_query_id_string(cJSON *obj, const char *name, uint64_t val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)(int64_t)val);
    cJSON_AddStringToObject(obj, name, buf);
}

/* Create a raw uint64 item for arrays */
static cJSON *cjson_create_uint64(uint64_t val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)val);
    return cJSON_CreateRaw(buf);
}

/* Print cJSON to stdout as a single line and clean up */
static void emit_json(cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (str) {
        puts(str);
        cJSON_free(str);
    }
    cJSON_Delete(root);
}

/* ── Query text map ───────────────────────────────────────── */

struct qt_entry {
    uint64_t query_id;
    char    *text;        /* heap-allocated SQL text */
};

/* ── Backend metadata map ─────────────────────────────────── */

struct bm_entry {
    uint32_t pid;
    uint32_t leader_pid;
    char     type[32];
    char     user[64];
    char     db[64];
};

/* ── Server state ─────────────────────────────────────────── */

/* ═══ Exact-coverage & clock domains (T1: FID-1/2/6/7) ═══════
 *
 * The exact-wins merge needs to know WHERE full-fidelity (transition) data
 * is authoritative, so overlapping samples can be dropped without double
 * counting. Coverage is derived per file:
 *
 *   1. Escalation-marker windows are the authority (FID-1). The daemon
 *      writes PGWT_MARKER_ESCALATE_START/END into the trace; a window
 *      covers [START_ts, END_ts]. Transition timestamps are wait-END
 *      times, so per-block ranges have a hole exactly over a long wait —
 *      markers do not.
 *   2. START with no matching END (daemon crash, or the window is still
 *      open in current.trace): the window is clamped to the LAST
 *      TRANSITIONS-block timestamp committed in its clock generation —
 *      exact coverage never extends past the last exact evidence, so
 *      samples beyond it (e.g. of a still-running wait whose transition
 *      has not landed yet) survive and remain visible.
 *   3. END with no earlier START (window opened before this file; older
 *      file rotated away/deleted): the window starts at that file's first
 *      block timestamp.
 *   4. A file with transitions and NO SAMPLES blocks (--mode full) is
 *      covered whole-file: [first_block_ts, last_block_ts].
 *   5. Legacy fallback: a file with samples + transitions but no
 *      escalation markers anywhere keeps the old per-TRANSITIONS-block
 *      coverage.
 *
 * Boundary rule (FID-6): a sample represents the interval
 * (ts − period, ts]. It is dropped iff its MIDPOINT (ts − period/2) falls
 * inside a covered span (inclusive); worst-case error per window edge is
 * period/2, zero-mean.
 *
 * Clock domain (FID-7): all coverage comparison happens in the MONOTONIC
 * domain. Files written by one daemon run share the monotonic clock; their
 * per-file mono→wall offsets differ whenever NTP steps the wall clock
 * between file opens, so comparing wall times derived from different
 * files' offsets can misplace coverage by the step size. Files are grouped
 * into clock "generations" (monotonic strictly increases across a run;
 * a backwards jump or a large offset change means reboot/restart), the
 * merge runs in mono within each generation, and event timestamps are
 * re-anchored to wall using ONE canonical offset per generation — the
 * newest file's, so "last 15 minutes from now" lines up with the data that
 * is actually newest. */

struct pgwt_span { uint64_t start_ns, end_ns; };    /* mono ns */

struct pgwt_cov_mark {
    uint64_t ts;        /* mono */
    int      is_start;  /* 1 = ESCALATE_START, 0 = ESCALATE_END */
};

struct pgwt_file_cov {
    char     path[512];
    int      present;              /* seen in the latest directory scan */
    int      valid;                /* readable, has at least one block */
    int      is_current;
    uint64_t hdr_start_wall_ns;    /* identity: header fields change when */
    uint64_t hdr_mono_ns;          /* the daemon restarts + truncates */
    int64_t  own_offset;           /* this file's mono→wall offset */
    uint64_t mono_first, mono_last;
    int      has_samples, has_transitions;
    uint64_t sample_period_ns;
    int      blocks_scanned;       /* header-scan watermark (incremental) */
    struct pgwt_span *s_spans; int n_s, cap_s;   /* SAMPLES block spans */
    struct pgwt_span *t_spans; int n_t, cap_t;   /* TRANSITIONS block spans */
    struct pgwt_cov_mark *marks; int n_marks, cap_marks;
    int     *pending_tb; int n_pending, cap_pending;  /* blocks awaiting
                                                         marker decode */
    int      gen;                  /* clock-domain generation */
    int64_t  canon_offset;         /* generation-canonical mono→wall */
};

/* Assembled per-generation coverage, rebuilt on every refresh. */
struct pgwt_gen_cov {
    int      gen;
    int64_t  canon_offset;
    struct pgwt_span *exact;   int n_exact;    /* sorted, merged */
    struct pgwt_span *sampled; int n_sampled;  /* sorted, merged */
    uint64_t sample_period_ns;
};

/* Per-file event cache — for immutable .trace.lz4 files only.
 * Timestamps are kept MONOTONIC; the wall conversion happens at query time
 * with the generation-canonical offset (FID-7). */
struct file_cache_entry {
    char     path[512];
    struct pgwt_trace_event *events;
    int      count;
    int      immutable;
    uint64_t first_mono_ns;  /* earliest event timestamp (for range skip) */
    uint64_t last_mono_ns;   /* latest event timestamp */
};

/* Fidelity summary of a loaded window, returned alongside the event array
 * so handlers can tag responses and gate EXACT-required views. */
struct pgwt_load_info {
    int      has_transitions;   /* a TRANSITIONS block contributed records */
    int      has_samples;       /* a SAMPLES record survived the merge */
    uint64_t sample_period_ns;  /* nominal sample interval (0 if no samples) */
    int      overloaded;        /* DUR-9: hard load bound hit — result is
                                   partial; handlers MUST error, not render */
};

struct pgwt_server {
    char trace_dir[512];
    int  num_cpus;
    struct pgwt_trace_file_entry files[256];
    int  num_files;
    uint64_t earliest_wall_ns;
    uint64_t latest_wall_ns;
    int64_t total_events;

    /* Per-file event cache */
    struct file_cache_entry cache[256];
    int  cache_count;

    /* Coverage / clock-domain state (refreshed per request) */
    struct pgwt_file_cov cov[256];
    int  cov_count;
    int  any_samples;              /* any file has SAMPLES blocks */
    struct pgwt_gen_cov *gens;
    int  num_gens;

    /* Query text map: query_id → SQL text (dynamic, power-of-2 sized) */
    struct qt_entry *qt_map;
    int  qt_capacity;
    int  qt_count;

    /* Backend metadata map: pid → type/user/db (dynamic, power-of-2 sized) */
    struct bm_entry *bm_map;
    int  bm_capacity;
    int  bm_count;

    /* T2: pid → category-flag table (sorted by pid), derived from bm_map.
     * Feeds pgwt_tag_events so every raw compute path can apply the
     * decomposed-AAS model (io_worker exclusion, categories). */
    struct pgwt_pid_cat *pid_cats;
    int  n_pid_cats;
};

/* ── JSON request parsing (cJSON) ─────────────────────────── */

struct pgwt_request {
    int64_t  id;
    char     cmd[32];
    uint64_t from_ns;
    uint64_t to_ns;
    int      num_buckets;
    int      limit;
    int      max_points;
    uint64_t start_ns;
    uint64_t end_ns;
    char     detail[16];       /* "events" for per-event AAS breakdown */
    struct pgwt_filter filter;
};

static uint64_t parse_json_uint64(cJSON *item)
{
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
        return strtoull(item->valuestring, NULL, 10);
    if (cJSON_IsNumber(item))
        return (uint64_t)item->valuedouble;
    return 0;
}

static void parse_filters(cJSON *root, struct pgwt_filter *f)
{
    memset(f, 0, sizeof(*f));

    cJSON *filters = cJSON_GetObjectItem(root, "filters");
    if (!filters) return;

    cJSON *cls = cJSON_GetObjectItem(filters, "class");
    if (cJSON_IsString(cls) && cls->valuestring)
        snprintf(f->class_name, sizeof(f->class_name), "%s", cls->valuestring);

    cJSON *eid = cJSON_GetObjectItem(filters, "event_id");
    if (cJSON_IsNumber(eid))
        f->event_id = (uint32_t)eid->valuedouble;

    cJSON *pid = cJSON_GetObjectItem(filters, "pid");
    if (cJSON_IsNumber(pid))
        f->pid = (uint32_t)pid->valuedouble;

    /* query_id sent as string to avoid JS precision loss on uint64 */
    cJSON *qid = cJSON_GetObjectItem(filters, "query_id");
    if (cJSON_IsString(qid) && qid->valuestring && qid->valuestring[0])
        f->query_id = (uint64_t)strtoll(qid->valuestring, NULL, 10);
    else if (cJSON_IsNumber(qid))
        f->query_id = (uint64_t)(int64_t)qid->valuedouble;
}

static void parse_request(const char *line, struct pgwt_request *req)
{
    memset(req, 0, sizeof(*req));

    cJSON *root = cJSON_Parse(line);
    if (!root) return;

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsNumber(id))
        req->id = (int64_t)id->valuedouble;

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd) && cmd->valuestring)
        snprintf(req->cmd, sizeof(req->cmd), "%s", cmd->valuestring);

    cJSON *from = cJSON_GetObjectItem(root, "from");
    req->from_ns = parse_json_uint64(from);

    cJSON *to = cJSON_GetObjectItem(root, "to");
    req->to_ns = parse_json_uint64(to);

    cJSON *buckets = cJSON_GetObjectItem(root, "buckets");
    if (cJSON_IsNumber(buckets))
        req->num_buckets = (int)buckets->valuedouble;

    cJSON *limit = cJSON_GetObjectItem(root, "limit");
    if (cJSON_IsNumber(limit))
        req->limit = (int)limit->valuedouble;

    cJSON *max_points = cJSON_GetObjectItem(root, "max_points");
    if (cJSON_IsNumber(max_points))
        req->max_points = (int)max_points->valuedouble;

    req->start_ns = parse_json_uint64(cJSON_GetObjectItem(root, "start_ns"));
    req->end_ns = parse_json_uint64(cJSON_GetObjectItem(root, "end_ns"));

    cJSON *detail = cJSON_GetObjectItem(root, "detail");
    if (cJSON_IsString(detail) && detail->valuestring)
        snprintf(req->detail, sizeof(req->detail), "%s", detail->valuestring);

    parse_filters(root, &req->filter);

    /* B6 accepts its documented top-level pid/query_id parameters while the
     * existing filters object remains supported for protocol consistency. */
    if (req->filter.pid == 0) {
        cJSON *pid = cJSON_GetObjectItem(root, "pid");
        if (cJSON_IsNumber(pid))
            req->filter.pid = (uint32_t)pid->valuedouble;
    }
    if (req->filter.query_id == 0)
        req->filter.query_id = parse_json_uint64(
            cJSON_GetObjectItem(root, "query_id"));

    cJSON_Delete(root);
}

/* ── Query text loading ───────────────────────────────────── */

static uint64_t qt_hash64(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

static void qt_map_insert(struct pgwt_server *srv, uint64_t query_id,
                           const char *text)
{
    if (!srv->qt_map) return;
    int mask = srv->qt_capacity - 1;
    uint32_t idx = (uint32_t)(qt_hash64(query_id) & mask);
    for (int i = 0; i < srv->qt_capacity; i++) {
        struct qt_entry *e = &srv->qt_map[idx];
        if (e->query_id == 0) {
            e->query_id = query_id;
            e->text = strdup(text);
            srv->qt_count++;
            return;
        }
        if (e->query_id == query_id)
            return;  /* already present */
        idx = (idx + 1) & mask;
    }
}

/* Free all heap-allocated text strings and reset the map */
static void qt_map_clear(struct pgwt_server *srv)
{
    if (srv->qt_map) {
        for (int i = 0; i < srv->qt_capacity; i++)
            free(srv->qt_map[i].text);
        free(srv->qt_map);
        srv->qt_map = NULL;
    }
    srv->qt_capacity = 0;
    srv->qt_count = 0;
}

static const char *qt_map_lookup(const struct pgwt_server *srv, uint64_t query_id)
{
    if (query_id == 0 || !srv->qt_map)
        return NULL;
    int mask = srv->qt_capacity - 1;
    uint32_t idx = (uint32_t)(qt_hash64(query_id) & mask);
    for (int i = 0; i < srv->qt_capacity; i++) {
        const struct qt_entry *e = &srv->qt_map[idx];
        if (e->query_id == query_id)
            return e->text;
        if (e->query_id == 0)
            return NULL;
        idx = (idx + 1) & mask;
    }
    return NULL;
}

/* Load query_texts.jsonl from trace dir into the hash map.
 * File format: {"q":<query_id>,"t":"<text>","ts":<wall_ns>} per line. */
static void server_load_query_texts(struct pgwt_server *srv)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/query_texts.jsonl", srv->trace_dir);

    /* Size the hash map to fit the file */
    int lines = count_lines(path);
    srv->qt_capacity = next_pow2(lines * 2);
    srv->qt_map = calloc(srv->qt_capacity, sizeof(struct qt_entry));
    if (!srv->qt_map) { srv->qt_capacity = 0; return; }

    FILE *fp = fopen(path, "r");
    if (!fp)
        return;

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;

    while ((line_len = getline(&line, &line_cap, fp)) > 0) {
        cJSON *root = cJSON_Parse(line);
        if (!root) continue;

        cJSON *q = cJSON_GetObjectItem(root, "q");
        cJSON *t = cJSON_GetObjectItem(root, "t");

        uint64_t qid = 0;
        if (cJSON_IsString(q) && q->valuestring)
            qid = (uint64_t)strtoll(q->valuestring, NULL, 10);
        else if (cJSON_IsNumber(q))
            qid = (uint64_t)(int64_t)q->valuedouble;  /* legacy numeric format */
        if (qid == 0 || !cJSON_IsString(t) || !t->valuestring) {
            cJSON_Delete(root);
            continue;
        }

        qt_map_insert(srv, qid, t->valuestring);
        cJSON_Delete(root);
    }

    free(line);
    fclose(fp);
    if (srv->qt_count > 0)
        fprintf(stderr, "pgwt-server: loaded %d query texts\n", srv->qt_count);
}

/* ── Backend metadata map ─────────────────────────────────── */

static void bm_map_insert(struct pgwt_server *srv, uint32_t pid,
                          uint32_t leader_pid, const char *type,
                          const char *user, const char *db)
{
    if (!srv->bm_map) return;
    int mask = srv->bm_capacity - 1;
    uint32_t idx = pid & mask;
    for (int i = 0; i < srv->bm_capacity; i++) {
        struct bm_entry *e = &srv->bm_map[idx];
        if (e->pid == 0) {
            e->pid = pid;
            e->leader_pid = leader_pid;
            snprintf(e->type, sizeof(e->type), "%s", type);
            snprintf(e->user, sizeof(e->user), "%s", user ? user : "");
            snprintf(e->db, sizeof(e->db), "%s", db ? db : "");
            srv->bm_count++;
            return;
        }
        if (e->pid == pid) {
            /* Update with latest info */
            if (leader_pid != 0) e->leader_pid = leader_pid;
            snprintf(e->type, sizeof(e->type), "%s", type);
            if (user && user[0]) snprintf(e->user, sizeof(e->user), "%s", user);
            if (db && db[0]) snprintf(e->db, sizeof(e->db), "%s", db);
            return;
        }
        idx = (idx + 1) & mask;
    }
}

static const struct bm_entry *bm_map_lookup(const struct pgwt_server *srv,
                                             uint32_t pid)
{
    if (pid == 0 || !srv->bm_map) return NULL;
    int mask = srv->bm_capacity - 1;
    uint32_t idx = pid & mask;
    for (int i = 0; i < srv->bm_capacity; i++) {
        const struct bm_entry *e = &srv->bm_map[idx];
        if (e->pid == pid) return e;
        if (e->pid == 0) return NULL;
        idx = (idx + 1) & mask;
    }
    return NULL;
}

static int cmp_backend_link_pid(const void *a, const void *b)
{
    const struct pgwt_backend_link *la = a, *lb = b;
    return (la->pid > lb->pid) - (la->pid < lb->pid);
}

static struct pgwt_backend_link *server_backend_links(
    const struct pgwt_server *srv, int *out_count)
{
    *out_count = 0;
    if (!srv->bm_map || srv->bm_capacity <= 0 || srv->bm_count <= 0)
        return NULL;

    struct pgwt_backend_link *links = calloc(srv->bm_count, sizeof(*links));
    if (!links) {
        *out_count = -1;
        return NULL;
    }
    for (int i = 0; i < srv->bm_capacity; i++) {
        const struct bm_entry *bm = &srv->bm_map[i];
        if (bm->pid == 0 || bm->leader_pid == 0)
            continue;
        links[*out_count].pid = bm->pid;
        links[*out_count].leader_pid = bm->leader_pid;
        (*out_count)++;
    }
    qsort(links, *out_count, sizeof(*links), cmp_backend_link_pid);
    return links;
}

/* Load backends.jsonl from trace dir.
 * Format: {"pid":<int>,"type":"<str>","user":"<str>","db":"<str>",
 *          "leader_pid":<int, optional>} */
static void server_load_backends(struct pgwt_server *srv)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/backends.jsonl", srv->trace_dir);

    /* Size the hash map to fit the file */
    int lines = count_lines(path);
    srv->bm_capacity = next_pow2(lines * 2);
    srv->bm_map = calloc(srv->bm_capacity, sizeof(struct bm_entry));
    if (!srv->bm_map) { srv->bm_capacity = 0; return; }

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        cJSON *root = cJSON_Parse(line);
        if (!root) continue;

        cJSON *pid_item = cJSON_GetObjectItem(root, "pid");
        if (!cJSON_IsNumber(pid_item)) { cJSON_Delete(root); continue; }
        uint32_t pid = (uint32_t)pid_item->valuedouble;
        if (pid == 0) { cJSON_Delete(root); continue; }

        cJSON *type_item = cJSON_GetObjectItem(root, "type");
        cJSON *user_item = cJSON_GetObjectItem(root, "user");
        cJSON *db_item   = cJSON_GetObjectItem(root, "db");
        cJSON *leader_item = cJSON_GetObjectItem(root, "leader_pid");

        const char *type = cJSON_IsString(type_item) ? type_item->valuestring : "unknown";
        const char *user = cJSON_IsString(user_item) ? user_item->valuestring : "";
        const char *db   = cJSON_IsString(db_item)   ? db_item->valuestring   : "";
        uint32_t leader_pid = cJSON_IsNumber(leader_item)
                            ? (uint32_t)leader_item->valuedouble : 0;

        bm_map_insert(srv, pid, leader_pid, type, user, db);
        cJSON_Delete(root);
    }

    fclose(fp);
    if (srv->bm_count > 0)
        fprintf(stderr, "pgwt-server: loaded %d backend metadata entries\n",
                srv->bm_count);
}

/* Map a backends.jsonl type string to its T2 category flag (mirrors
 * pgwt_backend_type_flag over the pgwt_backend_type_name strings). */
static uint32_t bm_type_to_cat_flag(const char *type)
{
    if (strcmp(type, "io_worker") == 0)
        return PGWT_EVENT_FLAG_IO_WORKER;
    if (strcmp(type, "autovac_worker") == 0)
        return PGWT_EVENT_FLAG_MAINT;
    if (strcmp(type, "client") == 0 || strcmp(type, "parallel_worker") == 0
        || strcmp(type, "unknown") == 0)
        return 0;   /* foreground */
    return PGWT_EVENT_FLAG_BACKGROUND;
}

static int pid_cat_cmp(const void *a, const void *b)
{
    uint32_t pa = ((const struct pgwt_pid_cat *)a)->pid;
    uint32_t pb = ((const struct pgwt_pid_cat *)b)->pid;
    return (pa > pb) - (pa < pb);
}

/* Build the sorted pid → category-flag table from the loaded bm_map. */
static void server_build_pid_cats(struct pgwt_server *srv)
{
    free(srv->pid_cats);
    srv->pid_cats = NULL;
    srv->n_pid_cats = 0;
    if (srv->bm_count <= 0 || !srv->bm_map)
        return;
    srv->pid_cats = calloc((size_t)srv->bm_count, sizeof(*srv->pid_cats));
    if (!srv->pid_cats)
        return;
    int n = 0;
    for (int i = 0; i < srv->bm_capacity && n < srv->bm_count; i++) {
        if (srv->bm_map[i].pid == 0)
            continue;
        srv->pid_cats[n].pid  = srv->bm_map[i].pid;
        srv->pid_cats[n].flag = bm_type_to_cat_flag(srv->bm_map[i].type);
        n++;
    }
    qsort(srv->pid_cats, (size_t)n, sizeof(*srv->pid_cats), pid_cat_cmp);
    srv->n_pid_cats = n;
}

/* ── Event caching + on-demand loading ────────────────────── */

/* Check if a file is the current (mutable) trace */
static int is_current_trace(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcmp(base, "current.trace") == 0;
}

/* Get cached events for an immutable file. Reads once, caches forever.
 * Returns NULL for current.trace (handled separately). */
/* Max total cached events — 25% of system RAM, capped at 2GB.
 * Computed once at startup. */
#include <unistd.h>
static int cache_max_events(void)
{
    static int cached = 0;
    if (cached) return cached;
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0) {
        cached = 14 * 1024 * 1024;  /* fallback ~500MB */
        return cached;
    }
    uint64_t ram = (uint64_t)pages * page_size;
    uint64_t budget = ram / 4;  /* 25% of RAM */
    if (budget > 2ULL * 1024 * 1024 * 1024) budget = 2ULL * 1024 * 1024 * 1024;  /* cap 2GB */
    cached = (int)(budget / sizeof(struct pgwt_trace_event));
    return cached;
}
#define CACHE_MAX_EVENTS cache_max_events()

static void cache_evict_oldest(struct pgwt_server *srv)
{
    if (srv->cache_count == 0) return;
    /* Evict the first (oldest) cached entry */
    free(srv->cache[0].events);
    srv->cache[0].events = NULL;
    memmove(&srv->cache[0], &srv->cache[1],
            (srv->cache_count - 1) * sizeof(srv->cache[0]));
    srv->cache_count--;
}

static int cache_total_events(struct pgwt_server *srv)
{
    int total = 0;
    for (int i = 0; i < srv->cache_count; i++)
        total += srv->cache[i].count;
    return total;
}

static struct file_cache_entry *
get_cached_immutable(struct pgwt_server *srv, const char *path)
{
    /* Look for existing entry */
    for (int i = 0; i < srv->cache_count; i++) {
        if (strcmp(srv->cache[i].path, path) == 0)
            return &srv->cache[i];
    }

    /* Check compressed file size — skip caching if too large.
     * Decompressed size ~5x compressed. Skip if decompressed would exceed cache budget. */
    {
        struct stat st;
        if (stat(path, &st) == 0) {
            uint64_t estimated_events = (uint64_t)st.st_size * 5 / sizeof(struct pgwt_trace_event);
            if (estimated_events > (uint64_t)CACHE_MAX_EVENTS) {
                /* Too large to cache — fall through to on-demand reading */
                return NULL;
            }
        }
    }

    /* Evict old entries if cache is full (by slot count or memory) */
    while (srv->cache_count >= 256 || cache_total_events(srv) > CACHE_MAX_EVENTS) {
        cache_evict_oldest(srv);
        if (srv->cache_count == 0) break;
    }

    struct file_cache_entry *ce = &srv->cache[srv->cache_count++];
    memset(ce, 0, sizeof(*ce));
    snprintf(ce->path, sizeof(ce->path), "%s", path);
    ce->immutable = 1;
    ce->count = 0;

    struct pgwt_event_reader reader;
    if (pgwt_reader_open(&reader, path) != 0)
        return ce;

    int cap = 16384;
    ce->events = malloc(cap * sizeof(*ce->events));
    struct pgwt_trace_event block_buf[PGWT_BLOCK_EVENTS];

    /* Store time range for this file (monotonic) */
    if (reader.num_blocks > 0) {
        ce->first_mono_ns = reader.block_index[0].timestamp_ns;
        ce->last_mono_ns = reader.block_index[reader.num_blocks - 1].timestamp_ns;
    }

    for (int b = 0; b < reader.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&reader, b, block_buf,
                                              PGWT_BLOCK_EVENTS, &bi);
        if (n < 0) continue;

        for (int i = 0; i < n; i++) {
            if (ce->count >= cap) {
                cap *= 2;
                struct pgwt_trace_event *tmp = realloc(ce->events, cap * sizeof(*tmp));
                if (!tmp) break;
                ce->events = tmp;
            }
            ce->events[ce->count] = block_buf[i];
            /* Normalize samples into interval shape so the duration-based
             * estimators apply ASH math (each sample worth sample_period_ns).
             * Keep the SAMPLE flag for the exact-wins merge and fidelity.
             * Timestamps stay MONOTONIC in the cache; the wall conversion
             * happens per query with the generation-canonical offset. */
            if (block_buf[i].flags & PGWT_EVENT_FLAG_SAMPLE) {
                ce->events[ce->count].old_event = block_buf[i].new_event;
                ce->events[ce->count].duration_ns = bi.sample_period_ns;
            }
            ce->count++;
        }
    }
    pgwt_reader_close(&reader);

    /* Evict older entries if this new file pushed us over the memory limit */
    while (srv->cache_count > 1 && cache_total_events(srv) > CACHE_MAX_EVENTS) {
        /* Don't evict the entry we just loaded */
        if (&srv->cache[0] == ce) break;
        cache_evict_oldest(srv);
        /* ce pointer may have shifted due to memmove */
        for (int i = 0; i < srv->cache_count; i++) {
            if (strcmp(srv->cache[i].path, path) == 0) {
                ce = &srv->cache[i];
                break;
            }
        }
    }

    return ce;
}

/* DUR-9: hard bound on the raw-load working array. Defaults to the same
 * budget as the immutable-file cache (25% of RAM, capped at 2 GB); the
 * PGWT_LOAD_MAX_EVENTS environment variable overrides it (tests use a tiny
 * value to prove the bound fires as a structured error, never an OOM). */
static int load_max_events(void)
{
    static int cached = 0;
    if (cached)
        return cached;
    const char *env = getenv("PGWT_LOAD_MAX_EVENTS");
    if (env && atoi(env) > 0)
        cached = atoi(env);
    else
        cached = cache_max_events();
    return cached;
}

/* Read events from a trace file for a MONO time range — on demand, no
 * caching. Opens file, seeks to the right blocks via block index, decodes
 * only the blocks that overlap [from_mono, to_mono]. Appends events at
 * *total (growing as needed) with timestamps left MONOTONIC. SAMPLES
 * records are normalized to interval shape (old_event = sampled event,
 * duration_ns = sample_period_ns) so the duration-based estimators apply
 * ASH math.
 * DUR-9: `pid` != 0 pushes the pid filter into the load (events for other
 * pids never enter the working array); *overloaded is set and the load
 * stops when *total reaches load_max_events(). */
static void load_file_range_mono(const char *path,
                                 uint64_t from_mono, uint64_t to_mono,
                                 uint32_t pid, int markers_only,
                                 struct pgwt_trace_event **events,
                                 int *total, int *cap, int *overloaded)
{
    struct pgwt_event_reader reader;
    if (pgwt_reader_open(&reader, path) != 0)
        return;

    int first_block = pgwt_reader_find_block(&reader, from_mono);
    struct pgwt_trace_event block_buf[PGWT_BLOCK_EVENTS];
    int max_events = load_max_events();

    for (int b = first_block; b < reader.num_blocks; b++) {
        if (reader.block_index[b].timestamp_ns > to_mono)
            break;

        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&reader, b, block_buf,
                                              PGWT_BLOCK_EVENTS, &bi);
        if (n < 0) continue;

        for (int i = 0; i < n; i++) {
            uint64_t ts_mono = block_buf[i].timestamp_ns;
            if (ts_mono < from_mono) continue;
            if (ts_mono > to_mono) break;
            if (pid != 0 && block_buf[i].pid != pid)
                continue;
            if (markers_only && !PGWT_IS_MARKER(block_buf[i].old_event))
                continue;

            if (*total >= max_events) {
                *overloaded = 1;
                pgwt_reader_close(&reader);
                return;
            }
            if (*total >= *cap) {
                *cap *= 2;
                struct pgwt_trace_event *tmp = realloc(*events, *cap * sizeof(**events));
                if (!tmp) { pgwt_reader_close(&reader); return; }
                *events = tmp;
            }

            (*events)[*total] = block_buf[i];
            if (block_buf[i].flags & PGWT_EVENT_FLAG_SAMPLE) {
                (*events)[*total].old_event = block_buf[i].new_event;
                (*events)[*total].duration_ns = bi.sample_period_ns;
            }
            (*total)++;
        }
    }
    pgwt_reader_close(&reader);
}

/* ── Coverage & clock-domain refresh (T1) ─────────────────── */

static void span_list_add(struct pgwt_span **spans, int *n, int *cap,
                          uint64_t start, uint64_t end, uint64_t merge_gap)
{
    if (end < start) end = start;
    /* Coalesce with the previous span (blocks arrive in file/time order) */
    if (*n > 0 && start <= (*spans)[*n - 1].end_ns + merge_gap) {
        if (end > (*spans)[*n - 1].end_ns)
            (*spans)[*n - 1].end_ns = end;
        return;
    }
    if (*n >= *cap) {
        int newcap = *cap ? *cap * 2 : 16;
        struct pgwt_span *tmp = realloc(*spans, newcap * sizeof(**spans));
        if (!tmp) return;
        *spans = tmp;
        *cap = newcap;
    }
    (*spans)[*n].start_ns = start;
    (*spans)[*n].end_ns = end;
    (*n)++;
}

static void cov_reset(struct pgwt_file_cov *fc)
{
    free(fc->s_spans);
    free(fc->t_spans);
    free(fc->marks);
    free(fc->pending_tb);
    char path[512];
    memcpy(path, fc->path, sizeof(path));
    memset(fc, 0, sizeof(*fc));
    memcpy(fc->path, path, sizeof(path));
}

static struct pgwt_file_cov *cov_find_or_add(struct pgwt_server *srv,
                                             const char *path)
{
    for (int i = 0; i < srv->cov_count; i++)
        if (strcmp(srv->cov[i].path, path) == 0)
            return &srv->cov[i];
    if (srv->cov_count >= 256)
        return NULL;
    struct pgwt_file_cov *fc = &srv->cov[srv->cov_count++];
    memset(fc, 0, sizeof(*fc));
    snprintf(fc->path, sizeof(fc->path), "%s", path);
    return fc;
}

/* Decode the pending TRANSITIONS blocks of a file, collecting escalation
 * markers. Committed blocks are immutable, so this is incremental-safe. */
static void cov_decode_markers(struct pgwt_file_cov *fc,
                               struct pgwt_event_reader *reader)
{
    struct pgwt_trace_event buf[PGWT_BLOCK_EVENTS];

    for (int p = 0; p < fc->n_pending; p++) {
        int b = fc->pending_tb[p];
        if (b >= reader->num_blocks)
            continue;
        int n = pgwt_reader_decode_block(reader, b, buf, PGWT_BLOCK_EVENTS);
        for (int i = 0; i < n; i++) {
            uint32_t m = buf[i].old_event;
            if (m != PGWT_MARKER_ESCALATE_START &&
                m != PGWT_MARKER_ESCALATE_END)
                continue;
            if (fc->n_marks >= fc->cap_marks) {
                int newcap = fc->cap_marks ? fc->cap_marks * 2 : 8;
                struct pgwt_cov_mark *tmp =
                    realloc(fc->marks, newcap * sizeof(*tmp));
                if (!tmp) return;
                fc->marks = tmp;
                fc->cap_marks = newcap;
            }
            fc->marks[fc->n_marks].ts = buf[i].timestamp_ns;
            fc->marks[fc->n_marks].is_start =
                (m == PGWT_MARKER_ESCALATE_START);
            fc->n_marks++;
        }
    }
    fc->n_pending = 0;
}

/* Incrementally scan one file's block headers into its coverage entry.
 * Marker decode is deferred (cov_decode_markers) so full-mode dirs — where
 * no samples exist and coverage is never consulted — never pay for it. */
static void cov_scan_file(struct pgwt_server *srv, struct pgwt_file_cov *fc)
{
    struct pgwt_event_reader reader;
    if (pgwt_reader_open(&reader, fc->path) != 0) {
        fc->valid = 0;
        return;
    }

    fc->is_current = is_current_trace(fc->path);

    /* Identity check: a daemon restart truncates + rewrites current.trace
     * (new header clocks); a shrunken block count means the same. */
    if (fc->blocks_scanned > 0 &&
        (fc->hdr_start_wall_ns != reader.header.start_time_ns ||
         fc->hdr_mono_ns != reader.header.clock_offset_ns ||
         reader.num_blocks < fc->blocks_scanned)) {
        cov_reset(fc);
        fc->is_current = is_current_trace(fc->path);
    }

    fc->hdr_start_wall_ns = reader.header.start_time_ns;
    fc->hdr_mono_ns = reader.header.clock_offset_ns;
    fc->own_offset = (int64_t)reader.header.start_time_ns
                   - (int64_t)reader.header.clock_offset_ns;

    if (reader.num_blocks <= 0) {
        fc->valid = 0;
        pgwt_reader_close(&reader);
        return;
    }
    fc->valid = 1;
    if (fc->blocks_scanned == 0) {
        /* block-index timestamps are block FIRST timestamps; mono_last is
         * extended block by block below (bi.last_timestamp_ns). */
        fc->mono_first = reader.block_index[0].timestamp_ns;
        fc->mono_last = reader.block_index[0].timestamp_ns;
    }

    for (int b = fc->blocks_scanned; b < reader.num_blocks; b++) {
        struct pgwt_block_info bi;
        if (pgwt_reader_block_info(&reader, b, &bi) != 0)
            continue;
        if (bi.last_timestamp_ns > fc->mono_last)
            fc->mono_last = bi.last_timestamp_ns;
        if (bi.block_type == PGWT_BLOCK_SAMPLES) {
            fc->has_samples = 1;
            if (bi.sample_period_ns)
                fc->sample_period_ns = bi.sample_period_ns;
            /* Merge adjacent sample spans (gap up to 2 periods) */
            span_list_add(&fc->s_spans, &fc->n_s, &fc->cap_s,
                          bi.first_timestamp_ns, bi.last_timestamp_ns,
                          fc->sample_period_ns * 2);
        } else {
            fc->has_transitions = 1;
            span_list_add(&fc->t_spans, &fc->n_t, &fc->cap_t,
                          bi.first_timestamp_ns, bi.last_timestamp_ns, 0);
            if (fc->n_pending >= fc->cap_pending) {
                int newcap = fc->cap_pending ? fc->cap_pending * 2 : 16;
                int *tmp = realloc(fc->pending_tb, newcap * sizeof(int));
                if (tmp) { fc->pending_tb = tmp; fc->cap_pending = newcap; }
            }
            if (fc->n_pending < fc->cap_pending)
                fc->pending_tb[fc->n_pending++] = b;
        }
    }
    fc->blocks_scanned = reader.num_blocks;

    /* Markers matter only when the merge has samples to arbitrate. */
    if (srv->any_samples || fc->has_samples)
        cov_decode_markers(fc, &reader);

    pgwt_reader_close(&reader);
}

/* Sort + merge a span list in place; returns new count. */
static int spans_normalize(struct pgwt_span *s, int n)
{
    if (n <= 1) return n;
    /* insertion sort by start (lists are small and mostly sorted) */
    for (int i = 1; i < n; i++) {
        struct pgwt_span tmp = s[i];
        int j = i - 1;
        while (j >= 0 && s[j].start_ns > tmp.start_ns) {
            s[j + 1] = s[j];
            j--;
        }
        s[j + 1] = tmp;
    }
    int w = 0;
    for (int i = 1; i < n; i++) {
        if (s[i].start_ns <= s[w].end_ns) {
            if (s[i].end_ns > s[w].end_ns)
                s[w].end_ns = s[i].end_ns;
        } else {
            s[++w] = s[i];
        }
    }
    return w + 1;
}

static void gens_free(struct pgwt_server *srv)
{
    for (int g = 0; g < srv->num_gens; g++) {
        free(srv->gens[g].exact);
        free(srv->gens[g].sampled);
    }
    free(srv->gens);
    srv->gens = NULL;
    srv->num_gens = 0;
}

/* Build one generation's assembled coverage from its files' entries. */
static void gen_cov_build(struct pgwt_server *srv, struct pgwt_gen_cov *gc)
{
    int ex_cap = 0, sm_cap = 0;
    gc->exact = NULL; gc->n_exact = 0;
    gc->sampled = NULL; gc->n_sampled = 0;
    gc->sample_period_ns = 0;

    /* Gather all escalation marks of the generation, tagged with the mono
     * start of their file (for the END-without-START rule). */
    struct gmark { uint64_t ts; int is_start; uint64_t file_first; };
    struct gmark *gm = NULL;
    int n_gm = 0, gm_cap = 0;
    uint64_t last_trans_end = 0;   /* latest exact evidence in the gen */

    for (int i = 0; i < srv->cov_count; i++) {
        struct pgwt_file_cov *fc = &srv->cov[i];
        if (!fc->present || !fc->valid || fc->gen != gc->gen)
            continue;

        if (fc->sample_period_ns > gc->sample_period_ns)
            gc->sample_period_ns = fc->sample_period_ns;

        for (int s = 0; s < fc->n_s; s++)
            span_list_add(&gc->sampled, &gc->n_sampled, &sm_cap,
                          fc->s_spans[s].start_ns, fc->s_spans[s].end_ns, 0);

        if (fc->has_transitions && fc->n_t > 0 &&
            fc->t_spans[fc->n_t - 1].end_ns > last_trans_end)
            last_trans_end = fc->t_spans[fc->n_t - 1].end_ns;

        if (!fc->has_samples && fc->has_transitions) {
            /* Rule 4: --mode full file → whole-file exact coverage. */
            span_list_add(&gc->exact, &gc->n_exact, &ex_cap,
                          fc->mono_first, fc->mono_last, 0);
            continue;
        }

        if (fc->has_samples && fc->has_transitions && fc->n_marks == 0) {
            /* Rule 5: legacy tiered file without markers — per-block spans. */
            for (int s = 0; s < fc->n_t; s++)
                span_list_add(&gc->exact, &gc->n_exact, &ex_cap,
                              fc->t_spans[s].start_ns, fc->t_spans[s].end_ns,
                              0);
            continue;
        }

        for (int m = 0; m < fc->n_marks; m++) {
            if (n_gm >= gm_cap) {
                gm_cap = gm_cap ? gm_cap * 2 : 16;
                struct gmark *tmp = realloc(gm, gm_cap * sizeof(*tmp));
                if (!tmp) { free(gm); return; }
                gm = tmp;
            }
            gm[n_gm].ts = fc->marks[m].ts;
            gm[n_gm].is_start = fc->marks[m].is_start;
            gm[n_gm].file_first = fc->mono_first;
            n_gm++;
        }
    }

    /* Marker state machine over the generation's mark stream (Rules 1-3) */
    if (n_gm > 0) {
        /* sort by ts (insertion, small) */
        for (int i = 1; i < n_gm; i++) {
            struct gmark tmp = gm[i];
            int j = i - 1;
            while (j >= 0 && gm[j].ts > tmp.ts) {
                gm[j + 1] = gm[j];
                j--;
            }
            gm[j + 1] = tmp;
        }
        uint64_t open_start = 0;
        int have_open = 0;
        for (int i = 0; i < n_gm; i++) {
            if (gm[i].is_start) {
                if (!have_open) {
                    open_start = gm[i].ts;
                    have_open = 1;
                }
            } else {
                uint64_t start = have_open ? open_start : gm[i].file_first;
                span_list_add(&gc->exact, &gc->n_exact, &ex_cap,
                              start, gm[i].ts, 0);
                have_open = 0;
            }
        }
        if (have_open) {
            /* Rule 2: unclosed window (crash / still open) — clamp to the
             * last committed exact evidence, never beyond. */
            uint64_t end = last_trans_end > open_start
                         ? last_trans_end : open_start;
            span_list_add(&gc->exact, &gc->n_exact, &ex_cap,
                          open_start, end, 0);
        }
    }
    free(gm);

    gc->n_exact = spans_normalize(gc->exact, gc->n_exact);
    gc->n_sampled = spans_normalize(gc->sampled, gc->n_sampled);
}

/* Refresh the coverage table and clock-domain generations. Called at the
 * start of every load / summary decision (cheap: incremental block-header
 * scans; marker decode only for files that can affect a merge). */
static void coverage_refresh(struct pgwt_server *srv)
{
    srv->num_files = pgwt_scan_trace_files(srv->trace_dir, srv->files, 256);

    for (int i = 0; i < srv->cov_count; i++)
        srv->cov[i].present = 0;

    int had_samples = srv->any_samples;
    for (int i = 0; i < srv->num_files; i++) {
        struct pgwt_file_cov *fc = cov_find_or_add(srv, srv->files[i].path);
        if (!fc) continue;
        fc->present = 1;
        cov_scan_file(srv, fc);
        if (fc->has_samples)
            srv->any_samples = 1;
    }

    /* If samples appeared for the first time, files scanned earlier may
     * have skipped marker decode — finish them now. */
    if (srv->any_samples && !had_samples) {
        for (int i = 0; i < srv->cov_count; i++) {
            struct pgwt_file_cov *fc = &srv->cov[i];
            if (!fc->present || !fc->valid || fc->n_pending == 0)
                continue;
            struct pgwt_event_reader reader;
            if (pgwt_reader_open(&reader, fc->path) != 0)
                continue;
            cov_decode_markers(fc, &reader);
            pgwt_reader_close(&reader);
        }
    }

    /* Drop entries for deleted files (compact the array). */
    int w = 0;
    for (int i = 0; i < srv->cov_count; i++) {
        if (srv->cov[i].present) {
            if (w != i) srv->cov[w] = srv->cov[i];
            w++;
        } else {
            cov_reset(&srv->cov[i]);
        }
    }
    srv->cov_count = w;

    /* Sort by own wall start so generation assignment sees run order. */
    for (int i = 1; i < srv->cov_count; i++) {
        struct pgwt_file_cov tmp = srv->cov[i];
        int64_t key = tmp.own_offset + (int64_t)tmp.mono_first;
        int j = i - 1;
        while (j >= 0 &&
               srv->cov[j].own_offset + (int64_t)srv->cov[j].mono_first > key) {
            srv->cov[j + 1] = srv->cov[j];
            j--;
        }
        srv->cov[j + 1] = tmp;
    }

    /* Assign clock-domain generations: within one daemon run/boot the
     * monotonic clock strictly increases across files. A backwards jump —
     * or an offset change too large to be an NTP step — means a new
     * domain. Canonical offset per generation = the newest file's. */
    int gen = -1;
    uint64_t prev_mono_last = 0;
    int64_t prev_offset = 0;
    for (int i = 0; i < srv->cov_count; i++) {
        struct pgwt_file_cov *fc = &srv->cov[i];
        if (!fc->valid) { fc->gen = gen < 0 ? 0 : gen; continue; }
        int64_t doff = fc->own_offset - prev_offset;
        if (gen < 0 ||
            fc->mono_first < prev_mono_last ||
            doff > 300LL * 1000000000LL || doff < -300LL * 1000000000LL)
            gen++;
        fc->gen = gen;
        if (fc->mono_last > prev_mono_last)
            prev_mono_last = fc->mono_last;
        prev_offset = fc->own_offset;
    }
    int num_gens = gen + 1;
    if (num_gens < 1) num_gens = srv->cov_count > 0 ? 1 : 0;

    /* canonical offset: newest (max mono_last) file of each generation */
    for (int g = 0; g < num_gens; g++) {
        uint64_t best_last = 0;
        int64_t canon = 0;
        int found = 0;
        for (int i = 0; i < srv->cov_count; i++) {
            if (srv->cov[i].gen != g || !srv->cov[i].valid) continue;
            if (!found || srv->cov[i].mono_last >= best_last) {
                best_last = srv->cov[i].mono_last;
                canon = srv->cov[i].own_offset;
                found = 1;
            }
        }
        for (int i = 0; i < srv->cov_count; i++)
            if (srv->cov[i].gen == g)
                srv->cov[i].canon_offset = canon;
    }

    /* Rebuild the assembled per-generation coverage. */
    gens_free(srv);
    if (num_gens > 0) {
        srv->gens = calloc(num_gens, sizeof(*srv->gens));
        if (srv->gens) {
            srv->num_gens = num_gens;
            for (int g = 0; g < num_gens; g++) {
                srv->gens[g].gen = g;
                for (int i = 0; i < srv->cov_count; i++)
                    if (srv->cov[i].gen == g && srv->cov[i].valid)
                        srv->gens[g].canon_offset = srv->cov[i].canon_offset;
                gen_cov_build(srv, &srv->gens[g]);
            }
        }
    }

    /* Overall wall range from the canonical offsets (re-anchored) and the
     * approximate total event count (block-count based, as before). */
    uint64_t earliest = 0, latest = 0;
    int64_t total = 0;
    for (int i = 0; i < srv->cov_count; i++) {
        struct pgwt_file_cov *fc = &srv->cov[i];
        if (!fc->valid) continue;
        uint64_t fw = (uint64_t)((int64_t)fc->mono_first + fc->canon_offset);
        uint64_t lw = (uint64_t)((int64_t)fc->mono_last + fc->canon_offset);
        if (earliest == 0 || fw < earliest) earliest = fw;
        if (lw > latest) latest = lw;
        total += (int64_t)fc->blocks_scanned * PGWT_BLOCK_EVENTS;
    }
    if (earliest) srv->earliest_wall_ns = earliest;
    if (latest) srv->latest_wall_ns = latest;
    srv->total_events = total;
}

/* True if mono_ns falls inside any span (spans sorted, merged). */
static int spans_contain(const struct pgwt_span *s, int n, uint64_t mono_ns)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s[mid].end_ns < mono_ns)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && mono_ns >= s[lo].start_ns && mono_ns <= s[lo].end_ns;
}

static const struct pgwt_gen_cov *gen_cov_get(const struct pgwt_server *srv,
                                              int gen)
{
    for (int g = 0; g < srv->num_gens; g++)
        if (srv->gens[g].gen == gen)
            return &srv->gens[g];
    return NULL;
}

/* ── Window fidelity from coverage metadata (FID-2) ───────── */

struct pgwt_wfid {
    enum pgwt_fidelity fid;
    uint64_t sample_period_ns;
};

/* Overlap length of [a1,a2] with [b1,b2]. */
static uint64_t span_overlap(uint64_t a1, uint64_t a2, uint64_t b1, uint64_t b2)
{
    uint64_t s = a1 > b1 ? a1 : b1;
    uint64_t e = a2 < b2 ? a2 : b2;
    return e > s ? e - s : 0;
}

/* Classify a wall window from coverage metadata WITHOUT loading events:
 * EXACT   — exact coverage overlaps and no sampled data lies outside it;
 * SAMPLED — only sampled data overlaps;
 * MIXED   — exact coverage plus sampled data outside it;
 * NONE    — nothing overlaps.
 * Used to gate the summary fast path: summaries are fed by exact records
 * only, so they are valid iff the window is EXACT (or NONE — empty). */
static struct pgwt_wfid window_fidelity(struct pgwt_server *srv,
                                        uint64_t from_wall, uint64_t to_wall)
{
    struct pgwt_wfid wf = { PGWT_FIDELITY_NONE, 0 };
    int has_exact = 0, has_uncovered_samples = 0;

    for (int g = 0; g < srv->num_gens; g++) {
        const struct pgwt_gen_cov *gc = &srv->gens[g];
        int64_t off = gc->canon_offset;
        if ((int64_t)to_wall - off <= 0)
            continue;
        uint64_t fm = ((int64_t)from_wall - off) > 0
                    ? (uint64_t)((int64_t)from_wall - off) : 0;
        uint64_t tm = (uint64_t)((int64_t)to_wall - off);

        for (int i = 0; i < gc->n_exact; i++)
            if (span_overlap(gc->exact[i].start_ns, gc->exact[i].end_ns,
                             fm, tm) > 0)
                has_exact = 1;

        /* Sampled data outside exact coverage? Tolerate up to one sample
         * period of slop at span/window edges. */
        uint64_t slop = gc->sample_period_ns ? gc->sample_period_ns
                                             : 1000000000ULL;
        for (int i = 0; i < gc->n_sampled; i++) {
            uint64_t s = gc->sampled[i].start_ns > fm
                       ? gc->sampled[i].start_ns : fm;
            uint64_t e = gc->sampled[i].end_ns < tm
                       ? gc->sampled[i].end_ns : tm;
            if (e <= s) continue;
            wf.sample_period_ns = gc->sample_period_ns;
            uint64_t covered = 0;
            for (int j = 0; j < gc->n_exact; j++)
                covered += span_overlap(gc->exact[j].start_ns,
                                        gc->exact[j].end_ns, s, e);
            if (e - s > covered + slop)
                has_uncovered_samples = 1;
        }
    }

    if (has_exact && has_uncovered_samples) wf.fid = PGWT_FIDELITY_MIXED;
    else if (has_exact)                     wf.fid = PGWT_FIDELITY_EXACT;
    else if (has_uncovered_samples)         wf.fid = PGWT_FIDELITY_SAMPLED;
    else                                    wf.fid = PGWT_FIDELITY_NONE;
    if (wf.fid == PGWT_FIDELITY_EXACT)
        wf.sample_period_ns = 0;
    return wf;
}

/* ── Event loading ────────────────────────────────────────── */

/*
 * Load events in [from_wall_ns, to_wall_ns] from trace files.
 * Immutable .trace.lz4: from cache (read once per session).
 * current.trace: on-demand block reads (no caching, no memory growth).
 * Returns malloc'd array. Caller must free(). Sets *out_count.
 *
 * Fidelity (trace format v2, D3 + T1): SAMPLES records are normalized so
 * the duration-based estimators apply ASH math. The exact-wins merge drops
 * any sample whose interval midpoint falls inside the exact coverage
 * (escalation-marker windows — see the coverage block comment above), so a
 * sampler running through a full-fidelity window does not double-count —
 * including across long waits whose single transition lands only at
 * wait-end (FID-1), and across files whose wall offsets disagree (FID-7:
 * the merge runs in the monotonic domain per clock generation).
 * When `info` is non-NULL it is filled with what actually contributed
 * (has_transitions / has_samples / sample_period_ns).
 *
 * DUR-9: `pid` != 0 is a pushdown of the request's pid filter — only that
 * pid's events enter the working array, so a pid-filtered query over a long
 * window costs memory proportional to ONE backend's events, not the whole
 * window. Callers pass 0 when the compute needs cross-pid records that a
 * uniform per-event filter would not see (variants reads every pid's
 * exec/plan markers). The array is hard-bounded by load_max_events(): on
 * overflow info->overloaded is set and handlers must emit the structured
 * "window too large" error instead of rendering the partial result.
 */
static struct pgwt_trace_event *
server_load_events_fi_mode(struct pgwt_server *srv,
                           uint64_t from_wall_ns, uint64_t to_wall_ns,
                           uint32_t pid, int markers_only,
                           int *out_count, struct pgwt_load_info *info)
{
    *out_count = 0;
    if (info) memset(info, 0, sizeof(*info));
    int overloaded = 0;
    int max_events = load_max_events();

    /* Rescan directory + refresh coverage/clock-domain state. */
    coverage_refresh(srv);

    if (from_wall_ns == 0)
        from_wall_ns = srv->earliest_wall_ns;
    if (to_wall_ns == 0)
        to_wall_ns = srv->latest_wall_ns;

    int cap = 65536;
    struct pgwt_trace_event *events = malloc(cap * sizeof(*events));
    if (!events) return NULL;
    int total = 0;

    /* Per-segment bookkeeping: each file contributes one contiguous run of
     * events; merge + wall conversion need its generation and offset. */
    struct load_seg { int start, count, gen; int64_t off; };
    struct load_seg segs[256];
    int n_segs = 0;
    uint64_t sample_period_ns = 0;

    for (int ci = 0; ci < srv->cov_count && n_segs < 256 && !overloaded;
         ci++) {
        struct pgwt_file_cov *fc = &srv->cov[ci];
        if (!fc->valid)
            continue;

        /* Query window in this file's mono domain (canonical offset). */
        if ((int64_t)to_wall_ns - fc->canon_offset <= 0)
            continue;
        uint64_t from_m = ((int64_t)from_wall_ns - fc->canon_offset) > 0
                        ? (uint64_t)((int64_t)from_wall_ns - fc->canon_offset)
                        : 0;
        uint64_t to_m = (uint64_t)((int64_t)to_wall_ns - fc->canon_offset);

        if (fc->mono_first > to_m || fc->mono_last < from_m)
            continue;

        if (fc->sample_period_ns)
            sample_period_ns = fc->sample_period_ns;

        int seg_start = total;

        if (fc->is_current) {
            /* On-demand: read only blocks in [from, to] */
            load_file_range_mono(fc->path, from_m, to_m, pid, markers_only,
                                 &events, &total, &cap, &overloaded);
        } else {
            struct file_cache_entry *ce = get_cached_immutable(srv, fc->path);
            if (!ce || !ce->events) {
                /* Cache miss (file too large or alloc failed) */
                load_file_range_mono(fc->path, from_m, to_m, pid, markers_only,
                                     &events, &total, &cap, &overloaded);
            } else {
                for (int i = 0; i < ce->count; i++) {
                    uint64_t ts = ce->events[i].timestamp_ns;
                    if (ts < from_m) continue;
                    if (ts > to_m) break;
                    if (pid != 0 && ce->events[i].pid != pid)
                        continue;
                    if (markers_only && !PGWT_IS_MARKER(ce->events[i].old_event))
                        continue;

                    if (total >= max_events) {
                        overloaded = 1;
                        break;
                    }
                    if (total >= cap) {
                        cap *= 2;
                        struct pgwt_trace_event *tmp =
                            realloc(events, cap * sizeof(*tmp));
                        if (!tmp) { *out_count = total; return events; }
                        events = tmp;
                    }
                    events[total++] = ce->events[i];
                }
            }
        }

        if (total > seg_start) {
            segs[n_segs].start = seg_start;
            segs[n_segs].count = total - seg_start;
            segs[n_segs].gen = fc->gen;
            segs[n_segs].off = fc->canon_offset;
            n_segs++;
        }
    }

    /* Exact-wins merge (mono, per generation) + wall re-anchoring, then
     * derive what actually contributed for the fidelity indicator. */
    int has_transitions = 0, has_samples = 0;
    int kept = 0;
    for (int s = 0; s < n_segs; s++) {
        const struct pgwt_gen_cov *gc = gen_cov_get(srv, segs[s].gen);
        for (int i = segs[s].start; i < segs[s].start + segs[s].count; i++) {
            if (events[i].flags & PGWT_EVENT_FLAG_SAMPLE) {
                /* Boundary rule (FID-6): drop iff the sample's interval
                 * midpoint lies inside exact coverage. */
                uint64_t mid = events[i].timestamp_ns
                             - events[i].duration_ns / 2;
                if (gc && gc->n_exact > 0 &&
                    spans_contain(gc->exact, gc->n_exact, mid))
                    continue;   /* exact data is authoritative here */
                has_samples = 1;
            } else {
                /* T2 backstop (study defect 2): an EXIT record whose closing
                 * interval lies OUTSIDE exact coverage in a generation that
                 * has sampled coverage is a phantom — pre-fix daemons
                 * "closed" sampled-tier seed entries at process exit,
                 * back-filling the backend's whole sampled lifetime as one
                 * interval (usually CPU) that the SAMPLES stream already
                 * covers. The producer is fixed (wp_live gate in BPF); this
                 * drop keeps already-recorded traces honest. Real full-mode
                 * exit intervals lie inside exact coverage and are kept. */
                if (events[i].new_event == PGWT_EVENT_EXIT
                    && gc && gc->n_sampled > 0) {
                    uint64_t mid = events[i].timestamp_ns
                                 - events[i].duration_ns / 2;
                    if (!(gc->n_exact > 0 &&
                          spans_contain(gc->exact, gc->n_exact, mid)))
                        continue;   /* phantom exit interval */
                }
                has_transitions = 1;
            }
            events[kept] = events[i];
            events[kept].timestamp_ns = (uint64_t)
                ((int64_t)events[kept].timestamp_ns + segs[s].off);
            kept++;
        }
    }
    total = kept;

    /* T2: annotate the merged window with categories (pid types from
     * backends.jsonl) and the PLAN/EXEC/CMD marker windows; classify exact
     * we==0 intervals against the command gate. One pass feeding every raw
     * estimator (docs/AAS_SEMANTICS_DECISION.md). */
    if (!markers_only)
        pgwt_tag_events(events, total, srv->pid_cats, srv->n_pid_cats);

    if (info) {
        info->has_transitions = has_transitions;
        info->has_samples = has_samples;
        info->sample_period_ns = has_samples ? sample_period_ns : 0;
        info->overloaded = overloaded;
    }
    if (overloaded)
        fprintf(stderr, "ERROR: window too large: the requested range holds "
                "more than %d events%s — narrow the time range%s\n",
                max_events, pid ? " for this pid" : "",
                pid ? "" : " or add a pid/query filter");

    *out_count = total;
    return events;
}

static struct pgwt_trace_event *
server_load_events_fi(struct pgwt_server *srv,
                      uint64_t from_wall_ns, uint64_t to_wall_ns,
                      uint32_t pid,
                      int *out_count, struct pgwt_load_info *info)
{
    return server_load_events_fi_mode(srv, from_wall_ns, to_wall_ns, pid, 0,
                                      out_count, info);
}

/* Lifecycle context is sparse but can span a long capture. Retain only
 * structural markers so row extraction can find a pre-window EXEC_START
 * without making ordinary waits consume the raw-load event budget. */
static struct pgwt_trace_event *
server_load_markers_fi(struct pgwt_server *srv,
                       uint64_t from_wall_ns, uint64_t to_wall_ns,
                       uint32_t pid,
                       int *out_count, struct pgwt_load_info *info)
{
    return server_load_events_fi_mode(srv, from_wall_ns, to_wall_ns, pid, 1,
                                      out_count, info);
}

/* Free everything the server owns (cache, coverage, metadata maps) so
 * sanitizer/valgrind runs end clean. */
static void server_destroy(struct pgwt_server *srv)
{
    for (int i = 0; i < srv->cache_count; i++)
        free(srv->cache[i].events);
    srv->cache_count = 0;
    for (int i = 0; i < srv->cov_count; i++)
        cov_reset(&srv->cov[i]);
    srv->cov_count = 0;
    gens_free(srv);
    qt_map_clear(srv);
    free(srv->bm_map);
    srv->bm_map = NULL;
    free(srv->pid_cats);
    srv->pid_cats = NULL;
    srv->n_pid_cats = 0;
}

/* ── Server init ──────────────────────────────────────────── */

static int server_init(struct pgwt_server *srv, const char *trace_dir)
{
    memset(srv, 0, sizeof(*srv));
    snprintf(srv->trace_dir, sizeof(srv->trace_dir), "%s", trace_dir);

    srv->num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (srv->num_cpus <= 0)
        srv->num_cpus = 1;

    pgwt_init_event_names(18); /* Default to PG18 tables */

    /* Try loading dynamic event names from sidecar file.
     * Written by the daemon when it queries pg_wait_events. */
    if (pgwt_load_names_json(trace_dir) == 0)
        fprintf(stderr, "pgwt-server: loaded event names from wait_event_names.json\n");

    srv->num_files = pgwt_scan_trace_files(trace_dir, srv->files, 256);
    if (srv->num_files <= 0) {
        fprintf(stderr, "pgwt-server: no trace files found in %s\n", trace_dir);
        return -1;
    }

    /* Scan block headers, build the coverage table + clock generations,
     * and derive the (re-anchored) wall time range. */
    srv->earliest_wall_ns = 0;
    srv->latest_wall_ns   = 0;
    srv->total_events     = 0;
    coverage_refresh(srv);

    fprintf(stderr, "pgwt-server: %d trace files, %d CPUs, ~%lld events\n",
            srv->num_files, srv->num_cpus, (long long)srv->total_events);

    server_load_query_texts(srv);
    server_load_backends(srv);
    server_build_pid_cats(srv);

    return 0;
}

/* ── Summary threshold (coverage-aware — FID-2) ───────────── */

/* Use the summary fast path for ranges >= 120 s (instant response, ~300KB
 * peak memory) — but ONLY when the window is fully exact-covered (or
 * empty): summaries are fed by the exact (watchpoint) path alone, so over
 * a window with uncovered sampled data they would silently drop it — in
 * default tiered mode that made "last 15 minutes" return escalation
 * slivers (or nothing) labeled "exact". Such windows fall back to the raw
 * path, which merges samples with ASH math and labels honestly.
 * Force raw events when a pid filter is active — summaries don't have
 * per-PID event/class breakdown. query_id is handled via v2 per-query
 * data. *wf is always filled with the window's coverage-derived fidelity
 * so the summary-path response can carry an honest label. */
static int should_use_summaries(struct pgwt_server *srv,
                                struct pgwt_request *req,
                                struct pgwt_wfid *wf)
{
    coverage_refresh(srv);
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    *wf = window_fidelity(srv, from, to);

    if (req->filter.pid != 0)
        return 0;
    if (to <= from) return 0;
    if (to - from < 120ULL * 1000000000ULL)
        return 0;
    /* Summaries are only honest when exact data covers everything the
     * window contains (EXACT), or the window is empty (NONE). */
    return wf->fid == PGWT_FIDELITY_EXACT || wf->fid == PGWT_FIDELITY_NONE;
}

/* ── Fidelity (trace format v2, D3) ───────────────────────── */

/* Derive the window fidelity from a load and add it as a "fidelity" field
 * to a response. Every view carries this so the client (B5) can render the
 * exact/sampled/mixed distinction. */
static enum pgwt_fidelity load_fidelity(const struct pgwt_load_info *info)
{
    return pgwt_fidelity_of(info->has_transitions, info->has_samples);
}

static void add_fidelity(cJSON *root, const struct pgwt_load_info *info)
{
    enum pgwt_fidelity fid = load_fidelity(info);
    cJSON_AddStringToObject(root, "fidelity", pgwt_fidelity_str(fid));
    if (info->has_samples && info->sample_period_ns)
        cjson_add_uint64(root, "sample_period_ns", info->sample_period_ns);
}

/* Label for a summary-path response: derived from coverage, never
 * hardcoded (FID-2). The summary path is only taken for EXACT or NONE. */
static void add_fidelity_window(cJSON *root, const struct pgwt_wfid *wf)
{
    cJSON_AddStringToObject(root, "fidelity", pgwt_fidelity_str(wf->fid));
    if (wf->sample_period_ns &&
        (wf->fid == PGWT_FIDELITY_SAMPLED || wf->fid == PGWT_FIDELITY_MIXED))
        cjson_add_uint64(root, "sample_period_ns", wf->sample_period_ns);
}

/* Emit the structured "unavailable" response for an EXACT-required view over
 * a window with no transition coverage (sampled-only). NEVER a silent empty
 * result — the client renders an explicit "escalate to capture" state. */
static void emit_unavailable(uint64_t req_id, enum pgwt_fidelity fid)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req_id);
    cJSON_AddStringToObject(root, "unavailable", PGWT_UNAVAILABLE_MSG);
    cJSON_AddStringToObject(root, "code", PGWT_UNAVAILABLE_CODE);
    cJSON_AddStringToObject(root, "hint", PGWT_UNAVAILABLE_HINT);
    cJSON_AddStringToObject(root, "fidelity", pgwt_fidelity_str(fid));
    emit_json(root);
}

/* DUR-9: the raw-load hard bound fired — the loaded array is PARTIAL.
 * Rendering it would silently under-report; emit a structured error the
 * client can present ("window too large") instead. Frees the partial array
 * and returns 1 when the handler must stop. */
static int reject_overload(struct pgwt_server *srv,
                           const struct pgwt_request *req,
                           struct pgwt_trace_event *events,
                           const struct pgwt_load_info *info)
{
    if (!info->overloaded)
        return 0;
    free(events);
    /* Keep the refusal's fidelity label in the same freshly-refreshed
     * coverage generation as the load which detected the bound. */
    coverage_refresh(srv);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cJSON_AddStringToObject(root, "error", "window too large");
    cJSON_AddStringToObject(root, "code", "window_too_large");
    cJSON_AddNumberToObject(root, "max_events", load_max_events());
    cJSON_AddStringToObject(root, "hint",
        "narrow the time range or add a pid/query filter");
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to = req->to_ns ? req->to_ns : srv->latest_wall_ns;
    if (strcmp(req->cmd, "execution_detail") == 0 &&
        req->start_ns && req->end_ns) {
        from = req->start_ns;
        to = req->end_ns;
    }
    struct pgwt_wfid wf = window_fidelity(srv, from, to);
    add_fidelity_window(root, &wf);
    emit_json(root);
    return 1;
}

/* ── Command handlers ─────────────────────────────────────── */

static void handle_info(struct pgwt_server *srv, struct pgwt_request *req)
{
    /* Refresh: rescan directory + coverage (updates earliest/latest with
     * the re-anchored canonical offsets), reload metadata. */
    coverage_refresh(srv);

    /* Reload query texts and backend metadata (daemon appends new entries
     * as it discovers backends/queries). These files are small — fast. */
    qt_map_clear(srv);
    server_load_query_texts(srv);
    free(srv->bm_map);
    srv->bm_map = NULL;
    srv->bm_capacity = 0;
    srv->bm_count = 0;
    server_load_backends(srv);
    server_build_pid_cats(srv);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cjson_add_uint64(root, "from_ns", srv->earliest_wall_ns);
    cjson_add_uint64(root, "to_ns", srv->latest_wall_ns);
    cJSON_AddNumberToObject(root, "num_events", (double)srv->total_events);
    cJSON_AddNumberToObject(root, "num_cpus", srv->num_cpus);

    /* Version handshake (T7 / TST-11): the client compares these against its
     * own build version / protocol revision and warns loudly on skew (a
     * mismatched Mac-client/Linux-server pair is the normal deployment
     * state — the point is visibility, never refusal). */
    cJSON_AddStringToObject(root, "server_version", PGWT_BUILD_VERSION);
    cJSON_AddNumberToObject(root, "protocol", PGWT_PROTOCOL_REV);

    /* Server wall clock — client uses this as "now" for relative time ranges */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    cjson_add_uint64(root, "now_ns", now_ns);

    emit_json(root);
}

static void handle_aas(struct pgwt_server *srv, struct pgwt_request *req)
{
    int num_buckets = req->num_buckets > 0 ? req->num_buckets : 120;

    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;

    int detail_events = (strcmp(req->detail, "events") == 0);

    struct pgwt_aas_result aas;
    struct pgwt_load_info linfo = {0};
    struct pgwt_wfid wfid = {0};
    int from_summaries = 0;

    /* Event-detail mode always uses raw events (no summary path yet) */
    if (!detail_events && should_use_summaries(srv, req, &wfid)) {
        pgwt_compute_aas_from_summaries(srv->trace_dir, from, to,
                                         &req->filter, num_buckets, &aas);
        from_summaries = 1;
    } else {
        int count;
        struct pgwt_trace_event *events =
            server_load_events_fi(srv, req->from_ns, req->to_ns,
                                  req->filter.pid, &count, &linfo);
        if (reject_overload(srv, req, events, &linfo)) return;
        pgwt_compute_aas(events, count, &req->filter, from, to, num_buckets,
                         detail_events, AAS_MAX_EVENT_SERIES, &aas);
        free(events);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    if (from_summaries) add_fidelity_window(root, &wfid);
    else                add_fidelity(root, &linfo);

    if (aas.num_event_series > 0) {
        /* Event-breakdown mode */
        int ns = aas.num_event_series;
        cJSON_AddStringToObject(root, "breakdown", "events");

        cJSON *series = cJSON_AddArrayToObject(root, "series");
        for (int s = 0; s < ns; s++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "event_id", aas.event_series[s].event_id);
            cJSON_AddStringToObject(item, "name", aas.event_series[s].name);
            cJSON_AddItemToArray(series, item);
        }

        cJSON *buckets_arr = cJSON_AddArrayToObject(root, "buckets");
        for (int i = 0; i < aas.num_buckets; i++) {
            cJSON *b = cJSON_CreateObject();
            cjson_add_uint64(b, "t", aas.buckets[i].start_ns);
            cJSON *aas_arr = cJSON_AddArrayToObject(b, "aas");
            for (int s = 0; s < ns; s++)
                cJSON_AddItemToArray(aas_arr,
                    cJSON_CreateNumber(aas.event_aas[i * ns + s]));
            cJSON_AddItemToArray(buckets_arr, b);
        }

        free(aas.event_aas);
    } else {
        /* Class-breakdown mode (default) */
        cJSON *buckets_arr = cJSON_AddArrayToObject(root, "buckets");
        for (int i = 0; i < aas.num_buckets; i++) {
            cJSON *b = cJSON_CreateObject();
            cjson_add_uint64(b, "t", aas.buckets[i].start_ns);
            for (int c = 0; c < PGWT_NUM_CLASSES; c++)
                cJSON_AddNumberToObject(b, pgwt_class_names[c],
                                        aas.buckets[i].class_aas[c]);
            /* T8 (additive): measured off-CPU AAS, a stacked sibling of "cpu"
             * (the measured on-CPU share). 0 where no measured cpu_ns exists
             * (sampled/v2), so consumers can treat it as any other class key. */
            cJSON_AddNumberToObject(b, "offcpu", aas.buckets[i].offcpu_aas);
            /* T2 (additive): the same AAS decomposed by category.
             * io_worker appears ONLY here — excluded from the class AAS
             * above and from max_aas. Raw path only (summaries carry no
             * category data). */
            if (!from_summaries) {
                cJSON *cat = cJSON_AddObjectToObject(b, "cat");
                for (int c = 0; c < PGWT_NUM_CATS; c++)
                    cJSON_AddNumberToObject(cat, pgwt_cat_names[c],
                                            aas.buckets[i].cat_aas[c]);
            }
            cJSON_AddItemToArray(buckets_arr, b);
        }
    }

    cJSON_AddNumberToObject(root, "max_aas", aas.max_aas);
    cjson_add_uint64(root, "bucket_ns", aas.bucket_ns);
    emit_json(root);

    free(aas.buckets);
}

static void handle_time_model(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;

    struct pgwt_tm_result tm;
    struct pgwt_load_info linfo = {0};
    struct pgwt_wfid wfid = {0};
    int from_summaries = 0;

    if (should_use_summaries(srv, req, &wfid)) {
        pgwt_compute_time_model_from_summaries(srv->trace_dir, from, to,
                                                &req->filter, wall_ms, &tm);
        from_summaries = 1;
    } else {
        int count;
        struct pgwt_trace_event *events =
            server_load_events_fi(srv, req->from_ns, req->to_ns,
                                  req->filter.pid, &count, &linfo);
        if (reject_overload(srv, req, events, &linfo)) return;
        pgwt_compute_time_model(events, count, &req->filter, wall_ms, &tm);
        free(events);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    if (from_summaries) add_fidelity_window(root, &wfid);
    else                add_fidelity(root, &linfo);

    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < tm.num_rows; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "name", tm.rows[i].name);
        cJSON_AddNumberToObject(r, "ms", tm.rows[i].time_ms);
        cJSON_AddNumberToObject(r, "pct", tm.rows[i].pct_db_time);
        cJSON_AddNumberToObject(r, "aas", tm.rows[i].aas);
        cJSON_AddNumberToObject(r, "indent", tm.rows[i].indent);
        cJSON_AddItemToArray(rows, r);
    }

    cJSON_AddNumberToObject(root, "db_time_ms", tm.db_time_ms);
    cJSON_AddNumberToObject(root, "idle_time_ms", tm.idle_time_ms);
    cJSON_AddNumberToObject(root, "aas", tm.aas);
    cJSON_AddNumberToObject(root, "wall_ms", tm.wall_ms);

    /* T8 (additive): measured-CPU decomposition + self-checks. has_measured_cpu
     * gates whether Off-CPU* is present (v3 exact data) vs absent (sampled/v2).
     * wait_gap_cpu_ms is the trace's own CPU-accounting proof (CPU measured
     * during wait-labeled gaps — should be ≈0); cpu_clamped_ms counts CPU that
     * exceeded a gap's wall (clock skew). */
    cJSON_AddBoolToObject(root, "has_measured_cpu", tm.has_measured_cpu);
    cJSON_AddNumberToObject(root, "cpu_ms", tm.cpu_ms);
    cJSON_AddNumberToObject(root, "offcpu_ms", tm.offcpu_ms);
    cJSON_AddNumberToObject(root, "wait_gap_cpu_ms", tm.wait_gap_cpu_ms);
    cJSON_AddNumberToObject(root, "cpu_clamped_ms", tm.cpu_clamped_ms);

    /* T2 (additive): category decomposition + io_worker utilization. Raw
     * path only — summaries carry no category data. cat "io_worker" ms is
     * OUTSIDE DB Time (utilization, not load). */
    if (!from_summaries) {
        cJSON *cats = cJSON_AddArrayToObject(root, "categories");
        for (int c = 0; c < PGWT_NUM_CATS; c++) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "name", pgwt_cat_names[c]);
            cJSON_AddNumberToObject(r, "ms", tm.cat_ms[c]);
            cJSON_AddNumberToObject(r, "aas",
                                    tm.wall_ms > 0 ? tm.cat_ms[c] / tm.wall_ms
                                                   : 0.0);
            cJSON_AddItemToArray(cats, r);
        }
        cJSON_AddNumberToObject(root, "io_worker_busy_pct",
                                tm.io_worker_busy_pct);
    }
    emit_json(root);

    free(tm.rows);
}

static void handle_top_events(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;

    struct pgwt_events_result res;
    struct pgwt_load_info linfo = {0};
    struct pgwt_wfid wfid = {0};
    int from_summaries = 0;

    if (should_use_summaries(srv, req, &wfid)) {
        pgwt_compute_top_events_from_summaries(srv->trace_dir, from, to,
                                                &req->filter, wall_ms, &res);
        from_summaries = 1;
    } else {
        int count;
        struct pgwt_trace_event *events =
            server_load_events_fi(srv, req->from_ns, req->to_ns,
                                  req->filter.pid, &count, &linfo);
        if (reject_overload(srv, req, events, &linfo)) return;
        pgwt_compute_top_events(events, count, &req->filter, wall_ms, &res);
        free(events);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    if (from_summaries) add_fidelity_window(root, &wfid);
    else                add_fidelity(root, &linfo);

    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < res.num_rows; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "event_id", res.rows[i].event_id);
        cJSON_AddStringToObject(r, "name", res.rows[i].name);
        cJSON_AddStringToObject(r, "class",
                                pgwt_class_name(res.rows[i].event_id));
        cjson_add_uint64(r, "count", res.rows[i].count);
        cJSON_AddNumberToObject(r, "total_ms", res.rows[i].total_ms);
        /* Latency statistics exist only where exact records contributed:
         * over sampled data they would be fabrications (p95 ≈ sample
         * period), so sampled-only rows carry null and render as "—"
         * (FID-3). count/total_ms stay valid via ASH math. */
        if (res.rows[i].exact_count > 0) {
            cJSON_AddNumberToObject(r, "avg_us", res.rows[i].avg_us);
            cJSON_AddNumberToObject(r, "p50_us", res.rows[i].p50_us);
            cJSON_AddNumberToObject(r, "p95_us", res.rows[i].p95_us);
            cJSON_AddNumberToObject(r, "p99_us", res.rows[i].p99_us);
            cJSON_AddNumberToObject(r, "max_us", res.rows[i].max_us);
        } else {
            cJSON_AddNullToObject(r, "avg_us");
            cJSON_AddNullToObject(r, "p50_us");
            cJSON_AddNullToObject(r, "p95_us");
            cJSON_AddNullToObject(r, "p99_us");
            cJSON_AddNullToObject(r, "max_us");
        }
        /* Idle-but-visible events (Client:ClientRead) have no meaningful
         * share of DB Time; emit null so the client renders "—". */
        if (PGWT_PCT_DB_IS_IDLE(res.rows[i].pct_db))
            cJSON_AddNullToObject(r, "pct");
        else
            cJSON_AddNumberToObject(r, "pct", res.rows[i].pct_db);
        cJSON_AddNumberToObject(r, "aas", res.rows[i].aas);
        cJSON_AddItemToArray(rows, r);
    }

    cJSON_AddNumberToObject(root, "db_time_ms", res.db_time_ms);
    emit_json(root);

    free(res.rows);
}

static void handle_top_sessions(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;

    struct pgwt_sessions_result res;
    struct pgwt_load_info linfo = {0};
    struct pgwt_wfid wfid = {0};
    int from_summaries = 0;

    /* Sessions + query_id: summaries lack per-session query data, use raw events */
    if (should_use_summaries(srv, req, &wfid) && req->filter.query_id == 0) {
        pgwt_compute_top_sessions_from_summaries(srv->trace_dir, from, to,
                                                  &req->filter, wall_ms, &res);
        from_summaries = 1;
    } else {
        int count;
        struct pgwt_trace_event *events =
            server_load_events_fi(srv, req->from_ns, req->to_ns,
                                  req->filter.pid, &count, &linfo);
        if (reject_overload(srv, req, events, &linfo)) return;
        pgwt_compute_top_sessions(events, count, &req->filter, wall_ms, &res);
        free(events);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    if (from_summaries) add_fidelity_window(root, &wfid);
    else                add_fidelity(root, &linfo);

    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < res.num_rows; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "pid", res.rows[i].pid);
        cJSON_AddNumberToObject(r, "db_time_ms", res.rows[i].db_time_ms);
        cJSON_AddNumberToObject(r, "cpu_pct", res.rows[i].cpu_pct);
        cJSON_AddNumberToObject(r, "wait_pct", res.rows[i].wait_pct);
        cJSON_AddStringToObject(r, "top_wait", res.rows[i].top_wait);
        cJSON_AddNumberToObject(r, "top_wait_id", res.rows[i].top_wait_id);
        const struct bm_entry *bm = bm_map_lookup(srv, res.rows[i].pid);
        if (bm) {
            cJSON_AddStringToObject(r, "type", bm->type);
            if (bm->user[0]) cJSON_AddStringToObject(r, "user", bm->user);
            if (bm->db[0]) cJSON_AddStringToObject(r, "db", bm->db);
        }
        cJSON_AddItemToArray(rows, r);
    }

    emit_json(root);

    free(res.rows);
}

static void handle_top_queries(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;

    /* Force raw events when class/event filter active (need per-event breakdown) */
    int has_event_filter = (req->filter.class_name[0] != '\0' ||
                            req->filter.event_id != 0);

    /* Load events once — used for both top_queries and lifecycle stats */
    int ecount = 0;
    struct pgwt_trace_event *all_events = NULL;
    struct pgwt_queries_result res;
    struct pgwt_load_info linfo = {0};
    struct pgwt_wfid wfid = {0};
    int from_summaries = 0;

    /* Use summaries for large ranges (>120s) for the class breakdown.
     * Always load raw events for lifecycle stats (exec/plan counts).
     * Large files are read on-demand (not cached) so this is safe.
     * No pid pushdown: the lifecycle stats below read exec/plan markers
     * across ALL pids (markers never pass the uniform filter). */
    all_events = server_load_events_fi(srv, req->from_ns, req->to_ns, 0,
                                       &ecount, &linfo);
    if (reject_overload(srv, req, all_events, &linfo)) return;
    if (!has_event_filter && should_use_summaries(srv, req, &wfid)) {
        pgwt_compute_top_queries_from_summaries(srv->trace_dir, from, to,
                                                 &req->filter, wall_ms, &res);
        from_summaries = 1;
    } else {
        pgwt_compute_top_queries(all_events, ecount, &req->filter, wall_ms, &res);
    }

    /* Compute per-query exec/plan stats from markers (same events, no second load) */
    struct qid_lifecycle {
        uint64_t query_id;
        int used;
        int exec_count, plan_count;
        double exec_total_ms, plan_total_ms;
        double *exec_times, *plan_times;
        int exec_nsamples, plan_nsamples;  /* actual entries written */
        int exec_cap, plan_cap;
    };
    #define QLC_HT_SIZE 1024
    #define QLC_HT_MASK (QLC_HT_SIZE - 1)
    struct qid_lifecycle *qlc = calloc(QLC_HT_SIZE, sizeof(*qlc));

    if (qlc && all_events) {
        struct { uint32_t pid; uint64_t exec_start_ns, plan_start_ns; uint64_t qid; }
            pid_st[512];
        int npids = 0;

        for (int i = 0; i < ecount; i++) {
            const struct pgwt_trace_event *ev = &all_events[i];
            uint32_t m = ev->old_event;
            if (!PGWT_IS_MARKER(m)) continue;

            int pi = -1;
            for (int j = 0; j < npids; j++)
                if (pid_st[j].pid == ev->pid) { pi = j; break; }
            if (pi < 0 && npids < 512) {
                pi = npids++;
                memset(&pid_st[pi], 0, sizeof(pid_st[0]));
                pid_st[pi].pid = ev->pid;
            }
            if (pi < 0) continue;

            if (m == PGWT_MARKER_EXEC_START) {
                pid_st[pi].exec_start_ns = ev->timestamp_ns;
                pid_st[pi].qid = ev->query_id;
            } else if (m == PGWT_MARKER_EXEC_END && pid_st[pi].exec_start_ns) {
                double ms = (ev->timestamp_ns - pid_st[pi].exec_start_ns) / 1e6;
                uint64_t qid = pid_st[pi].qid;
                pid_st[pi].exec_start_ns = 0;
                if (qid == 0) continue;
                uint32_t h = (uint32_t)((qid * 0x9e3779b9ULL) & QLC_HT_MASK);
                while (qlc[h].used && qlc[h].query_id != qid) h = (h + 1) & QLC_HT_MASK;
                if (!qlc[h].used) { qlc[h].used = 1; qlc[h].query_id = qid; }
                qlc[h].exec_count++;
                qlc[h].exec_total_ms += ms;
                if (ms >= 0 && qlc[h].exec_nsamples < 10000) {
                    if (qlc[h].exec_nsamples >= qlc[h].exec_cap) {
                        int nc = qlc[h].exec_cap ? qlc[h].exec_cap * 2 : 64;
                        double *t = realloc(qlc[h].exec_times, nc * sizeof(double));
                        if (t) { qlc[h].exec_times = t; qlc[h].exec_cap = nc; }
                    }
                    if (qlc[h].exec_times && qlc[h].exec_nsamples < qlc[h].exec_cap)
                        qlc[h].exec_times[qlc[h].exec_nsamples++] = ms;
                }
            } else if (m == PGWT_MARKER_PLAN_START) {
                pid_st[pi].plan_start_ns = ev->timestamp_ns;
                pid_st[pi].qid = ev->query_id;
            } else if (m == PGWT_MARKER_PLAN_END && pid_st[pi].plan_start_ns) {
                double ms = (ev->timestamp_ns - pid_st[pi].plan_start_ns) / 1e6;
                uint64_t qid = pid_st[pi].qid;
                pid_st[pi].plan_start_ns = 0;
                if (qid == 0) continue;
                uint32_t h = (uint32_t)((qid * 0x9e3779b9ULL) & QLC_HT_MASK);
                while (qlc[h].used && qlc[h].query_id != qid) h = (h + 1) & QLC_HT_MASK;
                if (!qlc[h].used) { qlc[h].used = 1; qlc[h].query_id = qid; }
                qlc[h].plan_count++;
                qlc[h].plan_total_ms += ms;
                if (ms >= 0 && qlc[h].plan_nsamples < 10000) {
                    if (qlc[h].plan_nsamples >= qlc[h].plan_cap) {
                        int nc = qlc[h].plan_cap ? qlc[h].plan_cap * 2 : 64;
                        double *t = realloc(qlc[h].plan_times, nc * sizeof(double));
                        if (t) { qlc[h].plan_times = t; qlc[h].plan_cap = nc; }
                    }
                    if (qlc[h].plan_times && qlc[h].plan_nsamples < qlc[h].plan_cap)
                        qlc[h].plan_times[qlc[h].plan_nsamples++] = ms;
                }
            }
        }
        /* events freed below with all_events */
    }

    /* Helper: compute percentile from sorted array */
    #define PERCENTILE(arr, n, pct) ((n) > 0 ? (arr)[(int)((n) * (pct))] : 0)

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    if (from_summaries) add_fidelity_window(root, &wfid);
    else                add_fidelity(root, &linfo);

    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < res.num_rows; i++) {
        cJSON *r = cJSON_CreateObject();

        /* query_id as string to avoid JS precision loss */
        char qid_str[32];
        snprintf(qid_str, sizeof(qid_str), "%lld",
                 (long long)res.rows[i].query_id);
        cJSON_AddStringToObject(r, "query_id", qid_str);

        cjson_add_uint64(r, "count", res.rows[i].count);
        cJSON_AddNumberToObject(r, "total_ms", res.rows[i].total_ms);
        cJSON_AddNumberToObject(r, "avg_us", res.rows[i].avg_us);
        cJSON_AddNumberToObject(r, "pct", res.rows[i].pct_db);
        cJSON_AddStringToObject(r, "top_wait", res.rows[i].top_wait);
        cJSON_AddNumberToObject(r, "top_wait_id", res.rows[i].top_wait_id);

        /* Override top_wait if CPU dominates (summary path doesn't track CPU as a wait) */
        if (res.rows[i].class_ms[PGWT_CLASS_CPU] > 0) {
            double cpu_ms = res.rows[i].class_ms[PGWT_CLASS_CPU];
            /* Check if CPU is the largest class */
            int cpu_is_top = 1;
            for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
                if (c != PGWT_CLASS_CPU && res.rows[i].class_ms[c] > cpu_ms) {
                    cpu_is_top = 0;
                    break;
                }
            }
            if (cpu_is_top) {
                snprintf(res.rows[i].top_wait, sizeof(res.rows[i].top_wait), "CPU*");
                res.rows[i].top_wait_id = 0;
            }
        }

        /* Add query text if available */
        const char *qt = qt_map_lookup(srv, res.rows[i].query_id);
        if (qt)
            cJSON_AddStringToObject(r, "text", qt);

        /* Per-class time breakdown */
        cJSON *classes = cJSON_AddArrayToObject(r, "classes");
        for (int c = 0; c < 11; c++)
            cJSON_AddItemToArray(classes,
                cJSON_CreateNumber(res.rows[i].class_ms[c]));

        /* Per-event breakdown (when available from raw events path) */
        if (res.rows[i].num_events > 0) {
            char ename[64];
            cJSON *events_arr = cJSON_AddArrayToObject(r, "events");
            for (int e = 0; e < res.rows[i].num_events; e++) {
                uint32_t eid = res.rows[i].event_ids[e];
                if (eid == 0)
                    snprintf(ename, sizeof(ename), "CPU");
                else
                    pgwt_event_full_name(eid, ename, sizeof(ename));
                cJSON *ev = cJSON_CreateObject();
                cJSON_AddNumberToObject(ev, "id", eid);
                cJSON_AddStringToObject(ev, "name", ename);
                cJSON_AddNumberToObject(ev, "ms", res.rows[i].event_ms[e]);
                cJSON_AddItemToArray(events_arr, ev);
            }
        }

        /* Exec/plan lifecycle stats from markers */
        if (qlc) {
            uint64_t qid = res.rows[i].query_id;
            uint32_t h = (uint32_t)((qid * 0x9e3779b9ULL) & QLC_HT_MASK);
            while (qlc[h].used && qlc[h].query_id != qid) h = (h + 1) & QLC_HT_MASK;
            if (qlc[h].used && qlc[h].query_id == qid) {
                struct qid_lifecycle *lc = &qlc[h];
                cJSON_AddNumberToObject(r, "exec_count", lc->exec_count);
                cJSON_AddNumberToObject(r, "plan_count", lc->plan_count);
                if (lc->exec_count > 0) {
                    cJSON_AddNumberToObject(r, "exec_total_ms", lc->exec_total_ms);
                    cJSON_AddNumberToObject(r, "avg_exec_ms",
                        lc->exec_total_ms / lc->exec_count);
                    int n = lc->exec_nsamples;
                    if (lc->exec_times && n > 1) {
                        for (int a = 0; a < n-1; a++)
                            for (int b = a+1; b < n; b++)
                                if (lc->exec_times[a] > lc->exec_times[b]) {
                                    double tmp = lc->exec_times[a];
                                    lc->exec_times[a] = lc->exec_times[b];
                                    lc->exec_times[b] = tmp;
                                }
                        cJSON_AddNumberToObject(r, "p95_exec_ms",
                            lc->exec_times[n > 20 ? (int)((n-1)*0.95) : n-1]);
                        cJSON_AddNumberToObject(r, "p99_exec_ms",
                            lc->exec_times[n > 100 ? (int)((n-1)*0.99) : n-1]);
                    }
                }
                if (lc->plan_count > 0) {
                    cJSON_AddNumberToObject(r, "avg_plan_ms",
                        lc->plan_total_ms / lc->plan_count);
                    int n = lc->plan_nsamples;
                    if (lc->plan_times && n > 1) {
                        for (int a = 0; a < n-1; a++)
                            for (int b = a+1; b < n; b++)
                                if (lc->plan_times[a] > lc->plan_times[b]) {
                                    double tmp = lc->plan_times[a];
                                    lc->plan_times[a] = lc->plan_times[b];
                                    lc->plan_times[b] = tmp;
                                }
                        cJSON_AddNumberToObject(r, "p95_plan_ms",
                            lc->plan_times[n > 20 ? (int)((n-1)*0.95) : n-1]);
                        cJSON_AddNumberToObject(r, "p99_plan_ms",
                            lc->plan_times[n > 100 ? (int)((n-1)*0.99) : n-1]);
                    }
                }
            }
        }

        cJSON_AddItemToArray(rows, r);
    }

    cJSON_AddNumberToObject(root, "db_time_ms", res.db_time_ms);
    emit_json(root);

    if (qlc) {
        for (int i = 0; i < QLC_HT_SIZE; i++) {
            free(qlc[i].exec_times);
            free(qlc[i].plan_times);
        }
        free(qlc);
    }
    free(all_events);
    free(res.rows);
}

static void handle_heatmap(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    int num_buckets = req->num_buckets > 0 ? req->num_buckets : 200;

    struct pgwt_heatmap_result res;
    struct pgwt_wfid wfid = {0};
    int from_summaries = 0;
    enum pgwt_fidelity out_fid = PGWT_FIDELITY_EXACT;

    /* Heatmap (latency distribution) is EXACT-required: it needs real
     * per-event durations, which samples do not carry (the compute skips
     * sample records, so mixed windows show exact cells only). */
    if (should_use_summaries(srv, req, &wfid) && req->filter.query_id == 0) {
        pgwt_compute_heatmap_from_summaries(srv->trace_dir, from, to,
                                             &req->filter, num_buckets, &res);
        from_summaries = 1;
        out_fid = wfid.fid;
    } else {
        int count;
        struct pgwt_load_info linfo = {0};
        struct pgwt_trace_event *events =
            server_load_events_fi(srv, req->from_ns, req->to_ns,
                                  req->filter.pid, &count, &linfo);
        if (reject_overload(srv, req, events, &linfo)) return;
        enum pgwt_fidelity fid = load_fidelity(&linfo);
        if (pgwt_fidelity_unavailable(PGWT_REQ_EXACT, fid)) {
            free(events);
            emit_unavailable(req->id, fid);
            return;
        }
        pgwt_compute_heatmap(events, count, &req->filter,
                             from, to, num_buckets, &res);
        free(events);
        out_fid = fid;
    }

    /* Latency bucket labels (UTF-8: µ = \xc2\xb5, ≥ = \xe2\x89\xa5) */
    static const char *labels[HISTOGRAM_BUCKETS] = {
        "<1\xc2\xb5s", "1-2\xc2\xb5s", "2-4\xc2\xb5s", "4-8\xc2\xb5s",
        "8-16\xc2\xb5s", "16-32\xc2\xb5s", "32-64\xc2\xb5s", "64-128\xc2\xb5s",
        "128-256\xc2\xb5s", "256-512\xc2\xb5s", "0.5-1ms", "1-2ms",
        "2-4ms", "4-8ms", "8-16ms", "\xe2\x89\xa5" "16ms"
    };

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cJSON_AddStringToObject(root, "fidelity", pgwt_fidelity_str(out_fid));
    (void)from_summaries;

    cJSON *times_arr = cJSON_AddArrayToObject(root, "times");
    for (int i = 0; i < res.num_buckets; i++)
        cJSON_AddItemToArray(times_arr, cjson_create_uint64(res.times[i]));

    cJSON *labels_arr = cJSON_AddArrayToObject(root, "labels");
    for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
        cJSON_AddItemToArray(labels_arr, cJSON_CreateString(labels[i]));

    /* Sparse cells: only emit non-zero [time_idx, latency_idx, count] */
    cJSON *cells_arr = cJSON_AddArrayToObject(root, "cells");
    for (int t = 0; t < res.num_buckets; t++) {
        for (int l = 0; l < HISTOGRAM_BUCKETS; l++) {
            uint64_t v = res.grid[t * HISTOGRAM_BUCKETS + l];
            if (v == 0) continue;
            cJSON *cell = cJSON_CreateArray();
            cJSON_AddItemToArray(cell, cJSON_CreateNumber(t));
            cJSON_AddItemToArray(cell, cJSON_CreateNumber(l));
            cJSON_AddItemToArray(cell, cjson_create_uint64(v));
            cJSON_AddItemToArray(cells_arr, cell);
        }
    }

    cjson_add_uint64(root, "max_count", res.max_count);
    cjson_add_uint64(root, "total_events", res.total_events);
    cjson_add_uint64(root, "bucket_ns", res.bucket_ns);
    emit_json(root);

    free(res.grid);
    free(res.times);
}

/* ── Session Timeline ─────────────────────────────────────── */

#define TIMELINE_MAX_EVENTS 5000
#define TIMELINE_MAX_PIDS   256

static void handle_session_timeline(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_load_info linfo = {0};
    struct pgwt_trace_event *events =
        server_load_events_fi(srv, req->from_ns, req->to_ns,
                              req->filter.pid, &count, &linfo);
    if (reject_overload(srv, req, events, &linfo)) return;

    /* First pass: count matching events and collect unique PIDs */
    uint32_t pids[TIMELINE_MAX_PIDS];
    int num_pids = 0;
    int total_matching = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(&req->filter, ev))
            continue;
        /* Timeline is a visibility view: keep Client:ClientRead bars. */
        if (pgwt_is_hidden_event(ev->old_event))
            continue;
        total_matching++;

        /* Collect unique PIDs (linear scan is fine for ≤256 PIDs) */
        int found = 0;
        for (int p = 0; p < num_pids; p++) {
            if (pids[p] == ev->pid) { found = 1; break; }
        }
        if (!found && num_pids < TIMELINE_MAX_PIDS)
            pids[num_pids++] = ev->pid;
    }

    /* Compute coalesce gap: if too many events, merge adjacent same-class
       events within same PID when gap between them is < coalesce_ns.
       This reduces bar count while preserving the visual pattern. */
    uint64_t range_ns = (req->to_ns && req->from_ns) ?
                        (req->to_ns - req->from_ns) : 0;
    if (range_ns == 0 && count > 0) {
        /* Estimate range from actual events */
        uint64_t min_ts = UINT64_MAX, max_ts = 0;
        for (int i = 0; i < count; i++) {
            if (events[i].timestamp_ns < min_ts) min_ts = events[i].timestamp_ns;
            uint64_t end = events[i].timestamp_ns + events[i].duration_ns;
            if (end > max_ts) max_ts = end;
        }
        range_ns = max_ts - min_ts;
    }
    /* Coalesce gap = range / (2 * max_events), so we get at most ~2*max visible bars */
    uint64_t coalesce_ns = total_matching > TIMELINE_MAX_EVENTS ?
                           range_ns / (TIMELINE_MAX_EVENTS * 2) : 0;

    /* Build JSON response */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    add_fidelity(root, &linfo);

    cJSON *pids_arr = cJSON_AddArrayToObject(root, "pids");
    for (int p = 0; p < num_pids; p++)
        cJSON_AddItemToArray(pids_arr, cJSON_CreateNumber(pids[p]));

    cJSON *events_arr = cJSON_AddArrayToObject(root, "events");

    /* Second pass: emit events with coalescing */
    struct {
        uint32_t pid;
        uint64_t start_ns;
        uint64_t end_ns;
        uint32_t event_id;
        int      class_idx;
        uint64_t query_id;
        int      active;
    } pending[TIMELINE_MAX_PIDS];
    memset(pending, 0, sizeof(pending));

    int nr = 0;

    /* Flush a pending bar to the events array */
    #define FLUSH_PENDING(pidx) do { \
        if (pending[pidx].active && nr < TIMELINE_MAX_EVENTS) { \
            char _ename[64]; \
            if (pending[pidx].event_id == 0) \
                snprintf(_ename, sizeof(_ename), "CPU"); \
            else \
                pgwt_event_full_name(pending[pidx].event_id, _ename, sizeof(_ename)); \
            cJSON *_ev = cJSON_CreateObject(); \
            cjson_add_uint64(_ev, "s", pending[pidx].start_ns); \
            cjson_add_uint64(_ev, "d", pending[pidx].end_ns - pending[pidx].start_ns); \
            cJSON_AddNumberToObject(_ev, "e", pending[pidx].event_id); \
            cJSON_AddStringToObject(_ev, "n", _ename); \
            cJSON_AddNumberToObject(_ev, "c", pending[pidx].class_idx); \
            cJSON_AddNumberToObject(_ev, "p", pending[pidx].pid); \
            char _qbuf[32]; \
            snprintf(_qbuf, sizeof(_qbuf), "%llu", (unsigned long long)pending[pidx].query_id); \
            cJSON_AddStringToObject(_ev, "q", _qbuf); \
            cJSON_AddItemToArray(events_arr, _ev); \
            nr++; \
            pending[pidx].active = 0; \
        } \
    } while(0)

    /* Build PID→index hash table for O(1) lookup in second pass */
    int pid_idx_ht[1024];  /* hash table: slot → pids[] index, -1 = empty */
    memset(pid_idx_ht, -1, sizeof(pid_idx_ht));
    for (int p = 0; p < num_pids; p++) {
        uint32_t slot = (pids[p] * 0x45d9f3b) & 1023;
        while (pid_idx_ht[slot] >= 0)
            slot = (slot + 1) & 1023;
        pid_idx_ht[slot] = p;
    }

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(&req->filter, ev))
            continue;
        /* Timeline is a visibility view: keep Client:ClientRead bars. */
        if (pgwt_is_hidden_event(ev->old_event))
            continue;

        /* O(1) PID index lookup */
        int pidx = -1;
        {
            uint32_t slot = (ev->pid * 0x45d9f3b) & 1023;
            while (pid_idx_ht[slot] >= 0) {
                if (pids[pid_idx_ht[slot]] == ev->pid) {
                    pidx = pid_idx_ht[slot];
                    break;
                }
                slot = (slot + 1) & 1023;
            }
        }
        if (pidx < 0) continue;

        int cls = pgwt_wait_class_index(ev->old_event);

        if (pending[pidx].active) {
            /* Can we merge? Same class + gap within threshold */
            uint64_t ev_start = ev->timestamp_ns - ev->duration_ns;
            uint64_t gap = (ev_start > pending[pidx].end_ns) ?
                           (ev_start - pending[pidx].end_ns) : 0;
            if (pending[pidx].class_idx == cls && gap <= coalesce_ns) {
                /* Extend the pending bar */
                if (ev->timestamp_ns > pending[pidx].end_ns)
                    pending[pidx].end_ns = ev->timestamp_ns;
                continue;
            }
            /* Different class or too big a gap — flush */
            FLUSH_PENDING(pidx);
            if (nr >= TIMELINE_MAX_EVENTS) break;
        }

        /* Start new pending bar */
        pending[pidx].active = 1;
        pending[pidx].pid = ev->pid;
        pending[pidx].start_ns = ev->timestamp_ns - ev->duration_ns;
        pending[pidx].end_ns = ev->timestamp_ns;
        pending[pidx].event_id = ev->old_event;
        pending[pidx].class_idx = cls;
        pending[pidx].query_id = ev->query_id;
    }

    /* Flush remaining */
    for (int p = 0; p < num_pids && nr < TIMELINE_MAX_EVENTS; p++)
        FLUSH_PENDING(p);

    #undef FLUSH_PENDING

    cJSON_AddBoolToObject(root, "truncated", nr < total_matching);
    cJSON_AddNumberToObject(root, "total_count", total_matching);
    emit_json(root);

    free(events);
}

/* ── Execution waterfall / scatter (U3/B6) ─────────────────────── */

#define EXECUTIONS_DEFAULT_LIMIT 100
#define EXECUTIONS_MAX_LIMIT     500
#define EXEC_DETAIL_LANE_CAP     2000
#define EXEC_SCATTER_DEFAULT     2000
#define EXEC_SCATTER_MAX         5000
#define EXEC_PLAN_LOOKBACK_NS    (15ULL * 60ULL * 1000000000ULL)

static int cmp_execution_start_desc(const void *a, const void *b)
{
    const struct pgwt_execution *ea = a, *eb = b;
    if (eb->start_ns > ea->start_ns) return 1;
    if (eb->start_ns < ea->start_ns) return -1;
    return (eb->pid > ea->pid) - (eb->pid < ea->pid);
}

static int cmp_execution_start_asc(const void *a, const void *b)
{
    const struct pgwt_execution *ea = a, *eb = b;
    if (ea->start_ns > eb->start_ns) return 1;
    if (ea->start_ns < eb->start_ns) return -1;
    return (ea->pid > eb->pid) - (ea->pid < eb->pid);
}

static int execution_matches_request(const struct pgwt_execution *row,
                                     const struct pgwt_request *req)
{
    if (req->filter.pid && row->pid != req->filter.pid)
        return 0;
    if (req->filter.query_id && row->query_id != req->filter.query_id)
        return 0;
    if ((req->filter.class_name[0] || req->filter.event_id) &&
        !row->matches_event_filter)
        return 0;
    return 1;
}

static void serialize_execution_row(cJSON *rows,
                                    const struct pgwt_execution *row)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "pid", row->pid);
    cjson_add_query_id_string(r, "query_id", row->query_id);
    cjson_add_uint64_string(r, "start_ns", row->start_ns);
    if (row->in_progress) {
        cJSON_AddNullToObject(r, "end_ns");
        cJSON_AddNullToObject(r, "duration_ms");
    } else {
        cjson_add_uint64_string(r, "end_ns", row->end_ns);
        cJSON_AddNumberToObject(r, "duration_ms",
            (double)(row->end_ns - row->start_ns) / 1e6);
    }
    if (row->has_plan)
        cJSON_AddNumberToObject(r, "plan_ms",
            (double)(row->plan_end_ns - row->plan_start_ns) / 1e6);
    else
        cJSON_AddNullToObject(r, "plan_ms");
    cJSON_AddNumberToObject(r, "n_events", row->n_events);
    cJSON_AddNumberToObject(r, "n_workers", row->n_workers);
    cJSON_AddBoolToObject(r, "in_progress", row->in_progress);
    cJSON_AddBoolToObject(r, "started_before_window",
                          row->started_before_window);
    cJSON_AddItemToArray(rows, r);
}

static void emit_execution_compute_failed(const struct pgwt_request *req,
                                          const struct pgwt_load_info *info)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cJSON_AddStringToObject(root, "error", "execution compute failed");
    cJSON_AddStringToObject(root, "code", "compute_failed");
    cJSON_AddStringToObject(root, "hint", "narrow the time range and retry");
    add_fidelity(root, info);
    emit_json(root);
}

static int load_execution_rows(struct pgwt_server *srv,
                               struct pgwt_request *req,
                               struct pgwt_executions_result *res,
                               struct pgwt_load_info *linfo)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to = req->to_ns ? req->to_ns : srv->latest_wall_ns;
    int window_count;
    struct pgwt_trace_event *window_events = server_load_events_fi(
        srv, from, to, 0, &window_count, linfo);
    if (reject_overload(srv, req, window_events, linfo))
        return -1;

    enum pgwt_fidelity fid = load_fidelity(linfo);
    if (pgwt_fidelity_unavailable(PGWT_REQ_EXACT, fid)) {
        free(window_events);
        emit_unavailable(req->id, fid);
        return -1;
    }

    /* A window can begin in the middle of an execution. Read the earlier
     * lifecycle stream as markers only (optionally with leader-pid pushdown),
     * then append the requested window's full events. This preserves the real
     * EXEC_START without letting historical waits consume the load budget. */
    int context_count = 0;
    struct pgwt_load_info context_info = {0};
    struct pgwt_trace_event *events = NULL;
    if (from > srv->earliest_wall_ns && from > 0) {
        events = server_load_markers_fi(srv, srv->earliest_wall_ns, from - 1,
                                        req->filter.pid, &context_count,
                                        &context_info);
        if (reject_overload(srv, req, events, &context_info)) {
            free(window_events);
            return -1;
        }
    }
    int count = context_count + window_count;
    struct pgwt_trace_event *combined = realloc(
        events, (count > 0 ? count : 1) * sizeof(*combined));
    if (!combined) {
        free(events);
        free(window_events);
        emit_execution_compute_failed(req, linfo);
        return -1;
    }
    events = combined;
    if (window_count > 0)
        memcpy(events + context_count, window_events,
               window_count * sizeof(*window_events));
    free(window_events);

    int n_links = 0;
    struct pgwt_backend_link *links = server_backend_links(srv, &n_links);
    if (n_links < 0) {
        free(events);
        emit_execution_compute_failed(req, linfo);
        return -1;
    }
    pgwt_compute_executions(events, count, from, to, &req->filter,
                            links, n_links, res);
    free(links);
    free(events);
    if (res->failed) {
        emit_execution_compute_failed(req, linfo);
        return -1;
    }
    return 0;
}

static void handle_executions(struct pgwt_server *srv,
                              struct pgwt_request *req)
{
    struct pgwt_load_info linfo = {0};
    struct pgwt_executions_result res;
    if (load_execution_rows(srv, req, &res, &linfo) != 0)
        return;

    int kept = 0;
    for (int i = 0; i < res.num_rows; i++)
        if (execution_matches_request(&res.rows[i], req))
            res.rows[kept++] = res.rows[i];
    res.num_rows = kept;
    qsort(res.rows, res.num_rows, sizeof(res.rows[0]),
          cmp_execution_start_desc);

    int limit = req->limit > 0 ? req->limit : EXECUTIONS_DEFAULT_LIMIT;
    if (limit > EXECUTIONS_MAX_LIMIT) limit = EXECUTIONS_MAX_LIMIT;
    int returned = res.num_rows < limit ? res.num_rows : limit;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    add_fidelity(root, &linfo);
    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < returned; i++)
        serialize_execution_row(rows, &res.rows[i]);
    cJSON_AddNumberToObject(root, "total_count", res.num_rows);
    cJSON_AddBoolToObject(root, "truncated", returned < res.num_rows);
    emit_json(root);
    free(res.rows);
}

static cJSON *serialize_exec_lane(const struct pgwt_exec_lane *lane,
                                  uint64_t query_id, int is_leader)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "pid", lane->pid);
    if (is_leader)
        cjson_add_query_id_string(obj, "query_id", query_id);
    cJSON *arr = cJSON_AddArrayToObject(obj, "events");
    for (int i = 0; i < lane->num_events; i++) {
        const struct pgwt_exec_event *ev = &lane->events[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "we", ev->event_id);
        cJSON_AddStringToObject(item, "name", ev->name);
        cjson_add_uint64_string(item, "start_ns", ev->start_ns);
        cjson_add_uint64_string(item, "dur_ns", ev->duration_ns);
        if (ev->has_cpu)
            cjson_add_uint64_string(item, "cpu_ns", ev->cpu_ns);
        else
            cJSON_AddNullToObject(item, "cpu_ns");
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddNumberToObject(obj, "total_count", lane->total_events);
    cJSON_AddBoolToObject(obj, "truncated",
                          lane->num_events < lane->total_events);
    return obj;
}

static int cmp_exec_lane_pid(const void *a, const void *b)
{
    const struct pgwt_exec_lane *la = a, *lb = b;
    return (la->pid > lb->pid) - (la->pid < lb->pid);
}

static void emit_invalid_execution_detail(struct pgwt_server *srv,
                                          const struct pgwt_request *req)
{
    coverage_refresh(srv);
    struct pgwt_wfid wf = window_fidelity(srv, req->start_ns, req->end_ns);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cJSON_AddStringToObject(root, "error",
                            "pid, start_ns, and end_ns are required");
    cJSON_AddStringToObject(root, "code", "invalid_request");
    cJSON_AddStringToObject(root, "hint",
                            "send pid and decimal-string start_ns/end_ns");
    add_fidelity_window(root, &wf);
    emit_json(root);
}

static void emit_execution_not_found(const struct pgwt_request *req,
                                     const struct pgwt_load_info *info)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cJSON_AddStringToObject(root, "error",
                            "execution start marker not found");
    cJSON_AddStringToObject(root, "code", "not_found");
    cJSON_AddStringToObject(root, "hint",
                            "select a marker-identified execution and retry");
    add_fidelity(root, info);
    emit_json(root);
}

static void handle_execution_detail(struct pgwt_server *srv,
                                    struct pgwt_request *req)
{
    if (req->filter.pid == 0 || req->start_ns == 0 || req->end_ns == 0 ||
        req->end_ns <= req->start_ns) {
        emit_invalid_execution_detail(srv, req);
        return;
    }

    /* Classify from records actually loaded in the requested execution
     * window. Coverage metadata deliberately tolerates one sample period at
     * edges for summary routing; that slop must not turn a small sampled-only
     * detail request into fidelity=none. */
    int window_count;
    struct pgwt_load_info window_info = {0};
    struct pgwt_trace_event *window_events = server_load_events_fi(
        srv, req->start_ns, req->end_ns, 0, &window_count, &window_info);
    if (reject_overload(srv, req, window_events, &window_info))
        return;
    enum pgwt_fidelity fid = load_fidelity(&window_info);
    if (pgwt_fidelity_unavailable(PGWT_REQ_EXACT, fid)) {
        free(window_events);
        emit_unavailable(req->id, fid);
        return;
    }

    /* Planning context is a bounded, leader-only marker pre-scan. The detail
     * window itself still contains all pids so parallel-worker lanes remain
     * visible, but unrelated historical waits are never decoded into this
     * per-click request's working array. */
    uint64_t context_from = req->start_ns > EXEC_PLAN_LOOKBACK_NS
                          ? req->start_ns - EXEC_PLAN_LOOKBACK_NS : 0;
    int context_count = 0;
    struct pgwt_load_info context_info = {0};
    struct pgwt_trace_event *events = server_load_markers_fi(
        srv, context_from, req->start_ns > 0 ? req->start_ns - 1 : 0,
        req->filter.pid, &context_count, &context_info);
    if (reject_overload(srv, req, events, &context_info)) {
        free(window_events);
        return;
    }
    int count = context_count + window_count;
    struct pgwt_trace_event *combined = realloc(
        events, (count > 0 ? count : 1) * sizeof(*combined));
    if (!combined) {
        free(events);
        free(window_events);
        emit_execution_compute_failed(req, &window_info);
        return;
    }
    events = combined;
    if (window_count > 0)
        memcpy(events + context_count, window_events,
               window_count * sizeof(*window_events));
    free(window_events);

    int n_links = 0;
    struct pgwt_backend_link *links = server_backend_links(srv, &n_links);
    if (n_links < 0) {
        free(events);
        emit_execution_compute_failed(req, &window_info);
        return;
    }
    struct pgwt_execution_detail_result detail;
    pgwt_compute_execution_detail(events, count, req->filter.pid,
                                  req->start_ns, req->end_ns,
                                  &req->filter,
                                  links, n_links, EXEC_DETAIL_LANE_CAP, &detail);
    free(links);
    free(events);
    if (detail.failed) {
        pgwt_free_execution_detail(&detail);
        emit_execution_compute_failed(req, &window_info);
        return;
    }
    if (!detail.found_execution) {
        pgwt_free_execution_detail(&detail);
        emit_execution_not_found(req, &window_info);
        return;
    }

    qsort(detail.workers, detail.num_workers, sizeof(detail.workers[0]),
          cmp_exec_lane_pid);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    add_fidelity(root, &window_info);
    cjson_add_query_id_string(root, "query_id", detail.query_id);
    cJSON_AddItemToObject(root, "leader",
                          serialize_exec_lane(&detail.leader,
                                              detail.query_id, 1));
    cJSON *workers = cJSON_AddArrayToObject(root, "workers");
    for (int i = 0; i < detail.num_workers; i++)
        cJSON_AddItemToArray(workers,
                            serialize_exec_lane(&detail.workers[i], 0, 0));
    if (detail.has_plan) {
        cJSON *plan = cJSON_AddObjectToObject(root, "plan");
        cjson_add_uint64_string(plan, "start_ns", detail.plan_start_ns);
        cjson_add_uint64_string(plan, "end_ns", detail.plan_end_ns);
    } else {
        cJSON_AddNullToObject(root, "plan");
    }
    cJSON_AddNumberToObject(root, "total_count", detail.total_events);
    cJSON_AddNumberToObject(root, "kept_count", detail.kept_events);
    cJSON_AddBoolToObject(root, "truncated",
                          detail.kept_events < detail.total_events);
    emit_json(root);
    pgwt_free_execution_detail(&detail);
}

static uint64_t scatter_rand(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static uint64_t execution_duration_ns(const struct pgwt_execution *row)
{
    return row->in_progress ? 0 : row->end_ns - row->start_ns;
}

/* Two-pass outlier-preserving reservoir. Pass one reserves the maximum
 * duration from every non-empty time bucket. Pass two redistributes every
 * unused bucket quota through one global deterministic reservoir, so
 * clustered workloads fill the requested budget instead of leaving most
 * slots empty. */
static struct pgwt_execution *scatter_downsample(
    const struct pgwt_execution *rows, int count, int max_points,
    uint64_t from_ns, uint64_t to_ns, int *out_count)
{
    *out_count = 0;
    int nb = max_points >= 4 ? max_points / 4 : 1;
    if (nb > count) nb = count;
    int *bucket_max = malloc(nb * sizeof(*bucket_max));
    if (!bucket_max) return NULL;
    for (int b = 0; b < nb; b++) bucket_max[b] = -1;
    uint64_t range = to_ns > from_ns ? to_ns - from_ns : 1;

    /* Pass one: one guaranteed local maximum per occupied time bucket. */
    for (int i = 0; i < count; i++) {
        uint64_t rel = rows[i].start_ns > from_ns
                     ? rows[i].start_ns - from_ns : 0;
        int b = (int)(((__uint128_t)rel * nb) / range);
        if (b >= nb) b = nb - 1;
        int old = bucket_max[b];
        if (old < 0 || execution_duration_ns(&rows[i]) >
                       execution_duration_ns(&rows[old]))
            bucket_max[b] = i;
    }

    struct pgwt_execution *sample = calloc(max_points, sizeof(*sample));
    int *reservoir = malloc(max_points * sizeof(*reservoir));
    unsigned char *is_reserved = calloc(count, sizeof(*is_reserved));
    if (!sample || !reservoir || !is_reserved) {
        free(sample);
        free(reservoir);
        free(is_reserved);
        free(bucket_max);
        return NULL;
    }

    int reserved = 0;
    for (int b = 0; b < nb; b++) {
        if (bucket_max[b] >= 0) {
            sample[reserved++] = rows[bucket_max[b]];
            is_reserved[bucket_max[b]] = 1;
        }
    }
    int cap = max_points - reserved;
    uint64_t seen = 0;
    int used = 0;
    for (int i = 0; i < count && cap > 0; i++) {
        if (is_reserved[i]) continue;
        seen++;
        if (used < cap) {
            reservoir[used++] = i;
            continue;
        }
        uint64_t rnd = scatter_rand(rows[i].start_ns ^
                                    ((uint64_t)rows[i].pid << 32) ^ seen);
        uint64_t replace = rnd % seen;
        if (replace < (uint64_t)cap)
            reservoir[replace] = i;
    }
    for (int i = 0; i < used; i++)
        sample[reserved + i] = rows[reservoir[i]];
    *out_count = reserved + used;
    free(reservoir);
    free(is_reserved);
    free(bucket_max);
    return sample;
}

static void handle_exec_scatter(struct pgwt_server *srv,
                                struct pgwt_request *req)
{
    struct pgwt_load_info linfo = {0};
    struct pgwt_executions_result res;
    if (load_execution_rows(srv, req, &res, &linfo) != 0)
        return;

    /* Open executions remain points too: their duration is explicit null,
     * never a fabricated window-edge duration. */
    int total = 0;
    for (int i = 0; i < res.num_rows; i++)
        if (execution_matches_request(&res.rows[i], req))
            res.rows[total++] = res.rows[i];
    res.num_rows = total;

    int max_points = req->max_points > 0
                   ? req->max_points : EXEC_SCATTER_DEFAULT;
    if (max_points > EXEC_SCATTER_MAX) max_points = EXEC_SCATTER_MAX;
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to = req->to_ns ? req->to_ns : srv->latest_wall_ns;
    int kept = total;
    struct pgwt_execution *points = res.rows;
    if (total > max_points) {
        points = scatter_downsample(res.rows, total, max_points,
                                    from, to, &kept);
        if (!points) {
            /* Allocation failure is not permission to return a partial chart. */
            free(res.rows);
            emit_execution_compute_failed(req, &linfo);
            return;
        }
    }
    qsort(points, kept, sizeof(points[0]), cmp_execution_start_asc);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    add_fidelity(root, &linfo);
    cJSON *arr = cJSON_AddArrayToObject(root, "points");
    for (int i = 0; i < kept; i++) {
        cJSON *p = cJSON_CreateObject();
        cjson_add_uint64_string(p, "t", points[i].start_ns);
        if (points[i].in_progress)
            cJSON_AddNullToObject(p, "duration_ms");
        else
            cJSON_AddNumberToObject(p, "duration_ms",
                (double)execution_duration_ns(&points[i]) / 1e6);
        cJSON_AddNumberToObject(p, "pid", points[i].pid);
        cjson_add_query_id_string(p, "query_id", points[i].query_id);
        cJSON_AddBoolToObject(p, "in_progress", points[i].in_progress);
        cJSON_AddItemToArray(arr, p);
    }
    cJSON_AddNumberToObject(root, "total_count", total);
    cJSON_AddNumberToObject(root, "kept_count", kept);
    cJSON_AddBoolToObject(root, "downsampled", kept < total);
    emit_json(root);
    if (points != res.rows) free(points);
    free(res.rows);
}

/* ── Dispatch ─────────────────────────────────────────────── */

static void handle_transitions(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_load_info linfo = {0};
    struct pgwt_trace_event *events =
        server_load_events_fi(srv, req->from_ns, req->to_ns,
                              req->filter.pid, &count, &linfo);
    if (reject_overload(srv, req, events, &linfo)) return;

    /* Transitions need real old→new order: EXACT-required. */
    enum pgwt_fidelity fid = load_fidelity(&linfo);
    if (pgwt_fidelity_unavailable(PGWT_REQ_EXACT, fid)) {
        free(events);
        emit_unavailable(req->id, fid);
        return;
    }

    int max_rows = req->num_buckets > 0 ? req->num_buckets : 50;
    struct pgwt_transitions_result res;
    pgwt_compute_transitions(events, count, &req->filter, max_rows, &res);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cJSON_AddStringToObject(root, "fidelity", pgwt_fidelity_str(fid));
    cjson_add_uint64(root, "total", res.total_transitions);
    cJSON_AddNumberToObject(root, "link_count", res.num_rows);
    cJSON_AddNumberToObject(root, "total_link_count", res.total_rows);
    cJSON_AddBoolToObject(root, "truncated", res.num_rows < res.total_rows);

    /* Compute per-node total time directly from ALL events.
     * Use a simple hash table keyed by event_id for O(1) lookup. */
    #define NODE_HT_SIZE 1024
    #define NODE_HT_MASK (NODE_HT_SIZE - 1)
    struct { uint32_t event_id; int used; char name[64]; double total_ms; }
        *node_ht = calloc(NODE_HT_SIZE, sizeof(*node_ht));
    int num_nodes = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        uint32_t eid = ev->old_event;
        /* Transition-graph node totals: visibility view, keep ClientRead. */
        if (pgwt_is_hidden_event(eid) || PGWT_IS_MARKER(eid))
            continue;
        double ms = ev->duration_ns / 1e6;
        uint32_t h = (eid * 0x9e3779b9) & NODE_HT_MASK;
        while (node_ht[h].used && node_ht[h].event_id != eid)
            h = (h + 1) & NODE_HT_MASK;
        if (node_ht[h].used) {
            node_ht[h].total_ms += ms;
        } else {
            node_ht[h].used = 1;
            node_ht[h].event_id = eid;
            node_ht[h].total_ms = ms;
            if (eid == 0)
                snprintf(node_ht[h].name, 64, "CPU*");
            else
                pgwt_event_full_name(eid, node_ht[h].name, sizeof(node_ht[h].name));
            num_nodes++;
        }
    }

    /* Nodes array */
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");
    for (int i = 0; i < NODE_HT_SIZE; i++) {
        if (!node_ht[i].used) continue;
        cJSON *node = cJSON_CreateObject();
        cJSON_AddStringToObject(node, "name", node_ht[i].name);
        cJSON_AddNumberToObject(node, "total_ms", node_ht[i].total_ms);
        /* U2 / P3 wire 5: the UI's DFG node click pivots on the event —
         * emit the id the table is already keyed by (0 = CPU*). */
        cJSON_AddNumberToObject(node, "event_id", node_ht[i].event_id);
        /* Extract wait class for coloring */
        const char *colon = strchr(node_ht[i].name, ':');
        if (colon) {
            char cls[32];
            int len = colon - node_ht[i].name;
            if (len > 31) len = 31;
            memcpy(cls, node_ht[i].name, len);
            cls[len] = '\0';
            cJSON_AddStringToObject(node, "class", cls);
        } else {
            cJSON_AddStringToObject(node, "class", "cpu");
        }
        cJSON_AddItemToArray(nodes, node);
    }
    free(node_ht);

    /* Links array */
    cJSON *links = cJSON_AddArrayToObject(root, "links");
    for (int i = 0; i < res.num_rows; i++) {
        cJSON *link = cJSON_CreateObject();
        cJSON_AddStringToObject(link, "source", res.rows[i].from_name);
        cJSON_AddStringToObject(link, "target", res.rows[i].to_name);
        cJSON_AddNumberToObject(link, "value", (double)res.rows[i].count);
        cJSON_AddNumberToObject(link, "duration_ms",
                                (double)res.rows[i].total_ns / 1e6);
        cJSON_AddItemToArray(links, link);
    }

    free(events);
    free(res.rows);
    emit_json(root);
}

static void handle_fingerprints(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_load_info linfo = {0};
    struct pgwt_trace_event *events =
        server_load_events_fi(srv, req->from_ns, req->to_ns,
                              req->filter.pid, &count, &linfo);
    if (reject_overload(srv, req, events, &linfo)) return;

    /* Fingerprints aggregate transition sequences: EXACT-required. */
    enum pgwt_fidelity fid = load_fidelity(&linfo);
    if (pgwt_fidelity_unavailable(PGWT_REQ_EXACT, fid)) {
        free(events);
        emit_unavailable(req->id, fid);
        return;
    }

    struct pgwt_fingerprint_result res;
    pgwt_compute_fingerprints(events, count, &req->filter, &res);
    free(events);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cJSON_AddStringToObject(root, "fidelity", pgwt_fidelity_str(fid));

    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < res.num_rows; i++) {
        cJSON *r = cJSON_CreateObject();
        cjson_add_int64(r, "query_id", (int64_t)res.rows[i].query_id);
        cJSON_AddNumberToObject(r, "transitions",
                                (double)res.rows[i].total_transitions);
        cJSON_AddStringToObject(r, "signature", res.rows[i].signature);

        /* Class distribution */
        cJSON *pct = cJSON_CreateObject();
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            if (res.rows[i].class_pct[c] >= 0.1)
                cJSON_AddNumberToObject(pct, pgwt_class_names[c],
                                        res.rows[i].class_pct[c]);
        }
        cJSON_AddItemToObject(r, "class_pct", pct);

        /* Top transition */
        char from_name[64], to_name[64];
        pgwt_event_full_name(res.rows[i].top_from, from_name, sizeof(from_name));
        pgwt_event_full_name(res.rows[i].top_to, to_name, sizeof(to_name));
        cJSON_AddStringToObject(r, "top_from", from_name);
        cJSON_AddStringToObject(r, "top_to", to_name);

        cJSON_AddItemToArray(rows, r);
    }

    free(res.rows);
    emit_json(root);
}

static void handle_lock_chains(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_load_info linfo = {0};
    struct pgwt_trace_event *events =
        server_load_events_fi(srv, req->from_ns, req->to_ns,
                              req->filter.pid, &count, &linfo);
    if (reject_overload(srv, req, events, &linfo)) return;

    /* Lock-chain inference needs real wait/CPU overlap intervals: EXACT. */
    enum pgwt_fidelity fid = load_fidelity(&linfo);
    if (pgwt_fidelity_unavailable(PGWT_REQ_EXACT, fid)) {
        free(events);
        emit_unavailable(req->id, fid);
        return;
    }

    struct pgwt_lock_chains_result res;
    pgwt_compute_lock_chains(events, count, &req->filter, 50, &res);
    free(events);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cJSON_AddStringToObject(root, "fidelity", pgwt_fidelity_str(fid));

    cJSON *chains = cJSON_AddArrayToObject(root, "chains");
    for (int i = 0; i < res.num_links; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "waiter", res.links[i].waiter_pid);
        cJSON_AddNumberToObject(c, "blocker", res.links[i].blocker_pid);
        cJSON_AddStringToObject(c, "lock", res.links[i].lock_name);
        cJSON_AddNumberToObject(c, "wait_ms",
                                (double)res.links[i].wait_ns / 1e6);
        cjson_add_uint64(c, "timestamp_ns", res.links[i].timestamp_ns);
        cJSON_AddItemToArray(chains, c);
    }

    free(res.links);
    emit_json(root);
}

static void handle_interference(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_load_info linfo = {0};
    struct pgwt_trace_event *events =
        server_load_events_fi(srv, req->from_ns, req->to_ns,
                              req->filter.pid, &count, &linfo);
    if (reject_overload(srv, req, events, &linfo)) return;

    /* Interference scoring needs real simultaneous-wait overlap: EXACT. */
    enum pgwt_fidelity fid = load_fidelity(&linfo);
    if (pgwt_fidelity_unavailable(PGWT_REQ_EXACT, fid)) {
        free(events);
        emit_unavailable(req->id, fid);
        return;
    }

    struct pgwt_interference_result res;
    pgwt_compute_interference(events, count, &req->filter, 30, &res);
    free(events);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cJSON_AddStringToObject(root, "fidelity", pgwt_fidelity_str(fid));

    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < res.num_rows; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "pid_a", res.rows[i].pid_a);
        cJSON_AddNumberToObject(r, "pid_b", res.rows[i].pid_b);
        cJSON_AddNumberToObject(r, "score", res.rows[i].score);
        cJSON_AddStringToObject(r, "top_event", res.rows[i].top_event_name);
        cJSON_AddNumberToObject(r, "overlap_ms",
                                (double)res.rows[i].overlap_ns / 1e6);
        cJSON_AddItemToArray(rows, r);
    }

    free(res.rows);
    emit_json(root);
}

static void handle_concurrency(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_load_info linfo = {0};
    struct pgwt_trace_event *events =
        server_load_events_fi(srv, req->from_ns, req->to_ns,
                              req->filter.pid, &count, &linfo);
    if (reject_overload(srv, req, events, &linfo)) return;

    /* Burst detection needs real simultaneous-wait intervals: EXACT. */
    enum pgwt_fidelity fid = load_fidelity(&linfo);
    if (pgwt_fidelity_unavailable(PGWT_REQ_EXACT, fid)) {
        free(events);
        emit_unavailable(req->id, fid);
        return;
    }

    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    int num_buckets = req->num_buckets > 0 ? req->num_buckets : 60;

    struct pgwt_concurrency_result res;
    pgwt_compute_concurrency(events, count, &req->filter,
                              from, to, num_buckets,
                              10000000ULL,  /* 10ms burst window */
                              4,            /* 4+ sessions = burst */
                              &res);
    free(events);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cJSON_AddStringToObject(root, "fidelity", pgwt_fidelity_str(fid));
    cjson_add_uint64(root, "bucket_ns", res.bucket_ns);

    /* Peak concurrency per bucket */
    cJSON *peaks = cJSON_AddArrayToObject(root, "peaks");
    for (int i = 0; i < res.num_buckets; i++) {
        cJSON *p = cJSON_CreateObject();
        uint64_t t_ns = from + (uint64_t)i * res.bucket_ns;
        cjson_add_uint64(p, "t", t_ns);
        cJSON_AddNumberToObject(p, "t_ms", (double)(t_ns / 1000000ULL));
        cJSON_AddNumberToObject(p, "max", res.peak_sessions[i]);
        if (res.peak_event[i]) {
            char ename[64];
            pgwt_event_full_name(res.peak_event[i], ename, sizeof(ename));
            cJSON_AddStringToObject(p, "event", ename);
        }
        cJSON_AddItemToArray(peaks, p);
    }

    /* Bursts */
    cJSON *bursts_arr = cJSON_AddArrayToObject(root, "bursts");
    int nb = res.num_bursts < 20 ? res.num_bursts : 20;
    for (int i = 0; i < nb; i++) {
        cJSON *b = cJSON_CreateObject();
        cjson_add_uint64(b, "timestamp_ns", res.bursts[i].timestamp_ns);
        cJSON_AddNumberToObject(b, "timestamp_ms",
                                (double)(res.bursts[i].timestamp_ns / 1000000ULL));
        cJSON_AddStringToObject(b, "event", res.bursts[i].event_name);
        cJSON_AddNumberToObject(b, "sessions", res.bursts[i].num_sessions);

        cJSON *pids = cJSON_AddArrayToObject(b, "pids");
        for (int j = 0; j < res.bursts[i].num_pids; j++)
            cJSON_AddItemToArray(pids, cJSON_CreateNumber(res.bursts[i].pids[j]));

        cJSON_AddItemToArray(bursts_arr, b);
    }

    free(res.peak_sessions);
    free(res.peak_event);
    free(res.bursts);
    emit_json(root);
}

static cJSON *serialize_variants(struct pgwt_server *srv,
                                  struct pgwt_variants_result *res)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < res->num_variants; i++) {
        struct pgwt_variant *v = &res->variants[i];
        cJSON *vj = cJSON_CreateObject();
        cJSON_AddNumberToObject(vj, "exec_count", v->exec_count);
        cJSON_AddNumberToObject(vj, "num_queries", v->num_query_ids);
        cJSON_AddNumberToObject(vj, "total_ms", (double)v->total_ns / 1e6);
        cJSON_AddNumberToObject(vj, "avg_ms", (double)v->avg_ns / 1e6);
        cJSON_AddNumberToObject(vj, "p95_ms", (double)v->p95_ns / 1e6);
        cJSON_AddNumberToObject(vj, "avg_loop_n", v->avg_loop_n);
        cjson_add_int64(vj, "top_query_id", (int64_t)v->top_query_id);

        cJSON *steps = cJSON_AddArrayToObject(vj, "steps");
        for (int s = 0; s < v->num_steps; s++) {
            cJSON *sj = cJSON_CreateObject();
            cJSON_AddStringToObject(sj, "name", v->steps[s].name);
            cJSON_AddNumberToObject(sj, "avg_ms", (double)v->step_avg_ns[s] / 1e6);
            if (v->steps[s].is_loop)
                cJSON_AddNumberToObject(sj, "loop", v->steps[s].loop_len);
            const char *colon = strchr(v->steps[s].name, ':');
            if (colon) {
                char cls[32];
                int len = colon - v->steps[s].name;
                if (len > 31) len = 31;
                memcpy(cls, v->steps[s].name, len);
                cls[len] = '\0';
                cJSON_AddStringToObject(sj, "class", cls);
            } else {
                cJSON_AddStringToObject(sj, "class", "cpu");
            }
            cJSON_AddItemToArray(steps, sj);
        }

        if (v->top_query_id) {
            const char *qt = qt_map_lookup(srv, v->top_query_id);
            if (qt) cJSON_AddStringToObject(vj, "query_text", qt);
        }

        cJSON_AddItemToArray(arr, vj);
    }
    return arr;
}

static void handle_variants(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_load_info linfo = {0};
    struct pgwt_trace_event *events =
        server_load_events_fi(srv, req->from_ns, req->to_ns,
                              /* no pid pushdown: variants read every pid's
                               * exec/plan markers, which never pass the
                               * uniform per-event filter */ 0,
                              &count, &linfo);
    if (reject_overload(srv, req, events, &linfo)) return;

    /* Variants are built from marker-delimited transition sequences, which
     * only exist in full-fidelity data: EXACT-required. */
    enum pgwt_fidelity fid = load_fidelity(&linfo);
    if (pgwt_fidelity_unavailable(PGWT_REQ_EXACT, fid)) {
        free(events);
        emit_unavailable(req->id, fid);
        return;
    }

    int max_v = req->num_buckets > 0 ? req->num_buckets : 20;

    /* Extract execution variants */
    struct pgwt_variants_result exec_res;
    pgwt_compute_variants(events, count, &req->filter, max_v,
                           PGWT_PHASE_EXEC, &exec_res);

    /* Extract planning variants */
    struct pgwt_variants_result plan_res;
    pgwt_compute_variants(events, count, &req->filter, max_v,
                           PGWT_PHASE_PLAN, &plan_res);

    free(events);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cJSON_AddStringToObject(root, "fidelity", pgwt_fidelity_str(fid));

    /* Execution */
    cJSON *exec_obj = cJSON_AddObjectToObject(root, "exec");
    cJSON_AddNumberToObject(exec_obj, "total", exec_res.total_executions);
    cJSON_AddNumberToObject(exec_obj, "num_variants", exec_res.num_variants);
    cJSON_AddItemToObject(exec_obj, "variants", serialize_variants(srv, &exec_res));

    /* Planning */
    cJSON *plan_obj = cJSON_AddObjectToObject(root, "plan");
    cJSON_AddNumberToObject(plan_obj, "total", plan_res.total_executions);
    cJSON_AddNumberToObject(plan_obj, "num_variants", plan_res.num_variants);
    cJSON_AddItemToObject(plan_obj, "variants", serialize_variants(srv, &plan_res));

    free(exec_res.variants);
    free(plan_res.variants);
    emit_json(root);
}

/* ── Daemon control proxy ─────────────────────────────────── */

/* Round-trip one JSON line to the daemon control socket at
 * {trace_dir}/pgwt.sock. Returns 0 on success (resp filled, newline
 * stripped), -1 if the socket does not exist (daemon not running),
 * -2 on connect/IO errors. */
static int control_roundtrip(const char *trace_dir, const char *req_line,
                             char *resp, size_t resp_size)
{
    char sock_path[600];
    snprintf(sock_path, sizeof(sock_path), "%s/pgwt.sock", trace_dir);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path))
        return -2;
    strcpy(addr.sun_path, sock_path);

    struct stat st;
    if (stat(sock_path, &st) != 0)
        return -1;  /* no socket — daemon not running */

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -2;

    /* Bounded waits — a stuck daemon must not hang the server */
    struct timeval tv = { .tv_sec = 3 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        /* Stale socket file with no listener == daemon not running */
        return (errno == ECONNREFUSED) ? -1 : -2;
    }

    /* Send request + newline (MSG_NOSIGNAL: no SIGPIPE if daemon dies) */
    size_t req_len = strlen(req_line);
    if (send(fd, req_line, req_len, MSG_NOSIGNAL) != (ssize_t)req_len ||
        send(fd, "\n", 1, MSG_NOSIGNAL) != 1) {
        close(fd);
        return -2;
    }

    /* Read one response line */
    size_t off = 0;
    while (off < resp_size - 1) {
        ssize_t r = read(fd, resp + off, resp_size - 1 - off);
        if (r <= 0)
            break;
        off += (size_t)r;
        if (memchr(resp, '\n', off))
            break;
    }
    close(fd);

    resp[off] = '\0';
    char *nl = strchr(resp, '\n');
    if (!nl)
        return -2;  /* truncated/empty response */
    *nl = '\0';
    return 0;
}

/* Proxy a control command to the daemon:
 *   {"id":N,"cmd":"control","request":{"cmd":"status"}}
 * → {"id":N,"response":<daemon reply>}
 * or {"id":N,"error":"daemon not running"} when the socket is absent. */
static void handle_control(struct pgwt_server *srv, struct pgwt_request *req,
                           const char *line)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);

    cJSON *outer = cJSON_Parse(line);
    cJSON *inner = outer ? cJSON_GetObjectItem(outer, "request") : NULL;
    if (!cJSON_IsObject(inner)) {
        cJSON_AddStringToObject(root, "error", "missing request object");
        cJSON_Delete(outer);
        emit_json(root);
        return;
    }

    char *inner_str = cJSON_PrintUnformatted(inner);
    cJSON_Delete(outer);
    if (!inner_str) {
        cJSON_AddStringToObject(root, "error", "cannot serialize request");
        emit_json(root);
        return;
    }

    char resp[8192];
    int rc = control_roundtrip(srv->trace_dir, inner_str, resp, sizeof(resp));
    cJSON_free(inner_str);

    if (rc == -1) {
        cJSON_AddStringToObject(root, "error", "daemon not running");
        emit_json(root);
        return;
    }
    if (rc != 0) {
        cJSON_AddStringToObject(root, "error", "control socket error");
        emit_json(root);
        return;
    }

    cJSON *daemon_resp = cJSON_Parse(resp);
    if (!daemon_resp) {
        cJSON_AddStringToObject(root, "error", "invalid daemon response");
        emit_json(root);
        return;
    }

    cJSON_AddItemToObject(root, "response", daemon_resp);
    emit_json(root);
}

static void dispatch(struct pgwt_server *srv, struct pgwt_request *req,
                     const char *line)
{
    if (strcmp(req->cmd, "info") == 0)
        handle_info(srv, req);
    else if (strcmp(req->cmd, "aas") == 0)
        handle_aas(srv, req);
    else if (strcmp(req->cmd, "time_model") == 0)
        handle_time_model(srv, req);
    else if (strcmp(req->cmd, "top_events") == 0)
        handle_top_events(srv, req);
    else if (strcmp(req->cmd, "top_sessions") == 0)
        handle_top_sessions(srv, req);
    else if (strcmp(req->cmd, "top_queries") == 0)
        handle_top_queries(srv, req);
    else if (strcmp(req->cmd, "heatmap") == 0)
        handle_heatmap(srv, req);
    else if (strcmp(req->cmd, "session_timeline") == 0)
        handle_session_timeline(srv, req);
    else if (strcmp(req->cmd, "executions") == 0)
        handle_executions(srv, req);
    else if (strcmp(req->cmd, "execution_detail") == 0)
        handle_execution_detail(srv, req);
    else if (strcmp(req->cmd, "exec_scatter") == 0)
        handle_exec_scatter(srv, req);
    else if (strcmp(req->cmd, "transitions") == 0)
        handle_transitions(srv, req);
    else if (strcmp(req->cmd, "fingerprints") == 0)
        handle_fingerprints(srv, req);
    else if (strcmp(req->cmd, "concurrency") == 0)
        handle_concurrency(srv, req);
    else if (strcmp(req->cmd, "lock_chains") == 0)
        handle_lock_chains(srv, req);
    else if (strcmp(req->cmd, "variants") == 0)
        handle_variants(srv, req);
    else if (strcmp(req->cmd, "interference") == 0)
        handle_interference(srv, req);
    else if (strcmp(req->cmd, "control") == 0)
        handle_control(srv, req, line);
    else {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "id", (double)req->id);
        char errmsg[64];
        snprintf(errmsg, sizeof(errmsg), "unknown command: %s", req->cmd);
        cJSON_AddStringToObject(root, "error", errmsg);
        emit_json(root);
    }
}

/* ── Main ─────────────────────────────────────────────────── */

/* ── Dump mode: text summary to stdout ──────────────────── */

/* If a daemon control socket is present in the trace dir, print a short
 * status block at the top of the dump. Silent when no daemon runs. */
static void dump_daemon_status(const char *trace_dir)
{
    char resp[8192];

    if (control_roundtrip(trace_dir, "{\"cmd\":\"status\"}",
                          resp, sizeof(resp)) != 0)
        return;

    cJSON *status = cJSON_Parse(resp);
    if (!status)
        return;

    const char *mode = "?";
    double uptime_s = 0;
    int backends = 0, pg_pid = 0;

    cJSON *it;
    if (cJSON_IsString((it = cJSON_GetObjectItem(status, "mode"))))
        mode = it->valuestring;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(status, "uptime_s"))))
        uptime_s = it->valuedouble;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(status, "backends"))))
        backends = (int)it->valuedouble;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(status, "pg_pid"))))
        pg_pid = (int)it->valuedouble;

    double events_per_sec = 0;
    if (control_roundtrip(trace_dir, "{\"cmd\":\"metrics\"}",
                          resp, sizeof(resp)) == 0) {
        cJSON *metrics = cJSON_Parse(resp);
        if (metrics) {
            if (cJSON_IsNumber((it = cJSON_GetObjectItem(metrics,
                                                         "events_per_sec"))))
                events_per_sec = it->valuedouble;
            cJSON_Delete(metrics);
        }
    }

    printf("Daemon: running    mode: %s    uptime: %.0fs    "
           "events/s: %.0f    backends: %d    pg_pid: %d\n\n",
           mode, uptime_s, events_per_sec, backends, pg_pid);

    cJSON_Delete(status);
}

static void dump_summary(struct pgwt_server *srv)
{
    uint64_t from = srv->earliest_wall_ns;
    uint64_t to = srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;
    struct pgwt_filter filt = {0};

    /* Load all events (fidelity-aware: samples normalized, exact-wins merge) */
    int count;
    struct pgwt_load_info linfo = {0};
    struct pgwt_trace_event *events =
        server_load_events_fi(srv, from, to, 0, &count, &linfo);
    if (linfo.overloaded) {
        printf("ERROR: window too large — more than %d events in range; "
               "use the JSON interface with a narrower range\n",
               load_max_events());
        free(events);
        return;
    }

    /* Count sample vs transition records so a sampled-mode trace is visible
     * at a glance (A2 evidence: SAMPLES blocks landed). */
    int n_samples = 0;
    for (int i = 0; i < count; i++)
        if (events[i].flags & PGWT_EVENT_FLAG_SAMPLE)
            n_samples++;

    enum pgwt_fidelity fid = load_fidelity(&linfo);

    printf("════════════════════════════════════════════════════════════════════════════════\n");
    printf("pgwt-server — Summary    Events: %d (%d transitions, %d samples)    Duration: %.1fs\n",
           count, count - n_samples, n_samples, wall_ms / 1000.0);
    printf("Fidelity: %s", pgwt_fidelity_str(fid));
    if (linfo.has_samples && linfo.sample_period_ns)
        printf("    Sample period: %.1f ms (%.0f Hz)",
               (double)linfo.sample_period_ns / 1e6,
               1e9 / (double)linfo.sample_period_ns);
    printf("\n");
    if (fid == PGWT_FIDELITY_SAMPLED)
        printf("Note: histogram / transitions / lock chains / interference / variants "
               "require full-fidelity data and are unavailable for this window.\n");
    printf("════════════════════════════════════════════════════════════════════════════════\n");

    /* Time Model */
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(events, count, &filt, wall_ms, &tm);

    printf("\n  === Time Model ===\n\n");
    printf("  AAS: %.2f    DB Time: %.1f ms    Idle: %.1f ms\n\n",
           tm.aas, tm.db_time_ms, tm.idle_time_ms);

    for (int i = 0; i < tm.num_rows; i++) {
        struct pgwt_tm_row *r = &tm.rows[i];
        if (r->indent == 0)
            printf("  %-32s %10.1f ms  %6.1f%%\n", r->name, r->time_ms, r->pct_db_time);
        else if (r->indent == 1)
            printf("    %-30s %10.1f ms  %6.1f%%\n", r->name, r->time_ms, r->pct_db_time);
        else
            printf("      %-28s %10.1f ms  %6.1f%%\n", r->name, r->time_ms, r->pct_db_time);
    }
    free(tm.rows);

    /* Top Events */
    struct pgwt_events_result ev;
    pgwt_compute_top_events(events, count, &filt, wall_ms, &ev);

    printf("\n  === Top Events ===\n\n");
    printf("  %-28s %10s %12s %10s %8s\n", "Wait Event", "Waits", "Total (ms)", "Avg (us)", "% DB");
    printf("  ─────────────────────────────────────────────────────────────────────────\n");

    int n = ev.num_rows < 15 ? ev.num_rows : 15;
    for (int i = 0; i < n; i++) {
        struct pgwt_event_row *r = &ev.rows[i];
        char pctbuf[16], avgbuf[16];
        /* Idle-but-visible events: time but no meaningful %DB share. */
        if (PGWT_PCT_DB_IS_IDLE(r->pct_db))
            snprintf(pctbuf, sizeof(pctbuf), "%8s", "—");
        else
            snprintf(pctbuf, sizeof(pctbuf), "%7.1f%%", r->pct_db);
        /* Sampled-only rows have no real latency measurements (FID-3). */
        if (r->exact_count > 0)
            snprintf(avgbuf, sizeof(avgbuf), "%10.1f", r->avg_us);
        else
            snprintf(avgbuf, sizeof(avgbuf), "%10s", "—");
        printf("  %-28s %10llu %12.1f %s %s\n",
               r->name, (unsigned long long)r->count, r->total_ms, avgbuf, pctbuf);
    }
    free(ev.rows);

    /* Top Sessions */
    struct pgwt_sessions_result sess;
    pgwt_compute_top_sessions(events, count, &filt, wall_ms, &sess);

    printf("\n  === Top Sessions ===\n\n");
    printf("  %7s %12s %7s %7s  %-20s\n",
           "PID", "DB Time(ms)", "CPU%", "Wait%", "Top Wait");
    printf("  ─────────────────────────────────────────────────────────────────────────\n");

    n = sess.num_rows < 15 ? sess.num_rows : 15;
    for (int i = 0; i < n; i++) {
        struct pgwt_session_row *r = &sess.rows[i];
        printf("  %7d %12.1f %6.1f%% %6.1f%%  %-20s\n",
               r->pid, r->db_time_ms, r->cpu_pct, r->wait_pct, r->top_wait);
    }
    free(sess.rows);

    /* Top Queries */
    struct pgwt_queries_result qry;
    pgwt_compute_top_queries(events, count, &filt, wall_ms, &qry);

    printf("\n  === Top Queries ===\n\n");
    printf("  %20s %10s %12s %8s  %-20s\n",
           "query_id", "Waits", "Total (ms)", "% DB", "Top Wait");
    printf("  ─────────────────────────────────────────────────────────────────────────\n");

    n = qry.num_rows < 15 ? qry.num_rows : 15;
    for (int i = 0; i < n; i++) {
        struct pgwt_query_row *r = &qry.rows[i];
        printf("  %20lld %10llu %12.1f %7.1f%%  %-20s\n",
               (long long)r->query_id, (unsigned long long)r->count,
               r->total_ms, r->pct_db, r->top_wait);
    }
    free(qry.rows);

    free(events);
    printf("\n");
}

int main(int argc, char **argv)
{
    int dump_mode = 0;
    const char *trace_dir = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0)
            dump_mode = 1;
        else if (argv[i][0] != '-')
            trace_dir = argv[i];
    }

    if (!trace_dir) {
        fprintf(stderr, "Usage: pgwt-server [--dump] <trace-dir>\n");
        return 1;
    }

    /* Line-buffered stdout is critical for SSH pipe */
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct pgwt_server srv;
    if (server_init(&srv, trace_dir) != 0)
        return 1;

    if (dump_mode) {
        dump_daemon_status(srv.trace_dir);
        dump_summary(&srv);
        server_destroy(&srv);
        return 0;
    }

    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        struct pgwt_request req;
        parse_request(line, &req);

        if (req.cmd[0] == '\0') {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "id", (double)req.id);
            cJSON_AddStringToObject(root, "error", "missing cmd");
            emit_json(root);
            continue;
        }

        dispatch(&srv, &req, line);
    }

    server_destroy(&srv);
    return 0;
}
