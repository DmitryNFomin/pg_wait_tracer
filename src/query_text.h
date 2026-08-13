/* query_text.h — SQL query text capture from PgBackendStatus.st_activity_raw
 *
 * When the daemon sees a new query_id in the event stream, it reads the
 * backend's st_activity_raw from shared memory via /proc/<pid>/mem and
 * writes the {query_id, text} pair to query_texts.jsonl in the trace dir.
 *
 * st_activity_raw is a char* pointer in PgBackendStatus (PG18+).
 * Read sequence: MyBEEntry → PgBackendStatus* → st_activity_raw ptr → string.
 */
#ifndef PGWT_QUERY_TEXT_H
#define PGWT_QUERY_TEXT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

/* Hash table size for seen query_ids (must be power of 2) */
#define QT_HT_SIZE 4096

/* Max length of captured query text (matches PG's max track_activity_query_size) */
#define QT_MAX_TEXT (1024 * 1024)

/* DUR-4: query_texts.jsonl is compacted (deduplicated in place, atomically)
 * at startup only when it exceeds this size. Overridable for tests via the
 * PGWT_QT_COMPACT_BYTES environment variable. */
#define QT_COMPACT_THRESHOLD (32 * 1024 * 1024)

struct pgwt_query_text_capture {
    char          trace_dir[256];
    FILE         *fp;                    /* query_texts.jsonl file handle */

    /* Seen set: open-addressing hash table of query_ids */
    uint64_t      seen[QT_HT_SIZE];
    int           num_seen;
    bool          cap_logged;            /* DUR-10: seen-table-full logged once */

    /* Addresses for reading st_activity_raw */
    uint64_t      my_be_entry_addr;     /* MyBEEntry symbol VA */
    int           st_activity_offset;   /* offset of st_activity_raw in PgBackendStatus */

    gid_t         trace_gid;             /* group for the file, (gid_t)-1 = none */

    char         *read_buf;              /* reusable read buffer (QT_MAX_TEXT bytes) */

    bool          enabled;
    bool          verbose;
};

/* Initialize query text capture. Returns 0 on success.
 * DUR-4: query_texts.jsonl is opened in APPEND mode and existing entries are
 * loaded into the seen set (dedup-on-load) — retained trace files reference
 * these query_ids across daemon restarts, so the file must never be
 * truncated. trace_gid mirrors the trace files' group/permissions (DUR-10);
 * pass (gid_t)-1 for none. */
int pgwt_qt_init(struct pgwt_query_text_capture *qt,
                 const char *trace_dir,
                 uint64_t my_be_entry_addr,
                 int st_activity_offset,
                 gid_t trace_gid);

/* Check if query_id is new; if so, capture st_activity_raw from the backend.
 * Called from the event stream handler for each trace event with query_id != 0.
 * wall_ns is wall-clock timestamp for the JSONL record. */
void pgwt_qt_check(struct pgwt_query_text_capture *qt,
                   pid_t pid, uint64_t query_id, uint64_t wall_ns);

/* Store query text directly (captured by BPF from debug_query_string).
 * No /proc/pid/mem read needed — text is already available. */
void pgwt_qt_store(struct pgwt_query_text_capture *qt,
                   uint64_t query_id, const char *text, pid_t pid);

/* Parse W3C traceparent from SQL comment text (F-C1).
 * Extracts 32-hex trace_id and 16-hex parent_span_id.
 * Accepts both unquoted (traceparent=00-...) and quoted (traceparent='00-...') formats.
 * Returns 1 if a valid traceparent was found and parsed, 0 otherwise. */
int pgwt_parse_traceparent(const char *text,
                           char *trace_id, size_t trace_id_sz,
                           char *parent_span_id, size_t parent_span_id_sz);

/* Close the JSONL file. */
void pgwt_qt_close(struct pgwt_query_text_capture *qt);

#endif /* PGWT_QUERY_TEXT_H */

