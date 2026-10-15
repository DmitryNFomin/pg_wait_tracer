"""Bypass suite for v9-dual-path-probe.sh: each way the probe could fail to
detect what it exists to detect. Writes the evidence log."""
import subprocess

root = "/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-a283b5c4ab073a48e/"
PROBE = "tests/results/issue424-daemon/v9-dual-path-probe.sh"
OUT = root + "tests/results/issue424-daemon/v9-probe-bypass.log"

CASES = [
    ("P1 v9 call loses one argument (the case the FIRST version of this probe "
     "reported as OK)", "src/map_reader.c",
     "                                              sval.last_ts, iv.cat_flag, now,\n                                              &obs);",
     "                                              sval.last_ts, iv.cat_flag,\n                                              &obs);"),
    ("P2 v9 counter field misspelled in the metrics block", "src/control.c",
     "open_cpu_credited_ns_total : 0);", "open_cpu_creditedx_ns_total : 0);"),
    ("P3 the #if guard removed, so the v8 build would not compile",
     "src/map_reader.c", "#if PGWT_SUMMARY_VERSION >= 9", "#if 1"),
    ("P4 the v9 call deleted entirely (producer silently unwired)",
     "src/map_reader.c",
     "                    pgwt_summary_note_open_ex(d->summary_writer, snext, we,\n                                              sval.last_ts, iv.cat_flag, now,\n                                              &obs);",
     "                    (void)obs;"),
    ("P7 the v9 metrics block deleted (the verification counter is gone)",
     "src/control.c",
     '    cjson_add_uint64(root, "summary_open_cpu_credited_ns",',
     '    cjson_add_uint64(root, "summary_open_cpu_credited_ns_DELETED",'),
]

def run(args):
    return subprocess.run([PROBE] + args, cwd=root, capture_output=True,
                          text=True)

lines = []
def say(s):
    print(s)
    lines.append(s)

r = run([])
say("BASELINE (clean tree): exit=%d  %s" % (r.returncode, r.stdout.strip().splitlines()[-1]))

for name, path, old, new in CASES:
    full = root + path
    src = open(full).read()
    assert src.count(old) == 1, name
    open(full, "w").write(src.replace(old, new, 1))
    try:
        r = run([])
        verdict = [l for l in r.stdout.splitlines()
                   if l.startswith("PROBE") or " FAIL " in l]
        say("%-12s exit=%d  %s" % (name.split()[0], r.returncode,
                                   "RED" if r.returncode != 0 else
                                   "*** GREEN: THE PROBE CANNOT SEE THIS ***"))
        for v in verdict[:3]:
            say("             " + v.strip())
    finally:
        open(full, "w").write(src)

# Refusal paths: the probe must REFUSE (exit 2), never pass, when it cannot
# resolve a v9 header to compile against.
r = run(["/nonexistent-worktree"])
say("P5 missing v9 worktree        exit=%d  %s" % (
    r.returncode, r.stderr.strip().splitlines()[0] if r.stderr else ""))
r = run(["/tmp/pgwtv8fake"])
say("P6 header is v8, not v9       exit=%d  %s" % (
    r.returncode, r.stderr.strip().splitlines()[0] if r.stderr else ""))

r = run([])
say("RESTORED: exit=%d  %s" % (r.returncode, r.stdout.strip().splitlines()[-1]))
open(OUT, "w").write("\n".join(lines) + "\n")
print("\nwrote " + OUT)
