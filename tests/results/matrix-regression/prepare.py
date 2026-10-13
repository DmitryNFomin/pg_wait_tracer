from pathlib import Path
import shutil
root=Path('/tmp/cx-matrix-regression-src')
old=(root/'pre/src/server.c').read_text()
start=old.index('static int stable_sort_events(')
end=old.index('static const struct pgwt_gen_cov *gen_cov_get', start)
oldsort=old[start:end]
pristine=__import__('subprocess').check_output(['git','show','612bd98:src/server.c'],text=True)
for v in ['final','aggregate','oldsort']:
 if v!='final': shutil.copytree(root/'final',root/v,dirs_exist_ok=True)
 p=root/v/'src/server.c'; s=pristine
 if v=='aggregate':
  gate=s.index('static int bagg_window_eligible('); pos=s.index('    return 0;',gate); s=s[:pos]+s[pos:].replace('    return 0;','    /* DIAGNOSTIC ONLY: restore unsafe aggregate gate. */',1)
 if v=='oldsort':
  start=s.index('static int stable_sort_events('); end=s.index('static const struct pgwt_gen_cov *gen_cov_get',start); s=s[:start]+oldsort+s[end:]
 # instrument handler only, monotonic timers, stderr
 start=s.index('static void handle_transitions('); end=s.index('static void handle_fingerprints',start)
 h=s[start:end]
 h=h.replace('    int max_rows', '    double mx0 = mx_now(), mx1, mx2, mx3, mx4;\n    int max_rows',1)
 h=h.replace('    /* Transitions need real', '    mx1 = mx_now();\n    /* Transitions need real',1)
 h=h.replace('    /* Node totals go', '    mx2 = mx_now();\n    /* Node totals go',1)
 h=h.replace('    emit_transitions_response(req, fid, max_rows, &res, 1, &raw_nodes, 0, 0);','    mx3 = mx_now();\n    emit_transitions_response(req, fid, max_rows, &res, 1, &raw_nodes, 0, 0);\n    mx4 = mx_now();\n    fprintf(stderr, "MXPHASE id=%llu count=%d load=%.3f links=%.3f nodes=%.3f emit=%.3f\\n", (unsigned long long)req->id, count, mx1-mx0, mx2-mx1, mx3-mx2, mx4-mx3);')
 helper='static double mx_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec*1000.0+t.tv_nsec/1e6; }\n'
 s=s[:start]+helper+h+s[end:]
 p.write_text(s)
