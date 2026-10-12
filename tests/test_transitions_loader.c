/* File-to-response regression: decoded block aggregates must agree with the
 * actual merged + tagged loader, or the server must use its raw fallback.
 * Both inputs come from the SAME FILE: the candidate uses reader-decoded
 * blocks, and the oracle uses the loader's merged + tagged load. Existing
 * aggregate tests feed both paths the same array, so they cannot catch the
 * reader-to-loader tagging difference exercised here.
 * Include the server to exercise its real eligibility gate and loader. */
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

static cJSON *response(struct pgwt_server *srv, struct pgwt_request *req)
{
    FILE *capture = tmpfile();
    REQUIRE(capture != NULL);
    REQUIRE(fflush(stdout) == 0);
    int saved = dup(STDOUT_FILENO);
    REQUIRE(saved >= 0);
    REQUIRE(dup2(fileno(capture), STDOUT_FILENO) >= 0);
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

int main(void)
{
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
