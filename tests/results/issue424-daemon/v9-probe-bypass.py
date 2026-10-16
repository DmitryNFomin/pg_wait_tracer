"""Bypass suite for v9-dual-path-probe.sh after the v9 producer was reverted:
the probe's job is now (a) control.c's two #if branches each compile and are
each confirmed, and (b) map_reader.c compiles under both writer versions and
does NOT call the dormant v9 producer entry point."""
import subprocess, os

root = "/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-a283b5c4ab073a48e/"
PROBE = "tests/results/issue424-daemon/v9-dual-path-probe.sh"
OUT = root + "tests/results/issue424-daemon/v9-probe-bypass.log"

CALL = """            if (d->summary_writer)
                pgwt_summary_note_open(d->summary_writer, snext, we,"""
REWIRED = """            if (d->summary_writer) {
                struct pgwt_summary_open_cpu obs = { .eff_we = 0,
                                                     .cpu_prefix_ns = 0 };
                pgwt_summary_note_open_ex(d->summary_writer, snext, we,
                                          sval.last_ts, 0, now, &obs);
            }
            if (0)
                pgwt_summary_note_open(d->summary_writer, snext, we,"""

CASES = [
    ("D1 the dormant v9 producer is re-wired in map_reader.c",
     "src/map_reader.c", CALL, REWIRED),
    ("D2 control.c's v9 metrics block leaks into the v8 build",
     "src/control.c", "#if PGWT_SUMMARY_VERSION >= 9\n    /* v9 (#424). THE CAPABILITY IS DORMANT",
     "#if 1\n    /* v9 (#424). THE CAPABILITY IS DORMANT"),
    ("D3 a v9 counter field misspelled",
     "src/control.c", "open_cpu_credited_ns_total : 0);",
     "open_cpu_creditedx_ns_total : 0);"),
    ("D4 the v9 metrics block renamed away (verification counter gone)",
     "src/control.c", '    cjson_add_uint64(root, "summary_open_cpu_credited_ns",',
     '    cjson_add_uint64(root, "summary_open_cpu_credited_ns_GONE",'),
]

def run(args=None):
    return subprocess.run([PROBE] + (args or []), cwd=root,
                          capture_output=True, text=True)

lines = []
def say(s):
    print(s); lines.append(s)

r = run()
say("BASELINE (dormant, clean tree)        exit=%d  %s"
    % (r.returncode, r.stdout.strip().splitlines()[-1]))

for name, path, old, new in CASES:
    full = root + path
    src = open(full).read()
    if src.count(old) != 1:
        say("%-38s SKIP (pattern not unique)" % name.split()[0]); continue
    open(full, "w").write(src.replace(old, new, 1))
    try:
        r = run()
        say("%-38s exit=%d  %s" % (name, r.returncode,
            "RED" if r.returncode != 0 else "*** GREEN: THE PROBE CANNOT SEE THIS ***"))
        for l in r.stdout.splitlines():
            if " FAIL " in l:
                say("      " + l.strip()[:120])
    finally:
        open(full, "w").write(src)

r = run(["/nonexistent-worktree"])
say("D5 missing v9 header                  exit=%d  %s"
    % (r.returncode, (r.stderr.strip().splitlines() or [""])[0]))
r = run(["/tmp/pgwtv8fake"])
say("D6 header is v8, not v9               exit=%d  %s"
    % (r.returncode, (r.stderr.strip().splitlines() or [""])[0]))

r = run()
say("RESTORED                              exit=%d  %s"
    % (r.returncode, r.stdout.strip().splitlines()[-1]))
open(OUT, "w").write("\n".join(lines) + "\n")
print("\nwrote " + OUT)
