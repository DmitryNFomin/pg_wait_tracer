#!/usr/bin/env python3
"""Break one guard at a time and require tests/test_top_queries_context.c to
notice. Evidence, not a gate: it recompiles the test once per mutation.

Three families:

  P*  PRODUCT mutations in src/query_ctx_index.h — the one-pass table is the
      code under test. Each must make the suite go RED.

  T*  TEST mutations: every way this suite could be satisfied without
      checking anything — a comparator blind to a field, an allocation
      injection that never fires, a growth point never crossed, a ledger that
      cannot see, a "deliberately blind" fixture that is no longer blind, and
      the case where the thing being checked is ABSENT rather than wrong.
      Each must also go RED.

  C*  COMBOS, expecting GREEN on purpose. Each pairs a product break with the
      removal of the one section that sees it: P6 alone is red, P6 with §4a
      deleted is green. That is the only way to show a refusal gate is the
      UNIQUE detector of its defect rather than incidentally covered — and
      the C family is why a weakened threshold (`> 0` -> `>= 0`) is tested
      here instead of alone, where it stays green because nothing is wrong
      yet and so proves nothing.

Scoring: BUILD-FAILED, NOT-APPLIED, a verdict other than the expected one,
and a RED control are each a FAILURE. A mutation that does not compile, or
whose anchor text no longer exists, proves nothing at all.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
HDR = os.path.join(ROOT, "src", "query_ctx_index.h")
TST = os.path.join(ROOT, "tests", "test_top_queries_context.c")

# Reusable edits, so a combo and a standalone mutation share the same text.
E_DROP_4A = (TST,
             "        CHECK(rc != 0, \"§4a NULL resolver: build returned success\");\n"
             "        CHECK(qc.failed, \"§4a NULL resolver: failed flag not set\");\n"
             "        CHECK(pgwt_query_ctx_lookup(&qc, 1001) == NULL,\n"
             "              \"§4a NULL resolver: a row resolved anyway\");",
             "        (void)rc;")
E_DROP_4C = (TST,
             "            CHECK(rc != 0, \"§4c %s: build reported success\", points[p]);\n"
             "            CHECK(qc.failed, \"§4c %s: failed flag not set\", points[p]);",
             "            (void)rc;")
E_NULL_RESOLVER_APPROVES = (HDR,
                            "    if (!resolve) {\n"
                            "        qc->failed = 1;\n"
                            "        return -1;\n"
                            "    }",
                            "    if (!resolve) {\n"
                            "        return 0;\n"
                            "    }")
E_ALLOC_FAIL_DROPS_ID = (HDR,
                         "            slot = pgwt_query_ctx_slot(qc, qid);\n"
                         "            if (slot < 0) {\n"
                         "                qc->failed = 1;\n"
                         "                return -1;\n"
                         "            }",
                         "            slot = pgwt_query_ctx_slot(qc, qid);\n"
                         "            if (slot < 0) {\n"
                         "                continue;\n"
                         "            }")
E_STARVE_LEDGER = (TST,
                   "    for (int q = 0; q < fx.n_qids; q++)\n"
                   "        ledger_classify(&fx, fx.qids[q]);",
                   "    /* mutated: the ledger sees nothing */")
E_WEAKEN_LEDGER_GATE = (TST,
                        "        CHECK(l[i].v > 0,",
                        "        CHECK(l[i].v >= 0,")
E_WEAKEN_MUTANT_GATE = (TST,
                        "        CHECK(mutant_caught[m] > 0,",
                        "        CHECK(mutant_caught[m] >= 0,")
E_ONLY_BLIND_FIXTURES = (TST,
                         "    run_fixture(fx_shapes);\n"
                         "    run_fixture(fx_all_clean);\n"
                         "    run_fixture(fx_no_synth);\n"
                         "    run_fixture(fx_wide);\n"
                         "    run_fixture(fx_dense);\n"
                         "    run_fixture(fx_random);",
                         "    run_fixture(fx_all_clean);\n"
                         "    run_fixture(fx_no_synth);")

# (id, expected verdict, [edits], why)
MUTATIONS = [
    # ── product ──────────────────────────────────────────────────────
    ("P1", "RED", [(HDR,
      "        } else if (e->databaseid != memo_db || e->userid != memo_uid) {\n"
      "            e->state = -1;\n"
      "            e->databaseid = 0;\n"
      "            e->userid = 0;\n"
      "        }",
      "        }")],
     "disagreement no longer refuses: one contributor's text on a mixed row"),
    ("P2", "RED", [(HDR,
      "        if (!memo_ok) {\n"
      "            e->state = -1;\n"
      "            e->databaseid = 0;\n"
      "            e->userid = 0;\n"
      "        } else if",
      "        if (!memo_ok) {\n"
      "            continue;\n"
      "        } else if")],
     "missing/ambiguous backend metadata skipped instead of poisoning the row"),
    ("P3", "RED", [(HDR,
      "        if (events[i].flags & PGWT_EVENT_FLAG_QUERY_SYNTH)\n"
      "            e->has_synth = 1;\n\n"
      "        /* Once ambiguous",
      "        if (events[i].flags & PGWT_EVENT_FLAG_QUERY_SYNTH)\n"
      "            e->has_synth = 1;\n"
      "        else\n"
      "            e->has_synth = 0;\n\n"
      "        /* Once ambiguous")],
     "last event wins the synth flag instead of any event"),
    ("P4", "RED", [(HDR,
      "        if (memo_ok < 0 || memo_pid != pid) {",
      "        if (memo_ok < 0) {")],
     "the per-pid memo never invalidates: every row inherits the first pid"),
    ("P5", "RED", [(HDR,
      "        if (memo_slot >= 0 && memo_qid == qid) {",
      "        if (memo_slot >= 0) {")],
     "the per-qid memo never invalidates: every event lands in one slot"),
    ("P6", "RED", [E_NULL_RESOLVER_APPROVES],
     "a NULL resolver approves an all-ambiguous table instead of refusing"),
    ("P7", "RED", [E_ALLOC_FAIL_DROPS_ID],
     "allocation failure drops the id silently: absence reported as zero"),
    ("P8", "RED", [(HDR,
      "        memset(ns + qc->cap, 0,\n"
      "               (size_t)(newcap - qc->cap) * sizeof(*ns));",
      "        memset(ns + qc->cap, 0xFF,\n"
      "               (size_t)(newcap - qc->cap) * sizeof(*ns));")],
     "a new slot is not initialised (the realloc tail, #271's shape)"),
    ("P9", "RED", [(HDR,
      "    int slot = pgwt_qid_index_find(&qc->by_qid, query_id);\n"
      "    if (slot < 0 || slot >= qc->n)\n"
      "        return NULL;",
      "    int slot = pgwt_qid_index_find(&qc->by_qid, query_id);\n"
      "    if (qc->n == 0)\n"
      "        return NULL;\n"
      "    if (slot < 0)\n"
      "        slot = 0;")],
     "an absent id resolves to slot 0's context"),
    ("P10", "RED", [(HDR,
      "        if (pgwt_test_alloc_fail(\"query_ctx_slots_grow\"))\n"
      "            return -1;",
      "        /* mutated: growth failure can no longer be injected */")],
     "the slots-growth refusal becomes unreachable"),
    ("P11", "RED", [(HDR,
      "        if (e->state < 0)\n            continue;",
      "        if (e->state < 0)\n            e->state = 0;")],
     "a refusal is forgotten, so a later good event re-resolves the row"),

    # ── test: the false-negative family ─────────────────────────────
    ("T1", "RED", [(TST,
      "    if (a.state != b.state) d |= DIFF_STATE;",
      "    if (0) d |= DIFF_STATE;")],
     "comparator blind to `state` (refused vs resolved vs absent)"),
    ("T2", "RED", [(TST,
      "    if (a.db    != b.db)    d |= DIFF_DB;",
      "    if (0) d |= DIFF_DB;")],
     "comparator blind to databaseid"),
    ("T3", "RED", [(TST,
      "    if (a.uid   != b.uid)   d |= DIFF_UID;",
      "    if (0) d |= DIFF_UID;")],
     "comparator blind to userid"),
    ("T4", "RED", [(TST,
      "    if (a.synth != b.synth) d |= DIFF_SYNTH;",
      "    if (0) d |= DIFF_SYNTH;")],
     "comparator blind to has_synth"),
    ("T5", "RED", [E_ONLY_BLIND_FIXTURES],
     "only the deliberately-BLIND fixtures run (the suite's power removed)"),
    ("T6", "RED", [(TST,
      "    { 102, 6, 7, 0, 1 },\n"
      "    { 103, 5, 8, 0, 1 },\n"
      "    { 104, 5, 7, 1, 1 },\n"
      "    { 105, 0, 7, 0, 1 },\n"
      "    { 106, 5, 0, 0, 1 },\n"
      "    { 107, 5, 7, 0, 0 },",
      "    { 102, 5, 7, 0, 1 },\n"
      "    { 103, 5, 7, 0, 1 },\n"
      "    { 104, 5, 7, 0, 1 },\n"
      "    { 105, 5, 7, 0, 1 },\n"
      "    { 106, 5, 7, 0, 1 },\n"
      "    { 107, 5, 7, 0, 1 },")],
     "no backend in the world is bad: every refusal shape disappears"),
    ("T7", "RED", [(TST,
      "        push(f, mk(100, q, PGWT_EVENT_FLAG_QUERY_SYNTH));\n"
      "        push(f, mk(101, q, PGWT_EVENT_FLAG_QUERY_SYNTH));\n"
      "        want(f, q);",
      "        push(f, mk(100, q, PGWT_EVENT_FLAG_QUERY_SYNTH));\n"
      "        push(f, mk(102, q, PGWT_EVENT_FLAG_QUERY_SYNTH));\n"
      "        want(f, q);")],
     "the deliberately-blind fixture is no longer blind (§3b must refuse)"),
    ("T9", "RED", [E_STARVE_LEDGER],
     "the ledger is starved: every counter zero"),
    ("T10", "RED", [(TST,
      "            setenv(\"PGWT_TEST_ALLOC_FAIL\", points[p], 1);",
      "            setenv(\"PGWT_TEST_ALLOC_FAIL\", \"never_matches\", 1);")],
     "the allocation-failure injection never fires (§4c would pass on air)"),
    ("T11", "RED", [(TST,
      "        for (int i = 0; i < 300; i++) wide[i] = mk(100, (uint64_t)i * 7 + 1, 0);",
      "        for (int i = 0; i < 300; i++) wide[i] = mk(100, 1, 0);")],
     "§4c's fixture no longer crosses any growth point"),
    ("T12", "RED", [(TST,
      "    if (n == 0) { led.absent++; return; }",
      "    if (n == 0) { return; }")],
     "the absent-row counter stops counting (the ledger must refuse)"),
    ("T13", "RED", [(TST,
      "    want(f, 9999);                                           /* absent from the events */\n"
      "    want(f, 1015);                                           /* absent, adjacent id */",
      "    /* mutated: no absent id queried here */"), (TST,
      "        want(f, q * 7 + 1);\n"
      "        want(f, q * 7 + 2);          /* absent neighbour */",
      "        want(f, q * 7 + 1);"), (TST,
      "    for (uint64_t q = 0; q < 45; q++) want(f, q);",
      "    for (uint64_t q = 0; q < 40; q++) want(f, q);")],
     "no absent id is queried anywhere: the thing checked is ABSENT, not wrong"),
    # ── product, continued ───────────────────────────────────────────
    ("P12", "RED", [(HDR,
      "    if (!events || count <= 0)\n        return 0;",
      "    if (!events || count <= 0)\n        return 0;\n    count = 1;")],
     "the pass stops after the first event (a partial range read as complete)"),

    # ── combos: expected GREEN, proving which section is the detector ──
    ("C1", "GREEN", [E_NULL_RESOLVER_APPROVES, E_DROP_4A],
     "P6 with §4a deleted: §4a is the UNIQUE detector of a NULL resolver"),
    ("C2", "GREEN", [E_ALLOC_FAIL_DROPS_ID, E_DROP_4C],
     "P7 with §4c deleted: §4c is the UNIQUE detector of a dropped id"),
    ("C3", "GREEN", [E_STARVE_LEDGER, E_WEAKEN_LEDGER_GATE],
     "T9 with the ledger's `> 0` weakened to `>= 0`: the threshold is what "
     "catches starvation, which is unprovable by weakening it alone"),
    ("C4", "GREEN", [E_ONLY_BLIND_FIXTURES, E_WEAKEN_MUTANT_GATE,
                     E_WEAKEN_LEDGER_GATE],
     "T5 with both `> 0` thresholds weakened: the mutant-caught and ledger "
     "gates are jointly what see a suite reduced to its blind fixtures"),
]


def run(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def build_and_run(tmpdir, label):
    bin_path = os.path.join(tmpdir, "t_" + label)
    b = run('cc -g -O2 -Wall -I"%s/src" -I"%s/include" -o "%s" "%s"'
            % (ROOT, ROOT, bin_path, TST))
    if b.returncode != 0:
        return "BUILD-FAILED", b.stderr
    r = run('"%s"' % bin_path)
    return ("GREEN" if r.returncode == 0 else "RED"), r.stdout + r.stderr


# --self-test drives this harness's OWN failure modes deliberately. Each must
# be REPORTED as a failure: a scorer that quietly treats "could not apply" or
# "did not compile" as a pass is the exact shape the bypass rule is about.
SELF_TEST = [
    ("S1", "RED", [(TST, "no such text anywhere in this file", "x")],
     "an anchor that matches nothing must be NOT-APPLIED, never a pass"),
    ("S2", "RED", [(TST, "static int failures;", "static int failures = ;")],
     "a mutation that does not compile must be BUILD-FAILED, never a pass"),
    ("S3", "RED", [(TST, "static int failures;", "static int  failures;")],
     "a no-op mutation expected RED must be reported as a GREEN mismatch"),
    ("S4", "GREEN", [(TST,
      "    if (a.state != b.state) d |= DIFF_STATE;",
      "    if (0) d |= DIFF_STATE;")],
     "a blinding mutation expected GREEN must be reported as a RED mismatch"),
]


def main():
    self_test = "--self-test" in sys.argv
    table = SELF_TEST if self_test else MUTATIONS
    tmpdir = tempfile.mkdtemp(prefix="tqc-mut-")
    results = []

    status, out = build_and_run(tmpdir, "control")
    print("control: %s" % status)
    if status != "GREEN":
        print(out[-4000:])
        print("\nFAIL: the control is not green; no mutation result means anything.")
        return 1

    detail = []
    for mid, expect, edits, why in table:
        backups = {}
        applied = True
        reason = ""
        for path, old, new in edits:
            if path not in backups:
                backups[path] = open(path).read()
            src = open(path).read()
            n = src.count(old)
            if n != 1:
                applied = False
                reason = "%d matches for an anchor in %s" % (n, os.path.basename(path))
                break
            open(path, "w").write(src.replace(old, new, 1))
        if applied:
            status, out = build_and_run(tmpdir, mid)
        for path, text in backups.items():
            open(path, "w").write(text)
        if not applied:
            status, out = "NOT-APPLIED", reason
        ok = (status == expect)
        results.append((mid, expect, status, ok, why))
        print("%-4s expect %-5s got %-12s %s  %s"
              % (mid, expect, status, "ok" if ok else "FAIL", why))
        if status == "RED":
            detail.append((mid, why,
                           [ln for ln in out.splitlines() if "FAIL" in ln][:3]))
        else:
            detail.append((mid, why, out.splitlines()[-12:]))

    print("\n--- what each mutation printed ---")
    for mid, why, lines in detail:
        print("\n%s  %s" % (mid, why))
        for ln in lines:
            print("   " + ln)

    bad = [r for r in results if not r[3]]
    print("\n%d/%d mutations matched their expected verdict"
          % (len(results) - len(bad), len(results)))
    for mid, expect, status, _, why in bad:
        print("  MISMATCH %s: expected %s, got %s — %s" % (mid, expect, status, why))

    status, _ = build_and_run(tmpdir, "restored")
    print("post-restore control: %s" % status)
    shutil.rmtree(tmpdir, ignore_errors=True)

    if self_test:
        # Every self-test entry is DESIGNED to mismatch. The harness passes
        # its own audit only when it reported all of them as failures.
        ok = (len(bad) == len(table)) and status == "GREEN"
        print("\nself-test: %d/%d deliberate faults reported as failures — %s"
              % (len(bad), len(table), "PASS" if ok else "FAIL"))
        return 0 if ok else 1
    return 0 if (not bad and status == "GREEN") else 1


if __name__ == "__main__":
    sys.exit(main())
