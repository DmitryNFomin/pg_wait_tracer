#!/usr/bin/env python3
"""test_uprobe_mask_gate.py -- the BYPASS suite for #591's exact-window mask
gate in tests/test_uprobe_fired.py.

The true-positive path of that gate has never been the broken one: a stale
`mask == ATTR_MASK` went red the moment #108's phase probes attached, loudly
and in every cycle. What this file exists for is the FALSE NEGATIVE -- every
way the gate could be made unable to see, and therefore able to approve:

  * the declaration is absent, empty or unparseable (so the gate would have
    to guess, or default, which is how it goes vacuous);
  * the mask field is absent from the JSON, or is not a number;
  * the thing being checked is ABSENT rather than wrong -- phase bits missing
    where they were declared present (that is #591's live red), and phase bits
    PRESENT where they were declared absent (which is what keeps the EL8
    branch of the assertion non-vacuous);
  * the required attribution pair is incomplete;
  * an unknown bundle bit appears (a plain `mask & ATTR_MASK` would accept it);
  * the gate is never CALLED at all -- two subprocess cases run the real
    tests/test_uprobe_fired.py and prove its main() refuses an undeclared
    environment before doing anything else, and gets past the declaration when
    one is given;
  * the runner stops declaring the expectation -- a case reads
    .github/workflows/ci.yml and fails if the capture-smoke cell no longer
    passes PGWT_EXPECT_PHASE_PROBES to tests/ci_smoke.sh.

Pure + offline: no root, no PostgreSQL, no eBPF, so it runs in `make check`
on macOS. Registered in scripts/check.sh.
"""
import ast
from contextlib import redirect_stdout
import io
import re
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, SCRIPT_DIR)

import test_uprobe_fired as uf  # noqa: E402
import testutil as tu  # noqa: E402

run = 0
failed = 0


def report(ok, msg):
    global run, failed
    run += 1
    if ok:
        print(f"  PASS: {msg}")
    else:
        failed += 1
        print(f"  FAIL: {msg}")


def refuses(raw, label):
    """resolve_expected_phase_mask must RAISE, not return a default."""
    try:
        got = uf.resolve_expected_phase_mask(raw)
    except ValueError as exc:
        report(uf.EXPECT_PHASE_ENV in str(exc),
               f"{label}: refused, and the message names the variable")
        return
    report(False, f"{label}: returned 0x{got:x} instead of refusing")


def accepts(raw, want, label):
    try:
        got = uf.resolve_expected_phase_mask(raw)
    except ValueError as exc:
        report(False, f"{label}: refused a valid declaration ({exc})")
        return
    report(got == want, f"{label}: resolved to 0x{got:x} (want 0x{want:x})")


def mask_rejects(mask, expected_phase, label):
    ok, why = uf.exact_window_mask_ok(mask, expected_phase)
    report(not ok, f"{label}: rejected ({why})" if not ok
           else f"{label}: ACCEPTED, gate is blind here ({why})")


def mask_accepts(mask, expected_phase, label):
    ok, why = uf.exact_window_mask_ok(mask, expected_phase)
    report(ok, f"{label}: accepted ({why})" if ok
           else f"{label}: rejected a legitimate mask ({why})")


def is_helper_call(node, helper):
    return isinstance(node, ast.Call) and (
        (isinstance(node.func, ast.Name) and node.func.id == helper)
        or (isinstance(node.func, ast.Attribute) and node.func.attr == helper))


def python_calls_helper(source, helper):
    """Imports, comments and strings cannot stand in for a helper invocation."""
    return any(is_helper_call(node, helper) for node in ast.walk(ast.parse(source)))


print("=== #591 exact-window mask gate: bypass suite ===")

# ── 1. The declaration cannot be skipped, defaulted or guessed ──────────
print("\n-- declaration resolution --")
refuses(None, "unset variable")
refuses("", "empty variable")
refuses("   ", "whitespace-only variable")
refuses("2", "out-of-range value '2'")
refuses("true", "non-numeric value 'true'")
refuses("yes", "non-numeric value 'yes'")
refuses("0x1", "hex-looking value '0x1'")
refuses("1,0", "unparseable value '1,0'")
accepts("1", uf.PHASE_MASK, "declared 1")
accepts("0", 0, "declared 0")
accepts(" 1 \n", uf.PHASE_MASK, "declared 1 with surrounding whitespace")

# ── 2. The gate cannot be satisfied without reading a usable mask ───────
print("\n-- unreadable / absent mask field --")
mask_rejects(None, uf.PHASE_MASK, "field absent from the JSON (None)")
mask_rejects("0xf", uf.PHASE_MASK, "field is a string '0xf'")
mask_rejects("", uf.PHASE_MASK, "field is an empty string")
mask_rejects(True, uf.PHASE_MASK, "field is the bool True (==1 in Python)")
mask_rejects([3], uf.PHASE_MASK, "field is a list")
mask_rejects(15.5, uf.PHASE_MASK, "field is a non-integral float")
mask_rejects(-1, uf.PHASE_MASK, "field is negative")
# cJSON numbers can decode as float; an integral one is still a real mask.
mask_accepts(15.0, uf.PHASE_MASK, "field is the integral float 15.0")

# ── 3. The thing checked is ABSENT rather than wrong ────────────────────
print("\n-- phase markers absent where declared present (#591's live red) --")
mask_rejects(0x3, uf.PHASE_MASK, "0x3: both phase markers missing")
mask_rejects(0x7, uf.PHASE_MASK, "0x7: plan marker missing")
mask_rejects(0xB, uf.PHASE_MASK, "0xb: execute marker missing")
mask_accepts(0xF, uf.PHASE_MASK, "0xf: full bundle, as declared")

print("\n-- phase markers present where declared absent (the EL8 branch) --")
mask_rejects(0xF, 0, "0xf against a declaration of 0")
mask_rejects(0x7, 0, "0x7 against a declaration of 0")
mask_accepts(0x3, 0, "0x3: attribution pair only, as declared")

# ── 4. Strictness kept: required bits, and no unknown bits ──────────────
print("\n-- required pair incomplete --")
mask_rejects(0xC, uf.PHASE_MASK, "0xc: phase only, no attribution pair")
mask_rejects(0xE, uf.PHASE_MASK, "0xe: query-id bit missing")
mask_rejects(0xD, uf.PHASE_MASK, "0xd: activity bit missing")
mask_rejects(0x0, 0, "0x0: nothing attached at all")

print("\n-- unknown bundle bits (a plain `mask & ATTR_MASK` accepts these) --")
mask_rejects(0x1F, uf.PHASE_MASK, "0x1f: a fifth, unknown bundle member")
mask_rejects(0x13, 0, "0x13: unknown bit 0x10 with the pair present")
mask_rejects(0xFFFFFFFF, uf.PHASE_MASK, "0xffffffff: garbage mask")

# ── 5. The gate is actually CALLED by the test it guards ────────────────
# Without this, every case above could pass while tests/test_uprobe_fired.py
# never consults the resolver -- the exact shape of defect this file is for.
print("\n-- the real test's main() consults the resolver --")
target = os.path.join(SCRIPT_DIR, "test_uprobe_fired.py")
if not os.path.exists(target):
    report(False, f"{target} is missing; cannot prove the gate is wired")
else:
    env = dict(os.environ)
    env.pop(uf.EXPECT_PHASE_ENV, None)
    # --pid 1 so an absent postmaster cannot be the reason it exits.
    r = subprocess.run([sys.executable, target, "--pid", "1"],
                       env=env, capture_output=True, text=True, timeout=120)
    out = r.stdout + r.stderr
    report(r.returncode != 0 and uf.EXPECT_PHASE_ENV in out,
           "undeclared environment: main() exits non-zero naming the "
           f"variable (rc={r.returncode})")
    report("must run as root" not in out,
           "the refusal happens BEFORE the root check, so it is reachable "
           "in make check (and cannot be masked by it)")

    env[uf.EXPECT_PHASE_ENV] = "1"
    r2 = subprocess.run([sys.executable, target, "--pid", "1"],
                        env=env, capture_output=True, text=True, timeout=120)
    out2 = r2.stdout + r2.stderr
    # A declaration of 1 is accepted, so execution proceeds to the next
    # precondition. On this Mac that is the root check; under sudo it would be
    # the postmaster. Either proves the resolver did not reject a valid value.
    report(uf.EXPECT_PHASE_ENV not in out2 or "is not set" not in out2,
           "a declaration of 1 is accepted and execution proceeds past it "
           f"(rc={r2.returncode})")

# ── 6. The runner still declares it ────────────────────────────────────
# A gate whose declaration has been dropped from the workflow would refuse
# every run -- loud, not silent -- but this names the cause up front instead
# of costing a 15-minute gate-box cell to discover.
print("\n-- .github/workflows/ci.yml declares the expectation --")
ci = os.path.join(PROJECT_DIR, ".github", "workflows", "ci.yml")
try:
    lines = io.open(ci, encoding="utf-8").read().splitlines()
except OSError as exc:
    lines = None
    report(False, f"cannot read {ci} ({exc}); refusing to approve blind")
if lines is not None:
    invocations = [i for i, l in enumerate(lines)
                   if "tests/ci_smoke.sh" in l and "--pg-version" in l]
    if not invocations:
        report(False, "no `tests/ci_smoke.sh --pg-version` invocation found "
                      "in ci.yml -- the parser drifted; refusing to approve "
                      "blind")
    for i in invocations:
        window = "\n".join(lines[max(0, i - 30):i + 1])
        report(uf.EXPECT_PHASE_ENV + "=" in window,
               f"ci.yml:{i + 1} passes {uf.EXPECT_PHASE_ENV} to ci_smoke.sh")

# #622: the four live gates must see an open window AND measured artifacts.
print("\n-- #622 shared phase contract bypasses --")


def contract_case(expected, masks, artifacts, want, label):
    verdict, reason = tu.phase_contract_verdict(expected, masks, artifacts)
    report(verdict == want, "%s: %s (%s)" % (label, verdict, reason))


for masks in ([], None, 3, "0xf", {}, ()):
    contract_case(tu.PHASE_MASK, masks, {"executions": 1}, "fail",
                  "no readable mask list %r" % (masks,))
for mask in (None, "15", True, 3.5, -1, {}, float("nan"), float("inf")):
    contract_case(tu.PHASE_MASK, [15, mask], {"executions": 1}, "fail",
                  "unreadable sample %r cannot hide behind a full mask" % (mask,))
contract_case(tu.PHASE_MASK, [3, 7, 15], {"executions": 4}, "pass",
              "union tolerates a torn read on the way to 0xf")
contract_case(tu.PHASE_MASK, [3, 3], {"executions": 4}, "fail",
              "union cannot invent missing markers")
contract_case(0, [3, 7], {"executions": 0}, "fail",
              "one phase bit ever seen contradicts declared 0")
contract_case(0, [3], {"executions": 4}, "fail",
              "declared-0 refusal is not vacuous")
contract_case(0, [3, 3], {"executions": 0, "exec_scatter": None}, "refuse",
              "declared 0 proves absent rows including explicit UNAVAIL")
contract_case(tu.PHASE_MASK, [15], {"executions": 0, "exec_scatter": None},
              "fail", "available-but-empty fails declared 1")
for expected, mask in ((0, 3), (tu.PHASE_MASK, 15)):
    for artifacts in ({}, {"x": "7"}, {"x": -1}, {"x": True}, {"x": 1.0}):
        contract_case(expected, [mask], artifacts, "fail",
                      "unreadable/absent artifacts %r" % artifacts)
    contract_case(expected, [mask, 0x10, mask], {"executions": 1}, "fail",
                  "unknown bit in any sample fails either declaration")
contract_case(1, [15], {"executions": 1}, "fail", "invalid expected phase mask")

print("\n-- #622 shared live wrapper --")
for expected, masks, artifacts, want, exit_code in (
        (tu.PHASE_MASK, [15], {"executions": 1}, "PASS", None),
        (0, [3], {"executions": 0}, "REFUSE", 78),
        (0, [15], {"executions": 0}, "FAIL", 1)):
    output = io.StringIO()
    got_exit = None
    with redirect_stdout(output):
        try:
            verdict = tu.phase_contract_apply(expected, masks, artifacts, "live-wrapper")
        except SystemExit as exc:
            got_exit = exc.code
    report(got_exit == exit_code and "  " + want + ": live-wrapper:" in output.getvalue()
           and (want != "REFUSE" or tu.PHASE_REFUSED_MARKER in output.getvalue()),
           "wrapper prints %s and exits %r" % (want, got_exit))
reasons = []
with redirect_stdout(io.StringIO()):
    tu.phase_contract_apply(0, [15], {"executions": 0}, "callback", fail=reasons.append)
report(len(reasons) == 1 and tu.EXPECT_PHASE_ENV in reasons[0],
       "wrapper delegates failures to the live test's failure reporter")

print("\n-- #622 shell resolver CLI --")
cli = os.path.join(SCRIPT_DIR, "testutil.py")
for raw, rc, stdout in ((None, 2, ""), ("1", 0, "1"), ("0", 0, "0"),
                        ("yes", 2, "")):
    env = dict(os.environ)
    env.pop(tu.EXPECT_PHASE_ENV, None)
    if raw is not None:
        env[tu.EXPECT_PHASE_ENV] = raw
    result = subprocess.run([sys.executable, cli, "phase-expectation"], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            universal_newlines=True)
    report(result.returncode == rc and result.stdout.strip() == stdout
           and (rc == 0 or tu.EXPECT_PHASE_ENV in result.stderr),
           "phase-expectation raw=%r: rc=%s stdout=%r" %
           (raw, result.returncode, result.stdout.strip()))
for args in ([], ["wrong"], ["phase-expectation", "extra"]):
    result = subprocess.run([sys.executable, cli] + args,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            universal_newlines=True)
    report(result.returncode == 2 and "Usage:" in result.stderr,
           "CLI rejects other arguments %r with usage" % args)

print("\n-- #622 the shared gate is wired into the live tier --")
runner = io.open(os.path.join(SCRIPT_DIR, "run_all.sh"), encoding="utf-8").read()
report("undeclared_phase_test" in runner
       and "undeclared phase-marker expectation" in runner
       and '"$phase_rc" -ne 0' in runner,
       "runner fails phase-dependent tests when no declaration resolves")
report('"$rc" -eq 78' in runner and '"$phase_expectation" == "0"' in runner
       and 'phase_dependent_test "$name"' in runner and 'refused=$((refused + 1))' in runner,
       "runner classifies exit 78 only for a declared-0 phase-dependent test")
for name in tu.PHASE_DEPENDENT_TESTS:
    report(name in runner, "%s is registered in the live runner" % name)


class UnwireHelper(ast.NodeTransformer):
    """Mutate only calls, leaving every import in the copied tree intact."""

    def __init__(self, helper, rename):
        self.helper = helper
        self.rename = rename

    def visit_Call(self, node):
        self.generic_visit(node)
        if not is_helper_call(node, self.helper):
            return node
        if not self.rename:
            return ast.copy_location(ast.Constant(value=None), node)
        if isinstance(node.func, ast.Name):
            node.func.id = "unwired_" + self.helper
        else:
            node.func.attr = "unwired_" + self.helper
        return node


for file, helper in (("test_execution_cardinality.py", "phase_contract_apply"),
                     ("test_execution_span_live.py", "phase_contract_apply"),
                     ("ui_live_smoke.py", "phase_tab_expectation")):
    source = io.open(os.path.join(SCRIPT_DIR, file), encoding="utf-8").read()
    report(python_calls_helper(source, helper)
           and python_calls_helper(source, "resolve_declared_phase_mask"),
           "%s calls the declared phase contract" % file)
    for rename in (False, True):
        mutated = UnwireHelper(helper, rename).visit(ast.parse(source))
        report(not python_calls_helper(ast.unparse(mutated), helper),
               "%s: wiring check rejects calls %s with imports kept" %
               (file, "renamed" if rename else "removed"))

for file, helper in (("test_escalation.sh", "phase_contract_verdict("),
                     ("ui_live_smoke.sh", "phase-expectation")):
    source = io.open(os.path.join(SCRIPT_DIR, file), encoding="utf-8").read()
    report(helper in source and ("resolve_declared_phase_mask" in source
                                or tu.EXPECT_PHASE_ENV in source),
           "%s calls the declared phase contract" % file)
for file in ("test_execution_cardinality.py", "test_execution_span_live.py",
             "test_escalation.sh", "ui_live_smoke.sh"):
    source = io.open(os.path.join(SCRIPT_DIR, file), encoding="utf-8").read()
    report("PGWT_TEST_FORCE_NO_PHASE_PROBES" in source and "--skip-usdt" in source,
           "%s starts the EL8 proxy when requested" % file)

# Exercise the real runner functions without its build/live preconditions.
# Text wiring alone cannot prove a stray 78 stays red, or that KNOWN_FAILING
# still refuses 126/127. No copy of the classifier lives in this harness.
print("\n-- #622 runner exit classification --")
functions = []
for name in ("known_failing_issue", "phase_dependent_test", "run_test",
             "undeclared_phase_test"):
    match = re.search(r"(?ms)^" + name + r"\(\) \{.*?^\}", runner)
    report(match is not None, "runner function %s is reachable" % name)
    if match:
        functions.append(match.group(0))
runner_harness = "\n".join(functions) + """
passed=0; failed=0; refused=0; known_failing=0; xpass=0
phase_expectation="$1"
KNOWN_FAILING=("known|622")
run_test "$2" bash -c 'exit "$1"' unused "$3"
printf 'COUNTS %s %s %s %s %s\n' "$passed" "$failed" "$refused" "$known_failing" "$xpass"
"""
for declared, name, rc, want in (
        ("0", "test_execution_cardinality", 78, "0 0 1 0 0"),
        ("0", "test_execution_span_live", 78, "0 0 1 0 0"),
        ("0", "test_escalation", 78, "0 0 1 0 0"),
        ("0", "test_ui_live_smoke", 78, "0 0 1 0 0"),
        ("1", "test_escalation", 78, "0 1 0 0 0"),
        ("", "test_escalation", 78, "0 1 0 0 0"),
        ("0", "unrelated", 78, "0 1 0 0 0"),
        ("0", "known", 126, "0 1 0 0 0"),
        ("0", "known", 127, "0 1 0 0 0"),
        ("0", "known", 1, "0 0 0 1 0"),
        ("0", "known", 0, "0 0 0 1 1"),
        ("0", "unrelated", 0, "1 0 0 0 0")):
    result = subprocess.run(["bash", "-uc", runner_harness, "runner-test",
                             declared, name, str(rc)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, universal_newlines=True)
    report(result.returncode == 0 and "COUNTS " + want in result.stdout,
           "declared=%r %s exit %s: %s" %
           (declared, name, rc, result.stdout.strip().split("\n")[-1]))

print(f"\n{run - failed}/{run} checks passed")
sys.exit(0 if failed == 0 else 1)
