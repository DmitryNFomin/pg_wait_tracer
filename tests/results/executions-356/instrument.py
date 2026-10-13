"""Instrument disposable source copies; production source is not rewritten."""
from pathlib import Path
import sys
root = Path(sys.argv[1])
p = root / 'src/server.c'
s = p.read_text()
start = s.index('static int load_execution_rows(')
end = s.index('static cJSON *serialize_exec_lane(', start)
h = s[start:end]
h = h.replace('    uint64_t from =', '    double x0 = exec_measure_now(), x1, x2, x3, x4;\n    uint64_t from =', 1)
h = h.replace('    enum pgwt_fidelity fid =', '    x1 = exec_measure_now();\n    enum pgwt_fidelity fid =', 1)
h = h.replace('    int count = context_count', '    x2 = exec_measure_now();\n    int count = context_count', 1)
h = h.replace('    pgwt_compute_executions(events,', '    x3 = exec_measure_now();\n    pgwt_compute_executions(events,', 1)
h = h.replace('    free(links);', '    x4 = exec_measure_now();\n    fprintf(stderr, "EXPHASE id=%llu window=%d prefix=%d load=%.3f prefix_ms=%.3f combine_links=%.3f compute=%.3f\\n", (unsigned long long)req->id, window_count, context_count, x1-x0, x2-x1, x3-x2, x4-x3);\n    free(links);', 1)
h = h.replace('    int kept = 0;', '    double x0 = exec_measure_now(), x1;\n    int kept = 0;', 1)
h = h.replace('    cJSON *root = cJSON_CreateObject();\n    cJSON_AddNumberToObject(root, "id", (double)req->id);\n    add_fidelity(root, &linfo);', '    x1 = exec_measure_now();\n    cJSON *root = cJSON_CreateObject();\n    cJSON_AddNumberToObject(root, "id", (double)req->id);\n    add_fidelity(root, &linfo);', 1)
h = h.replace('    free(res.rows);', '    fprintf(stderr, "EXEMIT id=%llu rows=%d returned=%d rank=%.3f serialize=%.3f\\n", (unsigned long long)req->id, res.num_rows, returned, x1-x0, exec_measure_now()-x1);\n    free(res.rows);', 1)
helper = 'static double exec_measure_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec*1000.0+t.tv_nsec/1e6; }\n'
p.write_text(s[:start] + helper + h + s[end:])
p = root / 'src/compute.c'
s = p.read_text()
a = s.index('void pgwt_compute_executions(')
b = s.index('static int interval_overlaps(', a)
h = s[a:b].replace('    free(worker_last_row);', '    fprintf(stderr, "EXSTATES states=%d rows=%d events=%d\\n", n_states, n_rows, count);\n    free(worker_last_row);', 1)
p.write_text(s[:a] + h + s[b:])
