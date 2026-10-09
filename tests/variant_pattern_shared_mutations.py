#!/usr/bin/env python3
"""Demonstrated red for src/variant_pattern.h, the primitives BOTH sides share.

WHY THIS EXISTS, AND WHY THE OTHER HARNESS IS NOT ENOUGH.
src/compute.c (pgwt_compute_variants -- the oracle) and src/variant_index.c
(the step-sequence index) used to hold two verbatim copies of the flow-pattern
primitives: detect_loop, compress, hash, the CPU-only fill-in, loop_n, the
per-step walk and the by-total-time comparator. A 42,266-case differential
between the two copies caught a change in either one.

The fold put them in ONE header. That removes the drift risk and, with it, the
differential's ability to see a change in them at all: a mutation now moves the
index AND the oracle together, so they still agree -- on the wrong answer. A
gate that cannot fail is not a gate, so the claim "the literal expectations
cover the shared primitives" is not asserted here, it is MEASURED: each
mutation below is applied to the shared header, compute.c is compiled out of
the mutant directory so that it sees the mutation too, and
tests/test_variant_index.c must still go RED.

Every RED line below names which check failed; each is an ORACLE-INDEPENDENT
literal expectation ([A,B] and [B,A] are two variants, [A] and [A,A] are two
variants, a CPU-only execution is one step with event id 0, ABAB compresses to
two steps, the per-step averages reproduce the #343 si-clamp quirk). A
STAYED-GREEN line is a blind spot the fold introduced and a FAILURE.

The three outcomes are named separately for the same reason the sibling
harness names them: this script's own false-negative path is a mutation that
never reached a binary.

  BUILD-FAILED  the mutant did not compile        -> FAILURE (proves nothing)
  NOT-APPLIED   the needle matched 0 or >1 times  -> FAILURE (did not apply)
  STAYED-GREEN  built, ran, test passed           -> FAILURE (blind spot)

and a pristine baseline that is not green aborts before any "red" is claimed.

Run from tests/:  python3 variant_pattern_shared_mutations.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
WORK = os.path.join(tempfile.gettempdir(), "pgwt-variant-pattern-mutants")
OUT = os.path.join(HERE, "results", "variant-index", "shared-mutations")

# Files copied into every mutant tree. compute.c and variant_index.c are BOTH
# compiled from there, so a quoted include of variant_pattern.h resolves to the
# mutated copy for both of them -- which is the whole point.
FILES = ["compute.c", "variant_index.c", "variant_index.h", "variant_pattern.h"]

MUTATIONS = [
    ("S01 step ORDER folded out of the hash, for both sides at once",
     "    for (int i = 0; i < p->num_steps; i++) {\n"
     "        h ^= p->steps[i];\n"
     "        h *= 0x100000001b3ULL;",
     "    uint64_t acc = 0;\n"
     "    for (int i = 0; i < p->num_steps; i++) acc += p->steps[i];\n"
     "    for (int i = 0; i < (p->num_steps ? 1 : 0); i++) {\n"
     "        h ^= acc;\n"
     "        h *= 0x100000001b3ULL;"),

    ("S02 the loop flag is not folded into the hash, for both sides at once",
     "        if (p->is_loop[i]) {\n"
     "            h ^= 0xDEADBEEF;\n"
     "            h *= 0x100000001b3ULL;\n"
     "        }",
     "        /* mutant: loop flag not hashed */"),

    ("S03 the CPU-only fill-in removed, for both sides at once",
     "    if (re->len == 0) {\n"
     "        re->events[0] = 0;  /* CPU */\n"
     "        re->durations[0] = re->total_ns;\n"
     "        re->len = 1;\n"
     "    }",
     "    /* mutant: no CPU-only fill-in */"),

    ("S04 the per-step si clamp removed (#343 'fixed' silently), for both "
     "sides at once",
     "            if (si >= cp->num_steps) si = cp->num_steps - 1;",
     "            /* mutant: no clamp */"),

    ("S05 a loop needs THREE repetitions, for both sides at once",
     "        if (reps >= 2) return body;  /* found a loop with >=2 repetitions */",
     "        if (reps >= 3) return body;"),

    ("S06 loop_n reported for a single-record sequence too, for both sides "
     "at once",
     "    double loop_n = 0;\n"
     "    if (raw_len > 1)\n"
     "        loop_n = num_steps > 0 ? (double)raw_len / num_steps : 1;",
     "    double loop_n = num_steps > 0 ? (double)raw_len / num_steps : 1;"),

    ("S07 the per-step walk does not advance past a non-loop step, for both "
     "sides at once",
     "            if (!cp->is_loop[si] || j + 1 >= re->len ||\n"
     "                re->events[j + 1] != cp->steps[si])\n"
     "                si++;",
     "            if (0)\n"
     "                si++;"),

    ("S08 the raw sequence cap drops to 3, for both sides at once",
     "#define PGWT_VARIANT_RAW_MAX 128",
     "#define PGWT_VARIANT_RAW_MAX 3"),
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
           os.path.join(HERE, "test_variant_index.c"),
           os.path.join(workdir, "variant_index.c"),
           os.path.join(workdir, "compute.c"),
           os.path.join(SRC, "wait_event.c"),
           os.path.join(SRC, "idle_rule.c"),
           os.path.join(SRC, "cJSON.c")] + extra_sources + ["-lm"]
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    os.makedirs(OUT, exist_ok=True)

    if sys.platform == "darwin":
        stub = os.path.join(WORK, "mac_stub.c")
        mac_stub(stub)
        extra = [stub]
    else:
        extra = [os.path.join(SRC, "spawn.c")]

    pristine = {}
    for name in FILES:
        with open(os.path.join(SRC, name)) as fh:
            pristine[name] = fh.read()

    failures = 0

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
                       text=True, timeout=3600)
    if r.returncode != 0:
        print("FAIL: pristine test is NOT green — nothing below is evidence")
        print(r.stdout[-3000:])
        return 1
    # Both files really are compiled from the mutant tree: if they were not,
    # the mutation would reach only one side and this harness would be the
    # sibling one over again.
    print("baseline: shared header pristine, test PASSED (%s)"
          % r.stdout.strip().splitlines()[-2])
    print()

    for i, (name, needle, repl) in enumerate(MUTATIONS, 1):
        tag = "s%02d" % i
        d = os.path.join(WORK, tag)
        os.makedirs(d)
        for n, t in pristine.items():
            with open(os.path.join(d, n), "w") as fh:
                fh.write(t)

        target = os.path.join(d, "variant_pattern.h")
        with open(target) as fh:
            text = fh.read()
        hits = text.count(needle)
        if hits != 1:
            print("NOT-APPLIED  %s: needle matched %d times in "
                  "variant_pattern.h, must match exactly 1 (this script "
                  "cannot see)" % (name, hits))
            failures += 1
            continue
        with open(target, "w") as fh:
            fh.write(text.replace(needle, repl))

        binary = os.path.join(d, "t")
        b = build(d, binary, extra)
        if b.returncode != 0:
            print("BUILD-FAILED  %s: a mutation that cannot build is not "
                  "evidence\n%s" % (name, b.stderr[-800:]))
            failures += 1
            continue
        try:
            r = subprocess.run([binary], capture_output=True, text=True,
                               timeout=3600)
        except subprocess.TimeoutExpired:
            print("BUILD-FAILED  %s: mutant HUNG (a gate that stalls is not a "
                  "gate that fails)" % name)
            failures += 1
            continue
        with open(os.path.join(OUT, "%s.txt" % tag), "w") as fh:
            fh.write("%s\n\n%s" % (name, r.stdout))
        fails = [l.strip() for l in r.stdout.splitlines()
                 if l.strip().startswith("FAIL")]
        tailm = re.findall(r"^(\d+) checks, (\d+) failed", r.stdout, re.M)
        nfail = int(tailm[0][1]) if tailm else -1
        if r.returncode == 0:
            print("STAYED-GREEN  %s: BLIND SPOT — the fold made this "
                  "invisible" % name)
            failures += 1
        else:
            first = fails[0][:160] if fails else "(crash/abort)"
            print("RED   %s\n        -> %d failing checks; first: %s"
                  % (name, nfail, first))

    print()
    shutil.rmtree(WORK, ignore_errors=True)
    print("%d shared-header mutations, %d not detected"
          % (len(MUTATIONS), failures))
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
