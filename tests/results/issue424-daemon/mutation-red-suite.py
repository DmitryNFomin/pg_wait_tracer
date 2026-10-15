"""Demonstrate that each new gate can go RED: break the product, run the
test, restore. Prints the failing lines for the report."""
import subprocess, sys

root = "/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-a283b5c4ab073a48e/"

CASES = [
    ("A: shared CPU helper's underflow floor removed (src/open_interval.h)",
     "src/open_interval.h",
     "    return (exact >= last_cpu_ns) ? exact - last_cpu_ns : 0;",
     "    return exact - last_cpu_ns;"),
    ("B: builder drops the PG13 QUERY_SYNTH flag (src/open_interval.h)",
     "src/open_interval.h",
     """        .flags        = (attr->cmd_open ? PGWT_EVENT_FLAG_CMD_OPEN : 0) |
            (attr->quality == PGWT_QUERY_QUALITY_PG13_SYNTH
                ? PGWT_EVENT_FLAG_QUERY_SYNTH : 0),""",
     "        .flags        = (attr->cmd_open ? PGWT_EVENT_FLAG_CMD_OPEN : 0),"),
    ("C: builder stops skipping markers (src/open_interval.h)",
     "src/open_interval.h",
     """    if (PGWT_IS_MARKER(st->last_event))
        return PGWT_OPEN_STRETCH_SKIP_MARKER;""",
     "    /* marker skip removed */"),
    ("D: a degraded reply leaks its rows (src/open_snapshot.h)",
     "src/open_snapshot.h",
     "    if (included) {",
     "    if (1) {"),
    ("E: the over-cap reply is sent anyway, truncated (src/open_snapshot.h)",
     "src/open_snapshot.h",
     "    if (len + 1 <= reply_max)\n        return root;",
     "    if (1)\n        return root;"),
    ("F: unmeasured cpu_ns serialised as a number (src/open_snapshot.h)",
     "src/open_snapshot.h",
     "            if (r->cpu_ns != PGWT_CPU_NS_UNKNOWN)\n                pgwt_json_u64(o, \"cpu_ns\", r->cpu_ns);",
     "            pgwt_json_u64(o, \"cpu_ns\", r->cpu_ns);"),
    ("G: the output queue drops on EAGAIN, as before #424 (src/ctrl_outq.h)",
     "src/ctrl_outq.h",
     "        if (errno == EAGAIN || errno == EWOULDBLOCK)\n            return 0;",
     "        if (errno == EAGAIN || errno == EWOULDBLOCK)\n            return -1;"),
    ("H: the backlog cap is removed (src/ctrl_outq.h)",
     "src/ctrl_outq.h",
     "    if (q->len + n > PGWT_CTRL_OUTQ_MAX) {",
     "    if (0) {"),
    ("I: wall-ns fields emitted as JSON numbers, not strings (src/open_snapshot.h)",
     "src/open_snapshot.h",
     "    snprintf(buf, sizeof(buf), \"%llu\", (unsigned long long)v);\n    cJSON_AddStringToObject(o, k, buf);",
     "    snprintf(buf, sizeof(buf), \"%llu\", (unsigned long long)v);\n    cJSON_AddRawToObject(o, k, buf);"),
    ("K: an unresolved wait category ships as \"\" (src/open_snapshot.h)",
     "src/open_snapshot.h",
     "            if (s->rows[i].category[0] != '\\0')\n                continue;",
     "            continue;"),
    ("L: the command-gate peek stops clamping to the stretch wall (src/map_reader.c)",
     "src/map_reader.c",
     "    uint64_t wall = now_ns > t0_ns ? now_ns - t0_ns : 0;\n    return open_ns < wall ? open_ns : wall;",
     "    return open_ns;"),
    ("M: the peek stops short-circuiting an unmarked pid (src/map_reader.c)",
     "src/map_reader.c",
     "uint64_t pgwt_live_cmd_gate_peek(const struct pgwt_live_cmd_gate *g,\n                                 uint64_t t0_ns, uint64_t now_ns)\n{\n    if (!g->seen)\n        return 0;",
     "uint64_t pgwt_live_cmd_gate_peek(const struct pgwt_live_cmd_gate *g,\n                                 uint64_t t0_ns, uint64_t now_ns)\n{"),
    ("J: the included+incomplete downgrade removed (src/open_snapshot.h)",
     "src/open_snapshot.h",
     "    if (s->status == PGWT_OPEN_SNAP_INCLUDED && !s->complete)",
     "    if (0)"),
]

def run(cmd):
    return subprocess.run(cmd, shell=True, cwd=root + "tests",
                          capture_output=True, text=True)

fails = 0
for name, path, old, new in CASES:
    full = root + path
    src = open(full).read()
    if src.count(old) != 1:
        print("SKIP (pattern not unique): " + name)
        fails += 1
        continue
    open(full, "w").write(src.replace(old, new, 1))
    try:
        b = run("make -B -s test_open_interval 2>&1")
        r = run("./test_open_interval")
        out = (b.stdout + b.stderr + r.stdout + r.stderr)
        red = r.returncode != 0 or b.returncode != 0
        lines = [l for l in out.splitlines()
                 if "FAIL(" in l or "error:" in l or "checks," in l]
        print(("RED  " if red else "GREEN") + "  " + name)
        for l in lines[:4]:
            print("        " + l.strip())
        if not red:
            fails += 1
            print("        !!! the gate did NOT detect this")
    finally:
        open(full, "w").write(src)

run("make -B -s test_open_interval")
r = run("./test_open_interval")
print("\nrestored tree: " + r.stdout.strip().splitlines()[-1] +
      "  exit=" + str(r.returncode))
sys.exit(1 if fails or r.returncode else 0)
