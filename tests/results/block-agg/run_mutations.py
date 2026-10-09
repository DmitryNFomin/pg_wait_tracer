#!/usr/bin/env python3
"""Mutation / bypass driver for tests/test_block_agg.c.

Why this exists: a gate nobody has seen fail is not evidence that it can.
Showing the true-positive path (the test goes red when the aggregate is wrong)
is only half of it — the half that has never been broken here. The other half
is the FALSE NEGATIVE: a check that was unreachable, skipped, or satisfied
without looking at anything. So each mutation below breaks ONE guard in
src/block_agg.c and the driver requires the test to go RED for it. A mutation
that leaves the suite GREEN is a hole in the suite, and the driver reports it
as such.

Each mutation names the source file it breaks, so the set covers every file
this phase touches that a C unit test can reach: src/block_agg.c and
src/compute.c. src/server.c is NOT mutated here -- its code is only reachable
through a running pgwt-server, so the loader's half-open bound and the
transitions handler are covered by the Linux synthetic-data and live tiers
instead (tests/run_all.sh), not by this driver. Saying so is the point: a
driver that silently covered less than it looked like it did would be the
same false-negative shape it exists to catch.

Usage (from the repo root):
    python3 tests/results/block-agg/run_mutations.py            # all
    python3 tests/results/block-agg/run_mutations.py M3         # one

Output: one log per mutation in this directory, plus mutations-summary.txt.
Runs anywhere a C compiler exists; on macOS it links a two-function stub for
src/spawn.c (pipe2 is Linux-only).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SRC = os.path.join(ROOT, "src")
TESTS = os.path.join(ROOT, "tests")

MAC_SPAWN_STUB = r'''
#include "spawn.h"
int pgwt_proc_open(struct pgwt_proc *p, char *const argv[])
{ (void)p; (void)argv; return -1; }
int pgwt_proc_close(struct pgwt_proc *p) { (void)p; return -1; }
'''

# (id, target source file, what guard is broken, which sections must catch
#  it, (old, new))
MUTATIONS = [
    ("M2", "block_agg.c",
     "plan(): SKIP is off by one at the START edge -- a block whose last "
     "record sits exactly at `from` is dropped, losing an in-window record.",
     "sec5/sec6",
     ("if (id->last_timestamp_ns < from_mono_ns)\n        return PGWT_BLOCK_SKIP;",
      "if (id->last_timestamp_ns <= from_mono_ns)\n        return PGWT_BLOCK_SKIP;")),

    ("M3", "block_agg.c",
     "merge(): the double-count guard is removed, so the same block can be "
     "folded in twice.",
     "sec8 B9",
     ("    for (int i = 0; i < a->n_keys; i++)\n"
      "        if (key_equal(&a->keys[i], k))\n"
      "            return PGWT_BAGG_REFUSED_DUPLICATE;",
      "    /* MUTANT: duplicate guard removed */")),

    ("M4", "block_agg.c",
     "plan(): a MISSING aggregate is merged anyway (`have_agg` ignored) -- "
     "absence read as an answer, violating C8.",
     "sec8 B2",
     ("    if (!have_agg)\n        return PGWT_BLOCK_DECODE;",
      "    /* MUTANT: have_agg ignored */")),

    ("M5", "block_agg.c",
     "build(): a SAMPLES block is aggregated as if it held transitions (C1 "
     "guard removed).",
     "sec8 B3",
     ("    if (block_type != PGWT_BLOCK_TRANSITIONS)\n"
      "        return PGWT_BAGG_REFUSED_BLOCK_TYPE;",
      "    /* MUTANT: block type guard removed */")),

    ("M6", "block_agg.c",
     "build(): an UNCOMMITTED block (the open block of current.trace) is "
     "aggregated (C2 guard removed).",
     "sec8 B4",
     ("    if (!committed)\n        return PGWT_BAGG_REFUSED_UNCOMMITTED;",
      "    /* MUTANT: committed guard removed */")),

    ("M7", "block_agg.c",
     "record_counts(): the raw path's impossible-record refusal "
     "(duration_ns > timestamp_ns) is dropped, so the aggregate counts a link "
     "pgwt_compute_transitions refuses. This is the divergence §1 actually "
     "found during development.",
     "sec1",
     ("    if (ev->duration_ns > ev->timestamp_ns)\n        return 0;",
      "    /* MUTANT: impossible-record refusal removed */")),

    ("M8", "block_agg.c",
     "trace_identity_equal(): an UNRESOLVABLE identity compares equal -- "
     "unknown treated as a match, which is how a cache approves a block it "
     "cannot identify.",
     "sec8 B5/B7",
     ("    if (!pgwt_trace_identity_resolvable(a) || !pgwt_trace_identity_resolvable(b))\n"
      "        return 0;                       /* unknown never equals anything */",
      "    if (!a || !b) return 0;  /* MUTANT: unresolvable treated as equal */")),

    ("M9", "block_agg.c",
     "matches(): num_events is no longer compared, so a REWRITTEN block at the "
     "same offset revalidates.",
     "sec8 B8",
     ("    return a->id.block_index        == now->block_index &&\n"
      "           a->id.num_events         == now->num_events &&",
      "    return a->id.block_index        == now->block_index &&\n"
      "           /* MUTANT: num_events no longer compared */")),

    ("M10", "block_agg.c",
     "build(): an allocation failure returns OK with a PARTIAL aggregate "
     "instead of refusing -- a short answer that looks like a real one.",
     "sec8 B10",
     ("            if (rc != PGWT_BAGG_OK) {\n"
      "                pgwt_block_agg_free(out);\n"
      "                return rc;              /* refuse whole, never partial */\n"
      "            }",
      "            if (rc != PGWT_BAGG_OK) {\n"
      "                /* MUTANT: keep the partial aggregate and claim OK */\n"
      "                break;\n"
      "            }")),

    ("M11", "block_agg.c",
     "node_counts(): SAMPLE records are counted as node time, so a sampled "
     "point observation fabricates a zero-duration node the raw node pass of "
     "the aggregate side does not have.",
     "sec1",
     ("int pgwt_block_agg_node_counts(const struct pgwt_trace_event *ev)\n"
      "{\n"
      "    if (!ev)\n"
      "        return 0;\n"
      "    if (ev->flags & PGWT_EVENT_FLAG_SAMPLE)\n"
      "        return 0;",
      "int pgwt_block_agg_node_counts(const struct pgwt_trace_event *ev)\n"
      "{\n"
      "    if (!ev)\n"
      "        return 0;\n"
      "    /* MUTANT: SAMPLE records counted as node time */")),

    ("M12", "block_agg.c",
     "plan(): the START containment test is dropped, so a BOUNDARY block is "
     "merged whole and records before `from` are counted.",
     "sec5/sec6/sec9",
     ("    if (id->first_timestamp_ns >= from_mono_ns &&\n"
      "        id->last_timestamp_ns <= to_mono_ns)\n"
      "        return PGWT_BLOCK_MERGE;",
      "    if (id->last_timestamp_ns <= to_mono_ns)\n"
      "        return PGWT_BLOCK_MERGE;   /* MUTANT: start bound dropped */")),

    ("M13", "block_agg.c",
     "pairs_sorted(): the total order is reduced to count-only, so the "
     "readout depends on hash-table layout and merge order.",
     "sec3",
     ("    if (a->from_event != b->from_event)\n"
      "        return a->from_event < b->from_event ? -1 : 1;\n"
      "    if (a->to_event != b->to_event)\n"
      "        return a->to_event < b->to_event ? -1 : 1;\n"
      "    return 0;\n"
      "}\n"
      "\n"
      "static int cmp_node_total",
      "    return 0;   /* MUTANT: ties left unordered */\n"
      "}\n"
      "\n"
      "static int cmp_node_total")),

    ("M15", "block_agg.c",
     "merge(): a pair table that cannot grow mid-merge returns OK instead of "
     "NOMEM, so a PARTIAL merge is handed back as a complete answer.",
     "sec8 B10",
     ("        rc = pair_add(dst, src->pairs[i].from_event, src->pairs[i].to_event,\n"
      "                      src->pairs[i].count, src->pairs[i].total_ns);\n"
      "        if (rc != PGWT_BAGG_OK)\n"
      "            return rc;",
      "        rc = pair_add(dst, src->pairs[i].from_event, src->pairs[i].to_event,\n"
      "                      src->pairs[i].count, src->pairs[i].total_ns);\n"
      "        if (rc != PGWT_BAGG_OK)\n"
      "            break;   /* MUTANT: partial merge reported as OK */")),

    ("M16", "block_agg.c",
     "window_from_reader(): a SAMPLES block that OVERLAPS the window is "
     "skipped instead of refused. The tables stay right and the caller's "
     "fidelity label goes wrong -- a wrong label on a right number.",
     "sec9",
     ("        if (bi.block_type != PGWT_BLOCK_TRANSITIONS) {\n"
      "            if (overlaps) {\n"
      "                rc = PGWT_BAGG_REFUSED_BLOCK_TYPE;\n"
      "                break;\n"
      "            }\n"
      "            continue;\n"
      "        }",
      "        if (bi.block_type != PGWT_BLOCK_TRANSITIONS)\n"
      "            continue;   /* MUTANT: overlapping SAMPLES block skipped */")),

    ("M17", "compute.c",
     "cmp_trans_desc(): back to count-only, so the raw path's tied rows are "
     "ordered by hash layout again and a row-by-row comparison against the "
     "aggregate's readout stops meaning anything.",
     "sec3c",
     ("    const struct trans_accum *x = a, *y = b;\n"
      "    if (x->count != y->count)\n"
      "        return x->count > y->count ? -1 : 1;\n"
      "    if (x->from_event != y->from_event)\n"
      "        return x->from_event < y->from_event ? -1 : 1;\n"
      "    if (x->to_event != y->to_event)\n"
      "        return x->to_event < y->to_event ? -1 : 1;\n"
      "    return 0;",
      "    uint64_t ca = ((const struct trans_accum *)a)->count;\n"
      "    uint64_t cb = ((const struct trans_accum *)b)->count;\n"
      "    return (cb > ca) - (cb < ca);   /* MUTANT: not a total order */")),

    ("M19", "block_agg.c",
     "in_window(): the END bound becomes half-open, so a record whose wait "
     "ended exactly at `to` is dropped even though its whole interval lies "
     "inside the window. This is the convention that zeroed test_data_aas "
     "(Total AAS 4.0 -> 0) and test_data_categories on the gate box.",
     "sec1/sec6b/sec6/sec9",
     ("    return ev->timestamp_ns >= from_mono_ns && ev->timestamp_ns <= to_mono_ns;",
      "    return ev->timestamp_ns >= from_mono_ns && ev->timestamp_ns < to_mono_ns;   /* MUTANT */")),

    ("M20", "block_agg.c",
     "plan(): MERGE requires last < to instead of last <= to, so a block "
     "ending exactly at `to` decodes instead of merging. Conservative, so the "
     "numbers stay right -- what goes red is the vacuity ledger's merge/seam "
     "counters, which is the point: a 'safe' plan that stops merging is a "
     "phase that stopped working.",
     "sec5",
     ("    if (id->first_timestamp_ns >= from_mono_ns &&\n"
      "        id->last_timestamp_ns <= to_mono_ns)\n"
      "        return PGWT_BLOCK_MERGE;",
      "    if (id->first_timestamp_ns >= from_mono_ns &&\n"
      "        id->last_timestamp_ns < to_mono_ns)\n"
      "        return PGWT_BLOCK_MERGE;   /* MUTANT: half-open containment */")),

    ("M21", "block_agg.c",
     "plan(): SKIP uses first >= to instead of first > to, so a block whose "
     "FIRST record sits exactly at `to` is skipped and that record is lost. "
     "The SKIP side is the one the half-open reading got wrong and the side "
     "MERGE's own strictness cannot compensate for.",
     "sec5/sec6/sec9",
     ("    if (id->first_timestamp_ns > to_mono_ns)\n"
      "        return PGWT_BLOCK_SKIP;",
      "    if (id->first_timestamp_ns >= to_mono_ns)\n"
      "        return PGWT_BLOCK_SKIP;   /* MUTANT: loses the ts == to record */")),

    ("M22", "block_agg.c",
     "pgwt_block_agg_file_can_contribute(): mono_first >= to instead of > to. "
     "This is M21's off-by-one one layer UP, at FILE granularity, and it is "
     "the form that actually SHIPPED: transitions_from_block_aggs() had a "
     "hand-written `fc->mono_first >= to_m` while the raw loader "
     "(server.c:2689) and the marker loader (:3089) both use `>`, so a file "
     "whose earliest record ends exactly at `to_m` was skipped WHOLE by the "
     "fast path and admitted by raw -- `total` and one link's value short by "
     "exactly those records, with NO refusal and NO fidelity change. M21 "
     "pinned this at block granularity and the suite still let the file-level "
     "copy through, which is why the rule is now one shared predicate with "
     "its own boundary sweep instead of three hand-written comparisons.",
     "sec1b",
     ("    if (mono_first > to_mono_ns)\n"
      "        return 0;",
      "    if (mono_first >= to_mono_ns)\n"
      "        return 0;   /* MUTANT: skips the file holding ts == to */")),

    ("M14", "block_agg.c",
     "lookup(): an ABSENT pair is reported as present with count 0 -- absence "
     "read as zero, the exact failure C8 forbids.",
     "sec2",
     ("    if (s->count == 0)\n"
      "        return 0;                       /* ABSENT, not zero */\n"
      "    if (count) *count = s->count;",
      "    if (count) *count = s->count;")),
]


# Sources the suite links. The mutated one is swapped for the copy in the
# work directory; everything else comes from src/.
LINKED = ["block_agg.c", "compute.c", "event_reader.c", "event_writer.c",
          "wait_event.c", "idle_rule.c", "cJSON.c"]


def build_and_run(workdir, label, mutated=None):
    """Compile the suite, taking `mutated` from `workdir`; return (exit, out)."""
    exe = os.path.join(workdir, "test_block_agg")
    srcs = [os.path.join(TESTS, "test_block_agg.c")]
    for name in LINKED:
        srcs.append(os.path.join(workdir if name == mutated else SRC, name))
    if sys.platform == "darwin":
        stub = os.path.join(workdir, "spawn_stub.c")
        with open(stub, "w") as fh:
            fh.write(MAC_SPAWN_STUB)
        srcs.append(stub)
    else:
        srcs.append(os.path.join(SRC, "spawn.c"))
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-g", "-O1", "-Wall", "-I" + SRC, "-I" + os.path.join(ROOT, "include"),
           "-DPGWT_SERVER"]
    # Homebrew keeps lz4 outside the default search path on this Mac; on Linux
    # these directories do not exist and are ignored.
    for extra in ("/opt/homebrew/include", "/usr/local/include"):
        if os.path.isdir(extra):
            cmd += ["-I" + extra]
    for extra in ("/opt/homebrew/lib", "/usr/local/lib"):
        if os.path.isdir(extra):
            cmd += ["-L" + extra]
    cmd += ["-o", exe] + srcs + ["-llz4", "-lm"]
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode != 0:
        return 126, "BUILD FAILED (%s)\n%s" % (label, b.stderr)
    r = subprocess.run([exe], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    want = sys.argv[1:] or None
    orig = {name: open(os.path.join(SRC, name)).read() for name in LINKED}

    # Control: the unmutated tree must be GREEN, or nothing below means anything.
    with tempfile.TemporaryDirectory() as wd:
        rc, out = build_and_run(wd, "control")
    with open(os.path.join(HERE, "mutation-control.log"), "w") as fh:
        fh.write(out)
    if rc != 0:
        print("CONTROL IS RED (exit %d) -- fix the tree before reading any "
              "mutation result" % rc)
        return 2
    print("control: GREEN (exit 0)")

    rows = []
    for mid, target, why, catches, (old, new) in MUTATIONS:
        if want and mid not in want:
            continue
        if old not in orig[target]:
            rows.append((mid, "NOT-APPLIED", catches,
                         "the mutation's anchor text is no longer in src/%s "
                         "-- this mutation proves nothing" % target))
            print("%-4s NOT-APPLIED (anchor missing in %s)" % (mid, target))
            continue
        text = orig[target].replace(old, new, 1)
        with tempfile.TemporaryDirectory() as wd:
            with open(os.path.join(wd, target), "w") as fh:
                fh.write(text)
            rc, out = build_and_run(wd, mid, mutated=target)
        fails = [ln for ln in out.splitlines() if ln.startswith("FAIL ")]
        log = os.path.join(HERE, "mutation-%s.log" % mid)
        with open(log, "w") as fh:
            fh.write("mutation %s (src/%s)\nbroken guard: %s\nexpected to be "
                     "caught by: %s\n\n--- %s\n+++ %s\n\nexit=%d, %d FAIL "
                     "line(s)\n\n"
                     % (mid, target, why, catches, old.strip()[:200],
                        new.strip()[:200], rc, len(fails)))
            fh.write(out)
        verdict = "RED" if rc != 0 else "GREEN-HOLE"
        if rc == 126:
            verdict = "BUILD-FAILED"
        rows.append((mid, verdict, catches,
                     "%d FAIL line(s); first: %s"
                     % (len(fails), fails[0][:140] if fails else "(none)")))
        print("%-4s %-12s %s" % (mid, verdict, fails[0][:100] if fails else ""))

    summary = os.path.join(HERE, "mutations-summary.txt")
    holes = [r for r in rows if r[1] != "RED"]
    with open(summary, "w") as fh:
        fh.write("tests/test_block_agg.c -- mutation / bypass results\n")
        fh.write("control (unmutated tree): GREEN\n")
        fh.write("every mutation below must be RED; a GREEN one is a hole in "
                 "the suite, not a passing build\n\n")
        for mid, verdict, catches, detail in rows:
            fh.write("%-4s %-12s caught-by=%-12s %s\n"
                     % (mid, verdict, catches, detail))
        fh.write("\n%d mutation(s), %d RED, %d NOT RED\n"
                 % (len(rows), len(rows) - len(holes), len(holes)))
        for mid, target, why, catches, _ in MUTATIONS:
            if want and mid not in want:
                continue
            fh.write("\n%s (src/%s): %s\n" % (mid, target, why))
    print("\n%d mutation(s), %d RED, %d NOT RED -> %s"
          % (len(rows), len(rows) - len(holes), len(holes), summary))
    return 1 if holes else 0


if __name__ == "__main__":
    sys.exit(main())
