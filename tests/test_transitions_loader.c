/* File-to-response regression: decoded block aggregates must agree with the
 * actual merged + tagged loader, or the server must use its raw fallback.
 * Both inputs come from the SAME FILE: the candidate uses reader-decoded
 * blocks, and the oracle uses the loader's merged + tagged load. Existing
 * aggregate tests feed both paths the same array, so they cannot catch the
 * reader-to-loader tagging difference exercised here. A separate three-file
 * no-CPU-source fixture can safely assert exact equality, exercising the
 * server's multi-file loop through a compile-time-only entry point.
 * Include the server to exercise its real eligibility gate and loader. */
#define PGWT_TEST_BLOCK_AGGS
#define main pgwt_server_main
#include "../src/server.c"
#undef main
#include "idle_rule.h"

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static const uint32_t client_read = WEI(PG_WAIT_CLIENT, 0);

static uint64_t cpu_link(const struct pgwt_transitions_result *res)
{
    for (int i = 0; i < res->num_rows; i++)
        if (res->rows[i].from_event == 0 &&
            res->rows[i].to_event == client_read)
            return res->rows[i].count;
    /* This fixture has a real in-command link. Missing is failure, not zero. */
    REQUIRE(0);
    return UINT64_MAX;
}

static cJSON *capture_response(struct pgwt_server *srv, struct pgwt_request *req,
                              const struct pgwt_block_agg *agg,
                              int merged, int decoded)
{
    FILE *capture = tmpfile();
    REQUIRE(capture != NULL);
    REQUIRE(fflush(stdout) == 0);
    int saved = dup(STDOUT_FILENO);
    REQUIRE(saved >= 0);
    REQUIRE(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    if (agg)
        emit_transitions_response(req, PGWT_FIDELITY_EXACT, req->num_buckets,
                                  NULL, 0, agg, merged, decoded);
    else
        handle_transitions(srv, req);
    REQUIRE(fflush(stdout) == 0);
    REQUIRE(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    REQUIRE(fseek(capture, 0, SEEK_END) == 0);
    long size = ftell(capture);
    REQUIRE(size > 0);
    rewind(capture);
    char *json = calloc((size_t)size + 1, 1);
    REQUIRE(json != NULL);
    REQUIRE(fread(json, 1, (size_t)size, capture) == (size_t)size);
    fclose(capture);
    cJSON *out = cJSON_Parse(json);
    free(json);
    REQUIRE(out != NULL);
    REQUIRE(cJSON_GetObjectItem(out, "error") == NULL);
    return out;
}

static cJSON *response(struct pgwt_server *srv, struct pgwt_request *req)
{
    return capture_response(srv, req, NULL, 0, 0);
}

static void require_number(cJSON *answer, const char *key, double expected)
{
    cJSON *value = cJSON_GetObjectItem(answer, key);
    REQUIRE(cJSON_IsNumber(value) && value->valuedouble == expected);
}

/* cJSON_Compare uses a tolerance for numbers. Compare parsed fields exactly,
 * including every double's bits, object keys and array/object child order. */
static void require_identical_json(const cJSON *a, const cJSON *b)
{
    REQUIRE(a != NULL && b != NULL && a->type == b->type);
    if (cJSON_IsNumber(a))
        REQUIRE(memcmp(&a->valuedouble, &b->valuedouble,
                       sizeof(a->valuedouble)) == 0);
    if (a->string || b->string)
        REQUIRE(a->string && b->string && strcmp(a->string, b->string) == 0);
    if (a->valuestring || b->valuestring)
        REQUIRE(a->valuestring && b->valuestring &&
                strcmp(a->valuestring, b->valuestring) == 0);
    const cJSON *ac = a->child, *bc = b->child;
    for (; ac && bc; ac = ac->next, bc = bc->next)
        require_identical_json(ac, bc);
    REQUIRE(ac == NULL && bc == NULL);
}

/* Three consecutive files, two committed blocks each, with deterministic
 * header clocks and unique trace identities. No CPU-source records, samples,
 * markers or idle sources: tagging cannot relabel any transition input.
 * All sums fit exactly in double's integer range. Varied pairs/durations
 * make equality exercise every link and node, including their ordering. */
#define NOCPU_FILES 3
#define NOCPU_PER_FILE (2 * PGWT_BLOCK_EVENTS)
#define NOCPU_COUNT (NOCPU_FILES * NOCPU_PER_FILE)
#define NOCPU_START 10000000000000ULL
#define NOCPU_STEP 1000ULL
#define NOCPU_OFFSET 5000000000ULL

static struct pgwt_trace_event nocpu_event(int index)
{
    static const uint32_t sources[] = {
        WEI(PG_WAIT_IO, 9), WEI(PG_WAIT_LOCK, 0), WEI(PG_WAIT_LWLOCK, 1),
    };
    struct pgwt_trace_event ev = {
        .timestamp_ns = NOCPU_START + (uint64_t)index * NOCPU_STEP,
        .pid = 100 + index % 5,
        .old_event = sources[index % 3],
        .new_event = sources[(index + 1) % 3],
        .duration_ns = 1 + (uint64_t)index % 997,
        .cpu_ns = PGWT_CPU_NS_UNKNOWN,
    };
    REQUIRE(ev.old_event != 0 && !PGWT_IS_MARKER(ev.old_event));
    REQUIRE(pgwt_block_agg_record_counts(&ev));
    REQUIRE(pgwt_block_agg_node_counts(&ev));
    return ev;
}

static void nocpu_file_path(char *path, size_t size, const char *dir, int file)
{
    if (file == NOCPU_FILES - 1)
        snprintf(path, size, "%s/current.trace", dir);
    else
        snprintf(path, size, "%s/2026-01-01_0%d.trace.lz4", dir, file);
}

static void write_nocpu_fixture(const char *dir)
{
    for (int file = 0; file < NOCPU_FILES; file++) {
        struct pgwt_event_writer writer;
        REQUIRE(pgwt_writer_init(&writer, dir, 180000, 0, NULL) == 0);
        REQUIRE(writer.enabled);
        for (int i = 0; i < NOCPU_PER_FILE; i++) {
            struct pgwt_trace_event ev = nocpu_event(file * NOCPU_PER_FILE + i);
            REQUIRE(pgwt_writer_push_event(&writer, &ev) == 0);
        }
        REQUIRE(pgwt_writer_close(&writer) == 0);
        pgwt_writer_destroy(&writer);

        char current[512], path[512];
        snprintf(current, sizeof(current), "%s/current.trace", dir);
        nocpu_file_path(path, sizeof(path), dir, file);
        if (file != NOCPU_FILES - 1)
            REQUIRE(rename(current, path) == 0);
        FILE *fp = fopen(path, "r+b");
        REQUIRE(fp != NULL);
        struct pgwt_trace_file_header hdr;
        REQUIRE(fread(&hdr, sizeof(hdr), 1, fp) == 1);
        hdr.clock_offset_ns = NOCPU_START + (uint64_t)file *
                             NOCPU_PER_FILE * NOCPU_STEP;
        hdr.start_time_ns = hdr.clock_offset_ns + NOCPU_OFFSET;
        REQUIRE(fseek(fp, 0, SEEK_SET) == 0);
        REQUIRE(fwrite(&hdr, sizeof(hdr), 1, fp) == 1);
        REQUIRE(fclose(fp) == 0);
        snprintf(current, sizeof(current), "%s/current.trace.meta", dir);
        if (unlink(current) != 0) REQUIRE(errno == ENOENT);

        /* The writer really committed two exact blocks to each file. */
        struct pgwt_event_reader reader;
        REQUIRE(pgwt_reader_open(&reader, path) == 0);
        REQUIRE(reader.num_blocks == 2);
        for (int b = 0; b < reader.num_blocks; b++) {
            struct pgwt_block_info bi;
            REQUIRE(pgwt_reader_block_info(&reader, b, &bi) == 0);
            REQUIRE(bi.block_type == PGWT_BLOCK_TRANSITIONS);
            REQUIRE(bi.num_events == PGWT_BLOCK_EVENTS);
        }
        pgwt_reader_close(&reader);
    }
}

static void compare_nocpu_window(struct pgwt_server *srv, const char *name,
                                 int first, int last, int expected_merged,
                                 int expected_decoded)
{
    struct pgwt_request req = {
        .id = 339,
        .from_ns = NOCPU_START + NOCPU_OFFSET + first * NOCPU_STEP,
        .to_ns = NOCPU_START + NOCPU_OFFSET + last * NOCPU_STEP,
        .num_buckets = 50,
    };
    int first_selected = first < 0 ? 0 : first;
    int last_selected = last >= NOCPU_COUNT ? NOCPU_COUNT - 1 : last;
    int expected_count = last_selected - first_selected + 1;
    REQUIRE(expected_count > 0);

    /* Independent oracle: the actual merged, wall-converted, tagged loader.
     * Check its inputs against the fixture too, before comparing results. */
    int count = -1;
    struct pgwt_load_info info = {0};
    struct pgwt_trace_event *events = server_load_events_fi(
        srv, req.from_ns, req.to_ns, 0, &count, &info);
    REQUIRE(events != NULL && count == expected_count);
    REQUIRE(!info.allocation_failed && !info.overloaded);
    REQUIRE(info.has_transitions && !info.has_samples);
    uint64_t node_counts[3] = {0}, node_ns[3] = {0};
    for (int i = 0; i < count; i++) {
        struct pgwt_trace_event expected = nocpu_event(first_selected + i);
        REQUIRE(events[i].timestamp_ns == expected.timestamp_ns + NOCPU_OFFSET);
        REQUIRE(events[i].old_event == expected.old_event);
        REQUIRE(events[i].new_event == expected.new_event);
        REQUIRE(events[i].duration_ns == expected.duration_ns);
        int source = (first_selected + i) % 3;
        node_counts[source]++;
        node_ns[source] += expected.duration_ns;
    }
    struct pgwt_transitions_result oracle;
    pgwt_compute_transitions(events, count, &req.filter, 50, &oracle);
    REQUIRE(oracle.total_transitions == (uint64_t)expected_count);
    REQUIRE(oracle.num_rows == 3 && oracle.total_rows == 3);

    struct pgwt_block_agg agg;
    uint64_t exact = UINT64_MAX;
    int merged = -1, decoded = -1;
    REQUIRE(pgwt_test_transitions_from_block_aggs(srv, req.from_ns, req.to_ns,
                &agg, &exact, &merged, &decoded) == PGWT_BAGG_OK);
    printf("%s: aggregate=%llu raw=%llu exact=%llu merged_blocks=%d decoded_blocks=%d\n",
           name, (unsigned long long)agg.total_transitions,
           (unsigned long long)oracle.total_transitions,
           (unsigned long long)exact, merged, decoded);
    fflush(stdout); /* Counts survive a failing assertion in the red run. */
    REQUIRE(agg.total_transitions == oracle.total_transitions);
    REQUIRE(exact == (uint64_t)count);
    REQUIRE(merged == expected_merged && decoded == expected_decoded);
    if (expected_merged > 0)
        REQUIRE(merged > 0);
    REQUIRE(agg.n_nodes == 3);
    for (int i = 0; i < 3; i++) {
        struct pgwt_trace_event ev = nocpu_event(i);
        uint64_t actual_count = UINT64_MAX, actual_ns = UINT64_MAX;
        REQUIRE(pgwt_block_agg_node_lookup(&agg, ev.old_event,
                                          &actual_count, &actual_ns));
        REQUIRE(actual_count == node_counts[i]);
        REQUIRE(actual_ns == node_ns[i]);
    }

    /* Compare integer pair sums to the independent compute result. Do not
     * memcmp structs: their padding is not part of the result. */
    struct pgwt_block_agg_pair *pairs = NULL;
    int n_pairs = -1;
    REQUIRE(pgwt_block_agg_pairs_sorted(&agg, &pairs, &n_pairs) == PGWT_BAGG_OK);
    REQUIRE(n_pairs == oracle.num_rows);
    for (int i = 0; i < n_pairs; i++) {
        REQUIRE(pairs[i].from_event == oracle.rows[i].from_event);
        REQUIRE(pairs[i].to_event == oracle.rows[i].to_event);
        REQUIRE(pairs[i].count == oracle.rows[i].count);
        double ns = (double)pairs[i].total_ns;
        REQUIRE(memcmp(&ns, &oracle.rows[i].total_ns, sizeof(ns)) == 0);
    }
    free(pairs);

    cJSON *candidate = capture_response(srv, &req, &agg, merged, decoded);
    cJSON *raw = response(srv, &req);
    require_number(candidate, "merged_blocks", expected_merged);
    require_number(candidate, "decoded_blocks", expected_decoded);
    require_number(raw, "merged_blocks", 0);
    require_number(raw, "decoded_blocks", 0);
    REQUIRE(cJSON_GetArraySize(cJSON_GetObjectItem(candidate, "links")) == 3);
    REQUIRE(cJSON_GetArraySize(cJSON_GetObjectItem(candidate, "nodes")) == 3);
    /* Only diagnostics differ. Compare the ENTIRE response exactly:
     * totals, fidelity, truncation, every link/node field and row order,
     * including bit-exact double comparisons for emitted durations. */
    cJSON_DeleteItemFromObject(candidate, "merged_blocks");
    cJSON_DeleteItemFromObject(candidate, "decoded_blocks");
    cJSON_DeleteItemFromObject(raw, "merged_blocks");
    cJSON_DeleteItemFromObject(raw, "decoded_blocks");
    require_identical_json(candidate, raw);
    cJSON_Delete(candidate);
    cJSON_Delete(raw);
    pgwt_block_agg_free(&agg);
    free(events);
    free(oracle.rows);
}

static void test_nocpu_multifile(void)
{
    char dir[] = "/tmp/pgwt_transitions_nocpu_XXXXXX";
    REQUIRE(mkdtemp(dir) != NULL);
    write_nocpu_fixture(dir);
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    REQUIRE(srv != NULL && server_init(srv, dir) == 0);
    REQUIRE(srv->cov_count == NOCPU_FILES);
    for (int i = 0; i < srv->cov_count; i++) {
        REQUIRE(srv->cov[i].valid);
        REQUIRE(srv->cov[i].canon_offset == NOCPU_OFFSET);
    }
    /* A cold decode populates the real aggregate cache. Every following
     * equality case MUST merge >0 blocks, including boundary-decode cases. */
    compare_nocpu_window(srv, "cold full", -1, NOCPU_COUNT, 0, 6);
    compare_nocpu_window(srv, "warm full", -1, NOCPU_COUNT, 6, 0);
    compare_nocpu_window(srv, "file first == to", -1, NOCPU_PER_FILE, 2, 1);
    compare_nocpu_window(srv, "file first > to", -1, NOCPU_PER_FILE - 1, 2, 0);
    compare_nocpu_window(srv, "file first < to", -1, NOCPU_PER_FILE + 1, 2, 1);
    compare_nocpu_window(srv, "file last == from", NOCPU_PER_FILE - 1,
                         NOCPU_COUNT, 4, 1);
    compare_nocpu_window(srv, "both window edges partial", 17,
                         NOCPU_COUNT - 29, 4, 2);
    compare_nocpu_window(srv, "block first == to", -1, PGWT_BLOCK_EVENTS, 1, 1);
    compare_nocpu_window(srv, "block last == from", PGWT_BLOCK_EVENTS - 1,
                         NOCPU_COUNT, 5, 1);
    server_destroy(srv);
    cur_cache_drop(&srv->cur);
    bagg_cache_drop(srv);
    free(srv);
    for (int file = 0; file < NOCPU_FILES; file++) {
        char path[512];
        nocpu_file_path(path, sizeof(path), dir, file);
        REQUIRE(unlink(path) == 0);
    }
    REQUIRE(rmdir(dir) == 0);
    puts("PASS: no-CPU-source multi-file equality; 8 warm windows merged blocks");
}

int main(void)
{
    REQUIRE(setenv("PGWT_TRANSITIONS_PROVENANCE", "1", 1) == 0);
    test_nocpu_multifile();
    char dir[] = "/tmp/pgwt_transitions_loader_XXXXXX";
    REQUIRE(mkdtemp(dir) != NULL);
    struct pgwt_event_writer writer;
    REQUIRE(pgwt_writer_init(&writer, dir, 180000, 0, NULL) == 0);
    const uint64_t start = 1700000000000000000ULL;
    for (int i = 0; i < 2 * PGWT_BLOCK_EVENTS; i++) {
        struct pgwt_trace_event ev = {
            .timestamp_ns = start + (uint64_t)i * 1000,
            .pid = 100,
            .old_event = PGWT_MARKER_ESCALATE_END,
            .new_event = PGWT_MARKER_ESCALATE_END,
            .cpu_ns = PGWT_CPU_NS_UNKNOWN,
        };
        if (i == 0) {
            ev.old_event = ev.new_event = PGWT_MARKER_CMD_START;
        } else if (i == 2) {
            ev.old_event = ev.new_event = PGWT_MARKER_CMD_END;
            ev.timestamp_ns = start + 1000;
        } else if (i == 1 || i == PGWT_BLOCK_EVENTS) {
            ev.old_event = 0;
            ev.new_event = client_read;
            ev.duration_ns = 1000;
        }
        REQUIRE(pgwt_writer_push_event(&writer, &ev) == 0);
    }
    REQUIRE(pgwt_writer_close(&writer) == 0);

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    REQUIRE(srv != NULL);
    REQUIRE(server_init(srv, dir) == 0);
    struct pgwt_request req = {
        .from_ns = srv->earliest_wall_ns,
        .to_ns = srv->latest_wall_ns + 1,
        .num_buckets = 50,
    };

    /* Candidate comes ONLY from reader-decoded blocks via bagg_decode_block;
     * it never receives the oracle's tagged array. Test cold and cached merges. */
    for (int pass = 0; pass < 2; pass++) {
        struct pgwt_block_agg agg;
        uint64_t exact = UINT64_MAX;
        int merged = -1, decoded = -1;
        REQUIRE(transitions_from_block_aggs(srv, req.from_ns, req.to_ns,
                    &agg, &exact, &merged, &decoded) == PGWT_BAGG_OK);
        REQUIRE(exact == 2 * PGWT_BLOCK_EVENTS);
        REQUIRE(pass == 0 ? decoded == 2 : merged == 2);
        struct pgwt_block_agg_pair *pairs = NULL;
        int n_pairs = -1;
        REQUIRE(pgwt_block_agg_pairs_sorted(&agg, &pairs, &n_pairs)
                == PGWT_BAGG_OK);
        REQUIRE(n_pairs == 1);
        REQUIRE(pairs[0].from_event == 0 && pairs[0].to_event == client_read);
        REQUIRE(pairs[0].count == 2);
        free(pairs);
        pgwt_block_agg_free(&agg);
    }

    int count = -1;
    struct pgwt_load_info info = {0};
    struct pgwt_trace_event *tagged = server_load_events_fi(
        srv, req.from_ns, req.to_ns, 0, &count, &info);
    REQUIRE(tagged != NULL && count == 2 * PGWT_BLOCK_EVENTS);
    REQUIRE(!info.allocation_failed && !info.overloaded);
    REQUIRE(info.has_transitions && !info.has_samples);
    REQUIRE(tagged[1].old_event == 0);
    REQUIRE(tagged[PGWT_BLOCK_EVENTS].old_event == PGWT_WEI_NONCMD_CPU);
    REQUIRE(pgwt_is_hidden_event(tagged[PGWT_BLOCK_EVENTS].old_event));
    REQUIRE(pgwt_is_idle_event(tagged[PGWT_BLOCK_EVENTS].old_event));
    struct pgwt_transitions_result oracle;
    pgwt_compute_transitions(tagged, count, &req.filter, 50, &oracle);
    uint64_t expected_link = cpu_link(&oracle);
    REQUIRE(expected_link == 1 && oracle.total_transitions == 1);

    /* Compare the shipping response with the independently loaded oracle.
     * Poison missing measurements AND retain a failure flag: -1 is truthy. */
    cJSON *out = response(srv, &req);
    require_number(out, "merged_blocks", 0);
    require_number(out, "decoded_blocks", 0);
    cJSON *warm = response(srv, &req);
    require_number(warm, "merged_blocks", 0);
    require_number(warm, "decoded_blocks", 0);
    require_identical_json(out, warm);
    cJSON_Delete(warm);
    puts("production cold/warm: merged_blocks=0 decoded_blocks=0");
    int failed = 1;
    uint64_t served_link = UINT64_MAX;
    cJSON *links = cJSON_GetObjectItem(out, "links");
    REQUIRE(cJSON_IsArray(links));
    cJSON *link;
    cJSON_ArrayForEach(link, links) {
        cJSON *source = cJSON_GetObjectItem(link, "source");
        cJSON *target = cJSON_GetObjectItem(link, "target");
        cJSON *value = cJSON_GetObjectItem(link, "value");
        REQUIRE(cJSON_IsString(source) && cJSON_IsString(target));
        if (strcmp(source->valuestring, "CPU*") == 0 &&
            strcmp(target->valuestring, "Client:ClientRead") == 0 &&
            cJSON_IsNumber(value) && value->valuedouble >= 0) {
            served_link = (uint64_t)value->valuedouble;
            failed = 0;
        }
    }
    printf("decoded CPU->ClientRead=2; tagged oracle=%llu; served=%llu; failed=%d\n",
           (unsigned long long)expected_link,
           (unsigned long long)served_link, failed);
    REQUIRE(!failed && served_link == expected_link);
    cJSON *total = cJSON_GetObjectItem(out, "total");
    REQUIRE(cJSON_IsNumber(total) && total->valuedouble == oracle.total_transitions);
    cJSON *nodes = cJSON_GetObjectItem(out, "nodes");
    REQUIRE(cJSON_IsArray(nodes) && cJSON_GetArraySize(nodes) == 1);
    cJSON *cpu = cJSON_GetArrayItem(nodes, 0);
    cJSON *ms = cJSON_GetObjectItem(cpu, "total_ms");
    cJSON *id = cJSON_GetObjectItem(cpu, "event_id");
    REQUIRE(cJSON_IsNumber(id) && id->valuedouble == 0);
    REQUIRE(cJSON_IsNumber(ms) && ms->valuedouble == 0.001);
    REQUIRE(!bagg_window_eligible(srv, &req.filter, req.from_ns, req.to_ns));

    cJSON_Delete(out);
    free(tagged);
    free(oracle.rows);
    bagg_cache_drop(srv);
    free(srv);
    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    REQUIRE(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/current.trace.meta", dir);
    if (unlink(path) != 0) REQUIRE(errno == ENOENT);
    REQUIRE(rmdir(dir) == 0);
    puts("PASS: decoded-block candidate refused; response matches tagged loader");
    return 0;
}
