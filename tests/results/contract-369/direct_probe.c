
#define main existing_probe_main
#include "test_probe_bounds.c"
#undef main
int main(void) {
    struct pgwt_summary_accum *a = rec_new(1000000000ULL);
    a->sessions[0].pid = BASE_PID;
    a->sessions[0].db_time_ns = 5 * MS;
    a->sessions[0].cpu_ns = MS;
    a->num_sessions = 1;
    struct pgwt_summary_accum *recs[] = {a};
    for (int present = 1; present >= 0; present--) {
        stub_set(recs, present);
        for (int kind = 0; kind < 4; kind++) {
            struct pgwt_filter f = {0};
            if (kind == 0) snprintf(f.class_name, sizeof(f.class_name), "Lock");
            if (kind == 1) f.event_id = WAIT_A;
            if (kind == 2) f.event_id = 0;
#ifdef CONTRACT_HAS_EVENT_FLAG
            if (kind == 1 || kind == 2) f.has_event_filter = 1;
#endif
            if (kind == 3) f.query_id = 100;
            struct pgwt_sessions_result out;
            pgwt_compute_top_sessions_from_summaries("ignored", 0, 0, &f, 0, &out);
            printf("{\"records\":%d,\"filter\":\"%s\",\"failed\":%d,"
                   "\"num_rows\":%d,\"rows_null\":%s}\n", present,
                   (const char *[]){"class", "event", "cpu", "query"}[kind],
                   out.failed, out.num_rows, out.rows == NULL ? "true" : "false");
            free(out.rows);
        }
    }
    free(a);
    return 0;
}
