/* query_text.c -- source- and context-aware SQL text sidecar. */
#include "query_text.h"
#include "synthetic_query.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t hash64(uint64_t x)
{
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

static uint64_t key_hash(const struct pgwt_query_text_key *key)
{
    return hash64(key->query_id ^ ((uint64_t)key->databaseid << 32) ^
                  (uint64_t)key->userid);
}

static bool key_equal(const struct pgwt_query_text_key *a,
                      const struct pgwt_query_text_key *b)
{
    return a->query_id == b->query_id &&
           a->databaseid == b->databaseid && a->userid == b->userid;
}

static int quality_rehash(struct pgwt_query_text_capture *qt, size_t capacity)
{
    struct pgwt_query_text_seen *table = calloc(capacity, sizeof(*table));
    if (!table) return -1;
    for (size_t i = 0; i < qt->quality_capacity; i++) {
        struct pgwt_query_text_seen *old = &qt->quality_seen[i];
        if (!old->used) continue;
        size_t idx = (size_t)(key_hash(&old->key) & (capacity - 1));
        while (table[idx].used) idx = (idx + 1) & (capacity - 1);
        table[idx] = *old;
    }
    free(qt->quality_seen);
    qt->quality_seen = table;
    qt->quality_capacity = capacity;
    return 0;
}

/* 0=new, 1=already present, -1=allocation failure. */
static int quality_check_or_insert(struct pgwt_query_text_capture *qt,
                                   const struct pgwt_query_text_key *key)
{
    if (!qt->quality_capacity) {
        if (quality_rehash(qt, 1024) != 0) return -1;
    } else if ((qt->quality_count + 1) * 10 >= qt->quality_capacity * 7) {
        if (qt->quality_capacity > SIZE_MAX / 2 ||
            quality_rehash(qt, qt->quality_capacity * 2) != 0)
            return -1;
    }
    size_t idx = (size_t)(key_hash(key) & (qt->quality_capacity - 1));
    while (qt->quality_seen[idx].used) {
        if (key_equal(&qt->quality_seen[idx].key, key)) return 1;
        idx = (idx + 1) & (qt->quality_capacity - 1);
    }
    qt->quality_seen[idx].used = true;
    qt->quality_seen[idx].key = *key;
    qt->quality_seen[idx].source = PGWT_QT_SOURCE_SYNTHETIC;
    qt->quality_count++;
    return 0;
}

/* 0=new or priority upgrade, 1=same/lower priority, 2=table full. */
static int table_check_or_insert(struct pgwt_query_text_seen *table,
                                 int *count,
                                 const struct pgwt_query_text_key *key,
                                 enum pgwt_query_text_source source)
{
    uint32_t idx = (uint32_t)(key_hash(key) & (QT_HT_SIZE - 1));
    for (int i = 0; i < QT_HT_SIZE; i++) {
        struct pgwt_query_text_seen *entry = &table[idx];
        if (entry->used && key_equal(&entry->key, key)) {
            if (source > entry->source) {
                entry->source = source;
                return 0;
            }
            return 1;
        }
        if (!entry->used) {
            entry->used = true;
            entry->key = *key;
            entry->source = source;
            (*count)++;
            return 0;
        }
        idx = (idx + 1) & (QT_HT_SIZE - 1);
    }
    return 2;
}

static bool table_contains(const struct pgwt_query_text_seen *table,
                           const struct pgwt_query_text_key *key)
{
    uint32_t idx = (uint32_t)(key_hash(key) & (QT_HT_SIZE - 1));
    for (int i = 0; i < QT_HT_SIZE; i++) {
        const struct pgwt_query_text_seen *entry = &table[idx];
        if (!entry->used) return false;
        if (key_equal(&entry->key, key)) return true;
        idx = (idx + 1) & (QT_HT_SIZE - 1);
    }
    return false;
}

static int seen_check_or_insert(struct pgwt_query_text_capture *qt,
                                const struct pgwt_query_text_key *key,
                                enum pgwt_query_text_source source)
{
    if (source == PGWT_QT_SOURCE_FULL)
        return table_check_or_insert(qt->seen, &qt->num_seen, key, source);
    /* Once raw exists, every sampled source is permanently lower priority. */
    if (table_contains(qt->seen, key))
        return 1;
    return table_check_or_insert(qt->sampled_seen, &qt->num_sampled_seen,
                                 key, source);
}

static void qt_log_cap_once(struct pgwt_query_text_capture *qt)
{
    if (qt->cap_logged)
        return;
    qt->cap_logged = true;
    fprintf(stderr,
            "WARN: query-text context table is FULL (%d unique keys) -- "
            "text for NEW query contexts will no longer be captured until "
            "the daemon restarts (ids and waits are unaffected)\n",
            QT_HT_SIZE);
}

static void qt_log_sampled_cap_once(struct pgwt_query_text_capture *qt)
{
    if (qt->sampled_cap_logged)
        return;
    qt->sampled_cap_logged = true;
    fprintf(stderr,
            "WARN: sampled query-text table is FULL (%d unique contexts) -- "
            "new sampled text is unavailable; FULL/raw first-seen capacity "
            "is reserved and unaffected\n", QT_HT_SIZE);
}

const char *pgwt_qt_source_name(enum pgwt_query_text_source source)
{
    switch (source) {
    case PGWT_QT_SOURCE_PGSS: return "pgss";
    case PGWT_QT_SOURCE_SYNTHETIC: return "synthetic";
    case PGWT_QT_SOURCE_FULL: return "full";
    default: return "unknown";
    }
}

static enum pgwt_query_text_source source_from_name(const char *name)
{
    if (!name || !name[0]) return PGWT_QT_SOURCE_FULL; /* legacy raw */
    if (strcmp(name, "pgss") == 0) return PGWT_QT_SOURCE_PGSS;
    if (strcmp(name, "synthetic") == 0) return PGWT_QT_SOURCE_SYNTHETIC;
    if (strcmp(name, "full") == 0) return PGWT_QT_SOURCE_FULL;
    return PGWT_QT_SOURCE_NONE;
}

static int read_activity(const struct pgwt_query_text_capture *qt,
                         pid_t pid, char *out, int out_size)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    uint64_t be_ptr = 0, activity_ptr = 0;
    if (pread(fd, &be_ptr, sizeof(be_ptr), qt->my_be_entry_addr) !=
            (ssize_t)sizeof(be_ptr) || !be_ptr ||
        pread(fd, &activity_ptr, sizeof(activity_ptr),
              be_ptr + qt->st_activity_offset) !=
            (ssize_t)sizeof(activity_ptr) || !activity_ptr) {
        close(fd);
        return 0;
    }
    memset(out, 0, (size_t)out_size);
    ssize_t n = pread(fd, out, (size_t)out_size - 1, activity_ptr);
    close(fd);
    if (n <= 0) return 0;
    int len = (int)strnlen(out, (size_t)n);
    out[len] = '\0';
    return len;
}

static void write_json_string(FILE *fp, const char *s, size_t len)
{
    fputc('"', fp);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\r': fputs("\\r", fp); break;
        case '\t': fputs("\\t", fp); break;
        default:
            if (c < 0x20) fprintf(fp, "\\u%04x", c);
            else fputc(c, fp);
        }
    }
    fputc('"', fp);
}

static int parse_uint_field(const char *line, const char *field, uint32_t *out)
{
    const char *p = strstr(line, field);
    if (!p) { *out = 0; return 0; }
    p += strlen(field);
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(p, &end, 10);
    if (errno || end == p || value > UINT32_MAX) return -1;
    *out = (uint32_t)value;
    return 0;
}

static int parse_line_key(const char *line, struct pgwt_query_text_key *key,
                          enum pgwt_query_text_source *source)
{
    long long signed_qid;
    if (sscanf(line, "{\"q\":\"%lld\"", &signed_qid) != 1)
        return -1;
    memset(key, 0, sizeof(*key));
    key->query_id = (uint64_t)(int64_t)signed_qid;
    if (!key->query_id || parse_uint_field(line, "\"d\":", &key->databaseid) ||
        parse_uint_field(line, "\"u\":", &key->userid))
        return -1;
    const char *p = strstr(line, "\"s\":\"");
    char name[16] = "";
    if (p) {
        p += strlen("\"s\":\"");
        size_t n = strcspn(p, "\"");
        if (n >= sizeof(name)) return -1;
        memcpy(name, p, n);
        name[n] = '\0';
    }
    *source = source_from_name(name);
    return *source == PGWT_QT_SOURCE_NONE ? -1 : 0;
}

static void load_existing(struct pgwt_query_text_capture *qt, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    long threshold = QT_COMPACT_THRESHOLD;
    const char *env = getenv("PGWT_QT_COMPACT_BYTES");
    if (env && atol(env) > 0) threshold = atol(env);
    bool compact = st.st_size > threshold;
    FILE *in = fopen(path, "r");
    if (!in) return;
    FILE *out = NULL;
    char tmp_path[600];
    if (compact) {
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
        out = fopen(tmp_path, "w");
        if (!out) compact = false;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int loaded = 0, dropped = 0;
    while ((len = getline(&line, &cap, in)) > 0) {
        struct pgwt_query_text_key key;
        enum pgwt_query_text_source source;
        if (parse_line_key(line, &key, &source) != 0) {
            dropped++;
            continue;
        }
        int rc = seen_check_or_insert(qt, &key, source);
        if (rc == 0) loaded++;
        else if (rc == 2) {
            if (source == PGWT_QT_SOURCE_FULL) qt_log_cap_once(qt);
            else qt_log_sampled_cap_once(qt);
        }
        /* Keep the first record and any later source upgrade. That preserves
         * the sampled record plus the raw replacement needed for precedence. */
        if (compact && (rc == 0 || rc == 2) &&
            fwrite(line, 1, (size_t)len, out) != (size_t)len) {
            fclose(out);
            out = NULL;
            unlink(tmp_path);
            compact = false;
        }
    }
    free(line);
    fclose(in);
    if (compact && out) {
        fflush(out);
        fsync(fileno(out));
        fclose(out);
        if (rename(tmp_path, path) != 0) unlink(tmp_path);
    }
    if (qt->verbose)
        fprintf(stderr, "INFO: loaded %d query-text context/source records "
                "(%d unreadable lines skipped)%s\n", loaded, dropped,
                (qt->cap_logged || qt->sampled_cap_logged)
                    ? " -- a source table is full" : "");
}

static void load_existing_quality(struct pgwt_query_text_capture *qt,
                                  const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, fp) > 0) {
        struct pgwt_query_text_key key;
        enum pgwt_query_text_source source;
        if (parse_line_key(line, &key, &source) == 0 &&
            source == PGWT_QT_SOURCE_SYNTHETIC)
            (void)quality_check_or_insert(qt, &key);
    }
    free(line);
    fclose(fp);
}

int pgwt_qt_init(struct pgwt_query_text_capture *qt, const char *trace_dir,
                 uint64_t my_be_entry_addr, int st_activity_offset,
                 gid_t trace_gid)
{
    memset(qt, 0, sizeof(*qt));
    if (pthread_mutex_init(&qt->lock, NULL) != 0) return -1;
    qt->lock_initialized = true;
    snprintf(qt->trace_dir, sizeof(qt->trace_dir), "%s", trace_dir);
    qt->my_be_entry_addr = my_be_entry_addr;
    qt->st_activity_offset = st_activity_offset;
    qt->trace_gid = trace_gid;
    qt->read_buf = malloc(QT_MAX_TEXT);
    char path[512];
    snprintf(path, sizeof(path), "%s/query_texts.jsonl", trace_dir);
    load_existing(qt, path);
    qt->fp = fopen(path, "a");
    if (!qt->fp) {
        fprintf(stderr, "WARN: cannot open %s: %s\n", path, strerror(errno));
    } else {
        qt->enabled = true;
    }
    char quality_path[512];
    snprintf(quality_path, sizeof(quality_path), "%s/query_sources.jsonl",
             trace_dir);
    load_existing_quality(qt, quality_path);
    qt->quality_fp = fopen(quality_path, "a");
    if (!qt->quality_fp) {
        fprintf(stderr, "WARN: cannot open %s: %s\n", quality_path,
                strerror(errno));
    }
    if (qt->trace_gid != (gid_t)-1) {
        if (qt->fp) {
            fchown(fileno(qt->fp), (uid_t)-1, qt->trace_gid);
            fchmod(fileno(qt->fp), 0640);
        }
        if (qt->quality_fp) {
            fchown(fileno(qt->quality_fp), (uid_t)-1, qt->trace_gid);
            fchmod(fileno(qt->quality_fp), 0640);
        }
    }
    if (qt->fp || qt->quality_fp)
        return 0;
    if (qt->fp) fclose(qt->fp);
    if (qt->quality_fp) fclose(qt->quality_fp);
    qt->fp = qt->quality_fp = NULL;
    free(qt->quality_seen);
    qt->quality_seen = NULL;
    qt->quality_capacity = qt->quality_count = 0;
    free(qt->read_buf);
    qt->read_buf = NULL;
    qt->enabled = false;
    pthread_mutex_destroy(&qt->lock);
    qt->lock_initialized = false;
    return -1;
}

static int store_text_locked(struct pgwt_query_text_capture *qt,
                             const struct pgwt_query_text_key *key,
                             enum pgwt_query_text_source source,
                             const char *text, pid_t pid)
{
    if (!qt->enabled || !qt->fp) return -1;
    int rc = seen_check_or_insert(qt, key, source);
    if (rc != 0) {
        if (rc == 2) {
            if (source == PGWT_QT_SOURCE_FULL) qt_log_cap_once(qt);
            else qt_log_sampled_cap_once(qt);
        }
        return rc == 1 ? 1 : -1;
    }
    size_t len = strnlen(text, QT_MAX_TEXT - 1);
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    uint64_t wall_ns = (uint64_t)wall.tv_sec * UINT64_C(1000000000) +
                       (uint64_t)wall.tv_nsec;
    fprintf(qt->fp,
            "{\"q\":\"%lld\",\"d\":%u,\"u\":%u,\"s\":\"%s\"",
            (long long)(int64_t)key->query_id, key->databaseid, key->userid,
            pgwt_qt_source_name(source));
    if (source == PGWT_QT_SOURCE_SYNTHETIC)
        fprintf(qt->fp, ",\"v\":\"%s\"", PGWT_PG13_SYNTH_VERSION);
    fputs(",\"t\":", qt->fp);
    write_json_string(qt->fp, text, len);
    fprintf(qt->fp, ",\"ts\":%llu}\n", (unsigned long long)wall_ns);
    fflush(qt->fp);
    if (qt->verbose)
        fprintf(stderr, "INFO: stored %s query text for (%u,%u,%lld) pid=%d\n",
                pgwt_qt_source_name(source), key->databaseid, key->userid,
                (long long)(int64_t)key->query_id, pid);
    return 0;
}

int pgwt_qt_store_text(struct pgwt_query_text_capture *qt,
                       const struct pgwt_query_text_key *key,
                       enum pgwt_query_text_source source,
                       const char *text, pid_t pid)
{
    if (!qt || !key || !key->query_id || source == PGWT_QT_SOURCE_NONE ||
        !text || !text[0] || !qt->lock_initialized) return -1;
    pthread_mutex_lock(&qt->lock);
    int rc = store_text_locked(qt, key, source, text, pid);
    pthread_mutex_unlock(&qt->lock);
    return rc;
}

void pgwt_qt_store_full(struct pgwt_query_text_capture *qt,
                        const struct pgwt_query_text_key *key,
                        const char *text, pid_t pid)
{
    (void)pgwt_qt_store_text(qt, key, PGWT_QT_SOURCE_FULL, text, pid);
}

void pgwt_qt_store_pgss(struct pgwt_query_text_capture *qt,
                        const struct pgwt_query_text_key *key,
                        const char *text)
{
    (void)pgwt_qt_store_text(qt, key, PGWT_QT_SOURCE_PGSS, text, 0);
}

void pgwt_qt_store_synthetic(struct pgwt_query_text_capture *qt,
                             const struct pgwt_query_text_key *key,
                             const char *text)
{
    if (qt && key && key->query_id && text && text[0] &&
        qt->lock_initialized) {
        pthread_mutex_lock(&qt->lock);
        if (qt->quality_fp) {
            int rc = quality_check_or_insert(qt, key);
            if (rc == 0) {
                fprintf(qt->quality_fp,
                        "{\"q\":\"%lld\",\"d\":%u,\"u\":%u,"
                        "\"s\":\"synthetic\",\"v\":\"%s\"}\n",
                        (long long)(int64_t)key->query_id, key->databaseid,
                        key->userid, PGWT_PG13_SYNTH_VERSION);
                fflush(qt->quality_fp);
                if (ferror(qt->quality_fp) &&
                    !qt->quality_write_failed_logged) {
                    qt->quality_write_failed_logged = true;
                    fprintf(stderr, "WARN: cannot persist PG13 synthetic "
                            "attribution quality: %s\n", strerror(errno));
                }
            }
        }
        /* Quality and normalized text are one critical section. Shutdown can
         * no longer close the text sink between the durable quality record
         * and its optional text record. */
        (void)store_text_locked(qt, key, PGWT_QT_SOURCE_SYNTHETIC, text, 0);
        pthread_mutex_unlock(&qt->lock);
    }
}

void pgwt_qt_check(struct pgwt_query_text_capture *qt, pid_t pid,
                   uint64_t query_id, uint64_t wall_ns)
{
    (void)wall_ns;
    if (!qt || !query_id || !qt->read_buf) return;
    int len = read_activity(qt, pid, qt->read_buf, QT_MAX_TEXT);
    if (len <= 0) return;
    struct pgwt_query_text_key key = {.query_id = query_id};
    pgwt_qt_store_full(qt, &key, qt->read_buf, pid);
}

void pgwt_qt_store(struct pgwt_query_text_capture *qt, uint64_t query_id,
                   const char *text, pid_t pid)
{
    struct pgwt_query_text_key key = {.query_id = query_id};
    pgwt_qt_store_full(qt, &key, text, pid);
}

void pgwt_qt_close(struct pgwt_query_text_capture *qt)
{
    if (!qt) return;
    if (qt->lock_initialized) pthread_mutex_lock(&qt->lock);
    qt->enabled = false;
    if (qt->fp) fclose(qt->fp);
    if (qt->quality_fp) fclose(qt->quality_fp);
    qt->fp = NULL;
    qt->quality_fp = NULL;
    free(qt->quality_seen);
    qt->quality_seen = NULL;
    qt->quality_capacity = qt->quality_count = 0;
    free(qt->read_buf);
    qt->read_buf = NULL;
    if (qt->lock_initialized) {
        pthread_mutex_unlock(&qt->lock);
        pthread_mutex_destroy(&qt->lock);
        qt->lock_initialized = false;
    }
}
