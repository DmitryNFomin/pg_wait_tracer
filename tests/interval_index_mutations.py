#!/usr/bin/env python3
"""Demonstrated red for tests/test_interval_index.c (paint-latency Phase 4).

src/interval_index.c is a NEW module, so "run the new test against the parent
tree" is not available: the parent tree has no module to test. The equivalent
evidence is this — every silent-wrong risk the index was written to avoid is
INJECTED into a copy of the module, one at a time, and the unit test must go
RED on it. A mutation that leaves the test GREEN is a blind spot in the test,
and this script reports it as a FAILURE.

WHY THIS SCRIPT'S OWN FALSE NEGATIVES ARE THE REAL RISK. Showing a gate can
go red proves the true-positive path, which has never been the broken one.
The ways a mutation harness silently approves everything are:

  NOT-APPLIED   the needle no longer matches (the module was edited), so the
                "mutant" is the pristine module and its pass means nothing;
  BUILD-FAILED  the mutant did not compile — a red exit code that is not a
                detection. A harness scoring that as a catch would "detect"
                16 of 16 mutants with a broken compiler;
  TOOL-MISSING  `cc` absent (127) or not executable (126), which turns every
                mutant into BUILD-FAILED at once;
  BASELINE-RED  the pristine test was already failing, so every mutant's red
                proves nothing;
  STAYED-GREEN  the mutation applied, built and ran, and the test did not
                notice. The only real blind spot, and the only one most
                harnesses look for.

All five are distinct outcomes here, all five are FAILURES, and
`--self-test` drives every one of them deliberately and requires this script
to exit nonzero on each. It also requires the pristine `oneblock`
falsification run (the unit test's own non-vacuity check) to exit nonzero.

Run from tests/:  python3 interval_index_mutations.py
                  python3 interval_index_mutations.py --self-test
"""

import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
TEST_SRC = os.path.join(HERE, "test_interval_index.c")
# Mutant build trees are throwaway BINARIES: a temp dir, never tests/results
# (which box-check rsyncs and a reviewer reads).
WORK = os.path.join(tempfile.gettempdir(), "pgwt-interval-index-mutants")
# Only the text output of each mutant run is kept as evidence.
OUT = os.path.join(HERE, "results", "interval-index", "mutations")

BUILD_TIMEOUT = 300
RUN_TIMEOUT = 600

# (name, file, needle, replacement). `needle` must appear EXACTLY ONCE in the
# pristine source.
MUTATIONS = [
    # ── silent-wrong mode 1: a clipped start_ns ──────────────────────────
    ("M01 start_ns CLIPPED to the block start (the cardinal error)",
     "interval_index.c",
     "        iv->start_ns = ev->timestamp_ns - ev->duration_ns;",
     "        iv->start_ns = ev->timestamp_ns - ev->duration_ns;\n"
     "        if (iv->start_ns < block_first_ns)\n"
     "            iv->start_ns = block_first_ns;"),

    ("M02 start_ns CLIPPED to the indexed coverage start",
     "interval_index.c",
     "        iv->start_ns = ev->timestamp_ns - ev->duration_ns;\n"
     "        iv->pid = ev->pid;",
     "        iv->start_ns = ev->timestamp_ns - ev->duration_ns;\n"
     "        if (idx->n_chunks > 0 && iv->start_ns < idx->cover_from_ns)\n"
     "            iv->start_ns = idx->cover_from_ns;\n"
     "        iv->pid = ev->pid;"),

    ("M03 start_ns := end_ns (the record's duration is ignored)",
     "interval_index.c",
     "        iv->start_ns = ev->timestamp_ns - ev->duration_ns;\n"
     "        iv->pid = ev->pid;\n"
     "        iv->event_id = ev->old_event;",
     "        iv->start_ns = ev->timestamp_ns;\n"
     "        iv->pid = ev->pid;\n"
     "        iv->event_id = ev->old_event;"),

    ("M04 the materialised record loses its duration",
     "interval_index.c",
     "                e->duration_ns = iv->end_ns - iv->start_ns;",
     "                e->duration_ns = 0;"),

    # ── silent-wrong mode 2: an interval counted twice ───────────────────
    ("M05 a boundary-straddling interval is stored TWICE",
     "interval_index.c",
     "        if (idx->n_chunks > 0 && iv->start_ns < block_first_ns)\n"
     "            idx->n_cross_block++;\n"
     "    }",
     "        if (idx->n_chunks > 0 && iv->start_ns < block_first_ns) {\n"
     "            idx->n_cross_block++;\n"
     "            struct pgwt_wait_interval dup = *iv;\n"
     "            if (rows_reserve(idx, 1) == 0)\n"
     "                idx->rows[idx->n_rows++] = dup;\n"
     "        }\n"
     "    }"),

    # ── silent-wrong mode 3: the open-past-`to` exclusion ────────────────
    ("M06 waits still open at `to` are INCLUDED (semantics changed)",
     "interval_index.c",
     "            if (iv->end_ns > to_ns) {",
     "            if (iv->end_ns > to_ns + 1000000000ULL) {"),

    # ── selection bounds: half-open where the loader is inclusive ────────
    ("M07 selection narrowed to half-open at `to`",
     "interval_index.c",
     "            if (iv->end_ns > to_ns) {",
     "            if (iv->end_ns >= to_ns) {"),

    ("M08 selection narrowed to half-open at `from`",
     "interval_index.c",
     "        if (rows[mid].end_ns < from_ns)",
     "        if (rows[mid].end_ns <= from_ns)"),

    ("M09 chunk prefilter narrowed at `from` (skips a live chunk)",
     "interval_index.c",
     "        if (ch->n_rows == 0 || ch->max_end_ns < from_ns ||",
     "        if (ch->n_rows == 0 || ch->max_end_ns <= from_ns ||"),

    ("M10 chunk prefilter narrowed at `to`",
     "interval_index.c",
     "            ch->min_end_ns > to_ns)",
     "            ch->min_end_ns >= to_ns)"),

    # ── the restated predicate ───────────────────────────────────────────
    ("M11 the idle-wait exclusion is dropped from the predicate",
     "interval_index.c",
     "    if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))",
     "    if (!pgwt_filter_matches(f, ev))"),

    ("M12 the FID-4 chokepoint is dropped (markers become intervals)",
     "interval_index.c",
     "    if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))\n"
     "        return 0;",
     "    if (pgwt_is_idle_event(ev->old_event))\n"
     "        return 0;"),

    ("M13 on-CPU records become intervals",
     "interval_index.c",
     "    if (ev->old_event == 0)\n"
     "        return 0;       /* on-CPU: not a wait, never part of a burst */",
     "    /* mutant: on-CPU records kept */"),

    # ── fall back, never guess ───────────────────────────────────────────
    ("M14 an empty index answers 'no concurrency' instead of refusing",
     "interval_index.c",
     "    if (idx->n_chunks == 0)\n"
     "        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_EMPTY);",
     "    if (idx->n_chunks == 0) {\n"
     "        out->refused = PGWT_INTERVAL_INDEX_OK;\n"
     "        return 0;\n"
     "    }"),

    ("M15 coverage check is an intersection, not containment",
     "interval_index.c",
     "    if (from_ns < idx->cover_from_ns || to_ns > idx->cover_to_ns)",
     "    if (to_ns < idx->cover_from_ns || from_ns > idx->cover_to_ns)"),

    ("M16 a FILTERED request is answered from the unfiltered index",
     "interval_index.c",
     "    if (!is_unfiltered(f))\n"
     "        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_FILTERED);",
     "    /* mutant: filtered requests answered anyway */"),

    ("M17 a window selecting nothing answers zeroes instead of refusing",
     "interval_index.c",
     "    if (nsel == 0)\n"
     "        return query_refuse(out, "
     "PGWT_INTERVAL_INDEX_REFUSE_NO_INTERVALS);",
     "    if (nsel == 0) {\n"
     "        out->refused = PGWT_INTERVAL_INDEX_OK;\n"
     "        out->have_result = 1;\n"
     "        return 0;\n"
     "    }"),

    ("M18 an inverted/zero-width window is not refused",
     "interval_index.c",
     "    if (from_ns >= to_ns)\n"
     "        return query_refuse(out, "
     "PGWT_INTERVAL_INDEX_REFUSE_BAD_WINDOW);",
     "    if (from_ns > to_ns + 1) { }"),

    ("M19 num_buckets <= 0 is not refused",
     "interval_index.c",
     "    if (num_buckets <= 0)\n"
     "        return query_refuse(out, "
     "PGWT_INTERVAL_INDEX_REFUSE_BAD_BUCKETS);",
     "    if (num_buckets < -1000000) { }"),

    ("M20 the version stamp is not checked on query",
     "interval_index.c",
     "    if (idx->version != PGWT_INTERVAL_INDEX_VERSION)\n"
     "        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_VERSION);",
     "    if (idx->version == 0)\n"
     "        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_VERSION);"),

    ("M21 an unsealed index answers",
     "interval_index.c",
     "    if (!idx->sealed)\n"
     "        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_UNSEALED);",
     "    /* mutant: unsealed indexes answer */"),

    ("M22 a skipped block in the feed is accepted",
     "interval_index.c",
     "    if (block_idx != idx->next_block_idx)\n"
     "        return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);",
     "    if (block_idx < 0)\n"
     "        return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);"),

    ("M23 an out-of-order record is accepted silently",
     "interval_index.c",
     "        if (have_last && ev->timestamp_ns < last_ts)\n"
     "            return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);",
     "        if (have_last && ev->timestamp_ns + 1 == 0)\n"
     "            return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);"),

    ("M24 a record outside its own block's bounds is accepted",
     "interval_index.c",
     "        if (ev->timestamp_ns < block_first_ns ||\n"
     "            ev->timestamp_ns > block_last_ns)\n"
     "            return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);",
     "        /* mutant: block bounds not enforced */"),

    ("M25 seal's sorted-by-end_ns invariant is not verified",
     "interval_index.c",
     "        if (idx->rows[i].end_ns < idx->rows[i - 1].end_ns)\n"
     "            return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);",
     "        if (idx->rows[i].end_ns + 1 == 0)\n"
     "            return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);"),

    ("M26 a build allocation failure does not make the index unusable",
     "interval_index.c",
     "    idx->failed = 1;\n"
     "    if (idx->build_refusal == PGWT_INTERVAL_INDEX_OK)",
     "    idx->failed = 0;\n"
     "    if (idx->build_refusal == PGWT_INTERVAL_INDEX_OK)"),

    ("M27 a failed computation is reported as a successful answer",
     "interval_index.c",
     "    if (out->result.failed) {",
     "    if (out->result.failed && 0) {"),
]

PRISTINE_FILES = ("interval_index.c", "interval_index.h")


def mac_stub(path):
    with open(path, "w") as fh:
        fh.write("#include <stdio.h>\n"
                 "FILE *pgwt_proc_open(char *const argv[], int *pid);\n"
                 "int pgwt_proc_close(FILE *fp, int pid);\n"
                 "FILE *pgwt_proc_open(char *const a[], int *p)"
                 " { (void)a; (void)p; return NULL; }\n"
                 "int pgwt_proc_close(FILE *f, int p)"
                 " { (void)f; (void)p; return -1; }\n")


def build(workdir, binary, extra_sources, cc="cc", test_src=TEST_SRC):
    cmd = [cc, "-g", "-O1", "-w",
           "-I", workdir, "-I", SRC, "-DPGWT_SERVER",
           "-o", binary,
           test_src,
           os.path.join(workdir, "interval_index.c"),
           os.path.join(SRC, "compute.c"),
           os.path.join(SRC, "wait_event.c"),
           os.path.join(SRC, "idle_rule.c"),
           os.path.join(SRC, "cJSON.c")] + extra_sources + ["-lm"]
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              timeout=BUILD_TIMEOUT)
    except FileNotFoundError as e:
        # TOOL-MISSING: equivalent to exit 127. Never a detection.
        return subprocess.CompletedProcess(cmd, 127, "", "cc not found: %s" % e)
    except PermissionError as e:
        return subprocess.CompletedProcess(cmd, 126, "",
                                           "cc not executable: %s" % e)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124, "", "build timed out")


def read_pristine():
    out = {}
    for name in PRISTINE_FILES:
        with open(os.path.join(SRC, name)) as fh:
            out[name] = fh.read()
    return out


def run_suite(mutations, cc="cc", test_src=TEST_SRC, label="", verbose=True,
              keep_outputs=True):
    """Returns (failures, tally). Every non-RED outcome is a failure."""
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    if keep_outputs:
        os.makedirs(OUT, exist_ok=True)

    if sys.platform == "darwin":
        stub = os.path.join(WORK, "mac_stub.c")
        mac_stub(stub)
        extra = [stub]
    else:
        extra = [os.path.join(SRC, "spawn.c")]

    pristine = read_pristine()
    tally = {"RED": 0, "STAYED-GREEN": 0, "BUILD-FAILED": 0,
             "NOT-APPLIED": 0, "TOOL-MISSING": 0, "BASELINE-RED": 0}

    # 0. The baseline MUST build and be green, or every "red" below is noise.
    base = os.path.join(WORK, "base")
    os.makedirs(base)
    for n, t in pristine.items():
        with open(os.path.join(base, n), "w") as fh:
            fh.write(t)
    b = build(base, os.path.join(base, "t"), extra, cc=cc, test_src=test_src)
    if b.returncode in (126, 127):
        tally["TOOL-MISSING"] += 1
        print("FAIL%s: the compiler is missing or not executable (exit %d). "
              "Every mutant would be BUILD-FAILED, which is NOT a detection."
              % (label, b.returncode))
        print(b.stderr[-800:])
        return 1, tally
    if b.returncode != 0:
        tally["BASELINE-RED"] += 1
        print("FAIL%s: pristine build failed — nothing below is evidence\n%s"
              % (label, b.stderr[-2000:]))
        return 1, tally
    try:
        r = subprocess.run([os.path.join(base, "t")], capture_output=True,
                           text=True, timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        tally["BASELINE-RED"] += 1
        print("FAIL%s: pristine test TIMED OUT — a hang is not a pass" % label)
        return 1, tally
    if r.returncode != 0:
        tally["BASELINE-RED"] += 1
        print("FAIL%s: pristine test is NOT green — nothing below is evidence"
              % label)
        print(r.stdout[-3000:])
        return 1, tally
    if verbose:
        print("baseline: pristine module, test PASSED (%s)"
              % r.stdout.strip().splitlines()[-2])

    # 0b. The unit test's own non-vacuity ledger must be falsifiable: feeding
    # every fixture as ONE block has to redden the boundary rows. A green
    # `oneblock` run would mean the ledger cannot see.
    try:
        ob = subprocess.run([os.path.join(base, "t"), "oneblock"],
                            capture_output=True, text=True,
                            timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        ob = subprocess.CompletedProcess([], 124, "", "")
    # startswith("FAIL") would also match the trailing "FAILED" verdict
    # line, so the count would be one too high. "FAIL(" is the CHECK macro's
    # own prefix and nothing else emits it.
    ob_fails = [l.strip() for l in ob.stdout.splitlines()
                if l.strip().startswith("FAIL(")]
    if ob.returncode == 0 or not ob_fails:
        tally["BASELINE-RED"] += 1
        print("FAIL%s: `test_interval_index oneblock` did NOT go red — the "
              "non-vacuity ledger cannot see a one-block feed" % label)
        return 1, tally
    if verbose:
        print("ledger falsification: `oneblock` exits %d with %d red ledger "
              "rows (first: %s)" % (ob.returncode, len(ob_fails),
                                    ob_fails[0][:90]))
        if keep_outputs:
            with open(os.path.join(OUT, "oneblock.txt"), "w") as fh:
                fh.write(ob.stdout)
        print()

    failures = 0
    for i, (name, fname, needle, repl) in enumerate(mutations, 1):
        d = os.path.join(WORK, "m%02d" % i)
        os.makedirs(d)
        for n, t in pristine.items():
            with open(os.path.join(d, n), "w") as fh:
                fh.write(t)

        target = os.path.join(d, fname)
        with open(target) as fh:
            text = fh.read()
        hits = text.count(needle)
        if hits != 1:
            tally["NOT-APPLIED"] += 1
            failures += 1
            print("FAIL  %s: NOT-APPLIED — needle matched %d times, must "
                  "match exactly 1. A mutation that did not apply is the "
                  "pristine module; its pass proves nothing." % (name, hits))
            continue
        with open(target, "w") as fh:
            fh.write(text.replace(needle, repl))

        binary = os.path.join(d, "t")
        b = build(d, binary, extra, cc=cc, test_src=test_src)
        if b.returncode in (126, 127):
            tally["TOOL-MISSING"] += 1
            failures += 1
            print("FAIL  %s: TOOL-MISSING — compiler exit %d"
                  % (name, b.returncode))
            continue
        if b.returncode != 0:
            tally["BUILD-FAILED"] += 1
            failures += 1
            print("FAIL  %s: BUILD-FAILED — a mutation that cannot compile is "
                  "NOT a detection\n%s" % (name, b.stderr[-700:]))
            continue
        try:
            r = subprocess.run([binary], capture_output=True, text=True,
                               timeout=RUN_TIMEOUT)
        except subprocess.TimeoutExpired:
            tally["BUILD-FAILED"] += 1
            failures += 1
            print("FAIL  %s: the mutant TIMED OUT — a hang is not a "
                  "detection" % name)
            continue
        if keep_outputs:
            with open(os.path.join(OUT, "m%02d.txt" % i), "w") as fh:
                fh.write("%s\n\n%s" % (name, r.stdout))
        # "FAIL(" is the CHECK macro's prefix; plain "FAIL" would also match
        # the trailing "FAILED" verdict line. Only used for the one-line
        # excerpt below — the verdict itself comes from `N checks, M failed`.
        fails = [l.strip() for l in r.stdout.splitlines()
                 if l.strip().startswith("FAIL(")]
        tail = re.findall(r"^(\d+) checks, (\d+) failed", r.stdout, re.M)
        nfail = int(tail[0][1]) if tail else -1
        if r.returncode == 0 or nfail == 0:
            tally["STAYED-GREEN"] += 1
            failures += 1
            print("FAIL  %s: STAYED-GREEN — BLIND SPOT" % name)
            continue
        tally["RED"] += 1
        first = fails[0][:140] if fails else "(crash/abort)"
        if verbose:
            print("RED   %s\n        -> %d failing checks; first: %s"
                  % (name, nfail, first))

    shutil.rmtree(WORK, ignore_errors=True)
    return failures, tally


def self_test():
    """Drive this script's OWN false-negative paths and require each to fail.

    Every case below is a way a mutation harness can report success while
    detecting nothing. A green self-test means none of them can slip through.
    """
    print("=== self-test: the harness's own false-negative paths ===")
    cases = []
    tmp = tempfile.mkdtemp(prefix="pgwt-ii-selftest-")

    # Every case names the tally bucket that MUST come back as 1. Asserting
    # only `failures > 0` is not enough: a case can be satisfied by the WRONG
    # refusal and still look green. That is not hypothetical -- case 7, the
    # ONLY one that exercises STAYED-GREEN (the single blind spot a mutation
    # harness exists to find), silently degraded to NOT-APPLIED during
    # development when its comment needle stopped matching, and still
    # reported PASS.

    # 1. compiler missing (exit 127 equivalent).
    cases.append(("compiler absent (127)", "TOOL-MISSING",
                  dict(cc=os.path.join(tmp, "no-such-cc"),
                       mutations=MUTATIONS[:1])))

    # 2. compiler present but not executable (exit 126 equivalent).
    noexec = os.path.join(tmp, "noexec-cc")
    with open(noexec, "w") as fh:
        fh.write("#!/bin/sh\nexit 0\n")
    os.chmod(noexec, stat.S_IRUSR)
    cases.append(("compiler not executable (126)", "TOOL-MISSING",
                  dict(cc=noexec, mutations=MUTATIONS[:1])))

    # 3. a compiler that exits 127 itself on every invocation.
    stub127 = os.path.join(tmp, "cc127")
    with open(stub127, "w") as fh:
        fh.write("#!/bin/sh\nexit 127\n")
    os.chmod(stub127, 0o755)
    cases.append(("compiler exits 127 on every call", "TOOL-MISSING",
                  dict(cc=stub127, mutations=MUTATIONS[:1])))

    # 4. the test source is missing: the baseline cannot build.
    cases.append(("test source missing", "BASELINE-RED",
                  dict(test_src=os.path.join(tmp, "nope.c"),
                       mutations=MUTATIONS[:1])))

    # 5. NOT-APPLIED: a needle that matches zero times.
    cases.append(("needle matches 0 times", "NOT-APPLIED",
                  dict(mutations=[("ZZ no such text", "interval_index.c",
                                   "/* this text does not exist */", "x")])))

    # 6. NOT-APPLIED: a needle that matches more than once.
    cases.append(("needle matches many times", "NOT-APPLIED",
                  dict(mutations=[("ZZ ambiguous needle", "interval_index.c",
                                   "return 0;", "return 0;")])))

    # 7. STAYED-GREEN: a mutation the test cannot see (a comment edit). This
    # case must land on text that really is in interval_index.c — otherwise
    # it is reported as NOT-APPLIED and the STAYED-GREEN branch, the one
    # blind spot a mutation harness exists to find, is never exercised.
    cases.append(("mutation the test cannot see (STAYED-GREEN)",
                  "STAYED-GREEN",
                  dict(mutations=[("ZZ comment-only no-op",
                                   "interval_index.c",
                                   "a half-built index must never answer",
                                   "a HALF-BUILT index must never answer")])))

    # 8. BUILD-FAILED: a mutation that does not compile.
    cases.append(("mutation that does not compile", "BUILD-FAILED",
                  dict(mutations=[("ZZ syntax error", "interval_index.c",
                                   "static int is_unfiltered",
                                   "static int is_unfiltered(((")])))

    bad = 0
    for name, want_bucket, kw in cases:
        kw.setdefault("mutations", MUTATIONS[:1])
        print("\n--- self-test case: %s (expect %s) ---" % (name, want_bucket))
        try:
            failures, tally = run_suite(verbose=False, keep_outputs=False,
                                        label=" [self-test]", **kw)
        except Exception as e:       # noqa: BLE001 - any crash is a failure
            print("  harness raised %r — counted as a refusal" % e)
            failures, tally = 1, {}
        got = tally.get(want_bucket, 0)
        ok = failures > 0 and got == 1
        why = ""
        if failures <= 0:
            why = " — it APPROVED"
        elif got != 1:
            why = (" — it refused for the WRONG reason: %s is %d, not 1, so "
                   "this path was never exercised" % (want_bucket, got))
        print("  -> harness reports %d failure(s), %s=%d %s  %s%s"
              % (failures, want_bucket, got, tally,
                 "PASS" if ok else "FAIL", why))
        if not ok:
            bad += 1

    shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d self-test cases, %d that the harness either APPROVED or "
          "refused for the wrong reason" % (len(cases), bad))
    print("SELF-TEST PASSED" if bad == 0 else "SELF-TEST FAILED")
    return 1 if bad else 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    print("=== interval_index mutations: %d injected silent-wrong risks ==="
          % len(MUTATIONS))
    failures, tally = run_suite(MUTATIONS)
    print()
    print("mutants injected: %d" % len(MUTATIONS))
    print("detected (RED):   %d" % tally["RED"])
    for k in ("STAYED-GREEN", "BUILD-FAILED", "NOT-APPLIED", "TOOL-MISSING",
              "BASELINE-RED"):
        print("%-17s %d%s" % (k + ":", tally[k],
                              "   <-- FAILURE" if tally[k] else ""))
    print("PASSED" if failures == 0 else "FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
