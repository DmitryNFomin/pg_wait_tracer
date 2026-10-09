#!/usr/bin/env python3
"""Mutation / bypass driver for PHASE 2 of tests/test_block_agg.c.

Same contract as the Phase-1 driver next door
(tests/results/block-agg/run_mutations.py), aimed at the v2 surface: the
per-event table, the per-class fold, the C10 window clip and the
REFUSED_CLIP -> DECODE downgrade.

Why it exists: a gate nobody has seen fail is not evidence that it can, and
the half that has never been broken here is the TRUE positive. So the list
below is weighted towards FALSE negatives -- N8..N14 each break a guard in a
way that leaves the module answering ZERO or SHORT instead of refusing, which
is the only failure mode this codebase actually ships.

Two things this driver does that the Phase-1 one did not:

  * it mutates the ORACLE too (N20..N23, src/compute.c). Mutating only the
    module under test can prove the two sides AGREE; it cannot prove either
    is right. A bug shared by aggregate and oracle is invisible to a
    differential, and the only thing that rules it out is breaking the oracle
    and watching the suite go red.

  * it has a SELF-TEST (--selftest) for its own blind spots, because a
    mutation driver is itself a gate. It checks that a missing compiler
    (exit 127) makes the driver REFUSE rather than report a tidy row of
    GREENs, and that a mutation whose anchor text no longer exists is
    reported NOT-APPLIED and counted as a hole rather than silently skipped.

Verdicts: RED (the suite caught it -- required), GREEN-HOLE (the suite did
not -- a hole in the suite, and the driver exits nonzero), BUILD-FAILED (the
mutation did not compile, so it proves nothing), NOT-APPLIED (the anchor text
is gone, so it proves nothing).

Usage (from the repo root):
    python3 tests/results/block-agg-event-class/run_mutations.py
    python3 tests/results/block-agg-event-class/run_mutations.py N12
    python3 tests/results/block-agg-event-class/run_mutations.py --selftest
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
SUMMARY_NAME = "mutations-summary.txt"

MUTATIONS = [
    # ── the v2 code itself ────────────────────────────────────────────────
    ("N1", "block_agg.c",
     "add_event(): the per-event duration is no longer CLIPPED to the "
     "declared window (C10), so a wait that started before `from` is counted "
     "in full -- the lock-pileup overcount event_window_ns exists to stop.",
     "sec9e/sec10b",
     ("        int rc = ev_observe(a, ev,\n"
      "                            pgwt_block_agg_clip_ns(ev, a->ev_from_ns,\n"
      "                                                   a->ev_to_ns));",
      "        int rc = ev_observe(a, ev, ev->duration_ns);")),

    ("N2", "block_agg.c",
     "merge(): the START-side clip pre-flight is removed, so a block holding "
     "a wait that began before `from` is folded in with its UNCLIPPED total.",
     "sec9e/sec10b",
     ("        if (dst->ev_from_ns != 0 && src->ev_min_start_set &&\n"
      "            src->ev_min_start_ns < dst->ev_from_ns)\n"
      "            return PGWT_BAGG_REFUSED_CLIP;",
      "        /* MUTANT: the clip pre-flight is gone */")),

    ("N3", "block_agg.c",
     "merge(): the END-side pre-flight is removed, so a block reaching past "
     "`to` can be folded whole.",
     "sec11 E7",
     ("            if (dst->ev_to_ns != 0 &&\n"
      "                src->last_timestamp_ns > dst->ev_to_ns)\n"
      "                return PGWT_BAGG_REFUSED_CLIP;",
      "            /* MUTANT: the end-side pre-flight is gone */")),

    ("N4", "block_agg.c",
     "C9: the T2 io_worker exclusion is dropped, so io_worker time enters "
     "the DB-Time-ranked event list and double-represents I/O already "
     "counted in the requesting backend.",
     "sec10a/sec10b",
     ("    if (ev->flags & PGWT_EVENT_FLAG_IO_WORKER)\n"
      "        return 0;\n"
      "    return 1;",
      "    return 1;")),

    ("N5", "block_agg.c",
     "C9 is narrowed to drop SAMPLE records, so a sampled window loses its "
     "DB time entirely (the latency columns already exclude them -- C12 -- "
     "and that is a different rule).",
     "sec10a/sec10b",
     ("    if (PGWT_IS_MARKER(ev->old_event))\n"
      "        return 0;\n"
      "    if (ev->duration_ns > ev->timestamp_ns)\n"
      "        return 0;\n"
      "    if (pgwt_is_hidden_event(ev->old_event))\n"
      "        return 0;",
      "    if (PGWT_IS_MARKER(ev->old_event))\n"
      "        return 0;\n"
      "    if (ev->duration_ns > ev->timestamp_ns)\n"
      "        return 0;\n"
      "    if (ev->flags & PGWT_EVENT_FLAG_SAMPLE)\n"
      "        return 0;\n"
      "    if (pgwt_is_hidden_event(ev->old_event))\n"
      "        return 0;")),

    ("N6", "block_agg.c",
     "C14: the `|| is_new` clause of the SAMPLE_CONT count rule is removed, "
     "so a continuation fragment arriving first leaves the slot at count 0 "
     "and the whole row reads as absent.",
     "sec11 E12",
     ("    if (!(ev->flags & PGWT_EVENT_FLAG_SAMPLE_CONT) || is_new) {",
      "    if (!(ev->flags & PGWT_EVENT_FLAG_SAMPLE_CONT)) {")),

    ("N7", "block_agg.c",
     "build(): the SAMPLE_CONT refusal is removed, so a block whose "
     "per-event count is ORDER-DEPENDENT becomes mergeable.",
     "sec11 E12",
     ("    for (int i = 0; i < count; i++)\n"
      "        if (events[i].flags & PGWT_EVENT_FLAG_SAMPLE_CONT)\n"
      "            return PGWT_BAGG_REFUSED_BLOCK_TYPE;",
      "    /* MUTANT: the order-dependence refusal is gone */")),

    # ── FALSE NEGATIVES: the gate made unable to see, and approving ──────
    ("N8", "block_agg.c",
     "FALSE NEGATIVE. events_available() ignores `events_on`, so an "
     "accumulator that never declared a window reports its (never built) "
     "per-event table as readable -- absence reading as an answer, C8/C11.",
     "sec11 E1",
     ("    return a->events_on ? 1 : 0;",
      "    return 1;   /* MUTANT: absence reads as available */")),

    ("N9", "block_agg.c",
     "FALSE NEGATIVE. events_sorted() answers an EMPTY LIST instead of "
     "refusing when the table is unavailable -- indistinguishable from "
     "'this window has no events', which is a different and possible fact.",
     "sec11 E1",
     ("        return PGWT_BAGG_REFUSED_UNAVAILABLE;\n"
      "    if (a->n_events == 0)\n"
      "        return PGWT_BAGG_OK;\n"
      "    struct pgwt_block_agg_event *arr =",
      "        return PGWT_BAGG_OK;   /* MUTANT: absence answers zero */\n"
      "    if (a->n_events == 0)\n"
      "        return PGWT_BAGG_OK;\n"
      "    struct pgwt_block_agg_event *arr =")),

    ("N10", "block_agg.c",
     "FALSE NEGATIVE. enable_events() lets a window be retro-fitted onto a "
     "half-built accumulator, so some records are clipped and some are not "
     "and the total is silently short.",
     "sec11 E3",
     ("    if (a->n_keys != 0 || a->n_pairs != 0 || a->n_nodes != 0 ||\n"
      "        a->n_events != 0 || a->bounds_set)\n"
      "        return PGWT_BAGG_REFUSED_MODE;",
      "    /* MUTANT: a window may be retro-fitted */")),

    ("N11", "block_agg.c",
     "An INVERTED window is accepted, clipping every record to zero -- which "
     "looks exactly like a legitimately empty window.",
     "sec11 E5",
     ("    if (from_mono_ns != 0 && to_mono_ns != 0 && to_mono_ns < from_mono_ns)\n"
      "        return PGWT_BAGG_REFUSED_INVALID;",
      "    /* MUTANT: an inverted window is accepted */")),

    ("N12", "block_agg.c",
     "FALSE NEGATIVE. window_from_reader() SKIPS a clip-refused block "
     "instead of DECODEing it, so its records vanish with no refusal. "
     "Reachable ONLY through the shipped builder, which is why sec9e exists: "
     "sec10b's mirror cannot catch this.",
     "sec9e",
     ("            if (rc == PGWT_BAGG_REFUSED_CLIP) {\n"
      "                acc->clip_decoded_blocks++;\n"
      "                plan = PGWT_BLOCK_DECODE;\n"
      "                rc = PGWT_BAGG_OK;\n"
      "            } else if (rc != PGWT_BAGG_OK) {",
      "            if (rc == PGWT_BAGG_REFUSED_CLIP) {\n"
      "                acc->clip_decoded_blocks++;\n"
      "                rc = PGWT_BAGG_OK;\n"
      "                continue;   /* MUTANT: the records are lost */\n"
      "            } else if (rc != PGWT_BAGG_OK) {")),

    ("N13", "block_agg.c",
     "THE TRIPWIRE Phase 2 makes live, put back: the non-TRANSITIONS "
     "overlap test returns to a hand-written HALF-OPEN "
     "`first_timestamp_ns < to_mono_ns` while C7 is inclusive, so a SAMPLES "
     "block starting exactly at `to` is silently skipped instead of refused.",
     "sec9c-bis",
     ("        int overlaps = pgwt_block_agg_file_can_contribute(\n"
      "            bi.first_timestamp_ns, bi.last_timestamp_ns,\n"
      "            from_mono_ns, to_mono_ns);",
      "        int overlaps = bi.last_timestamp_ns >= from_mono_ns &&\n"
      "                       bi.first_timestamp_ns < to_mono_ns;")),

    ("N14", "block_agg.c",
     "build() no longer refuses WHOLE on a per-event allocation failure: it "
     "returns an aggregate with pair/node tables and a short per-event one.",
     "sec11 E10",
     ("            int rc = ev_observe(out, ev, ev->duration_ns);\n"
      "            if (rc != PGWT_BAGG_OK) {\n"
      "                pgwt_block_agg_free(out);\n"
      "                return rc;              /* refuse whole, never partial */\n"
      "            }",
      "            (void)ev_observe(out, ev, ev->duration_ns);  /* MUTANT */")),

    # ── arithmetic, where a silently wrong number lives ─────────────────
    ("N15", "block_agg.c",
     "db_time_ns() charges idle-but-visible events to DB Time, so every %DB "
     "in the Events tab is computed against an inflated denominator.",
     "sec9e/sec10b/sec12 P5",
     ("        if (pgwt_is_idle_event(a->events[i].event_id))\n"
      "            continue;\n"
      "        t += a->events[i].total_ns;",
      "        t += a->events[i].total_ns;")),

    ("N16", "block_agg.c",
     "The latency ladder is off by one at the 16 us edge, so a 16 us wait "
     "lands in the bucket below and every percentile drawn from it shifts.",
     "sec10c",
     ("    if (us < 16)    return 4;",
      "    if (us <= 16)   return 4;")),

    ("N17", "block_agg.c",
     "ev_add_row(): the latency HISTOGRAM is not merged, so a merged block "
     "contributes counts with no distribution behind them and the "
     "percentiles the wiring step derives come from a different population "
     "than the row claims.",
     "sec10b",
     ("    for (int b = 0; b < HISTOGRAM_BUCKETS; b++)\n"
      "        s->hist[b] += src->hist[b];",
      "    /* MUTANT: the histogram is not merged */")),

    ("N18", "block_agg.c",
     "ev_add_row(): exact_count is not merged, so every merged row reports "
     "its latency columns as unavailable.",
     "sec9e/sec10b",
     ("    s->exact_count    += src->exact_count;",
      "    /* MUTANT: exact_count is not merged */")),

    ("N19", "block_agg.c",
     "classes_sorted() emits only the FIRST class, dropping the rest. A "
     "comparison that only walked the rows it was GIVEN would not notice, "
     "which is why sec10b reconciles the class totals against the event "
     "totals AND against the oracle's own row sum.",
     "sec10b",
     ("    for (int c = 0; c < PGWT_NUM_CLASSES; c++)\n"
      "        if (acc[c].count != 0)\n"
      "            arr[k++] = acc[c];\n"
      "    qsort(arr, (size_t)k, sizeof(*arr), cmp_class_total);",
      "    for (int c = 0; c < PGWT_NUM_CLASSES; c++)\n"
      "        if (acc[c].count != 0 && k == 0)\n"
      "            arr[k++] = acc[c];\n"
      "    qsort(arr, (size_t)k, sizeof(*arr), cmp_class_total);")),

    # ── the ORACLE, mutated too ─────────────────────────────────────────
    # Without these the suite could only prove the two sides AGREE, not that
    # they are both right: a bug shared by aggregate and oracle would be
    # invisible. Each one must go RED, which is what rules that out.
    ("N20", "compute.c",
     "ORACLE. pgwt_compute_top_events() stops clipping to the window and "
     "uses the full duration. If this stayed GREEN, the aggregate would be "
     "agreeing with an unclipped oracle -- i.e. not clipping either.",
     "sec9e/sec10b",
     ("        uint64_t clip_u = event_window_ns(ev, from_ns, to_ns);\n"
      "\n"
      "        /* Idle-but-visible events",
      "        uint64_t clip_u = ev->duration_ns;   /* MUTANT */\n"
      "\n"
      "        /* Idle-but-visible events")),

    ("N21", "compute.c",
     "ORACLE. event_window_ns() loses its START clamp, so the oracle counts "
     "pre-window time. The shared-bug check for C10 from the other side.",
     "sec9e/sec10b",
     ("    if (from_ns != 0 && start < from_ns) start = from_ns;",
      "    /* MUTANT: no start clamp */")),

    ("N22", "compute.c",
     "ORACLE. the T2 io_worker exclusion is removed from "
     "pgwt_compute_top_events(), so the oracle emits a row the aggregate "
     "refuses. Pins C9 from the oracle's side.",
     "sec10a/sec10b",
     ("        if (ev->flags & PGWT_EVENT_FLAG_IO_WORKER)\n"
      "            continue;\n"
      "\n"
      "        /* Time-in-window for the DB-Time columns",
      "        /* MUTANT: the io_worker exclusion is gone */\n"
      "\n"
      "        /* Time-in-window for the DB-Time columns")),

    ("N23", "compute.c",
     "ORACLE. compute.c's OWN latency ladder is moved at the 16 us edge. "
     "sec10c learns its edges from this function, so this proves sec10c is "
     "measuring the product rather than comparing two copies of one table.",
     "sec10c",
     ("    if (us < 16)    return 4;",
      "    if (us <= 16)   return 4;")),
]


def selftest():
    """The driver is itself a gate; these are the ways it could go blind."""
    global SUMMARY_NAME
    SUMMARY_NAME = "mutations-summary-selftest.txt"   # never clobber the real one
    out = []
    ok = True

    # (1) A missing compiler. Everything below depends on being able to
    #     build, so a driver that cannot build must REFUSE, not report a
    #     clean sweep. Expected: control is RED, exit 2, no mutation row.
    env = dict(os.environ)
    env["CC"] = "/nonexistent/cc-127"
    r = subprocess.run([sys.executable, os.path.abspath(__file__), "N1"],
                       capture_output=True, text=True, env=env, cwd=ROOT)
    refused = r.returncode != 0 and "CONTROL IS RED" in r.stdout
    out.append("missing compiler (exit 127 dependency): exit=%d refused=%s\n"
               "    %s" % (r.returncode, refused,
                          r.stdout.strip().splitlines()[-1] if r.stdout.strip()
                          else "(no output)"))
    if not refused:
        ok = False
        out.append("    FAIL: a driver that cannot compile must refuse, "
                   "never approve")

    # (2) A mutation whose anchor text is gone. It proves nothing, so it must
    #     be reported NOT-APPLIED and counted as a hole (nonzero exit), not
    #     quietly skipped.
    global MUTATIONS
    saved = MUTATIONS
    MUTATIONS = [("SELFTEST-ANCHOR", "block_agg.c",
                  "an anchor that does not exist in the tree",
                  "nothing", ("/* this text is not in block_agg.c */",
                              "/* replacement */"))]
    try:
        rc = main_inner(["SELFTEST-ANCHOR"])
    finally:
        MUTATIONS = saved
    out.append("absent anchor: driver exit=%d (must be nonzero)" % rc)
    if rc == 0:
        ok = False
        out.append("    FAIL: a mutation that could not be applied must "
                   "count as a hole")

    # (3) An EMPTY mutation list. A driver asked to prove nothing must not
    #     report success, because "0 holes" would read as "fully covered".
    MUTATIONS = []
    try:
        rc_empty = main_inner(["NOTHING-MATCHES"])
    finally:
        MUTATIONS = saved
    out.append("empty mutation set: driver exit=%d (must be nonzero)"
               % rc_empty)
    if rc_empty == 0:
        ok = False
        out.append("    FAIL: an empty run must refuse, not approve")

    text = ("run_mutations.py SELF-TEST -- the driver's own blind spots\n"
            "every case below must show the driver REFUSING rather than "
            "approving\n\n" + "\n".join(out) +
            "\n\nVERDICT: %s\n" % ("PASS" if ok else "FAIL"))
    with open(os.path.join(HERE, "selftest.txt"), "w") as fh:
        fh.write(text)
    print(text)
    return 0 if ok else 1



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
    cmd += ["-o", exe] + srcs + ["-llz4", "-lz", "-lm"]
    # A missing or non-executable compiler must be reported, not raised: an
    # uncaught OSError here exits the driver with a traceback and no verdict,
    # which reads as "the run broke" rather than "the driver could not see".
    # Exit 127 is the shell's own "command not found", kept deliberately.
    try:
        b = subprocess.run(cmd, capture_output=True, text=True)
    except OSError as e:
        return 127, ("COMPILER UNAVAILABLE (%s): %s\n"
                     "cannot build, therefore cannot judge -- refusing\n"
                     % (label, e))
    if b.returncode != 0:
        return 126, "BUILD FAILED (%s)\n%s" % (label, b.stderr)
    r = subprocess.run([exe], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


# A mutation that breaks an arithmetic rule fails once PER COMPARED ROW, and
# the suite compares 17,899 rows over 5,067 windows. The first cut of this
# driver wrote every one of those lines to disk: 11 of the 23 logs held
# 111,901 of 114,034 total lines, and mutation-N18.log alone carried 13,994
# FAIL lines expressing 75 DISTINCT assertions. The repetition is not
# evidence -- it is the same assertion firing again on the next row -- and
# 112k lines of it in git history is not something a reviewer can read.
#
# So a long log becomes a DIGEST that keeps everything load-bearing: the true
# line and FAIL counts, one example of every distinct failing assertion, the
# first raw FAIL lines, and the tail (which carries the vacuity ledger and
# the "FAILED: n check(s)" total). The banner says TRUNCATED in as many
# words, with both counts and the command to reproduce in full, so a digest
# can never be mistaken for a complete run. A quietly shortened log would be
# worse than an unreadably long one.
LOG_VERBATIM_MAX = 400      # lines; at or below this the output is kept as-is
DIGEST_SHAPES = 80          # distinct assertions to show one example of
DIGEST_HEAD = 20            # raw FAIL lines from the start
DIGEST_TAIL = 60            # trailing lines (ledger + totals)


def fail_shape(line):
    """The assertion's IDENTITY, with every per-row VALUE stripped off.

    Keyed on `file:line` plus the message with all digit runs collapsed to
    `#`, so one CHECK firing on 13,994 different rows becomes a single shape
    while two different CHECKs never merge. The absolute path is dropped so
    the key does not depend on which checkout produced the log.

    The first cut kept 70 raw characters of the message, which still held the
    event id and both compared numbers -- so it collapsed nothing and the
    banner reported "13,921 distinct assertions" out of 13,994 FAIL lines.
    That was worse than no summary at all: it stated the opposite of the
    truth in a line a reviewer would otherwise trust. Hence the digit
    normalisation, and hence the self-check in digest() below.
    """
    where, _, body = line.partition(": ")
    where = re.sub(r"^FAIL .*/", "FAIL ", where)
    return "%s: %s" % (where, re.sub(r"\d+", "#", body))


def digest(out, fails):
    lines = out.splitlines()
    if len(lines) <= LOG_VERBATIM_MAX:
        return out + "\n[complete output, %d line(s), not truncated]\n" % len(lines)

    shapes, seen_shapes = [], set()
    for ln in fails:
        k = fail_shape(ln)
        if k not in seen_shapes:
            seen_shapes.add(k)
            shapes.append(ln)

    parts = [
        "=" * 72,
        "TRUNCATED -- this is a DIGEST of the run, NOT the complete output.",
        "  complete output : %d line(s), of which %d are FAIL lines"
        % (len(lines), len(fails)),
        "  distinct failing assertions : %d%s" % (
            len(shapes),
            "" if len(shapes) * 2 <= len(fails) else
            "   <-- WARNING: barely below the FAIL count, so the shape "
            "normalisation in fail_shape() is NOT collapsing per-row values; "
            "treat this number as unmeasured"),
        "  the other %d FAIL line(s) are those same assertions firing again"
        % max(0, len(fails) - len(shapes)),
        "  on further rows and windows; they carry no further information.",
        "  reproduce in full:",
        "    python3 tests/results/block-agg-event-class/run_mutations.py <id>",
        "=" * 72,
        "",
        "-- one example of each distinct failing assertion (%d of %d shown):"
        % (min(len(shapes), DIGEST_SHAPES), len(shapes)),
    ]
    parts += shapes[:DIGEST_SHAPES]
    if len(shapes) > DIGEST_SHAPES:
        parts.append("   ... %d further distinct assertion(s) not shown"
                     % (len(shapes) - DIGEST_SHAPES))
    parts += ["", "-- first %d FAIL line(s), verbatim:" % DIGEST_HEAD]
    parts += fails[:DIGEST_HEAD]
    parts += ["",
              "-- last %d line(s) of output (the vacuity ledger and the "
              "total):" % DIGEST_TAIL]
    parts += lines[-DIGEST_TAIL:]
    return "\n".join(parts) + "\n"


def main_inner(want):
    orig = {name: open(os.path.join(SRC, name)).read() for name in LINKED}

    # Control: the unmutated tree must be GREEN, or nothing below means anything.
    with tempfile.TemporaryDirectory() as wd:
        rc, out = build_and_run(wd, "control")
    with open(os.path.join(HERE, "mutation-control%s.log"
                           % ("-selftest" if SUMMARY_NAME != "mutations-summary.txt"
                              else "")), "w") as fh:
        fh.write(out)
    if rc != 0:
        print("CONTROL IS RED (exit %d) -- fix the tree before reading any "
              "mutation result" % rc)
        print(out.strip().splitlines()[0] if out.strip() else "(no output)")
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
            fh.write(digest(out, fails))
        # A nonzero exit is only RED if the SUITE rejected the mutant. A
        # build that never ran, or a compiler that does not exist, is not a
        # caught mutation -- labelling either RED would be the driver taking
        # credit for a hole it never probed.
        verdict = "RED" if rc != 0 else "GREEN-HOLE"
        if rc == 126:
            verdict = "BUILD-FAILED"
        elif rc == 127:
            verdict = "NO-COMPILER"
        rows.append((mid, verdict, catches,
                     "%d FAIL line(s); first: %s"
                     % (len(fails), fails[0][:140] if fails else "(none)")))
        print("%-4s %-12s %s" % (mid, verdict, fails[0][:100] if fails else ""))

    summary = os.path.join(HERE, SUMMARY_NAME)
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
    if not rows:
        print("NO MUTATION RAN -- a driver that proved nothing must not "
              "report success")
        return 3
    return 1 if holes else 0


def main():
    args = sys.argv[1:]
    if args and args[0] == "--selftest":
        return selftest()
    return main_inner(args or None)


if __name__ == "__main__":
    sys.exit(main())
