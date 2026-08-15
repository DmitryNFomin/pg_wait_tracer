/* query_text.h -- source- and context-aware SQL text sidecar. */
#ifndef PGWT_QUERY_TEXT_H
#define PGWT_QUERY_TEXT_H

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define QT_HT_SIZE 4096
#define QT_MAX_TEXT (1024 * 1024)
#define QT_COMPACT_THRESHOLD (32 * 1024 * 1024)
#define QT_TEXT_INDEX_SIZE (QT_HT_SIZE * 4)
#define QT_QUALITY_SIZE QT_HT_SIZE
#define QT_QUALITY_MAX_BYTES (512 * 1024)
#define QT_SYNTH_QUEUE_SIZE 256

struct pgwt_query_text_key {
    uint32_t databaseid;
    uint32_t userid;
    uint64_t query_id;
};

enum pgwt_query_text_source {
    PGWT_QT_SOURCE_NONE = 0,
    PGWT_QT_SOURCE_PGSS = 1,       /* sampled, normalized */
    PGWT_QT_SOURCE_SYNTHETIC = 2,  /* PG13 sampled normalized activity */
    PGWT_QT_SOURCE_FULL = 3,       /* exact/full raw first-seen */
};

struct pgwt_query_text_seen {
    struct pgwt_query_text_key key;
    enum pgwt_query_text_source source;
    bool used;
};

/* Append-only during a capture lifetime. Publishing query_id last lets sampler
 * and resolver threads perform exact, lock-free persisted-text lookups. */
struct pgwt_query_text_index_entry {
    _Atomic uint64_t query_id;
    _Atomic uint32_t databaseid;
    _Atomic uint32_t userid;
    _Atomic unsigned source;
};

struct pgwt_qt_async;

struct pgwt_query_text_capture {
    char trace_dir[256];
    FILE *fp;
    /* Synthetic attribution quality is durable independently of sampled text
     * admission. It is fixed-size and evicts on the async worker; its sidecar
     * is compacted at a bounded size. */
    FILE *quality_fp;
    struct pgwt_query_text_seen *quality_seen;
    size_t quality_capacity;
    size_t quality_count;
    size_t quality_evict_cursor;
    size_t quality_bytes;
    bool quality_write_failed_logged;
    /* Raw and sampled admission are deliberately separate. A burst of pgss
     * resolutions can never consume the FULL/raw first-seen capacity. */
    struct pgwt_query_text_seen seen[QT_HT_SIZE]; /* FULL/raw only */
    struct pgwt_query_text_seen sampled_seen[QT_HT_SIZE];
    int num_seen;          /* FULL/raw first-seen contexts only */
    int num_sampled_seen;
    bool cap_logged;
    bool sampled_cap_logged;
    uint64_t my_be_entry_addr;
    int st_activity_offset;
    gid_t trace_gid;
    char *read_buf;
    bool enabled;
    bool verbose;
    pthread_mutex_t lock; /* pgss worker and exact lifecycle consumer */
    bool lock_initialized;
    struct pgwt_query_text_index_entry *text_index;
    struct pgwt_qt_async *synth_async;
};

const char *pgwt_qt_source_name(enum pgwt_query_text_source source);

int pgwt_qt_init(struct pgwt_query_text_capture *qt,
                 const char *trace_dir,
                 uint64_t my_be_entry_addr,
                 int st_activity_offset,
                 gid_t trace_gid);

/* General source-aware store. A higher-priority source for the same
 * (databaseid,userid,query_id) appends a replacement record; lower-priority
 * sampled text can therefore never consume the raw first-seen slot. */
int pgwt_qt_store_text(struct pgwt_query_text_capture *qt,
                       const struct pgwt_query_text_key *key,
                       enum pgwt_query_text_source source,
                       const char *text, pid_t pid);

/* Exact, lock-free lookup used on sampler/resolver hot paths. */
bool pgwt_qt_has_text(const struct pgwt_query_text_capture *qt,
                      const struct pgwt_query_text_key *key);

void pgwt_qt_store_full(struct pgwt_query_text_capture *qt,
                        const struct pgwt_query_text_key *key,
                        const char *text, pid_t pid);
void pgwt_qt_store_pgss(struct pgwt_query_text_capture *qt,
                        const struct pgwt_query_text_key *key,
                        const char *text);
void pgwt_qt_store_pgss_deferred(struct pgwt_query_text_capture *qt,
                                 const struct pgwt_query_text_key *key,
                                 const char *text);
void pgwt_qt_flush(struct pgwt_query_text_capture *qt);
void pgwt_qt_store_synthetic(struct pgwt_query_text_capture *qt,
                             const struct pgwt_query_text_key *key,
                             const char *text);

/* Compatibility wrappers: legacy records have context (0,0) and are full/raw. */
void pgwt_qt_check(struct pgwt_query_text_capture *qt,
                   pid_t pid, uint64_t query_id, uint64_t wall_ns);
void pgwt_qt_store(struct pgwt_query_text_capture *qt,
                   uint64_t query_id, const char *text, pid_t pid);

void pgwt_qt_close(struct pgwt_query_text_capture *qt);

#ifdef PGWT_TEST
void pgwt_qt_test_pause_synthetic(struct pgwt_query_text_capture *qt,
                                  bool paused);
void pgwt_qt_test_drain_synthetic(struct pgwt_query_text_capture *qt);
#endif

#endif
