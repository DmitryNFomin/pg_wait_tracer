#!/usr/bin/env python3
"""Demonstrated red for tests/test_exec_index_guards.c.

The two guards under test (pgwt_execution_filter_verdict and
pgwt_execution_counts_known, src/compute.c) are NEW functions, so "run the
new test against the parent tree" cannot compile. The equivalent evidence is
this: every way the guard could be made blind is INJECTED into a copy of
compute.c, one at a time, and the unit test must go RED on it. A mutation
that leaves the test GREEN is a blind spot in the test, and this script
reports it as a FAILURE.

G01 is the original defect verbatim -- the predicate as it shipped at
f78ab4c, `!row->matches_event_filter`, which is TRUE for the index's -1
poison and therefore passed a never-tested row through a class filter.

The rest are the false-negative shapes: a gate that collapses "undecidable"
to keep or to drop, a gate that tests for exactly -1 instead of "not a
verdict", a gate that needs BOTH count fields poisoned, a gate that always
approves, and a gate so strict it refuses the legitimate 0/0 row (a gate
that refuses everything is indistinguishable from a broken feature, and is
how a guard gets reverted).

THE HARNESS'S OWN FALSE-NEGATIVE PATHS are tested too, because a mutation
harness that silently could not apply, could not build, or could not run
reports "0 not detected" and looks like success. --self-test exercises:
a needle that matches zero or several times, a mutant that does not compile,
a toolchain that is absent (exit 127) or not executable (exit 126), and a
no-op control that must stay GREEN. Each must come back FAIL / GREEN
respectively, never a silent pass.

Run from tests/:  python3 exec_index_guard_mutations.py [--self-test]
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
WORK = os.path.join(tempfile.gettempdir(), "pgwt-exec-guard-mutants")
OUT = os.path.join(HERE, "results", "exec-index-wiring", "mutations")

# The file that is copied and mutated. Only compute.c carries the guards.
MUTABLE = "compute.c"

# (name, needle, replacement). `needle` must appear EXACTLY ONCE.
MUTATIONS = [
    ("G01 the predicate as it shipped: `!row->matches_event_filter`",
     "        if (row->matches_event_filter < 0)\n"
     "            return -1;\n"
     "        if (!row->matches_event_filter)\n"
     "            return 0;",
     "        if (!row->matches_event_filter)\n"
     "            return 0;"),

    ("G02 undecidable collapses to DROP (tab silently shortened)",
     "        if (row->matches_event_filter < 0)\n"
     "            return -1;",
     "        if (row->matches_event_filter < 0)\n"
     "            return 0;"),

    ("G03 undecidable collapses to KEEP (tab silently unfiltered)",
     "        if (row->matches_event_filter < 0)\n"
     "            return -1;",
     "        if (row->matches_event_filter < 0)\n"
     "            return 1;"),

    ("G04 poison test is `== -1`, so any other sentinel walks through",
     "        if (row->matches_event_filter < 0)\n"
     "            return -1;",
     "        if (row->matches_event_filter == -1)\n"
     "            return -1;"),

    ("G05 boundary dimensions checked AFTER the event one: a pid mismatch "
     "becomes undecidable, so a pid-filtered drill-down can never be served",
     "    if (f->pid && row->pid != f->pid)\n"
     "        return 0;\n"
     "    if (f->query_id && row->query_id != f->query_id)\n"
     "        return 0;\n"
     "    if (f->class_name[0] || f->event_id) {",
     "    if (f->class_name[0] || f->event_id) {\n"
     "        if (row->matches_event_filter < 0)\n"
     "            return -1;\n"
     "    }\n"
     "    if (f->pid && row->pid != f->pid)\n"
     "        return 0;\n"
     "    if (f->query_id && row->query_id != f->query_id)\n"
     "        return 0;\n"
     "    if (f->class_name[0] || f->event_id) {"),

    ("G06 a NULL row reads as KEEP",
     "int pgwt_execution_filter_verdict(const struct pgwt_execution *row,\n"
     "                                  const struct pgwt_filter *f)\n"
     "{\n"
     "    if (!row)\n"
     "        return 0;",
     "int pgwt_execution_filter_verdict(const struct pgwt_execution *row,\n"
     "                                  const struct pgwt_filter *f)\n"
     "{\n"
     "    if (!row)\n"
     "        return 1;"),

    ("G07 counts_known needs BOTH fields poisoned (half-poisoned row passes)",
     "    return row->n_events >= 0 && row->n_workers >= 0;",
     "    return row->n_events >= 0 || row->n_workers >= 0;"),

    ("G08 counts_known tests `!= -1` instead of `>= 0`",
     "    return row->n_events >= 0 && row->n_workers >= 0;",
     "    return row->n_events != -1 && row->n_workers != -1;"),

    ("G09 counts_known always approves (a gate that cannot see, approving)",
     "    return row->n_events >= 0 && row->n_workers >= 0;",
     "    return 1;"),

    ("G10 counts_known rejects the legitimate 0/0 row (refuses everything)",
     "    return row->n_events >= 0 && row->n_workers >= 0;",
     "    return row->n_events > 0 && row->n_workers > 0;"),

    ("G11 a NULL row reads as serialisable",
     "int pgwt_execution_counts_known(const struct pgwt_execution *row)\n"
     "{\n"
     "    if (!row)\n"
     "        return 0;",
     "int pgwt_execution_counts_known(const struct pgwt_execution *row)\n"
     "{\n"
     "    if (!row)\n"
     "        return 1;"),
]

# Semantic no-ops. These must stay GREEN: if a control goes red, the test is
# reacting to something other than the behaviour it claims to pin.
CONTROLS = [
    ("C01 a comment added inside the verdict function",
     "    if (!row)\n        return 0;\n    if (!f)\n        return 1;",
     "    /* control: no-op */\n"
     "    if (!row)\n        return 0;\n    if (!f)\n        return 1;"),

    ("C02 `< 0` written as `<= -1` (identical on ints)",
     "        if (row->matches_event_filter < 0)\n"
     "            return -1;",
     "        if (row->matches_event_filter <= -1)\n"
     "            return -1;"),
]


def mac_stub(path):
    with open(path, "w") as fh:
        fh.write("#include <stdio.h>\n"
                 "FILE *pgwt_proc_open(char *const argv[], int *pid);\n"
                 "int pgwt_proc_close(FILE *fp, int pid);\n"
                 "FILE *pgwt_proc_open(char *const a[], int *p)"
                 " { (void)a; (void)p; return NULL; }\n"
                 "int pgwt_proc_close(FILE *f, int p)"
                 " { (void)f; (void)p; return -1; }\n")


def build(workdir, binary, extra_sources, cc="cc"):
    cmd = [cc, "-g", "-O1", "-w",
           "-I", workdir, "-I", SRC, "-DPGWT_SERVER",
           "-o", binary,
           os.path.join(HERE, "test_exec_index_guards.c"),
           os.path.join(workdir, MUTABLE),
           os.path.join(SRC, "wait_event.c"),
           os.path.join(SRC, "idle_rule.c"),
           os.path.join(SRC, "cJSON.c")] + extra_sources + ["-lm"]
    try:
        return subprocess.run(cmd, capture_output=True, text=True)
    except OSError as exc:
        # Toolchain absent entirely. Shaped like a 127 so the caller's one
        # verdict path handles it: a harness that cannot build must FAIL.
        return subprocess.CompletedProcess(cmd, 127, "", "%s" % exc)


def evaluate(name, needle, repl, extra, want, cc="cc", keep=None):
    """Apply one mutation and judge it.

    Returns (verdict, detail) where verdict is one of "RED", "GREEN" or
    "FAIL". `want` is "RED" for a mutation or "GREEN" for a control; FAIL
    is reserved for the harness's OWN failure modes -- needle did not apply,
    mutant did not compile, binary could not be executed.
    """
    d = os.path.join(WORK, "m_%s" % abs(hash(name)))
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    with open(os.path.join(SRC, MUTABLE)) as fh:
        text = fh.read()

    hits = text.count(needle)
    if hits != 1:
        return ("FAIL", "needle matched %d times, must match exactly 1 "
                        "(the mutation did not apply -- this script cannot "
                        "see)" % hits)
    with open(os.path.join(d, MUTABLE), "w") as fh:
        fh.write(text.replace(needle, repl))

    binary = os.path.join(d, "t")
    b = build(d, binary, extra, cc=cc)
    if b.returncode != 0:
        return ("FAIL", "mutant did not COMPILE (cc exit %d) -- a mutation "
                        "that cannot build is not evidence\n%s"
                        % (b.returncode, b.stderr[-600:]))
    try:
        r = subprocess.run([binary], capture_output=True, text=True,
                           timeout=300)
    except OSError as exc:
        return ("FAIL", "mutant binary could not be executed: %s" % exc)
    if r.returncode in (126, 127):
        return ("FAIL", "mutant binary exited %d (not executable / not "
                        "found) -- that is a broken invocation, not a "
                        "detected mutation" % r.returncode)
    if keep:
        os.makedirs(OUT, exist_ok=True)
        with open(os.path.join(OUT, keep), "w") as fh:
            fh.write("%s\n\n%s" % (name, r.stdout))
    fails = [l.strip() for l in r.stdout.splitlines()
             if l.strip().startswith("FAIL")]
    if r.returncode == 0:
        verdict = "GREEN"
        detail = "test passed"
    else:
        verdict = "RED"
        detail = "%d failing checks; first: %s" % (
            len(fails), fails[0][:140] if fails else "(crash/abort)")
    shutil.rmtree(d, ignore_errors=True)
    return (verdict, detail)


def platform_extra():
    # src/spawn.c uses pipe2(), which Darwin lacks; wait_event.c needs its
    # two symbols. Linux links the real file, macOS a stub. Neither path
    # SKIPS: a build failure is reported as a failure, because a harness
    # that could not build proves nothing.
    if sys.platform == "darwin":
        stub = os.path.join(WORK, "mac_stub.c")
        mac_stub(stub)
        return [stub]
    return [os.path.join(SRC, "spawn.c")]


def self_test(extra):
    """The harness's own false-negative paths. Each must come back FAIL."""
    print("--- self-test: the harness's own blind spots ---")
    bad = 0
    cases = [
        ("needle matches ZERO times",
         "this string is not in compute.c anywhere", "x", "cc"),
        ("needle matches SEVERAL times",
         "    return 0;", "    return 0;", "cc"),
        ("mutant does not COMPILE",
         "    return row->n_events >= 0 && row->n_workers >= 0;",
         "    return row->n_events >= 0 && ;", "cc"),
        ("toolchain missing (exit 127)",
         "    return row->n_events >= 0 && row->n_workers >= 0;",
         "    return 1;", "cc-that-does-not-exist"),
    ]
    for label, needle, repl, cc in cases:
        v, detail = evaluate("self:" + label, needle, repl, extra,
                             want="RED", cc=cc)
        ok = v == "FAIL"
        print("  %-7s %s%s" % ("ok" if ok else "BROKEN", label,
                               "" if ok else "  -> got %s" % v))
        if not ok:
            bad += 1

    # A not-executable dependency (exit 126). Built, then chmod -x.
    d = os.path.join(WORK, "x126")
    os.makedirs(d, exist_ok=True)
    shutil.copy(os.path.join(SRC, MUTABLE), os.path.join(d, MUTABLE))
    binary = os.path.join(d, "t")
    if build(d, binary, extra).returncode == 0:
        os.chmod(binary, 0o444)
        r = subprocess.run(["/bin/sh", "-c", '"%s"; echo exit=$?' % binary],
                           capture_output=True, text=True)
        ok = "exit=126" in r.stdout
        print("  %-7s a dependency exiting 126 is observable%s"
              % ("ok" if ok else "BROKEN", "" if ok else " -> %s" % r.stdout))
        if not ok:
            bad += 1
    else:
        print("  BROKEN  could not build the 126 probe")
        bad += 1
    shutil.rmtree(d, ignore_errors=True)
    return bad


def main():
    want_self_test = "--self-test" in sys.argv
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    os.makedirs(OUT, exist_ok=True)
    extra = platform_extra()

    # 0. The baseline MUST be green, or every "red" below proves nothing.
    base = os.path.join(WORK, "base")
    os.makedirs(base)
    shutil.copy(os.path.join(SRC, MUTABLE), os.path.join(base, MUTABLE))
    b = build(base, os.path.join(base, "t"), extra)
    if b.returncode != 0:
        print("FAIL: pristine build failed\n%s" % b.stderr[-2000:])
        return 1
    r = subprocess.run([os.path.join(base, "t")], capture_output=True,
                       text=True)
    if r.returncode != 0:
        print("FAIL: pristine test is NOT green -- nothing below is evidence")
        print(r.stdout[-3000:])
        return 1
    print("baseline: pristine compute.c, test PASSED (%s)"
          % r.stdout.strip().splitlines()[-1])
    print()

    failures = 0
    for i, (name, needle, repl) in enumerate(MUTATIONS, 1):
        v, detail = evaluate(name, needle, repl, extra, want="RED",
                             keep="g%02d.txt" % i)
        if v == "RED":
            print("RED   %s\n        -> %s" % (name, detail))
        elif v == "GREEN":
            print("FAIL  %s: test stayed GREEN -- BLIND SPOT" % name)
            failures += 1
        else:
            print("FAIL  %s: %s" % (name, detail))
            failures += 1

    print()
    for name, needle, repl in CONTROLS:
        v, detail = evaluate(name, needle, repl, extra, want="GREEN")
        if v == "GREEN":
            print("GREEN %s (control: a no-op must not go red)" % name)
        else:
            print("FAIL  %s: a semantic no-op changed the verdict (%s: %s)"
                  % (name, v, detail))
            failures += 1

    print()
    if want_self_test:
        failures += self_test(extra)
        print()

    shutil.rmtree(WORK, ignore_errors=True)
    print("%d mutations, %d controls, %d not detected"
          % (len(MUTATIONS), len(CONTROLS), failures))
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
