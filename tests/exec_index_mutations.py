#!/usr/bin/env python3
"""Demonstrated red for tests/test_exec_index.c (paint-latency Phase 3).

src/exec_index.c is a NEW module, so "run the new test against the parent
tree" is not available: the parent tree has no module to test. The equivalent
evidence is this — every silent-wrong risk the index was written to avoid is
INJECTED into a copy of the module, one at a time, and the unit test must go
RED on it. A mutation that leaves the test GREEN is a blind spot in the test,
and this script reports it as a FAILURE.

Each entry below is (name, file, needle, replacement, expected-red-marker).
`needle` must appear EXACTLY ONCE in the pristine source; a needle that
matches zero or several times is itself reported as a failure, because a
mutation that did not apply would otherwise look like a test that cannot see
(this script's own false-negative path).

Run from tests/:  python3 exec_index_mutations.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
# Mutant build trees are throwaway BINARIES: they go to a temp dir, never
# into tests/results (which box-check rsyncs and a reviewer reads).
WORK = os.path.join(tempfile.gettempdir(), "pgwt-exec-index-mutants")
# Only the text output of each mutant run is kept as evidence.
OUT = os.path.join(HERE, "results", "exec-index", "mutations")

MUTATIONS = [
    ("M01 no carry-over across blocks (per-pid stacks reset each block)",
     "exec_index.c",
     "    uint64_t last_ts = idx->last_event_ns;",
     "    for (int z = 0; z < idx->n_pids; z++)\n"
     "        ((struct exec_pid_carry *)idx->pids)[z].n_open = 0;\n"
     "    uint64_t last_ts = idx->last_event_ns;"),

    ("M02 #222: one active row per pid instead of a stack",
     "exec_index.c",
     "    st->open_rows[st->n_open++] = row_idx;",
     "    st->n_open = 0;\n    st->open_rows[st->n_open++] = row_idx;"),

    ("M03 an execution still open is given the window end as its end",
     "exec_index.c",
     "            int in_progress = (!e->closed || e->close_ns > to_ns);\n"
     "            uint64_t end_ns = in_progress ? 0 : e->close_ns;",
     "            int in_progress = 0;\n"
     "            uint64_t end_ns = e->closed ? e->close_ns : to_ns;"),

    ("M04 retention predicate narrowed to half-open at to_ns",
     "exec_index.c",
     "    if (start_ns > to_ns)\n        return 0;",
     "    if (start_ns >= to_ns)\n        return 0;"),

    ("M05 chunk prefilter narrowed to half-open at to_ns",
     "exec_index.c",
     "        if (ch->n_rows == 0 || ch->min_start_ns > to_ns ||",
     "        if (ch->n_rows == 0 || ch->min_start_ns >= to_ns ||"),

    ("M06 an empty index answers 'no executions' instead of refusing",
     "exec_index.c",
     "    if (idx->n_chunks == 0)\n"
     "        return query_refuse(out, PGWT_EXEC_INDEX_REFUSE_EMPTY);",
     "    if (idx->n_chunks == 0) {\n"
     "        out->refused = PGWT_EXEC_INDEX_OK;\n"
     "        return 0;\n"
     "    }"),

    ("M07 coverage check is an intersection, not containment",
     "exec_index.c",
     "    if (from_ns < idx->cover_from_ns || to_ns > idx->cover_to_ns)",
     "    if (to_ns < idx->cover_from_ns || from_ns > idx->cover_to_ns)"),

    ("M08 unindexed counts come back as 0 instead of poisoned",
     "exec_index.c",
     "            r->n_events = PGWT_EXEC_INDEX_NOT_INDEXED;\n"
     "            r->n_workers = PGWT_EXEC_INDEX_NOT_INDEXED;\n"
     "            r->matches_event_filter = PGWT_EXEC_INDEX_NOT_INDEXED;",
     "            r->n_events = 0;\n"
     "            r->n_workers = 0;\n"
     "            r->matches_event_filter = 0;"),

    ("M09 a skipped block in the feed is accepted",
     "exec_index.c",
     "    if (block_idx != idx->next_block_idx)\n"
     "        return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_GAP);",
     "    if (block_idx < 0)\n"
     "        return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_GAP);"),

    ("M10 the closing EXEC_END's query_id is never applied",
     "exec_index.c",
     "            if (r->query_id == 0 && !in_progress && !e->close_inferred)\n"
     "                r->query_id = e->close_query_id;",
     "            /* mutant: query_id backfill removed */"),

    ("M11 CMD_END closes only the innermost row, not every open one",
     "exec_index.c",
     "            while ((row_idx = carry_pop(st)) >= 0) {",
     "            if ((row_idx = carry_pop(st)) >= 0) {"),

    ("M12 a ready plan is attached regardless of query_id",
     "exec_index.c",
     "            if (st->plan_ready &&\n"
     "                (st->ready_plan_query_id == 0 || e.query_id == 0 ||\n"
     "                 st->ready_plan_query_id == e.query_id)) {",
     "            if (st->plan_ready) {"),

    ("M13 a build allocation failure does not make the index unusable",
     "exec_index.c",
     "    idx->failed = 1;\n"
     "    if (idx->build_refusal == PGWT_EXEC_INDEX_OK)",
     "    idx->failed = 0;\n"
     "    if (idx->build_refusal == PGWT_EXEC_INDEX_OK)"),

    ("M14 out-of-order records are accepted silently",
     "exec_index.c",
     "        if (have_last && ev->timestamp_ns < last_ts)\n"
     "            return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_GAP);",
     "        if (have_last && ev->timestamp_ns + 1 == 0)\n"
     "            return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_GAP);"),

    ("M15 the version stamp is not checked on query",
     "exec_index.c",
     "    if (idx->version != PGWT_EXEC_INDEX_VERSION)\n"
     "        return query_refuse(out, PGWT_EXEC_INDEX_REFUSE_VERSION);",
     "    if (idx->version == 0)\n"
     "        return query_refuse(out, PGWT_EXEC_INDEX_REFUSE_VERSION);"),

    ("M16 chunk max_end ignores rows still open at capture end",
     "exec_index.c",
     "            uint64_t end = e->closed ? e->close_ns : UINT64_MAX;",
     "            uint64_t end = e->close_ns;"),
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


def build(workdir, binary, extra_sources):
    cmd = ["cc", "-g", "-O1", "-w",
           "-I", workdir, "-I", SRC, "-DPGWT_SERVER",
           "-o", binary,
           os.path.join(HERE, "test_exec_index.c"),
           os.path.join(workdir, "exec_index.c"),
           os.path.join(SRC, "compute.c"),
           os.path.join(SRC, "wait_event.c"),
           os.path.join(SRC, "idle_rule.c"),
           os.path.join(SRC, "cJSON.c")] + extra_sources + ["-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r


def main():
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    os.makedirs(OUT, exist_ok=True)

    # src/spawn.c uses pipe2(), which Darwin lacks; wait_event.c needs its
    # two symbols. Linux links the real file, macOS a stub. Neither path
    # SKIPS: a build failure below is reported as a failure, because a
    # mutation harness that could not build proves nothing.
    if sys.platform == "darwin":
        stub = os.path.join(WORK, "mac_stub.c")
        mac_stub(stub)
        extra = [stub]
    else:
        extra = [os.path.join(SRC, "spawn.c")]

    pristine = {}
    for name in ("exec_index.c", "exec_index.h"):
        with open(os.path.join(SRC, name)) as fh:
            pristine[name] = fh.read()

    failures = 0

    # 0. The baseline MUST be green, or every "red" below proves nothing.
    base = os.path.join(WORK, "base")
    os.makedirs(base)
    for n, t in pristine.items():
        with open(os.path.join(base, n), "w") as fh:
            fh.write(t)
    b = build(base, os.path.join(base, "t"), extra)
    if b.returncode != 0:
        print("FAIL: pristine build failed\n%s" % b.stderr[-2000:])
        return 1
    r = subprocess.run([os.path.join(base, "t")], capture_output=True,
                       text=True)
    if r.returncode != 0:
        print("FAIL: pristine test is NOT green — nothing below is evidence")
        print(r.stdout[-3000:])
        return 1
    print("baseline: pristine module, test PASSED (%s)"
          % r.stdout.strip().splitlines()[-2])
    print()

    for i, (name, fname, needle, repl) in enumerate(MUTATIONS, 1):
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
            print("FAIL  %s: needle matched %d times, must match exactly 1 "
                  "(the mutation did not apply — this script cannot see)"
                  % (name, hits))
            failures += 1
            continue
        with open(target, "w") as fh:
            fh.write(text.replace(needle, repl))

        binary = os.path.join(d, "t")
        b = build(d, binary, extra)
        if b.returncode != 0:
            print("FAIL  %s: mutant did not COMPILE — a mutation that cannot "
                  "build is not evidence\n%s" % (name, b.stderr[-800:]))
            failures += 1
            continue
        r = subprocess.run([binary], capture_output=True, text=True,
                           timeout=600)
        with open(os.path.join(OUT, "m%02d.txt" % i), "w") as fh:
            fh.write("%s\n\n%s" % (name, r.stdout))
        fails = [l.strip() for l in r.stdout.splitlines()
                 if l.strip().startswith("FAIL")]
        tail = re.findall(r"^(\d+) checks, (\d+) failed", r.stdout, re.M)
        nfail = int(tail[0][1]) if tail else -1
        if r.returncode == 0:
            print("FAIL  %s: test stayed GREEN — BLIND SPOT" % name)
            failures += 1
        else:
            first = fails[0][:150] if fails else "(crash/abort)"
            print("RED   %s\n        -> %d failing checks; first: %s"
                  % (name, nfail, first))

    print()
    shutil.rmtree(WORK, ignore_errors=True)
    print("%d mutations, %d not detected" % (len(MUTATIONS), failures))
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
