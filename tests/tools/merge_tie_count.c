/* Refs #871. Physical corpus count from CONTRACT.md section 2.
 * C follows tests/dump_markers.c and keeps the production reader/decoder
 * authoritative rather than reimplementing its LZ4/columnar format in Python.
 *
 * Each argument is ONE immutable, quiescent capture, including rotations.
 * Files are ordered by header monotonic creation anchor; blocks by file offset,
 * rows by stored order. Equal file creation anchors are refused rather than
 * inventing their physical order. A capture must come from one boot.
 * No timestamp sorting is used for before/after.
 *
 * Legacy v2-v4 traces have NO backend generation identity, including in
 * backends.jsonl. Conservatively report every group with multiple eligible
 * records as generation ambiguous (a singleton cannot mix generations).
 * N is still the exact raw pid/timestamp pair count; it must not
 * be used as proof of a same-generation tie or a sequence-free decision.
 * The equal-state records built by open_interval.h are administrative flushes;
 * watchpoint genuine transitions suppress equal-state writes.
 */
#include "merge_tie_count.h"
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const uint32_t marker_kinds[TIE_MARKER_KINDS] = {
    PGWT_MARKER_PLAN_START, PGWT_MARKER_PLAN_END,
    PGWT_MARKER_EXEC_START, PGWT_MARKER_EXEC_END,
    PGWT_MARKER_CMD_START, PGWT_MARKER_CMD_END
};

static int add_pid(struct tie_pids *p, uint32_t pid)
{
    if (!pid) return 0;
    size_t lo = 0, hi = p->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->values[mid] < pid) lo = mid + 1;
        else hi = mid;
    }
    if (lo < p->count && p->values[lo] == pid) return 0;
    if (p->count == p->capacity) {
        size_t cap = p->capacity ? p->capacity * 2 : 64;
        if (cap < p->capacity || cap > SIZE_MAX / sizeof(*p->values)) return -1;
        void *values = realloc(p->values, cap * sizeof(*p->values));
        if (!values) return -1;
        p->values = values;
        p->capacity = cap;
    }
    memmove(p->values + lo + 1, p->values + lo,
            (p->count - lo) * sizeof(*p->values));
    p->values[lo] = pid;
    ++p->count;
    return 0;
}

int tie_add(struct tie_capture *c, const struct pgwt_trace_event *e,
            enum pgwt_block_type type)
{
    ++c->scanned;
    ++c->position;
    if (add_pid(&c->pids, e->pid)) return -1;
    if (type != PGWT_BLOCK_TRANSITIONS) return 0;
    uint32_t kind = e->new_event;
    bool marker = kind == PGWT_MARKER_PLAN_START || kind == PGWT_MARKER_PLAN_END
        || kind == PGWT_MARKER_EXEC_START || kind == PGWT_MARKER_EXEC_END
        || kind == PGWT_MARKER_CMD_START || kind == PGWT_MARKER_CMD_END;
    bool transition = !marker && !PGWT_IS_MARKER(kind)
        && !PGWT_IS_MARKER(e->old_event) && e->old_event != e->new_event;
    int category = TIE_IGNORED;
    if (e->pid && !(e->flags & PGWT_EVENT_FLAG_SAMPLE))
        category = marker ? TIE_MARKER : transition ? TIE_TRANSITION : TIE_IGNORED;
    if (category == TIE_MARKER) {
        for (size_t i = 0; i < TIE_MARKER_KINDS; ++i)
            if (kind == marker_kinds[i]) ++c->markers_by_kind[i];
    } else if (category == TIE_TRANSITION) ++c->transition_records;
    if (c->count == c->capacity) {
        size_t cap = c->capacity ? c->capacity * 2 : 4096;
        if (cap < c->capacity || cap > SIZE_MAX / sizeof(*c->records)) return -1;
        void *p = realloc(c->records, cap * sizeof(*c->records));
        if (!p) return -1;
        c->records = p;
        c->capacity = cap;
    }
    c->records[c->count++] = (struct tie_record){
        e->timestamp_ns, c->position, e->pid, category
    };
    return 0;
}

static int record_compare(const void *a, const void *b)
{
    const struct tie_record *x = a, *y = b;
    if (x->pid != y->pid) return x->pid < y->pid ? -1 : 1;
    if (x->ts != y->ts) return x->ts < y->ts ? -1 : 1;
    return (x->position > y->position) - (x->position < y->position);
}

static int add_checked(uint64_t *sum, uint64_t n)
{
    if (UINT64_MAX - *sum < n) return -1;
    *sum += n;
    return 0;
}

int tie_finish(struct tie_capture *c, struct tie_counts *out)
{
    struct tie_counts v = { .captures = 1, .records_scanned = c->scanned };
    v.transition_records = c->transition_records;
    for (size_t i = 0; i < TIE_MARKER_KINDS; ++i) {
        v.markers_by_kind[i] = c->markers_by_kind[i];
        if (add_checked(&v.marker_records, c->markers_by_kind[i])) return -1;
    }
    if (c->count) qsort(c->records, c->count, sizeof(*c->records), record_compare);
    for (size_t i = 0; i < c->count;) {
        size_t j = i;
        uint64_t markers = 0, transitions = 0;
        ++v.groups;
        while (j < c->count && c->records[j].pid == c->records[i].pid
               && c->records[j].ts == c->records[i].ts) {
            if (c->records[j].kind == TIE_MARKER) {
                if (add_checked(&v.marker_after_transition, transitions)) return -1;
                ++markers;
            } else if (c->records[j].kind == TIE_TRANSITION) {
                if (add_checked(&v.marker_before_transition, markers)) return -1;
                ++transitions;
            }
            ++j;
        }
        if (markers && transitions) ++v.mixed_groups;
        if (markers + transitions > 1) ++v.ambiguous_generation_groups;
        i = j;
    }
    v.N = v.marker_before_transition;
    if (add_checked(&v.N, v.marker_after_transition)) return -1;
#define ADD(field) if (add_checked(&out->field, v.field)) return -1
    ADD(captures); ADD(records_scanned); ADD(groups); ADD(mixed_groups); ADD(N);
    ADD(marker_before_transition); ADD(marker_after_transition);
    ADD(ambiguous_generation_groups);
    ADD(marker_records); ADD(transition_records);
    for (size_t i = 0; i < TIE_MARKER_KINDS; ++i)
        if (add_checked(&out->markers_by_kind[i], v.markers_by_kind[i])) return -1;
    for (size_t i = 0; i < c->pids.count; ++i)
        if (add_pid(&out->pids, c->pids.values[i])) return -1;
#undef ADD
    return 0;
}

struct input_file { char *path; uint64_t anchor; };
static int file_compare(const void *a, const void *b)
{
    const struct input_file *x = a, *y = b;
    if (x->anchor != y->anchor) return x->anchor < y->anchor ? -1 : 1;
    return strcmp(x->path, y->path);
}

int tie_scan(const char *directory, struct tie_counts *out)
{
    DIR *dir = opendir(directory);
    if (!dir) { perror(directory); return -1; }
    struct input_file *files = NULL;
    size_t nfiles = 0;
    struct tie_capture capture = {0};
    struct pgwt_trace_event *events = NULL;
    int rc = -1;
    struct dirent *ent;
    /* Own inventory: pgwt_scan_trace_files silently caps its array and accepts
     * only one archive naming pattern. Every .trace.lz4 is physical input. */
    for (;;) {
        errno = 0;
        ent = readdir(dir);
        if (!ent) {
            if (errno) { perror(directory); goto done; }
            break;
        }
        size_t len = strlen(ent->d_name);
        if (strcmp(ent->d_name, "current.trace") &&
            (len < 10 || strcmp(ent->d_name + len - 10, ".trace.lz4"))) continue;
        size_t plen = strlen(directory) + len + 2;
        char *path = malloc(plen);
        if (!path) goto done;
        snprintf(path, plen, "%s/%s", directory, ent->d_name);
        struct input_file *p = realloc(files, (nfiles + 1) * sizeof(*files));
        if (!p) { free(path); goto done; }
        files = p;
        files[nfiles++] = (struct input_file){path, 0};
    }
    if (!nfiles) { fprintf(stderr, "%s: no trace payloads; count unknown\n", directory); goto done; }
    for (size_t i = 0; i < nfiles; ++i) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, files[i].path)) goto done;
        files[i].anchor = r.header.clock_offset_ns;
        pgwt_reader_close(&r);
    }
    qsort(files, nfiles, sizeof(*files), file_compare);
    events = malloc(PGWT_BLOCK_EVENTS * sizeof(*events));
    if (!events) goto done;
    for (size_t i = 0; i < nfiles; ++i) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, files[i].path)) goto done;
        int bad = 0;
        /* Verify contiguous full-file coverage ourselves: the reader may
         * recover a valid prefix from a truncated file. A prefix is not a
         * complete corpus. Footer must follow ALL indexed payloads exactly. */
        uint64_t offset = pgwt_trace_file_header_size(r.header.version);
        for (int b = 0; b < r.num_blocks; ++b) {
            struct pgwt_block_info info;
            if (r.block_index[b].file_offset != offset ||
                pgwt_reader_block_info(&r, b, &info) || !info.num_events ||
                info.num_events > PGWT_BLOCK_EVENTS) { bad = 1; break; }
            int n = pgwt_reader_decode_block_info(&r, b, events, PGWT_BLOCK_EVENTS, &info);
            if (n < 0 || (uint32_t)n != info.num_events) { bad = 1; break; }
            long pos = ftell(r.fp);
            if (pos < 0) { bad = 1; break; }
            offset = (uint64_t)pos;
            for (int k = 0; k < n; ++k) {
                if (tie_add(&capture, &events[k], info.block_type)) { bad = 1; break; }
            }
            if (bad) break;
        }
        struct stat st;
        uint64_t footer = (uint64_t)r.num_blocks * sizeof(*r.block_index) + 4;
        uint32_t footer_blocks = 0;
        if (fstat(fileno(r.fp), &st) || !r.num_blocks ||
            (uint64_t)st.st_size != offset + footer ||
            fseek(r.fp, -4, SEEK_END) ||
            fread(&footer_blocks, sizeof(footer_blocks), 1, r.fp) != 1 ||
            footer_blocks != (uint32_t)r.num_blocks) bad = 1;
        if (!files[i].anchor || (i && files[i].anchor == files[i - 1].anchor)) bad = 1;
        pgwt_reader_close(&r);
        if (bad) {
            fprintf(stderr, "%s: incomplete trace or unprovable file order; count unknown\n", files[i].path);
            goto done;
        }
    }
    rc = tie_finish(&capture, out);
done:
    closedir(dir);
    free(events);
    free(capture.records);
    free(capture.pids.values);
    for (size_t i = 0; i < nfiles; ++i) free(files[i].path);
    free(files);
    return rc;
}

#ifndef MERGE_TIE_COUNT_NO_MAIN
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s CAPTURE_DIR [CAPTURE_DIR ...]\n", argv[0]); return 2; }
    struct tie_counts c = {0};
    for (int i = 1; i < argc; ++i) {
        struct stat a, b;
        if (stat(argv[i], &a)) { perror(argv[i]); return 1; }
        for (int j = 1; j < i; ++j)
            if (!stat(argv[j], &b) && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
                fprintf(stderr, "duplicate capture: %s\n", argv[i]); return 1;
            }
        if (tie_scan(argv[i], &c)) return 1;
    }
    printf("{\"captures\":%" PRIu64 ",\"records_scanned\":%" PRIu64
           ",\"groups\":%" PRIu64 ",\"nonempty_mixed_groups\":%" PRIu64
           ",\"N\":%" PRIu64 ",\"marker_before_transition\":%" PRIu64
           ",\"marker_after_transition\":%" PRIu64
           ",\"ambiguous_generation_groups\":%" PRIu64
           ",\"marker_records\":%" PRIu64
           ",\"markers_by_kind\":{\"PLAN_START\":%" PRIu64 ",\"PLAN_END\":%" PRIu64
           ",\"EXEC_START\":%" PRIu64 ",\"EXEC_END\":%" PRIu64
           ",\"CMD_START\":%" PRIu64 ",\"CMD_END\":%" PRIu64 "}"
           ",\"transition_records\":%" PRIu64 ",\"distinct_pids\":%zu"
           ",\"generation_evidence\":\"unavailable_in_v2_v4\"}\n",
           c.captures, c.records_scanned, c.groups, c.mixed_groups, c.N,
           c.marker_before_transition, c.marker_after_transition,
           c.ambiguous_generation_groups, c.marker_records,
           c.markers_by_kind[TIE_PLAN_START], c.markers_by_kind[TIE_PLAN_END],
           c.markers_by_kind[TIE_EXEC_START], c.markers_by_kind[TIE_EXEC_END],
           c.markers_by_kind[TIE_CMD_START], c.markers_by_kind[TIE_CMD_END],
           c.transition_records, c.pids.count);
    free(c.pids.values);
    return 0;
}
#endif
