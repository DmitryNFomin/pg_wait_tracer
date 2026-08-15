/* pgss_resolver.c -- asynchronous sampled query-text resolution. */
#include "pgss_resolver.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, char *out, size_t out_size)
{
    size_t len = strlen(hex);
    if ((len & 1) || len / 2 + 1 > out_size) return -1;
    for (size_t i = 0; i < len; i += 2) {
        int hi = hex_value((unsigned char)hex[i]);
        int lo = hex_value((unsigned char)hex[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (char)((hi << 4) | lo);
    }
    out[len / 2] = '\0';
    return (int)(len / 2);
}

enum pgwt_pgss_discovery_result
pgwt_pgss_parse_discovery(const char *output, char *schema,
                          size_t schema_size)
{
    if (schema && schema_size) schema[0] = '\0';
    if (!output) return PGWT_PGSS_DISCOVERY_ERROR;
    while (isspace((unsigned char)*output)) output++;
    if (strncmp(output, "ABSENT", 6) == 0)
        return PGWT_PGSS_DISCOVERY_ABSENT;
    if (strncmp(output, "PERMISSION", 10) == 0)
        return PGWT_PGSS_DISCOVERY_PERMISSION;
    if (strncmp(output, "SCHEMA|", 7) != 0 || !schema || !schema_size)
        return PGWT_PGSS_DISCOVERY_ERROR;
    const char *end = output + 7;
    while (isxdigit((unsigned char)*end)) end++;
    size_t hex_len = (size_t)(end - (output + 7));
    if (!hex_len || hex_len / 2 + 1 > schema_size) return PGWT_PGSS_DISCOVERY_ERROR;
    char encoded[256];
    if (hex_len >= sizeof(encoded)) return PGWT_PGSS_DISCOVERY_ERROR;
    memcpy(encoded, output + 7, hex_len);
    encoded[hex_len] = '\0';
    if (hex_decode(encoded, schema, schema_size) <= 0)
        return PGWT_PGSS_DISCOVERY_ERROR;
    return PGWT_PGSS_DISCOVERY_SCHEMA;
}

int pgwt_pgss_parse_database(const char *output, char *database,
                             size_t database_size)
{
    if (database && database_size) database[0] = '\0';
    if (!output || !database || !database_size) return -1;
    while (isspace((unsigned char)*output)) output++;
    if (strncmp(output, "ABSENT", 6) == 0) return 0;
    if (strncmp(output, "DATABASE|", 9) != 0) return -1;
    const char *end = output + 9;
    while (isxdigit((unsigned char)*end)) end++;
    size_t hex_len = (size_t)(end - (output + 9));
    if (!hex_len || hex_len >= 256) return -1;
    char encoded[256];
    memcpy(encoded, output + 9, hex_len);
    encoded[hex_len] = '\0';
    return hex_decode(encoded, database, database_size) > 0 ? 1 : -1;
}

static int append_sql(char *sql, size_t size, size_t *used,
                      const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(sql + *used, size - *used, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size - *used) return -1;
    *used += (size_t)n;
    return 0;
}

int pgwt_pgss_build_lookup_sql(char *sql, size_t sql_size,
                               const char *schema,
                               const struct pgwt_query_text_key *keys,
                               size_t count)
{
    if (!sql || !sql_size || !schema || !schema[0] || !keys || !count ||
        count > PGWT_PGSS_BATCH_MAX) return -1;
    size_t used = 0;
    if (append_sql(sql, sql_size, &used,
            "WITH wanted(dbid,userid,queryid) AS (VALUES ") != 0) return -1;
    for (size_t i = 0; i < count; i++) {
        if (!keys[i].databaseid || !keys[i].userid || !keys[i].query_id)
            return -1;
        if (append_sql(sql, sql_size, &used,
                "%s(%u::oid,%u::oid,%lld::bigint)", i ? "," : "",
                keys[i].databaseid, keys[i].userid,
                (long long)(int64_t)keys[i].query_id) != 0) return -1;
    }
    /* Double-quote the discovered extension schema locally. PostgreSQL names
     * cannot contain NUL; double quotes are escaped by duplication. */
    if (append_sql(sql, sql_size, &used,
            ") SELECT p.dbid,p.userid,p.queryid,"
            "CASE WHEN p.query IS NULL OR p.query='<insufficient privilege>' "
            "THEN 'U' ELSE 'T' END,"
            "encode(convert_to(left(p.query,%u),'UTF8'),'hex') FROM \"",
            PGWT_PGSS_TEXT_CHARS) != 0)
        return -1;
    for (const char *p = schema; *p; p++) {
        if (*p == '"' && append_sql(sql, sql_size, &used, "\"\"") != 0)
            return -1;
        if (*p != '"' && append_sql(sql, sql_size, &used, "%c", *p) != 0)
            return -1;
    }
    return append_sql(sql, sql_size, &used,
        "\".pg_stat_statements(true) p JOIN wanted w USING "
        "(dbid,userid,queryid) ORDER BY p.dbid,p.userid,p.queryid");
}

int pgwt_pgss_parse_lookup_row(const char *line,
                               struct pgwt_query_text_key *key,
                               char *text, size_t text_size)
{
    if (!line || !key || !text || !text_size) return -1;
    unsigned dbid, userid;
    long long queryid;
    int consumed = 0;
    char availability = '\0';
    if (sscanf(line, "%u|%u|%lld|%c|%n", &dbid, &userid, &queryid,
               &availability, &consumed) != 4 || consumed <= 0 ||
        (availability != 'T' && availability != 'U')) return -1;
    key->databaseid = dbid;
    key->userid = userid;
    key->query_id = (uint64_t)(int64_t)queryid;
    if (availability == 'U') {
        text[0] = '\0';
        return 0; /* visible pgss row, but text is permission-unavailable */
    }
    const char *hex = line + consumed;
    size_t len = strcspn(hex, "\r\n");
    char *copy = malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, hex, len);
    copy[len] = '\0';
    int rc = hex_decode(copy, text, text_size);
    free(copy);
    if (rc < 0) return -1;
    return rc;
}

enum pgwt_pgss_miss_action pgwt_pgss_miss_action(unsigned attempts)
{
    return attempts >= PGWT_PGSS_MAX_RETRIES ? PGWT_PGSS_MISS_EVICTED
                                              : PGWT_PGSS_MISS_RETRY;
}

#ifndef PGWT_SERVER

#include "daemon.h"
#include "spawn.h"

#include <pthread.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PGWT_PGSS_QUEUE_SIZE QT_HT_SIZE
#define PGWT_PGSS_PRETHROTTLE_BATCH_MAX 32

enum entry_state {
    ENTRY_EMPTY = 0,
    ENTRY_PENDING,
    ENTRY_DONE,       /* resolved or terminally unavailable */
    ENTRY_COOLDOWN,   /* retry exhaustion; a later sighting may reopen */
};
struct resolver_entry {
    struct pgwt_query_text_key key;
    enum entry_state state;
    unsigned attempts;
    uint64_t retry_at_ms;
};

struct pgwt_pgss_resolver {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool stop;
    bool started;
    bool test_unthrottled;
    uint64_t next_batch_at_ms;
    uint32_t last_batch_databaseid;
    struct resolver_entry entries[PGWT_PGSS_QUEUE_SIZE];
    struct pgwt_daemon *daemon;
    struct pgwt_query_text_capture *query_text;
    pid_t postmaster_pid;
    int port;
    char psql[512];
    char socket_dir[512];
    char os_user[128];
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint64_t resolver_hash(const struct pgwt_query_text_key *key)
{
    uint64_t x = key->query_id ^ ((uint64_t)key->databaseid << 32) ^ key->userid;
    x ^= x >> 33; x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33; x *= UINT64_C(0xc4ceb9fe1a85ec53);
    return x ^ (x >> 33);
}

static bool resolver_key_equal(const struct pgwt_query_text_key *a,
                               const struct pgwt_query_text_key *b)
{
    return a->query_id == b->query_id && a->databaseid == b->databaseid &&
           a->userid == b->userid;
}

static void counter_add(uint64_t *counter, uint64_t value)
{
    __atomic_fetch_add(counter, value, __ATOMIC_RELAXED);
}

static int connection_info(struct pgwt_pgss_resolver *r,
                           const char *postgres_binary)
{
    char cwd[64], pgdata[512];
    snprintf(cwd, sizeof(cwd), "/proc/%d/cwd", r->postmaster_pid);
    ssize_t n = readlink(cwd, pgdata, sizeof(pgdata) - 1);
    if (n < 0) return -1;
    pgdata[n] = '\0';
    char pidfile[640];
    snprintf(pidfile, sizeof(pidfile), "%s/postmaster.pid", pgdata);
    FILE *fp = fopen(pidfile, "r");
    if (!fp) return -1;
    r->port = 5432;
    char line[512];
    for (int lineno = 1; fgets(line, sizeof(line), fp); lineno++) {
        line[strcspn(line, "\r\n")] = '\0';
        if (lineno == 4) r->port = atoi(line);
        else if (lineno == 5)
            snprintf(r->socket_dir, sizeof(r->socket_dir), "%s", line);
    }
    fclose(fp);
    char *comma = strchr(r->socket_dir, ',');
    if (comma) *comma = '\0';
    while (isspace((unsigned char)r->socket_dir[0]))
        memmove(r->socket_dir, r->socket_dir + 1, strlen(r->socket_dir));
    char status[64];
    snprintf(status, sizeof(status), "/proc/%d/status", r->postmaster_pid);
    fp = fopen(status, "r");
    if (!fp) return -1;
    uid_t uid = (uid_t)-1;
    while (fgets(line, sizeof(line), fp)) {
        unsigned value;
        if (sscanf(line, "Uid:\t%u", &value) == 1) { uid = value; break; }
    }
    fclose(fp);
    struct passwd *pw = uid == (uid_t)-1 ? NULL : getpwuid(uid);
    if (!pw) return -1;
    snprintf(r->os_user, sizeof(r->os_user), "%s", pw->pw_name);
    snprintf(r->psql, sizeof(r->psql), "%s", postgres_binary);
    char *slash = strrchr(r->psql, '/');
    if (!slash) return -1;
    snprintf(slash + 1, (size_t)(r->psql + sizeof(r->psql) - slash - 1),
             "psql");
    return r->port > 0 && access(r->psql, X_OK) == 0 ? 0 : -1;
}

static int run_sql(struct pgwt_pgss_resolver *r, const char *database,
                   const char *sql,
                   char *output, size_t output_size)
{
    char port[16];
    snprintf(port, sizeof(port), "%d", r->port);
    char *with_socket[] = {
        "runuser", "-u", r->os_user, "--", r->psql,
        "-X", "-Atq", "-w", "-v", "ON_ERROR_STOP=1",
        "-h", r->socket_dir, "-p", port, "-d", (char *)database, "-c",
        (char *)sql, NULL,
    };
    char *default_socket[] = {
        "runuser", "-u", r->os_user, "--", r->psql,
        "-X", "-Atq", "-w", "-v", "ON_ERROR_STOP=1",
        "-p", port, "-d", (char *)database, "-c", (char *)sql, NULL,
    };
    struct pgwt_proc proc;
    if (pgwt_proc_open(&proc, r->socket_dir[0] ? with_socket : default_socket))
        return -1;
    size_t used = 0;
    while (used + 1 < output_size) {
        size_t got = fread(output + used, 1, output_size - used - 1, proc.out);
        used += got;
        if (!got) break;
    }
    output[used] = '\0';
    int status = pgwt_proc_close(&proc);
    return status == 0 ? 0 : -1;
}

static int resolve_database(struct pgwt_pgss_resolver *r, uint32_t databaseid,
                            char *database, size_t database_size)
{
    char sql[384], output[512];
    snprintf(sql, sizeof(sql),
             "SELECT COALESCE((SELECT CASE WHEN datallowconn THEN "
             "'DATABASE|'||encode(convert_to(datname,'UTF8'),'hex') ELSE "
             "'ABSENT' END FROM pg_database WHERE oid=%u::oid),'ABSENT')",
             databaseid);
    if (run_sql(r, "template1", sql, output, sizeof(output)) != 0)
        return -1;
    return pgwt_pgss_parse_database(output, database, database_size);
}

static int discover_schema(struct pgwt_pgss_resolver *r,
                           const char *database, char *schema,
                           size_t schema_size)
{
    static const char sql[] =
        "SELECT COALESCE((SELECT CASE WHEN NOT "
        "has_schema_privilege(n.oid,'USAGE') OR NOT "
        "has_function_privilege(p.oid,'EXECUTE') THEN 'PERMISSION' ELSE "
        "'SCHEMA|'||encode(convert_to(n.nspname,'UTF8'),'hex') END "
        "FROM pg_extension e JOIN pg_namespace n ON n.oid=e.extnamespace "
        "JOIN pg_proc p ON p.pronamespace=e.extnamespace AND "
        "p.proname='pg_stat_statements' AND p.pronargs=1 "
        "WHERE e.extname='pg_stat_statements' LIMIT 1),'ABSENT')";
    char output[512];
    if (run_sql(r, database, sql, output, sizeof(output)) != 0)
        return PGWT_PGSS_DISCOVERY_ERROR;
    return pgwt_pgss_parse_discovery(output, schema, schema_size);
}

static void finish_entry(struct pgwt_pgss_resolver *r, int idx,
                         uint64_t *counter)
{
    pthread_mutex_lock(&r->mutex);
    if (r->entries[idx].state == ENTRY_PENDING) {
        r->entries[idx].state = ENTRY_DONE;
        counter_add(&r->daemon->counters.sampled_text_pending,
                    UINT64_MAX); /* atomic decrement */
        if (counter) counter_add(counter, 1);
    }
    pthread_mutex_unlock(&r->mutex);
}

static void retry_entries(struct pgwt_pgss_resolver *r, const int *indices,
                          size_t count, bool successful_miss)
{
    static const uint64_t delay[] = {200, 500, 1000, 2000, 4000};
    uint64_t now = now_ms();
    pthread_mutex_lock(&r->mutex);
    for (size_t i = 0; i < count; i++) {
        struct resolver_entry *e = &r->entries[indices[i]];
        if (e->state != ENTRY_PENDING) continue;
        e->attempts++;
        if (pgwt_pgss_miss_action(e->attempts) == PGWT_PGSS_MISS_EVICTED) {
            e->state = ENTRY_COOLDOWN;
            e->retry_at_ms = now + PGWT_PGSS_REQUEUE_COOLDOWN_MS;
            counter_add(&r->daemon->counters.sampled_text_pending,
                        UINT64_MAX);
            counter_add(&r->daemon->counters
                            .sampled_text_retry_exhausted_total, 1);
            if (successful_miss)
                counter_add(&r->daemon->counters.sampled_text_evicted_total, 1);
        } else {
            unsigned di = e->attempts - 1;
            if (di >= sizeof(delay) / sizeof(delay[0]))
                di = sizeof(delay) / sizeof(delay[0]) - 1;
            e->retry_at_ms = now + delay[di];
        }
    }
    pthread_mutex_unlock(&r->mutex);
}

/* Caller holds r->mutex. Persisted text is terminal even if it arrived after
 * enqueue; no resolver SQL is issued for that key. */
static size_t select_batch_locked(struct pgwt_pgss_resolver *r, uint64_t now,
                                  int *indices,
                                  struct pgwt_query_text_key *keys,
                                  uint64_t *next_due)
{
    size_t count = 0;
    uint32_t first_databaseid = 0;
    uint32_t next_databaseid = 0;
    *next_due = UINT64_MAX;

    /* Choose by database OID, not queue slot: advance monotonically past the
     * last serviced database and wrap. Every due database is therefore served
     * once per round even when one tenant occupies most low-numbered slots. */
    for (int i = 0; i < PGWT_PGSS_QUEUE_SIZE; i++) {
        struct resolver_entry *e = &r->entries[i];
        if (e->state != ENTRY_PENDING) continue;
        if (pgwt_qt_has_text(r->query_text, &e->key)) {
            e->state = ENTRY_DONE;
            counter_add(&r->daemon->counters.sampled_text_pending,
                        UINT64_MAX);
            continue;
        }
        uint64_t due = e->retry_at_ms;
        if (due < r->next_batch_at_ms) due = r->next_batch_at_ms;
        if (due > now) {
            if (due < *next_due) *next_due = due;
            continue;
        }
        uint32_t databaseid = e->key.databaseid;
        if (!first_databaseid || databaseid < first_databaseid)
            first_databaseid = databaseid;
        if (databaseid > r->last_batch_databaseid &&
            (!next_databaseid || databaseid < next_databaseid))
            next_databaseid = databaseid;
    }

    uint32_t batch_databaseid = next_databaseid ? next_databaseid
                                                 : first_databaseid;
    if (!batch_databaseid)
        return 0;
    size_t batch_max = r->test_unthrottled
                     ? PGWT_PGSS_PRETHROTTLE_BATCH_MAX
                     : PGWT_PGSS_BATCH_MAX;
    for (int i = 0; i < PGWT_PGSS_QUEUE_SIZE && count < batch_max; i++) {
        struct resolver_entry *e = &r->entries[i];
        if (e->state != ENTRY_PENDING ||
            e->key.databaseid != batch_databaseid ||
            e->retry_at_ms > now)
            continue;
        indices[count] = i;
        keys[count++] = e->key;
    }
    if (count) {
        /* Charge the 10 s scan budget in proportion to keys amortized by this
         * SRF call. Sparse databases no longer burn a full window, while the
         * floor keeps the expensive showtext scan at or below one per second. */
        if (r->test_unthrottled) {
            r->next_batch_at_ms = now;
        } else {
            uint64_t delay = (PGWT_PGSS_BATCH_INTERVAL_MS * count +
                              PGWT_PGSS_BATCH_MAX - 1) /
                             PGWT_PGSS_BATCH_MAX;
            if (delay < PGWT_PGSS_SCAN_MIN_INTERVAL_MS)
                delay = PGWT_PGSS_SCAN_MIN_INTERVAL_MS;
            r->next_batch_at_ms = now + delay;
        }
        r->last_batch_databaseid = batch_databaseid;
    }
    return count;
}

static size_t filter_persisted(struct pgwt_pgss_resolver *r, int *indices,
                               struct pgwt_query_text_key *keys, size_t count)
{
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (pgwt_qt_has_text(r->query_text, &keys[i])) {
            finish_entry(r, indices[i], NULL);
            continue;
        }
        indices[kept] = indices[i];
        keys[kept++] = keys[i];
    }
    return kept;
}

static void *resolver_main(void *arg)
{
    struct pgwt_pgss_resolver *r = arg;
    for (;;) {
        int indices[PGWT_PGSS_BATCH_MAX];
        struct pgwt_query_text_key keys[PGWT_PGSS_BATCH_MAX];
        size_t count = 0;
        uint64_t next_due = UINT64_MAX;
        pthread_mutex_lock(&r->mutex);
        while (!r->stop) {
            uint64_t now = now_ms();
            count = select_batch_locked(r, now, indices, keys, &next_due);
            if (count) break;
            if (next_due == UINT64_MAX) {
                pthread_cond_wait(&r->cond, &r->mutex);
            } else {
                struct timespec wall;
                clock_gettime(CLOCK_REALTIME, &wall);
                uint64_t wait_ms = next_due > now ? next_due - now : 1;
                wall.tv_sec += (time_t)(wait_ms / 1000);
                wall.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
                if (wall.tv_nsec >= 1000000000L) {
                    wall.tv_sec++;
                    wall.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&r->cond, &r->mutex, &wall);
            }
        }
        bool stopping = r->stop;
        pthread_mutex_unlock(&r->mutex);
        if (stopping) break;

        count = filter_persisted(r, indices, keys, count);
        if (!count) continue;

        char database[128], schema[128];
        int database_result = resolve_database(
            r, keys[0].databaseid, database, sizeof(database));
        if (database_result == 0) {
            for (size_t i = 0; i < count; i++)
                finish_entry(r, indices[i],
                    &r->daemon->counters.sampled_text_absent_total);
            continue;
        }
        if (database_result < 0) {
            counter_add(&r->daemon->counters.sampled_text_error_total, count);
            retry_entries(r, indices, count, false);
            continue;
        }
        enum pgwt_pgss_discovery_result discovery = discover_schema(
            r, database, schema, sizeof(schema));
        if (discovery == PGWT_PGSS_DISCOVERY_ABSENT ||
            discovery == PGWT_PGSS_DISCOVERY_PERMISSION) {
            for (size_t i = 0; i < count; i++)
                finish_entry(r, indices[i],
                    &r->daemon->counters.sampled_text_absent_total);
            continue;
        }
        if (discovery == PGWT_PGSS_DISCOVERY_ERROR) {
            counter_add(&r->daemon->counters.sampled_text_error_total, count);
            retry_entries(r, indices, count, false);
            continue;
        }

        count = filter_persisted(r, indices, keys, count);
        if (!count) continue;

        char sql[65536], output[QT_MAX_TEXT];
        bool found[PGWT_PGSS_BATCH_MAX] = {0};
        int sql_ok = pgwt_pgss_build_lookup_sql(sql, sizeof(sql), schema,
                                                keys, count) == 0;
        if (sql_ok) {
            counter_add(&r->daemon->counters.sampled_text_pgss_scans_total,
                        1);
            sql_ok = run_sql(r, database, sql, output, sizeof(output)) == 0;
        }
        if (!sql_ok)
            counter_add(&r->daemon->counters.sampled_text_error_total, count);
        if (sql_ok) {
            char *save = NULL;
            for (char *line = strtok_r(output, "\n", &save); line;
                 line = strtok_r(NULL, "\n", &save)) {
                struct pgwt_query_text_key got_key;
                char text[PGWT_PGSS_TEXT_BYTES];
                int parsed = pgwt_pgss_parse_lookup_row(
                    line, &got_key, text, sizeof(text));
                if (parsed < 0)
                    continue;
                for (size_t i = 0; i < count; i++) {
                    if (resolver_key_equal(&keys[i], &got_key)) {
                        found[i] = true;
                        if (parsed > 0) {
                            pgwt_qt_store_pgss_deferred(r->query_text,
                                                        &got_key, text);
                            finish_entry(
                                r, indices[i], &r->daemon->counters
                                                    .sampled_text_resolved_total);
                        } else {
                            finish_entry(
                                r, indices[i], &r->daemon->counters
                                                    .sampled_text_absent_total);
                        }
                        break;
                    }
                }
            }
            pgwt_qt_flush(r->query_text);
        }
        int missing_indices[PGWT_PGSS_BATCH_MAX];
        size_t missing_count = 0;
        for (size_t i = 0; i < count; i++)
            if (!found[i]) missing_indices[missing_count++] = indices[i];
        if (missing_count)
            retry_entries(r, missing_indices, missing_count, sql_ok);
    }
    return NULL;
}

int pgwt_pgss_resolver_start(struct pgwt_pgss_resolver **out,
                             struct pgwt_daemon *daemon,
                             struct pgwt_query_text_capture *query_text,
                             const char *postgres_binary,
                             pid_t postmaster_pid)
{
    if (out) *out = NULL;
    if (!out || !daemon || !query_text || !postgres_binary ||
        postmaster_pid <= 0) return -1;
    struct pgwt_pgss_resolver *r = calloc(1, sizeof(*r));
    if (!r) return -1;
    r->daemon = daemon;
    r->query_text = query_text;
    r->postmaster_pid = postmaster_pid;
    r->test_unthrottled = getenv("PGWT_TEST_PGSS_UNTHROTTLED") != NULL;
    if (r->test_unthrottled)
        fprintf(stderr, "WARN: PGWT_TEST_PGSS_UNTHROTTLED -- restoring "
                "pre-throttle 32-key pgss scans (TEST ONLY)\n");
    if (connection_info(r, postgres_binary) != 0 ||
        pthread_mutex_init(&r->mutex, NULL) != 0 ||
        pthread_cond_init(&r->cond, NULL) != 0) {
        free(r);
        return -1;
    }
    if (pthread_create(&r->thread, NULL, resolver_main, r) != 0) {
        pthread_cond_destroy(&r->cond);
        pthread_mutex_destroy(&r->mutex);
        free(r);
        return -1;
    }
    r->started = true;
    *out = r;
    return 0;
}

void pgwt_pgss_resolver_queue(struct pgwt_pgss_resolver *r,
                              const struct pgwt_query_text_key *key)
{
    if (!r || !key || !key->databaseid || !key->userid || !key->query_id)
        return;
    if (pgwt_qt_has_text(r->query_text, key))
        return;
    pthread_mutex_lock(&r->mutex);
    if (pgwt_qt_has_text(r->query_text, key)) {
        pthread_mutex_unlock(&r->mutex);
        return;
    }
    uint64_t now = now_ms();
    uint32_t idx = (uint32_t)(resolver_hash(key) & (PGWT_PGSS_QUEUE_SIZE - 1));
    int reclaim = -1;
    /* The first PGWT_PGSS_QUEUE_PROBES slots are the normal fast path. Under
     * throttle-induced PENDING pressure, continue through the bounded table so
     * a distant empty/reclaimable slot (or duplicate) is not reported as full. */
    for (int i = 0; i < PGWT_PGSS_QUEUE_SIZE; i++) {
        struct resolver_entry *e = &r->entries[idx];
        if (e->state != ENTRY_EMPTY && resolver_key_equal(&e->key, key)) {
            if (e->state == ENTRY_COOLDOWN && now >= e->retry_at_ms) {
                e->state = ENTRY_PENDING;
                e->attempts = 0;
                e->retry_at_ms = 0;
                counter_add(&r->daemon->counters.sampled_text_pending, 1);
                pthread_cond_signal(&r->cond);
            }
            pthread_mutex_unlock(&r->mutex);
            return;
        }
        if ((e->state == ENTRY_DONE || e->state == ENTRY_COOLDOWN) &&
            reclaim < 0)
            reclaim = (int)idx;
        if (e->state == ENTRY_EMPTY) {
            if (reclaim >= 0)
                e = &r->entries[reclaim];
            e->key = *key;
            e->state = ENTRY_PENDING;
            e->attempts = 0;
            e->retry_at_ms = 0;
            counter_add(&r->daemon->counters.sampled_text_pending, 1);
            pthread_cond_signal(&r->cond);
            pthread_mutex_unlock(&r->mutex);
            return;
        }
        idx = (idx + 1) & (PGWT_PGSS_QUEUE_SIZE - 1);
    }
    if (reclaim >= 0) {
        struct resolver_entry *e = &r->entries[reclaim];
        e->key = *key;
        e->state = ENTRY_PENDING;
        e->attempts = 0;
        e->retry_at_ms = 0;
        counter_add(&r->daemon->counters.sampled_text_pending, 1);
        pthread_cond_signal(&r->cond);
        pthread_mutex_unlock(&r->mutex);
        return;
    }
    counter_add(&r->daemon->counters.sampled_text_error_total, 1);
    pthread_mutex_unlock(&r->mutex);
}

void pgwt_pgss_resolver_stop(struct pgwt_pgss_resolver *r)
{
    if (!r) return;
    pthread_mutex_lock(&r->mutex);
    r->stop = true;
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->mutex);
    if (r->started) pthread_join(r->thread, NULL);
    pthread_cond_destroy(&r->cond);
    pthread_mutex_destroy(&r->mutex);
    free(r);
}

#ifdef PGWT_TEST
struct pgwt_pgss_resolver *pgwt_pgss_test_create(
    struct pgwt_daemon *daemon, struct pgwt_query_text_capture *query_text)
{
    struct pgwt_pgss_resolver *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->daemon = daemon;
    r->query_text = query_text;
    if (pthread_mutex_init(&r->mutex, NULL) != 0 ||
        pthread_cond_init(&r->cond, NULL) != 0) {
        free(r);
        return NULL;
    }
    return r;
}

int pgwt_pgss_test_cooldown(struct pgwt_pgss_resolver *r,
                            const struct pgwt_query_text_key *key,
                            bool immediately_due)
{
    if (!r || !key) return -1;
    pthread_mutex_lock(&r->mutex);
    for (int i = 0; i < PGWT_PGSS_QUEUE_SIZE; i++) {
        struct resolver_entry *e = &r->entries[i];
        if (e->state == ENTRY_PENDING && resolver_key_equal(&e->key, key)) {
            e->state = ENTRY_COOLDOWN;
            e->retry_at_ms = immediately_due
                ? 0 : now_ms() + PGWT_PGSS_REQUEUE_COOLDOWN_MS;
            counter_add(&r->daemon->counters.sampled_text_pending,
                        UINT64_MAX);
            pthread_mutex_unlock(&r->mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&r->mutex);
    return -1;
}

size_t pgwt_pgss_test_claim_batch(struct pgwt_pgss_resolver *r,
                                  uint64_t now, uint64_t *next_due)
{
    if (!r || !next_due) return 0;
    int indices[PGWT_PGSS_BATCH_MAX];
    struct pgwt_query_text_key keys[PGWT_PGSS_BATCH_MAX];
    pthread_mutex_lock(&r->mutex);
    size_t count = select_batch_locked(r, now, indices, keys, next_due);
    pthread_mutex_unlock(&r->mutex);
    return count;
}

size_t pgwt_pgss_test_take_batch(struct pgwt_pgss_resolver *r,
                                 uint64_t now, uint64_t *next_due,
                                 uint32_t *databaseid)
{
    if (databaseid) *databaseid = 0;
    if (!r || !next_due) return 0;
    int indices[PGWT_PGSS_BATCH_MAX];
    struct pgwt_query_text_key keys[PGWT_PGSS_BATCH_MAX];
    pthread_mutex_lock(&r->mutex);
    size_t count = select_batch_locked(r, now, indices, keys, next_due);
    if (count && databaseid) *databaseid = keys[0].databaseid;
    for (size_t i = 0; i < count; i++) {
        r->entries[indices[i]].state = ENTRY_DONE;
        counter_add(&r->daemon->counters.sampled_text_pending, UINT64_MAX);
    }
    pthread_mutex_unlock(&r->mutex);
    return count;
}

uint32_t pgwt_pgss_test_hash_slot(const struct pgwt_query_text_key *key)
{
    return key ? (uint32_t)(resolver_hash(key) &
                            (PGWT_PGSS_QUEUE_SIZE - 1)) : 0;
}
#endif

#endif
