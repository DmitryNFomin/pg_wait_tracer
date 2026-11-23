/* timeline_summary.h — per-event and per-query summary of timeline mode.
 *
 * One aggregation, two producers that must agree (RULE 2: two views of one
 * quantity): the live path feeds every record the assembler emits, the file
 * path re-reads the .pgwtl segments. pgwt_tl_summary_equal compares them.
 *
 * Aggregation rules (docs/TIMELINE_FORMAT.md):
 *   - only WAIT records are aggregated; other kinds are counted in totals;
 *   - a ZERO_MARKER wait counts in n_zero only: never in n, total_ns,
 *     max_ns or syscalls;
 *   - total_ns and max_ns use end_ns - start_ns (TRUNC_* waits included,
 *     with their truncated duration);
 *   - per query rows key on (query_id, qid_flags), qid_flags being the
 *     record's QID_UNAVAILABLE | IN_CMD_NO_QID | PG13_SYNTH_QID bits.
 *
 * Output: timeline-summary.json (cJSON) and a text table at stop; event
 * names via pgwt_event_full_name (src/wait_event.c).
 *
 * Status: S0 stub with the FROZEN prototypes; S2 implements.
 */
#ifndef PGWT_TIMELINE_SUMMARY_H
#define PGWT_TIMELINE_SUMMARY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "timeline_format.h"

struct pgwt_tl_event_row {
    uint32_t wait_event_info;
    uint32_t reserved;
    uint64_t n;            /* waits with duration (ZERO_MARKER excluded) */
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t syscalls;     /* sum of n_syscalls */
    uint64_t n_zero;       /* ZERO_MARKER waits */
};

struct pgwt_tl_query_row {
    uint64_t query_id;
    uint32_t wait_event_info;  /* 0 = the query's total over all events */
    uint16_t qid_flags;        /* QID_* flag bits of the records */
    uint16_t reserved;
    uint64_t n;                /* as in the event row */
    uint64_t total_ns;
    uint64_t n_zero;
};

struct pgwt_tl_summary_totals {
    uint64_t records;          /* records added, every kind */
    uint64_t waits;            /* WAIT records incl. ZERO_MARKER */
    uint64_t zero_marker;
    uint64_t wait_ns;          /* sum of total_ns over events */
    uint64_t loss_spans;       /* LOSS records */
    uint64_t other_records;    /* any other kind except STATS */
    uint64_t lost_raw_total;   /* from the capture or the last STATS */
    uint32_t segments;         /* segments read (file path), 0 live */
    uint32_t truncated;        /* 1 = loss unknown (missing STATS trailer) */
};

struct pgwt_tl_summary;

struct pgwt_tl_summary *pgwt_tl_summary_create(void);
void pgwt_tl_summary_destroy(struct pgwt_tl_summary *s);

/* Adds one record of any kind; a STATS record sets lost_raw_total. 0 or a
 * negative errno. */
int pgwt_tl_summary_add(struct pgwt_tl_summary *s, const struct pgwt_tl_record *rec);

/* Live path: what the writer's final trailer will say. */
void pgwt_tl_summary_set_capture(struct pgwt_tl_summary *s,
                                 uint64_t lost_raw_total, int truncated);

/* File path: adds every record of every segment in dir. 0 or a negative
 * errno. */
int pgwt_tl_summary_from_dir(struct pgwt_tl_summary *s, const char *dir);

/* Rows sorted by total_ns descending, then wait_event_info (then query_id,
 * qid_flags) ascending. Valid until the next add. Returns the row count. */
size_t pgwt_tl_summary_events(const struct pgwt_tl_summary *s,
                              const struct pgwt_tl_event_row **rows);
size_t pgwt_tl_summary_queries(const struct pgwt_tl_summary *s,
                               const struct pgwt_tl_query_row **rows);
void pgwt_tl_summary_get_totals(const struct pgwt_tl_summary *s,
                                struct pgwt_tl_summary_totals *out);

/* 1 when every row and the totals except `segments` agree, else 0 and the
 * first difference in why. */
int pgwt_tl_summary_equal(const struct pgwt_tl_summary *a,
                          const struct pgwt_tl_summary *b,
                          char *why, size_t whysz);

/* timeline-summary.json at path (0640, trace_gid unless (gid_t)-1). */
int pgwt_tl_summary_write_json(const struct pgwt_tl_summary *s,
                               const char *path, gid_t trace_gid);

/* The stdout table at stop. 0 or a negative errno. */
int pgwt_tl_summary_print_text(const struct pgwt_tl_summary *s, FILE *out);

#endif /* PGWT_TIMELINE_SUMMARY_H */
