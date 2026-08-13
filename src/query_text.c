/* query_text.c — SQL query text capture from PgBackendStatus.st_activity_raw
 *
 * Captures the actual running SQL (not normalized) when a new query_id
 * appears in the event stream. Writes to query_texts.jsonl sidecar file. */
#include "query_text.h"
#include "pg_wait_tracer.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

/* ── Hash set for seen query_ids ─────────────────────────── */

static uint64_t hash64(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

/* Returns 0 if newly inserted, 1 if already in the set, 2 if the table is
 * full (id NOT tracked — DUR-10: callers log this loudly, once). */
static int seen_check_or_insert(struct pgwt_query_text_capture *qt,
                                 uint64_t query_id)
{
    uint32_t idx = (uint32_t)(hash64(query_id) & (QT_HT_SIZE - 1));
    for (int i = 0; i < QT_HT_SIZE; i++) {
        if (qt->seen[idx] == query_id)
            return 1;  /* already seen */
        if (qt->seen[idx] == 0) {
            /* Empty slot — insert */
            qt->seen[idx] = query_id;
            qt->num_seen++;
            return 0;  /* newly inserted */
        }
        idx = (idx + 1) & (QT_HT_SIZE - 1);
    }
    return 2;  /* table full */
}

/* DUR-10: the 4096-id dedup table capping out means query TEXT for further
 * new query_ids is never captured again — that must be loud, once. */
static void qt_log_cap_once(struct pgwt_query_text_capture *qt)
{
    if (qt->cap_logged)
        return;
    qt->cap_logged = true;
    fprintf(stderr,
            "WARN: query-text id table is FULL (%d unique query_ids) — "
            "text for NEW query_ids will no longer be captured until the "
            "daemon restarts (ids and waits are unaffected)\n", QT_HT_SIZE);
}

/* ── Read st_activity_raw from backend process ───────────── */

/* Read the activity string for a backend via /proc/<pid>/mem.
 * Returns length of string read, or 0 on failure. */
static int read_activity(const struct pgwt_query_text_capture *qt,
                          pid_t pid, char *out, int out_size)
{
    char mem_path[64];
    snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);

    int fd = open(mem_path, O_RDONLY);
    if (fd < 0)
        return 0;

    /* Step 1: Read MyBEEntry pointer (PgBackendStatus*) */
    uint64_t be_ptr = 0;
    if (pread(fd, &be_ptr, sizeof(be_ptr), qt->my_be_entry_addr) != sizeof(be_ptr)
        || be_ptr == 0) {
        close(fd);
        return 0;
    }

    /* Step 2: Read st_activity_raw pointer (char*) from PgBackendStatus */
    uint64_t activity_ptr = 0;
    if (pread(fd, &activity_ptr, sizeof(activity_ptr),
              be_ptr + qt->st_activity_offset) != sizeof(activity_ptr)
        || activity_ptr == 0) {
        close(fd);
        return 0;
    }

    /* Step 3: Read the actual string from the activity pointer */
    memset(out, 0, out_size);
    ssize_t n = pread(fd, out, out_size - 1, activity_ptr);
    close(fd);

    if (n <= 0)
        return 0;

    out[out_size - 1] = '\0';

    /* Find actual string length (null-terminated) */
    int len = (int)strnlen(out, n);
    out[len] = '\0';
    return len;
}

/* ── JSON-safe string writing ────────────────────────────── */

/* Write a JSON-escaped string to file.  Escapes: " \ \n \r \t and control chars. */
static void write_json_string(FILE *fp, const char *s, int len)
{
    fputc('"', fp);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp);  break;
        case '\r': fputs("\\r", fp);  break;
        case '\t': fputs("\\t", fp);  break;
        default:
            if (c < 0x20) {
                /* Control character — use \uXXXX */
                fprintf(fp, "\\u%04x", c);
            } else {
                fputc(c, fp);
            }
        }
    }
    fputc('"', fp);
}

/* ── Load / compact existing file (DUR-4) ────────────────── */

/* Parse the query_id out of a JSONL line ({"q":"<signed id>",...}).
 * Returns 0 on success. */
static int qt_parse_line_qid(const char *line, uint64_t *qid)
{
    long long v;
    if (sscanf(line, "{\"q\":\"%lld\"", &v) != 1)
        return -1;
    *qid = (uint64_t)(int64_t)v;
    return 0;
}

/* Load the ids of an existing query_texts.jsonl into the seen set so a
 * restarted daemon appends instead of re-capturing (and NEVER truncates —
 * retained traces reference these ids). Memory is bounded by the fixed
 * QT_HT_SIZE table; the line buffer is the only allocation (getline, freed).
 *
 * If the file exceeds the compaction threshold, it is rewritten atomically
 * (tmp + rename) keeping the FIRST line per query_id — first-seen matches
 * the capture semantics. Lines whose ids no longer fit in the table are
 * kept verbatim (they may be referenced by retained traces; dropping them
 * would lose data). */
static void qt_load_existing(struct pgwt_query_text_capture *qt,
                             const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return;

    long threshold = QT_COMPACT_THRESHOLD;
    const char *env = getenv("PGWT_QT_COMPACT_BYTES");
    if (env && atol(env) > 0)
        threshold = atol(env);
    int compact = st.st_size > threshold;

    FILE *in = fopen(path, "r");
    if (!in)
        return;

    FILE *out = NULL;
    char tmp_path[600];
    if (compact) {
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
        out = fopen(tmp_path, "w");
        if (!out)
            compact = 0;   /* fall back to plain load */
    }

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t len;
    int loaded = 0, dropped = 0;
    while ((len = getline(&line, &line_cap, in)) > 0) {
        uint64_t qid;
        if (qt_parse_line_qid(line, &qid) != 0 || qid == 0) {
            dropped++;   /* torn/garbage line (e.g. crash mid-append) */
            continue;
        }
        int rc = seen_check_or_insert(qt, qid);
        if (rc == 0)
            loaded++;
        else if (rc == 2)
            qt_log_cap_once(qt);
        if (compact && (rc == 0 || rc == 2)) {
            /* keep first occurrence; keep untracked (table-full) lines */
            if (fwrite(line, 1, (size_t)len, out) != (size_t)len) {
                /* tmp write failed — abandon compaction, keep original */
                fclose(out);
                out = NULL;
                unlink(tmp_path);
                compact = 0;
            }
        }
    }
    free(line);
    fclose(in);

    if (compact && out) {
        fflush(out);
        fsync(fileno(out));
        fclose(out);
        if (rename(tmp_path, path) != 0)
            unlink(tmp_path);
        else if (qt->verbose)
            fprintf(stderr, "INFO: compacted query_texts.jsonl\n");
    }

    if (qt->verbose)
        fprintf(stderr, "INFO: loaded %d existing query text ids "
                "(%d unreadable lines skipped)%s\n", loaded, dropped,
                qt->cap_logged ? " — id table full" : "");
}

/* ── W3C traceparent parsing (F-C1) ──────────────────────── */

int pgwt_parse_traceparent(const char *text,
                           char *trace_id, size_t trace_id_sz,
                           char *parent_span_id, size_t parent_span_id_sz)
{
    if (trace_id_sz > 0) trace_id[0] = '\0';
    if (parent_span_id_sz > 0) parent_span_id[0] = '\0';
    if (!text || text[0] == '\0') return 0;

    /* Bound rescan cost: inspect at most 4096 characters */
    size_t text_len = strlen(text);
    if (text_len > 4096) text_len = 4096;

    for (size_t i = 0; i + 55 <= text_len; i++) {
        if (strncasecmp(&text[i], "traceparent", 11) != 0)
            continue;

        const char *tp = &text[i] + 11;
        /* Skip separator characters: spaces, tabs, colons, equals, single/double quotes */
        while (*tp == ' ' || *tp == '\t' || *tp == ':' || *tp == '=' || *tp == '\'' || *tp == '"')
            tp++;

        /* Verify remaining length is enough for W3C traceparent (53 chars: 2+1+32+1+16) */
        if ((size_t)(tp - text) + 53 > text_len)
            break;

        /* Version: 2 hex digits followed by '-' */
        if (!isxdigit((unsigned char)tp[0]) || !isxdigit((unsigned char)tp[1]) || tp[2] != '-')
            continue;

        /* Trace ID: 32 hex digits, followed by '-', cannot be all zeros */
        const char *tid_start = tp + 3;
        int valid = 1;
        int all_zero_tid = 1;
        for (int k = 0; k < 32; k++) {
            if (!isxdigit((unsigned char)tid_start[k])) { valid = 0; break; }
            if (tid_start[k] != '0') all_zero_tid = 0;
        }
        if (!valid || all_zero_tid || tid_start[32] != '-')
            continue;

        /* Parent Span ID: 16 hex digits, cannot be all zeros */
        const char *pid_start = tid_start + 33;
        int all_zero_pid = 1;
        for (int k = 0; k < 16; k++) {
            if (!isxdigit((unsigned char)pid_start[k])) { valid = 0; break; }
            if (pid_start[k] != '0') all_zero_pid = 0;
        }
        if (!valid || all_zero_pid)
            continue;

        if (trace_id_sz > 32) {
            for (int k = 0; k < 32; k++)
                trace_id[k] = (char)tolower((unsigned char)tid_start[k]);
            trace_id[32] = '\0';
        }
        if (parent_span_id_sz > 16) {
            for (int k = 0; k < 16; k++)
                parent_span_id[k] = (char)tolower((unsigned char)pid_start[k]);
            parent_span_id[16] = '\0';
        }
        return 1;
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────── */

int pgwt_qt_init(struct pgwt_query_text_capture *qt,
                 const char *trace_dir,
                 uint64_t my_be_entry_addr,
                 int st_activity_offset,
                 gid_t trace_gid)
{
    memset(qt, 0, sizeof(*qt));
    snprintf(qt->trace_dir, sizeof(qt->trace_dir), "%s", trace_dir);
    qt->my_be_entry_addr = my_be_entry_addr;
    qt->st_activity_offset = st_activity_offset;
    qt->trace_gid = trace_gid;
    qt->enabled = true;

    /* Allocate reusable read buffer (1 MB on heap, not stack) */
    qt->read_buf = malloc(QT_MAX_TEXT);
    if (!qt->read_buf) {
        fprintf(stderr, "WARN: cannot allocate %d byte read buffer\n", QT_MAX_TEXT);
        qt->enabled = false;
        return -1;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/query_texts.jsonl", trace_dir);

    /* DUR-4: dedup-on-load + bounded compaction, then APPEND — never "w".
     * Retained trace files reference these ids across restarts. */
    qt_load_existing(qt, path);

    qt->fp = fopen(path, "a");
    if (!qt->fp) {
        fprintf(stderr, "WARN: cannot open %s: %s\n", path, strerror(errno));
        qt->enabled = false;
        return -1;
    }

    /* DUR-10: match trace-file group/permissions (raw SQL may contain
     * literals — it must not be more readable than the traces). */
    if (qt->trace_gid != (gid_t)-1) {
        fchown(fileno(qt->fp), (uid_t)-1, qt->trace_gid);
        fchmod(fileno(qt->fp), 0640);
    }

    return 0;
}

void pgwt_qt_check(struct pgwt_query_text_capture *qt,
                   pid_t pid, uint64_t query_id, uint64_t wall_ns)
{
    if (!qt->enabled || !qt->fp || query_id == 0)
        return;

    /* Check if already seen */
    int rc = seen_check_or_insert(qt, query_id);
    if (rc != 0) {
        if (rc == 2)
            qt_log_cap_once(qt);
        return;
    }

    /* New query_id — read st_activity_raw from the backend */
    int len = read_activity(qt, pid, qt->read_buf, QT_MAX_TEXT);
    if (len <= 0) {
        if (qt->verbose)
            fprintf(stderr, "WARN: cannot read st_activity for PID %d query_id %llu\n",
                    pid, (unsigned long long)query_id);
        return;
    }

    /* Get wall-clock timestamp (only called for new query_ids, so fine) */
    if (wall_ns == 0) {
        struct timespec wall;
        clock_gettime(CLOCK_REALTIME, &wall);
        wall_ns = (uint64_t)wall.tv_sec * 1000000000ULL + wall.tv_nsec;
    }

    char trace_id[36] = {0};
    char parent_span_id[20] = {0};
    pgwt_parse_traceparent(qt->read_buf, trace_id, sizeof(trace_id), parent_span_id, sizeof(parent_span_id));

    /* Write JSONL line: {"q":"<query_id>","t":"<text>","ts":<wall_ns>}
     * query_id as string to preserve full int64 precision (JSON numbers
     * are doubles with only 53 bits). Signed — can be negative. */
    fprintf(qt->fp, "{\"q\":\"%lld\",\"t\":", (long long)(int64_t)query_id);
    write_json_string(qt->fp, qt->read_buf, len);
    fprintf(qt->fp, ",\"ts\":%llu", (unsigned long long)wall_ns);
    if (trace_id[0] != '\0') {
        fprintf(qt->fp, ",\"trace_id\":\"%s\",\"parent_span_id\":\"%s\"",
                trace_id, parent_span_id);
    }
    fprintf(qt->fp, "}\n");
    fflush(qt->fp);

    if (qt->verbose)
        fprintf(stderr, "INFO: captured query text for query_id %llu: %.60s%s\n",
                (unsigned long long)query_id,
                qt->read_buf,
                len > 60 ? "..." : "");
}

void pgwt_qt_store(struct pgwt_query_text_capture *qt,
                   uint64_t query_id, const char *text, pid_t pid)
{
    if (!qt->enabled || !qt->fp || query_id == 0 || !text || !text[0])
        return;

    /* Check if already seen */
    int rc = seen_check_or_insert(qt, query_id);
    if (rc != 0) {
        if (rc == 2)
            qt_log_cap_once(qt);
        return;
    }

    int len = (int)strnlen(text, PGWT_QUERY_TEXT_LEN - 1);

    /* Get wall-clock timestamp */
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    uint64_t wall_ns = (uint64_t)wall.tv_sec * 1000000000ULL + wall.tv_nsec;

    char trace_id[36] = {0};
    char parent_span_id[20] = {0};
    pgwt_parse_traceparent(text, trace_id, sizeof(trace_id), parent_span_id, sizeof(parent_span_id));

    /* Write JSONL line */
    fprintf(qt->fp, "{\"q\":\"%lld\",\"t\":", (long long)(int64_t)query_id);
    write_json_string(qt->fp, text, len);
    fprintf(qt->fp, ",\"ts\":%llu", (unsigned long long)wall_ns);
    if (trace_id[0] != '\0') {
        fprintf(qt->fp, ",\"trace_id\":\"%s\",\"parent_span_id\":\"%s\"",
                trace_id, parent_span_id);
    }
    fprintf(qt->fp, "}\n");
    fflush(qt->fp);

    if (qt->verbose)
        fprintf(stderr, "INFO: captured query text for query_id %llu: %.60s%s\n",
                (unsigned long long)query_id,
                text, len > 60 ? "..." : "");
}

void pgwt_qt_close(struct pgwt_query_text_capture *qt)
{
    if (qt->fp) {
        fclose(qt->fp);
        qt->fp = NULL;
    }
    free(qt->read_buf);
    qt->read_buf = NULL;
    qt->enabled = false;
}
