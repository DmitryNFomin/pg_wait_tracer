#!/usr/bin/env python3
"""Test-side sabotage probes for issue #334 / #348.

Each case copies the tree, applies ONE sabotage to tests/
test_data_current_trace_cache.py (never to src/), runs the single affected
function, and requires the named assertions to appear as FAIL. A sabotage that
does not go red means the assertion it targets cannot see what it claims to
check.
"""
import os, shutil, subprocess, sys

SRC = "/root/pgwt-334-iter"
WORK = "/root/sab-all"
FILE = "tests/test_data_current_trace_cache.py"

FIXTURE = ('    trace_dir = generate_traces(busy_scenario(span_s=12, pids=3, '
           'per_pid=2500))\n'
           '    tmp = tempfile.mkdtemp(prefix="pgwt_cc_err_")\n'
           '    try:\n'
           '        err = os.path.join(tmp, "trunc.err")')
ENVBLOCK = ('        env = dict(STATS_ENV)\n'
            '        env["PGWT_CURRENT_TRACE_CACHE"] = "1"\n'
            '        path = os.path.join(trace_dir, "current.trace")')
TAIL_CHECK = "    tail = size - off\n    if tail == 0:\n        return out"
NO_BLOCK = ('    if not out:\n'
            '        raise AssertionError("%s holds no readable block at all '
            '(%d bytes, "\n'
            '                             "stopped at %d)" % (path, size, off))')

CASES = [
    ("S1 cut by byte count again -- the ORIGINAL vacuity",
     "bypass_truncated_file_under_meta",
     [("        cut = extents[keep - 1][1]",
       "        cut = os.path.getsize(path) // 3")],
     ["a byte-truncated current.trace serves a nonempty SMALLER",
      "the fresh uncached read of the truncated file is nonempty"]),
    ("S2 cache never consulted -- the blindness guard",
     "bypass_truncated_file_under_meta",
     [(ENVBLOCK, ENVBLOCK.replace('"PGWT_CURRENT_TRACE_CACHE"] = "1"',
                                  '"PGWT_CURRENT_TRACE_CACHE"] = "0"'))],
     ["the cache served the pre-truncation request"]),
    ("S3 a truncation that removes nothing at all",
     "bypass_truncated_file_under_meta",
     [("        cut = extents[keep - 1][1]",
       "        cut = os.path.getsize(path)")],
     ["the file really lost its trailing block"]),
    ("S4 a one-block fixture: nothing to remove must REFUSE, never skip",
     "bypass_truncated_file_under_meta",
     [(FIXTURE, FIXTURE.replace("pids=3, per_pid=2500",
                                "pids=1, per_pid=1200"))],
     ["the fixture has whole blocks to remove"]),
    ("S5 block_extents stops accounting for the tail (no footer cross-check)",
     "bypass_block_extents_refuses",
     [(TAIL_CHECK, TAIL_CHECK.replace("if tail == 0:", "if True:"))],
     ["trailing bytes after the footer is refused",
      "a footer that counts one block too many is refused",
      "a footer that puts block 0 somewhere else is refused"]),
    ("S6 block_extents answers on a file it could not read",
     "bypass_block_extents_refuses",
     [(NO_BLOCK, "    if not out:\n        return [(28, size)]")],
     ["a header with no block at all is refused",
      "a torn block header is refused",
      "a block payload cut short is refused"]),
    # Does this test LEAN on #342 (the one request after a shrink answers
    # empty), or merely order around it? Delete the interposed request and
    # find out: if the assertion stayed green, #342 would be satisfying it.
    ("S7 the #342 ordering request removed: does the test lean on the bug?",
     "bypass_truncated_file_under_meta",
     [('            srv.query("info")\n'
       '            after = srv.query(PROBE_CMD, from_=w_from, to_=w_to)',
       '            after = srv.query(PROBE_CMD, from_=w_from, to_=w_to)')],
     ["a byte-truncated current.trace serves a nonempty SMALLER"]),
]


def main():
    rc = 0
    for label, fn, patches, markers in CASES:
        shutil.rmtree(WORK, ignore_errors=True)
        subprocess.run(["cp", "-a", SRC, WORK], check=True)
        path = os.path.join(WORK, FILE)
        text = open(path).read()
        bad = False
        for old, new in patches:
            if text.count(old) != 1:
                print("FAIL %s: patch needle matched %d times"
                      % (label, text.count(old)))
                rc = 1
                bad = True
                break
            text = text.replace(old, new)
        if bad:
            continue
        open(path, "w").write(text)
        run = ("import sys; sys.path.insert(0,'.');"
               "import test_data_current_trace_cache as m;"
               "r=m.TestRunner('%s');m.%s(r);"
               "raise SystemExit(0 if r.summary() else 1)" % (label, fn))
        res = subprocess.run([sys.executable, "-c", run],
                             cwd=os.path.join(WORK, "tests"),
                             text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=900)
        lines = [l.strip() for l in res.stdout.splitlines()
                 if l.strip().startswith(("PASS:", "FAIL:"))]
        fails = [l for l in lines if l.startswith("FAIL:")]
        got = all(any(m in l for l in fails) for m in markers)
        ok = res.returncode != 0 and got
        print("%s %s   (%d checks, %d failed)"
              % ("RED " if ok else "NOT-RED", label, len(lines), len(fails)))
        for l in fails:
            print("     %s" % l[:200])
        if not ok:
            print(res.stdout[-1500:])
            rc = 1
    shutil.rmtree(WORK, ignore_errors=True)
    print("ALL SABOTAGES WENT RED" if rc == 0 else "SOME SABOTAGE WAS NOT RED")
    return rc


if __name__ == "__main__":
    sys.exit(main())
