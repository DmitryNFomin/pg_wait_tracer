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

struct synth_queue_entry {
    struct pgwt_query_text_key key;
    char text[PGWT_PG13_SYNTH_TEXT_MAX];
};

struct pgwt_qt_async {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct synth_queue_entry entries[QT_SYNTH_QUEUE_SIZE];
    size_t head;
    size_t count;
    uint64_t dropped;
    bool active;
    bool stop;
    bool started;
#ifdef PGWT_TEST
    bool paused;
#endif
    struct pgwt_query_text_capture *qt;
};

static void store_synthetic_now(struct pgwt_query_text_capture *qt,
                                const struct pgwt_query_text_key *key,
                                const char *text);
static void flush_synthetic_files(struct pgwt_query_text_capture *qt);

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

static int quality_check_or_insert(struct pgwt_query_text_capture *qt,
                                   const struct pgwt_query_text_key *key)
{
    if (!qt->quality_seen || qt->quality_capacity != QT_QUALITY_SIZE)
        return -1;
    size_t idx = (size_t)(key_hash(key) & (QT_QUALITY_SIZE - 1));
    for (size_t i = 0; i < QT_QUALITY_SIZE; i++) {
        struct pgwt_query_text_seen *entry = &qt->quality_seen[idx];
        if (entry->used && key_equal(&entry->key, key)) return 1;
        if (!entry->used) {
            entry->used = true;
            entry->key = *key;
            entry->source = PGWT_QT_SOURCE_SYNTHETIC;
            qt->quality_count++;
            return 0;
        }
        idx = (idx + 1) & (QT_QUALITY_SIZE - 1);
    }
    /* Fixed-size clock-slot eviction. This and any sidecar rewrite run only
     * on the synthetic worker, never on the 10 Hz sampler. */
    idx = qt->quality_evict_cursor++ & (QT_QUALITY_SIZE - 1);
    qt->quality_seen[idx].key = *key;
    qt->quality_seen[idx].source = PGWT_QT_SOURCE_SYNTHETIC;
    qt->quality_seen[idx].used = true;
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

static unsigned text_index_source(
    const struct pgwt_query_text_capture *qt,
    const struct pgwt_query_text_key *key)
{
    if (!qt || !qt->text_index || !key || !key->query_id)
        return PGWT_QT_SOURCE_NONE;
    size_t idx = (size_t)(key_hash(key) & (QT_TEXT_INDEX_SIZE - 1));
    for (size_t i = 0; i < QT_TEXT_INDEX_SIZE; i++) {
        const struct pgwt_query_text_index_entry *entry =
            &qt->text_index[idx];
        uint64_t query_id = atomic_load_explicit(&entry->query_id,
                                                 memory_order_acquire);
        if (!query_id) return PGWT_QT_SOURCE_NONE;
        if (query_id == key->query_id &&
            atomic_load_explicit(&entry->databaseid,
                                 memory_order_relaxed) == key->databaseid &&
            atomic_load_explicit(&entry->userid,
                                 memory_order_relaxed) == key->userid)
            return atomic_load_explicit(&entry->source,
                                        memory_order_acquire);
        idx = (idx + 1) & (QT_TEXT_INDEX_SIZE - 1);
    }
    return PGWT_QT_SOURCE_NONE;
}

bool pgwt_qt_has_text(const struct pgwt_query_text_capture *qt,
                      const struct pgwt_query_text_key *key)
{
    return text_index_source(qt, key) != PGWT_QT_SOURCE_NONE;
}

/* Writers serialize on qt->lock. query_id is the publication marker. */
static int text_index_insert(struct pgwt_query_text_capture *qt,
                             const struct pgwt_query_text_key *key,
                             enum pgwt_query_text_source source)
{
    if (!qt->text_index) return -1;
    size_t idx = (size_t)(key_hash(key) & (QT_TEXT_INDEX_SIZE - 1));
    for (size_t i = 0; i < QT_TEXT_INDEX_SIZE; i++) {
        struct pgwt_query_text_index_entry *entry = &qt->text_index[idx];
        uint64_t query_id = atomic_load_explicit(&entry->query_id,
                                                 memory_order_acquire);
        if (!query_id) {
            atomic_store_explicit(&entry->databaseid, key->databaseid,
                                  memory_order_relaxed);
            atomic_store_explicit(&entry->userid, key->userid,
                                  memory_order_relaxed);
            atomic_store_explicit(&entry->source, (unsigned)source,
                                  memory_order_relaxed);
            atomic_store_explicit(&entry->query_id, key->query_id,
                                  memory_order_release);
            return 0;
        }
        if (query_id == key->query_id &&
            atomic_load_explicit(&entry->databaseid,
                                 memory_order_relaxed) == key->databaseid &&
            atomic_load_explicit(&entry->userid,
                                 memory_order_relaxed) == key->userid) {
            unsigned old = atomic_load_explicit(&entry->source,
                                                memory_order_relaxed);
            if ((unsigned)source > old)
                atomic_store_explicit(&entry->source, (unsigned)source,
                                      memory_order_release);
            return 0;
        }
        idx = (idx + 1) & (QT_TEXT_INDEX_SIZE - 1);
    }
    return -1;
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
    int fd = open(path, O_RDONLY | O_CLOEXEC);
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
    FILE *in = fopen(path, "re");
    if (!in) return;
    FILE *out = NULL;
    char tmp_path[600];
    if (compact) {
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
        out = fopen(tmp_path, "we");
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
        (void)text_index_insert(qt, &key, source);
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
    struct stat st;
    if (stat(path, &st) == 0)
        qt->quality_bytes = (size_t)st.st_size;
    FILE *fp = fopen(path, "re");
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

static int write_quality_record(FILE *fp,
                                const struct pgwt_query_text_key *key)
{
    return fprintf(fp,
                   "{\"q\":\"%lld\",\"d\":%u,\"u\":%u,"
                   "\"s\":\"synthetic\",\"v\":\"%s\"}\n",
                   (long long)(int64_t)key->query_id, key->databaseid,
                   key->userid, PGWT_PG13_SYNTH_VERSION);
}

static void compact_quality_locked(struct pgwt_query_text_capture *qt)
{
    if (!qt->quality_fp || qt->quality_bytes <= QT_QUALITY_MAX_BYTES)
        return;
    char path[512], tmp_path[520];
    snprintf(path, sizeof(path), "%s/query_sources.jsonl", qt->trace_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *out = fopen(tmp_path, "we");
    if (!out) return;
    size_t bytes = 0;
    bool ok = true;
    for (size_t i = 0; i < qt->quality_capacity; i++) {
        if (!qt->quality_seen[i].used) continue;
        int n = write_quality_record(out, &qt->quality_seen[i].key);
        if (n < 0) { ok = false; break; }
        bytes += (size_t)n;
    }
    if (ok && (fflush(out) != 0 || fsync(fileno(out)) != 0)) ok = false;
    if (ok && qt->trace_gid != (gid_t)-1) {
        fchown(fileno(out), (uid_t)-1, qt->trace_gid);
        fchmod(fileno(out), 0640);
    }
    if (fclose(out) != 0) ok = false;
    if (!ok || rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return;
    }
    FILE *replacement = fopen(path, "ae");
    if (!replacement) {
        fclose(qt->quality_fp);
        qt->quality_fp = NULL;
        return;
    }
    fclose(qt->quality_fp);
    qt->quality_fp = replacement;
    qt->quality_bytes = bytes;
}

static void *synthetic_worker_main(void *arg)
{
    struct pgwt_qt_async *async = arg;
    size_t unflushed = 0;
    struct timespec flush_at = {0};
    for (;;) {
        struct synth_queue_entry item;
        bool timed_flush = false;
        pthread_mutex_lock(&async->mutex);
        while (!async->stop &&
               (async->count == 0
#ifdef PGWT_TEST
                || async->paused
#endif
               )) {
            if (unflushed &&
#ifdef PGWT_TEST
                !async->paused &&
#endif
                async->count == 0) {
                int rc = pthread_cond_timedwait(&async->cond, &async->mutex,
                                                &flush_at);
                if (rc == ETIMEDOUT) {
                    timed_flush = true;
                    break;
                }
            } else {
                pthread_cond_wait(&async->cond, &async->mutex);
            }
        }
        if (unflushed && async->count == 0 &&
            (timed_flush || async->stop)) {
            async->active = true;
            pthread_mutex_unlock(&async->mutex);
            flush_synthetic_files(async->qt);
            unflushed = 0;
            memset(&flush_at, 0, sizeof(flush_at));
            pthread_mutex_lock(&async->mutex);
            async->active = false;
            pthread_cond_broadcast(&async->cond);
            pthread_mutex_unlock(&async->mutex);
            continue;
        }
        if (async->stop && async->count == 0) {
            pthread_mutex_unlock(&async->mutex);
            break;
        }
        item = async->entries[async->head];
        async->head = (async->head + 1) & (QT_SYNTH_QUEUE_SIZE - 1);
        async->count--;
        async->active = true;
        pthread_mutex_unlock(&async->mutex);

        store_synthetic_now(async->qt, &item.key, item.text);
        unflushed++;
        if (unflushed == 1) {
            clock_gettime(CLOCK_REALTIME, &flush_at);
            flush_at.tv_sec++;
        }

        bool flush_now = unflushed >= 32;
        if (flush_now) {
            flush_synthetic_files(async->qt);
            unflushed = 0;
            memset(&flush_at, 0, sizeof(flush_at));
        }

        pthread_mutex_lock(&async->mutex);
        async->active = false;
        pthread_cond_broadcast(&async->cond);
        pthread_mutex_unlock(&async->mutex);
    }
    return NULL;
}

static int synthetic_async_start(struct pgwt_query_text_capture *qt)
{
    struct pgwt_qt_async *async = calloc(1, sizeof(*async));
    if (!async) return -1;
    async->qt = qt;
    if (pthread_mutex_init(&async->mutex, NULL) != 0) {
        free(async);
        return -1;
    }
    if (pthread_cond_init(&async->cond, NULL) != 0) {
        pthread_mutex_destroy(&async->mutex);
        free(async);
        return -1;
    }
    if (pthread_create(&async->thread, NULL, synthetic_worker_main, async) != 0) {
        pthread_cond_destroy(&async->cond);
        pthread_mutex_destroy(&async->mutex);
        free(async);
        return -1;
    }
    async->started = true;
    qt->synth_async = async;
    return 0;
}

static void synthetic_async_stop(struct pgwt_query_text_capture *qt)
{
    struct pgwt_qt_async *async = qt->synth_async;
    if (!async) return;
    pthread_mutex_lock(&async->mutex);
    async->stop = true;
    pthread_cond_broadcast(&async->cond);
    pthread_mutex_unlock(&async->mutex);
    if (async->started) pthread_join(async->thread, NULL);
    if (async->dropped)
        fprintf(stderr, "WARN: PG13 synthetic text queue dropped %llu "
                "records before persistence\n",
                (unsigned long long)async->dropped);
    pthread_cond_destroy(&async->cond);
    pthread_mutex_destroy(&async->mutex);
    free(async);
    qt->synth_async = NULL;
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
    qt->text_index = calloc(QT_TEXT_INDEX_SIZE, sizeof(*qt->text_index));
    qt->quality_seen = calloc(QT_QUALITY_SIZE, sizeof(*qt->quality_seen));
    if (!qt->read_buf || !qt->text_index || !qt->quality_seen)
        goto fail;
    qt->quality_capacity = QT_QUALITY_SIZE;
    for (size_t i = 0; i < QT_TEXT_INDEX_SIZE; i++) {
        atomic_init(&qt->text_index[i].query_id, 0);
        atomic_init(&qt->text_index[i].databaseid, 0);
        atomic_init(&qt->text_index[i].userid, 0);
        atomic_init(&qt->text_index[i].source, PGWT_QT_SOURCE_NONE);
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/query_texts.jsonl", trace_dir);
    load_existing(qt, path);
    qt->fp = fopen(path, "ae");
    if (!qt->fp) {
        fprintf(stderr, "WARN: cannot open %s: %s\n", path, strerror(errno));
    } else {
        qt->enabled = true;
    }
    char quality_path[512];
    snprintf(quality_path, sizeof(quality_path), "%s/query_sources.jsonl",
             trace_dir);
    load_existing_quality(qt, quality_path);
    qt->quality_fp = fopen(quality_path, "ae");
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
    compact_quality_locked(qt);
    if (qt->fp || qt->quality_fp) {
        if (synthetic_async_start(qt) != 0)
            fprintf(stderr, "WARN: PG13 synthetic query-text worker is "
                    "unavailable\n");
        return 0;
    }

fail:
    if (qt->fp) fclose(qt->fp);
    if (qt->quality_fp) fclose(qt->quality_fp);
    qt->fp = qt->quality_fp = NULL;
    free(qt->quality_seen);
    qt->quality_seen = NULL;
    qt->quality_capacity = qt->quality_count = 0;
    free(qt->text_index);
    qt->text_index = NULL;
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
                             const char *text, pid_t pid, bool flush)
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
    if ((flush && fflush(qt->fp) != 0) || ferror(qt->fp))
        return -1;
    (void)text_index_insert(qt, key, source);
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
    int rc = store_text_locked(qt, key, source, text, pid, true);
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

void pgwt_qt_store_pgss_deferred(struct pgwt_query_text_capture *qt,
                                 const struct pgwt_query_text_key *key,
                                 const char *text)
{
    if (!qt || !key || !key->query_id || !text || !text[0] ||
        !qt->lock_initialized)
        return;
    pthread_mutex_lock(&qt->lock);
    (void)store_text_locked(qt, key, PGWT_QT_SOURCE_PGSS, text, 0, false);
    pthread_mutex_unlock(&qt->lock);
}

void pgwt_qt_flush(struct pgwt_query_text_capture *qt)
{
    if (!qt || !qt->lock_initialized) return;
    pthread_mutex_lock(&qt->lock);
    if (qt->fp) (void)fflush(qt->fp);
    pthread_mutex_unlock(&qt->lock);
}

void pgwt_qt_store_synthetic(struct pgwt_query_text_capture *qt,
                             const struct pgwt_query_text_key *key,
                             const char *text)
{
    if (!qt || !key || !key->query_id || !text || !text[0] ||
        !qt->synth_async ||
        text_index_source(qt, key) >= PGWT_QT_SOURCE_SYNTHETIC)
        return;
    struct pgwt_qt_async *async = qt->synth_async;
    pthread_mutex_lock(&async->mutex);
    if (text_index_source(qt, key) >= PGWT_QT_SOURCE_SYNTHETIC) {
        pthread_mutex_unlock(&async->mutex);
        return;
    }
    if (async->count == QT_SYNTH_QUEUE_SIZE) {
        async->dropped++;
        pthread_mutex_unlock(&async->mutex);
        return;
    }
    size_t tail = (async->head + async->count) &
                  (QT_SYNTH_QUEUE_SIZE - 1);
    async->entries[tail].key = *key;
    snprintf(async->entries[tail].text, sizeof(async->entries[tail].text),
             "%s", text);
    async->count++;
    pthread_cond_signal(&async->cond);
    pthread_mutex_unlock(&async->mutex);
}

static void store_synthetic_now(struct pgwt_query_text_capture *qt,
                                const struct pgwt_query_text_key *key,
                                const char *text)
{
    pthread_mutex_lock(&qt->lock);
    if (qt->quality_fp) {
        int rc = quality_check_or_insert(qt, key);
        if (rc == 0) {
            int n = write_quality_record(qt->quality_fp, key);
            if (n > 0) qt->quality_bytes += (size_t)n;
            if (n < 0 || ferror(qt->quality_fp)) {
                if (!qt->quality_write_failed_logged) {
                    qt->quality_write_failed_logged = true;
                    fprintf(stderr, "WARN: cannot persist PG13 synthetic "
                            "attribution quality: %s\n", strerror(errno));
                }
            }
        }
    }
    (void)store_text_locked(qt, key, PGWT_QT_SOURCE_SYNTHETIC, text, 0,
                            false);
    pthread_mutex_unlock(&qt->lock);
}

static void flush_synthetic_files(struct pgwt_query_text_capture *qt)
{
    pthread_mutex_lock(&qt->lock);
    if (qt->fp) (void)fflush(qt->fp);
    if (qt->quality_fp) {
        if (fflush(qt->quality_fp) != 0 || ferror(qt->quality_fp)) {
            if (!qt->quality_write_failed_logged) {
                qt->quality_write_failed_logged = true;
                fprintf(stderr, "WARN: cannot persist PG13 synthetic "
                        "attribution quality: %s\n", strerror(errno));
            }
        } else {
            compact_quality_locked(qt);
        }
    }
    pthread_mutex_unlock(&qt->lock);
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
    synthetic_async_stop(qt);
    if (qt->lock_initialized) pthread_mutex_lock(&qt->lock);
    qt->enabled = false;
    if (qt->fp) fclose(qt->fp);
    if (qt->quality_fp) fclose(qt->quality_fp);
    qt->fp = NULL;
    qt->quality_fp = NULL;
    free(qt->quality_seen);
    qt->quality_seen = NULL;
    qt->quality_capacity = qt->quality_count = 0;
    free(qt->text_index);
    qt->text_index = NULL;
    free(qt->read_buf);
    qt->read_buf = NULL;
    if (qt->lock_initialized) {
        pthread_mutex_unlock(&qt->lock);
        pthread_mutex_destroy(&qt->lock);
        qt->lock_initialized = false;
    }
}

#ifdef PGWT_TEST
void pgwt_qt_test_pause_synthetic(struct pgwt_query_text_capture *qt,
                                  bool paused)
{
    if (!qt || !qt->synth_async) return;
    pthread_mutex_lock(&qt->synth_async->mutex);
    qt->synth_async->paused = paused;
    pthread_cond_broadcast(&qt->synth_async->cond);
    pthread_mutex_unlock(&qt->synth_async->mutex);
}

void pgwt_qt_test_drain_synthetic(struct pgwt_query_text_capture *qt)
{
    if (!qt || !qt->synth_async) return;
    pthread_mutex_lock(&qt->synth_async->mutex);
    while (qt->synth_async->count || qt->synth_async->active)
        pthread_cond_wait(&qt->synth_async->cond,
                          &qt->synth_async->mutex);
    pthread_mutex_unlock(&qt->synth_async->mutex);
    flush_synthetic_files(qt);
}
#endif
