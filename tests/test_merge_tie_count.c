#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "tools/merge_tie_count.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <lz4.h>

static void feed(struct tie_capture *c, uint64_t ts, uint32_t pid,
                 uint32_t old, uint32_t next, enum pgwt_block_type type)
{
    struct pgwt_trace_event e = {
        .timestamp_ns = ts, .pid = pid, .old_event = old, .new_event = next
    };
    assert(tie_add(c, &e, type) == 0);
}
/* Real v4/LZ4 files exercise the production reader and cross-file/block
 * grouping; expected answers remain hand-counted. */
static void fixture(const char *path, uint64_t anchor,
                    const struct pgwt_trace_event *events, unsigned count)
{
    FILE *fp = fopen(path, "wb");
    assert(fp);
    struct pgwt_trace_file_header h = {
        .magic = PGWT_TRACE_MAGIC, .version = PGWT_TRACE_VERSION,
        .start_time_ns = anchor + 1000, .clock_offset_ns = anchor
    };
    uint64_t extra = 0;
    assert(fwrite(&h, sizeof(h), 1, fp) == 1);
    assert(fwrite(&extra, sizeof(extra), 1, fp) == 1);
    struct pgwt_block_index_entry index[3];
    assert(count <= 3);
    for (unsigned i = 0; i < count; ++i) {
        uint8_t encoded[256];
        char compressed[512];
        size_t len = pgwt_encode_block(&events[i], 1, encoded, sizeof(encoded));
        int clen = LZ4_compress_default((const char *)encoded, compressed,
                                       (int)len, sizeof(compressed));
        assert(clen > 0);
        index[i] = (struct pgwt_block_index_entry){events[i].timestamp_ns, (uint64_t)ftell(fp)};
        struct pgwt_trace_block_header b = {
            .first_timestamp_ns = events[i].timestamp_ns,
            .last_timestamp_ns = events[i].timestamp_ns, .num_events = 1,
            .compressed_size = (uint32_t)clen, .uncompressed_size = (uint32_t)len
        };
        assert(fwrite(&b, sizeof(b), 1, fp) == 1);
        assert(fwrite(&extra, sizeof(extra), 1, fp) == 1);
        assert(fwrite(compressed, (size_t)clen, 1, fp) == 1);
    }
    assert(fwrite(index, sizeof(*index), count, fp) == count);
    uint32_t blocks = count;
    assert(fwrite(&blocks, sizeof(blocks), 1, fp) == 1);
    assert(fclose(fp) == 0);
}
static void test_reader(void)
{
    char directory[] = "/tmp/merge-tie-unit-XXXXXX";
    assert(mkdtemp(directory));
    char first[256], second[256];
    /* Names deliberately reverse creation order; neither semantic timestamp
     * nor lexicographic filename order supplies the physical tie order. */
    snprintf(first, sizeof(first), "%s/z.trace.lz4", directory);
    snprintf(second, sizeof(second), "%s/a.trace.lz4", directory);
    const struct pgwt_trace_event events[] = {
        {.timestamp_ns = 100, .pid = 10, .old_event = PGWT_MARKER_PLAN_START,
         .new_event = PGWT_MARKER_PLAN_START},
        {.timestamp_ns = 100, .pid = 10, .old_event = 1, .new_event = PGWT_EVENT_EXIT},
        {.timestamp_ns = 100, .pid = 10, .old_event = PGWT_MARKER_CMD_END,
         .new_event = PGWT_MARKER_CMD_END}
    };
    fixture(first, 10, events, 1);
    fixture(second, 20, events + 1, 2);
    struct tie_counts n = {0};
    assert(tie_scan(directory, &n) == 0);
    assert(n.N == 2 && n.records_scanned == 3 && n.groups == 1);
    assert(n.marker_before_transition == 1 && n.marker_after_transition == 1);
    assert(n.ambiguous_generation_groups == 1);
    FILE *fp = fopen(second, "ab");
    assert(fp && fputc(0, fp) == 0 && fclose(fp) == 0);
    struct tie_counts unchanged = n;
    assert(tie_scan(directory, &n) != 0); /* no successful-prefix answer */
    assert(memcmp(&unchanged, &n, sizeof(n)) == 0);
    assert(unlink(first) == 0 && unlink(second) == 0 && rmdir(directory) == 0);
}
int main(void)
{
    struct tie_capture c = {0};
    struct tie_counts n = {0};
    /* M,T,M: two pairs, opposite physical directions, non-adjacent groups
     * and duplicate marker payloads retained across feed/block boundaries. */
    feed(&c, 100, 10, PGWT_MARKER_PLAN_START, PGWT_MARKER_PLAN_START, 0);
    feed(&c, 200, 20, 1, 2, 0);
    feed(&c, 100, 10, 1, PGWT_EVENT_EXIT, 0);
    feed(&c, 100, 10, PGWT_MARKER_PLAN_START, PGWT_MARKER_PLAN_START, 0);
    feed(&c, 100, 10, 0, 2, PGWT_BLOCK_SAMPLES);
    feed(&c, 100, 10, 2, 2, 0); /* administrative flush */
    feed(&c, 100, 0, PGWT_MARKER_ESCALATE_START, PGWT_MARKER_ESCALATE_START, 0);
    assert(tie_finish(&c, &n) == 0);
    assert(n.N == 2 && n.mixed_groups == 1 && n.groups == 3);
    assert(n.marker_before_transition == 1 && n.marker_after_transition == 1);
    assert(n.records_scanned == 7 && n.ambiguous_generation_groups == 1);
    free(c.records);
    c = (struct tie_capture){0};
    n = (struct tie_counts){0};
    feed(&c, 100, 10, 1, 2, 0);
    feed(&c, 101, 10, PGWT_MARKER_CMD_END, PGWT_MARKER_CMD_END, 0);
    feed(&c, 100, 20, PGWT_MARKER_EXEC_END, PGWT_MARKER_EXEC_END, 0);
    assert(tie_finish(&c, &n) == 0);
    assert(n.N == 0 && n.mixed_groups == 0 && n.groups == 3);
    /* A different capture with the same pid/time must not create a pair. */
    free(c.records);
    c = (struct tie_capture){0};
    feed(&c, 100, 10, PGWT_MARKER_CMD_START, PGWT_MARKER_CMD_START, 0);
    assert(tie_finish(&c, &n) == 0);
    assert(n.captures == 2 && n.N == 0);
    free(c.records);
    c = (struct tie_capture){0};
    n = (struct tie_counts){0};
    feed(&c, 100, 10, PGWT_MARKER_EXEC_START, PGWT_MARKER_EXEC_START, 0);
    feed(&c, 100, 10, 1, 2, 0);
    feed(&c, 100, 10, PGWT_MARKER_EXEC_END, PGWT_MARKER_EXEC_END, 0);
    feed(&c, 100, 10, 2, 3, 0);
    feed(&c, 100, 10, 3, 4, 0);
    assert(tie_finish(&c, &n) == 0);
    assert(n.N == 6 && n.marker_before_transition == 5 && n.marker_after_transition == 1);
    free(c.records);
    test_reader();
    puts("merge_tie_count: PASS (N=2, N=0, N=6; reader, order, exclusions, capture isolation, truncation)");
    return 0;
}
