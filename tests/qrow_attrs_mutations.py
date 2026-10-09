#!/usr/bin/env python3
"""Demonstrated red for tests/test_qrow_attrs.c (#324).

pgwt_compute_qrow_attrs is NEW code, so "run the new test against the parent
tree" shows only that the symbol does not exist there. The equivalent
evidence is this: every silent-wrong risk the one-pass rewrite could have
introduced is INJECTED into a copy of src/compute.c, one at a time, and the
unit test must go RED on it. A mutation that leaves the test GREEN is a blind
spot in the test, and this script reports it as a FAILURE.

M13..M18 mutate the TEST's own ORACLE and FIXTURE instead of the product
code — a differential whose oracle can be broken without anyone noticing is
not a differential, and a fixture on which a verdict is unreachable makes
every assertion about that verdict vacuous. That false negative is the one
this repo keeps finding, and M16/M17/M18 are the three that found the first
version of this very fixture blind (every row came back -1 with synth set,
so "report -1 and synth for everything" passed §1).
(§5 of the test carries in-binary equivalents; these are the source-level
form, so the two are independent.)

Each entry is (kind, name, file, needle, replacement) where kind is:

  "RED"    the test MUST fail on this mutant. Green = a blind spot.
  "EQUIV"  the mutant is claimed to be SEMANTICALLY EQUIVALENT, so green is
           correct — but "equivalent mutant" is the standard excuse for a
           real blind spot, so the claim is PROVEN rather than asserted: the
           mutant's stdout must be BYTE-IDENTICAL to the pristine run's.
           A mutant that merely passes while printing something different is
           reported as a failure, same as a missed RED. Two of these exist
           and both were discovered by this script marking them green:
           see M01 and M08.

`needle` must appear EXACTLY ONCE in the pristine source; zero or several
matches is itself a failure, because a mutation that did not apply would
otherwise look like a test that cannot see — this script's own false-negative
path. A mutant that does not COMPILE is likewise a failure, never a
detection.

Run from tests/:  python3 qrow_attrs_mutations.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
WORK = os.path.join(tempfile.gettempdir(), "pgwt-qrow-attrs-mutants")
OUT = os.path.join(HERE, "results", "qrow-attrs", "mutations")

MUTATIONS = [
    # ── Mutations of the PRODUCT code (src/compute.c) ─────────────────────
    ("EQUIV",
     "M01 the -1 short-circuit removed (a performance guard, not a "
     "correctness one)",
     "compute.c",
     "        if (a->context < 0)\n            continue;                  "
     "/* -1 is absorbing */",
     "        /* MUTANT: -1 short-circuit removed */"),

    ("RED", "M02 the synth flag is never ORed in",
     "compute.c",
     "        if (events[i].flags & PGWT_EVENT_FLAG_QUERY_SYNTH)\n"
     "            a->has_synth = 1;",
     "        /* MUTANT: synth dropped */"),

    ("RED", "M03 the duplicate-query_id fixup is removed",
     "compute.c",
     "        int first = qa_ix_find(&qix, out->rows[i].query_id);\n"
     "        if (first >= 0 && first != i)\n"
     "            out->rows[i] = out->rows[first];",
     "        /* MUTANT: duplicates left unanswered */"),

    ("RED", "M04 an unresolvable pid is SKIPPED instead of poisoning the row",
     "compute.c",
     "        if (!qa_pid_ctx(&memo, events[i].pid, &db, &uid)) {\n"
     "            a->context = -1;\n"
     "            a->databaseid = 0;\n"
     "            a->userid = 0;\n"
     "            continue;\n"
     "        }",
     "        if (!qa_pid_ctx(&memo, events[i].pid, &db, &uid))\n"
     "            continue;   /* MUTANT: unresolvable pid ignored */"),

    ("RED", "M05 two disagreeing contexts no longer poison the row",
     "compute.c",
     "        } else if (a->databaseid != db || a->userid != uid) {\n"
     "            a->context = -1;\n"
     "            a->databaseid = 0;\n"
     "            a->userid = 0;\n"
     "        }",
     "        }   /* MUTANT: disagreement accepted */"),

    # M06/M06b are what make M01 safe: the -1 short-circuit is removable
    # ONLY because poisoning also ZEROES the ids, so a poisoned row can
    # never match a later record's context. Break the zeroing and the
    # absorbing property depends on the short-circuit again.
    ("RED", "M06 poisoning by DISAGREEMENT leaves the stale ids in place",
     "compute.c",
     "        } else if (a->databaseid != db || a->userid != uid) {\n"
     "            a->context = -1;\n"
     "            a->databaseid = 0;\n"
     "            a->userid = 0;\n"
     "        }",
     "        } else if (a->databaseid != db || a->userid != uid) {\n"
     "            a->context = -1;   /* MUTANT: ids not cleared */\n"
     "        }"),

    ("RED", "M06b poisoning by an UNUSABLE PID leaves the stale ids in place",
     "compute.c",
     "        if (!qa_pid_ctx(&memo, events[i].pid, &db, &uid)) {\n"
     "            a->context = -1;\n"
     "            a->databaseid = 0;\n"
     "            a->userid = 0;\n"
     "            continue;\n"
     "        }",
     "        if (!qa_pid_ctx(&memo, events[i].pid, &db, &uid)) {\n"
     "            a->context = -1;   /* MUTANT: ids not cleared */\n"
     "            continue;\n"
     "        }"),

    ("RED",
     "M07 the qid index treats key 0 as an empty slot (query_id 0 dropped)",
     "compute.c",
     "        if (ix->vals[i] < 0) return -1;\n"
     "        if (ix->keys[i] == key) return ix->vals[i];",
     "        if (ix->vals[i] < 0 || ix->keys[i] == 0) return -1;\n"
     "        if (ix->keys[i] == key) return ix->vals[i];"),

    ("EQUIV",
     "M08 duplicate insert keeps the LAST index instead of the first",
     "compute.c",
     "        if (ix->keys[i] == key)\n"
     "            return;                 /* duplicate: one canonical index */",
     "        if (ix->keys[i] == key) {\n"
     "            ix->vals[i] = val;   /* MUTANT: last index wins */\n"
     "            return;\n"
     "        }"),

    ("RED", "M09 a fatal allocation failure returns 0 (success) with no rows",
     "compute.c",
     "    out->num_rows = 0;\n"
     "    out->failed = 1;\n"
     "    return -1;\n"
     "}\n"
     "\n"
     "void pgwt_qrow_attrs_free",
     "    out->num_rows = 0;\n"
     "    out->failed = 1;\n"
     "    return 0;   /* MUTANT: absence reported as success */\n"
     "}\n"
     "\n"
     "void pgwt_qrow_attrs_free"),

    ("RED", "M10 the pid memo never grows (a full table makes inserts no-ops)",
     "compute.c",
     "        if ((m->n + 1) * 4 > m->ix.cap) {",
     "        if (0) {   /* MUTANT: never grow */"),

    ("RED", "M11 the row's own query_id is not carried into the result",
     "compute.c",
     "        out->rows[i].query_id = query_ids[i];",
     "        out->rows[i].query_id = query_ids[i] + 1;   /* MUTANT */"),

    ("RED", "M12 a memo allocation failure is reported as a FATAL refusal",
     "compute.c",
     "    if (qa_ix_init(&memo.ix, 128, \"qrow_pid_index\") != 0)\n"
     "        memo.failed = 1;",
     "    if (qa_ix_init(&memo.ix, 128, \"qrow_pid_index\") != 0) {\n"
     "        qa_ix_free(&qix);\n"
     "        goto oom;   /* MUTANT: degradable failure made fatal */\n"
     "    }"),

    # ── Mutations of the TEST's ORACLE and FIXTURE ────────────────────────
    ("RED", "M13 ORACLE: the deleted context loop stops at the first match",
     "test_qrow_attrs.c",
     "        if (!found) {\n"
     "            *databaseid = db;\n"
     "            *userid = uid;\n"
     "            found = 1;\n"
     "        } else if (*databaseid != db || *userid != uid) {",
     "        if (!found) {\n"
     "            *databaseid = db;\n"
     "            *userid = uid;\n"
     "            found = 1;\n"
     "            return 1;   /* MUTANT ORACLE: first match wins */\n"
     "        } else if (*databaseid != db || *userid != uid) {"),

    ("RED", "M14 ORACLE: the deleted synth loop always reports 'no synth'",
     "test_qrow_attrs.c",
     "    int synthetic_quality = 0;\n"
     "    for (int e = 0; !synthetic_quality && events && e < count; e++)",
     "    int synthetic_quality = 0;\n"
     "    return 0;   /* MUTANT ORACLE: never synth */\n"
     "    for (int e = 0; !synthetic_quality && events && e < count; e++)"),

    ("RED", "M15 ORACLE: an unusable pid is skipped instead of returning -1",
     "test_qrow_attrs.c",
     "        if (!resolve || !resolve(rctx, events[i].pid, &db, &uid)) {\n"
     "            *databaseid = 0;\n"
     "            *userid = 0;\n"
     "            return -1;\n"
     "        }",
     "        if (!resolve || !resolve(rctx, events[i].pid, &db, &uid))\n"
     "            continue;   /* MUTANT ORACLE: skip, do not poison */"),

    ("RED",
     "M16 FIXTURE: class D stops using the second pid pool, so the "
     "DISAGREEMENT verdict is no longer reachable",
     "test_qrow_attrs.c",
     "        return ordinal % 3 == 2 ? CLEAN_B_0 + (uint32_t)(ordinal % 20)\n"
     "                                : CLEAN_A_0 + (uint32_t)(ordinal % 20);",
     "        return CLEAN_A_0 + (uint32_t)(ordinal % 20);   /* MUTANT */"),

    ("RED",
     "M17 FIXTURE: class B stops using an unusable pid, so the REJECTION "
     "verdict is no longer reachable",
     "test_qrow_attrs.c",
     "        return ordinal % 5 == 3 ? BAD_0 + (uint32_t)((qid / 4) % 4)\n"
     "                                : CLEAN_A_0 + (uint32_t)(ordinal % 20);",
     "        return CLEAN_A_0 + (uint32_t)(ordinal % 20);   /* MUTANT */"),

    ("RED",
     "M18 FIXTURE: the synth flag is set on every record, so 'always synth' "
     "would satisfy the differential",
     "test_qrow_attrs.c",
     "        if ((qid % 3) == 0 && (i % 7) == 1)\n"
     "            ev[i].flags |= PGWT_EVENT_FLAG_QUERY_SYNTH;",
     "        ev[i].flags |= PGWT_EVENT_FLAG_QUERY_SYNTH;   /* MUTANT */"),

    ("RED",
     "M19 FIXTURE: the synth flag is set on NO record, so 'never synth' "
     "would satisfy the differential",
     "test_qrow_attrs.c",
     "        if ((qid % 3) == 0 && (i % 7) == 1)\n"
     "            ev[i].flags |= PGWT_EVENT_FLAG_QUERY_SYNTH;",
     "        if (0)   /* MUTANT */\n"
     "            ev[i].flags |= PGWT_EVENT_FLAG_QUERY_SYNTH;"),
]


def stub(path):
    """src/spawn.c uses pipe2(), which Darwin lacks, and wait_event.c needs
    its two symbols. Stubbing them on BOTH platforms keeps one code path —
    nothing in this test reaches either. A build failure below is reported as
    a failure, never skipped: a harness that could not build proves nothing.
    """
    with open(path, "w") as fh:
        fh.write("#include <stdio.h>\n"
                 "FILE *pgwt_proc_open(char *const argv[], int *pid);\n"
                 "int pgwt_proc_close(FILE *fp, int pid);\n"
                 "FILE *pgwt_proc_open(char *const a[], int *p)"
                 " { (void)a; (void)p; return NULL; }\n"
                 "int pgwt_proc_close(FILE *f, int p)"
                 " { (void)f; (void)p; return -1; }\n")


def build(workdir, binary):
    cmd = ["cc", "-g", "-O1", "-w",
           "-I", SRC, "-DPGWT_SERVER", "-o", binary,
           os.path.join(workdir, "test_qrow_attrs.c"),
           os.path.join(workdir, "compute.c"),
           os.path.join(SRC, "wait_event.c"),
           os.path.join(SRC, "idle_rule.c"),
           os.path.join(SRC, "cJSON.c"),
           os.path.join(workdir, "stub.c"), "-lm"]
    return subprocess.run(cmd, capture_output=True, text=True)


def populate(d, pristine):
    os.makedirs(d, exist_ok=True)
    for name, text in pristine.items():
        with open(os.path.join(d, name), "w") as fh:
            fh.write(text)
    stub(os.path.join(d, "stub.c"))


def main():
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    os.makedirs(OUT, exist_ok=True)

    pristine = {}
    for path, name in ((os.path.join(SRC, "compute.c"), "compute.c"),
                       (os.path.join(HERE, "test_qrow_attrs.c"),
                        "test_qrow_attrs.c")):
        with open(path) as fh:
            pristine[name] = fh.read()

    failures = 0

    # 0. The baseline MUST be green, or every "red" below proves nothing.
    base = os.path.join(WORK, "base")
    populate(base, pristine)
    b = build(base, os.path.join(base, "t"))
    if b.returncode != 0:
        print("FAIL: pristine build failed\n%s" % b.stderr[-3000:])
        return 1
    r = subprocess.run([os.path.join(base, "t")], capture_output=True,
                       text=True)
    if r.returncode != 0:
        print("FAIL: pristine test is NOT green — nothing below is evidence")
        print(r.stdout[-3000:])
        return 1
    print("baseline: pristine sources, test PASSED — %s"
          % r.stdout.strip().splitlines()[-1])
    print()

    baseline_stdout = r.stdout

    for i, (kind, name, fname, needle, repl) in enumerate(MUTATIONS, 1):
        d = os.path.join(WORK, "m%02d" % i)
        populate(d, pristine)

        target = os.path.join(d, fname)
        with open(target) as fh:
            text = fh.read()
        hits = text.count(needle)
        if hits != 1:
            print("FAIL  %s\n      needle matched %d times, must match "
                  "exactly 1 (the mutation did not apply — this script "
                  "cannot see)" % (name, hits))
            failures += 1
            continue
        with open(target, "w") as fh:
            fh.write(text.replace(needle, repl))

        binary = os.path.join(d, "t")
        bb = build(d, binary)
        if bb.returncode != 0:
            # A mutant that does not compile is NOT evidence of detection.
            print("FAIL  %s\n      mutant did not build (not a detection)\n%s"
                  % (name, bb.stderr[-1200:]))
            failures += 1
            continue
        rr = subprocess.run([binary], capture_output=True, text=True)
        with open(os.path.join(OUT, "m%02d.txt" % i), "w") as fh:
            fh.write("%s [%s]\n\n%s" % (name, kind, rr.stdout))

        if kind == "EQUIV":
            # Green is the expected outcome, but only if the mutant is
            # genuinely indistinguishable. Byte-identical stdout is the
            # proof; "it passed" is not, because that is exactly what a
            # blind test also looks like.
            if rr.returncode != 0:
                print("RED   %s\n      claimed EQUIVALENT but the test FAILED "
                      "— the claim is wrong, not the test" % name)
                failures += 1
            elif rr.stdout != baseline_stdout:
                print("FAIL  %s\n      claimed EQUIVALENT but the output "
                      "DIFFERS from pristine — not equivalent, and the test "
                      "cannot see the difference" % name)
                failures += 1
            else:
                print("EQUIV %s\n      output byte-identical to pristine "
                      "(%d bytes): provably indistinguishable"
                      % (name, len(rr.stdout)))
            continue

        if rr.returncode == 0:
            print("FAIL  %s\n      mutant is GREEN — the test cannot see this"
                  % name)
            failures += 1
        else:
            first = [ln.strip() for ln in rr.stdout.splitlines()
                     if ln.strip().startswith("FAIL")]
            print("RED   %s\n      %s" % (name, first[0] if first else "?"))

    print()
    n_red = sum(1 for m in MUTATIONS if m[0] == "RED")
    n_eq = sum(1 for m in MUTATIONS if m[0] == "EQUIV")
    print("%d mutations (%d must-be-RED, %d proven-equivalent), "
          "%d not accounted for" % (len(MUTATIONS), n_red, n_eq, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
