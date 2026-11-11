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
print("\n-- #622 forced-absent seam wiring --")
exact_source = io.open(os.path.join(PROJECT_DIR, "src", "exact_probe.c"),
                       encoding="utf-8").read()
# Extract the daemon's actual getenv argument feeding the pure parser, then
# compare every client assignment against it. Two unrelated literal searches
# would approve a one-sided rename (or a name left only in a comment).
seam_match = re.search(
    r'const char \*raw = getenv\("([^"\n]+)"\);\s*'
    r'enum pgwt_phase_force force = pgwt_exact_probe_parse_no_sema_refcnt\(raw\);',
    exact_source)
seam_env = seam_match.group(1) if seam_match else None
report(seam_env == "PGWT_TEST_NO_USDT_SEMA_REFCNT",
       "daemon reads the named #622 seam into its pure parser")


def phase_env_assignment(node, seam):
    return (isinstance(node, ast.Assign) and len(node.targets) == 1
            and isinstance(node.targets[0], ast.Subscript)
            and isinstance(node.targets[0].value, ast.Name)
            and node.targets[0].value.id == "env"
            and isinstance(node.targets[0].slice, ast.Constant)
            and node.targets[0].slice.value == seam
            and isinstance(node.value, ast.Constant) and node.value.value == "1")


def python_forces_absence(tree, seam):
    if seam is None:
        return False
    switch = ast.parse('env.get("PGWT_TEST_FORCE_NO_PHASE_PROBES") == "1"',
                       mode="eval").body
    guarded = any(isinstance(node, ast.If)
                  and ast.dump(node.test) == ast.dump(switch)
                  and any(phase_env_assignment(child, seam)
                          for statement in node.body for child in ast.walk(statement))
                  for node in ast.walk(tree))
    daemon_env = any(is_helper_call(node, "Popen")
                     and any(kw.arg == "env" and isinstance(kw.value, ast.Name)
                             and kw.value.id == "env" for kw in node.keywords)
                     for node in ast.walk(tree))
    return guarded and daemon_env


class UnwirePhaseEnv(ast.NodeTransformer):
    """Like UnwireHelper: preserve imports/comments/literals elsewhere."""

    def __init__(self, seam, rename):
        self.seam = seam
        self.rename = rename

    def visit_Assign(self, node):
        if not phase_env_assignment(node, self.seam):
            return self.generic_visit(node)
        if not self.rename:
            return ast.copy_location(ast.Pass(), node)
        node.targets[0].slice = ast.Constant(value="RENAMED_" + self.seam)
        return node


def shell_forces_absence(source, seam):
    if seam is None:
        return False
    return re.search(
        r'^if \[\[ "\$\{PGWT_TEST_FORCE_NO_PHASE_PROBES:-\}" == "1" \]\]; then\n'
        r'    export ' + re.escape(seam) + r'=1\nfi$', source, re.MULTILINE) is not None


for file in ("test_execution_cardinality.py", "test_execution_span_live.py",
             "test_escalation.sh", "ui_live_smoke.sh"):
    source = io.open(os.path.join(SCRIPT_DIR, file), encoding="utf-8").read()
    report("--skip-usdt" not in source,
           "%s no longer passes the policy-level proxy" % file)
    if file.endswith(".py"):
        report(python_forces_absence(ast.parse(source), seam_env),
               "%s explicitly sets daemon's %s when switch is 1" % (file, seam_env))
        for rename in (False, True):
            mutated = UnwirePhaseEnv(seam_env, rename).visit(ast.parse(source))
            report(not python_forces_absence(mutated, seam_env),
                   "%s rejects env assignment %s" %
                   (file, "renamed" if rename else "removed"))
        wrong_switch = source.replace('env.get("PGWT_TEST_FORCE_NO_PHASE_PROBES") == "1"',
                                      'env.get("PGWT_TEST_FORCE_NO_PHASE_PROBES") == "0"')
        report(not python_forces_absence(ast.parse(wrong_switch), seam_env),
               "%s rejects inverted forcing switch" % file)
    else:
        report(shell_forces_absence(source, seam_env),
               "%s exports daemon's %s when switch is 1" % (file, seam_env))
        for rename in (False, True):
            mutated = re.sub(r'(?m)^    export ' + re.escape(seam_env or "") + r'=1$',
                             "    export RENAMED_%s=1" % seam_env if rename else "    :",
                             source)
            report(not shell_forces_absence(mutated, seam_env),
                   "%s rejects env assignment %s" %
                   (file, "renamed" if rename else "removed"))
        report(not shell_forces_absence(source.replace(':-}" == "1"', ':-}" == "0"'),
                                       seam_env),
               "%s rejects inverted forcing switch" % file)

# The markers-absent rehearsal is only reachable through `make box-check` if
# the launcher forwards the switch past sudo's env_reset. Without this the
# capability exists but no documented path reaches it, and a declared-0 run
# would silently execute markers-PRESENT and go red for the wrong reason.
box_check = io.open(os.path.join(PROJECT_DIR, "scripts", "box-check.sh"),
                    encoding="utf-8").read()
report(re.search(r'sudo_env="\$sudo_env PGWT_TEST_FORCE_NO_PHASE_PROBES='
                 r'\$PGWT_TEST_FORCE_NO_PHASE_PROBES"', box_check) is not None
       and re.search(r'sudo_env="\$sudo_env PGWT_EXPECT_PHASE_PROBES='
                     r'\$PGWT_EXPECT_PHASE_PROBES"', box_check) is not None,
       "box-check.sh forwards both phase env vars past sudo env_reset")

absent_path = os.path.join(SCRIPT_DIR, "test_phase_absent_live.py")
report(os.path.isfile(absent_path), "new A/B live test exists")
if os.path.isfile(absent_path):
    absent_tree = ast.parse(io.open(absent_path, encoding="utf-8").read())
    for expected in (0, "PHASE_MASK"):
        report(any(is_helper_call(node, "phase_contract_verdict") and node.args
                   and ((isinstance(node.args[0], ast.Constant)
                         and node.args[0].value == expected)
                        if expected == 0 else
                        (isinstance(node.args[0], ast.Name)
                         and node.args[0].id == expected))
                   for node in ast.walk(absent_tree)),
               "A/B test calls phase_contract_verdict with %s" % expected)
live_array = re.search(r'(?ms)^LIVE_TESTS=\(.*?^\)', runner)
report(live_array is not None and re.search(
    r'^\s*"test_phase_absent\|python3\|test_phase_absent_live\.py"$',
    live_array.group(0), re.MULTILINE) is not None,
    "A/B test is registered in LIVE_TESTS as test_phase_absent")
unit_list = io.open(os.path.join(SCRIPT_DIR, "unit_tests.list"), encoding="utf-8").read()
exact_test = io.open(os.path.join(SCRIPT_DIR, "test_exact_probe.c"), encoding="utf-8").read()
report("test_exact_probe" in [line.split("#", 1)[0].strip()
                             for line in unit_list.splitlines()]
       and re.search(r'(?s)int main\(void\).*?test_phase_force_parse\(\);', exact_test)
       is not None and "pgwt_exact_probe_parse_no_sema_refcnt(" in exact_test,
       "pure parser assertions run in the registered test_exact_probe unit")

# Drive the live test's actual assertions with hostile telemetry. These are
# offline refusal probes, not a substitute for the real Ubuntu A/B capture.
print("\n-- #622 A/B assertion refusal probes --")
import errno
# A bare `import test_phase_absent_live` raises on a missing or syntactically
# broken module, which aborts the file: this suite would still exit non-zero,
# but the ~60 runner-classification checks BELOW would silently stop running
# and the "N/M checks passed" line would never print. A guard that loses
# coverage without saying so is the shape of a false negative, so the import
# failure is reported BY NAME as a failed check and the dependent probes are
# the only thing skipped.
try:
    import test_phase_absent_live as absent
except Exception as exc:  # ImportError, SyntaxError, anything at import time
    absent = None
    report(False, "A/B live test module imports: %s: %s"
           % (type(exc).__name__, exc))
else:
    report(True, "A/B live test module imports")

if absent is not None:
    forced_status = {"ok": True, "exact_probe_attached_mask": 3}
    forced_status.update({f: "attach_failed" for f in absent.OUTCOME_FIELDS})
    forced_status.update({f: errno.ENOTSUP for f in absent.ERRNO_FIELDS})
    forced_status.update({f: 1 for f in absent.FIRE_FIELDS})

    def live_assertions(forced, masks, artifacts, status=None):
        output = io.StringIO()
        absent.tests_run = absent.tests_failed = 0
        with redirect_stdout(output):
            if forced:
                absent.assert_forced(masks, status or forced_status, artifacts)
            else:
                absent.assert_control(masks, artifacts)
        return output.getvalue(), absent.tests_failed

    for forced, masks, artifacts, checks in (
            (True, [0, 1, 3, 3], {"executions": 0}, 5),
            (False, [3, 7, 15], {"executions": 1}, 2)):
        output, failures = live_assertions(forced, masks, artifacts)
        report(failures == 0 and output.count("  PASS:") == checks,
               "live %s accepts its measured fixture, including torn masks" %
               ("FORCED" if forced else "CONTROL"))
    for forced, masks, artifacts, number, label in (
            (True, [15], {"executions": 0}, 1, "inert seam (0xF)"),
            (True, [0], {"executions": 0}, 2, "too-broad seam (0x0)"),
            (True, [3], {"executions": 1}, 5, "forced markers produced artifacts"),
            (False, [3], {"executions": 1}, 6, "control markers absent"),
            (False, [15], {"executions": 0}, 7, "control artifacts empty")):
        output, failures = live_assertions(forced, masks, artifacts)
        report(failures > 0 and "  FAIL: %s " % number in output,
               "live check %s goes red: %s" % (number, label))
    for field in absent.OUTCOME_FIELDS + absent.ERRNO_FIELDS + absent.FIRE_FIELDS:
        bad = dict(forced_status)
        bad[field] = "disabled" if field in absent.OUTCOME_FIELDS else 0
        number = 4 if field in absent.FIRE_FIELDS else 3
        output, failures = live_assertions(True, [3], {"executions": 0}, bad)
        report(failures > 0 and "  FAIL: %s " % number in output,
               "live check %s goes red: %s=%r" % (number, field, bad[field]))
    for field in (("exact_probe_attached_mask",) + absent.OUTCOME_FIELDS
                  + absent.ERRNO_FIELDS + absent.FIRE_FIELDS):
        for raw in (None, "unreadable"):
            bad = dict(forced_status)
            if raw is None:
                del bad[field]
            else:
                # Outcome fields are strings; a non-string is their read failure.
                bad[field] = 99 if field in absent.OUTCOME_FIELDS else raw
            try:
                absent.validate_status(bad)
            except ValueError as exc:
                report(field in str(exc), "live status rejects %s %s by name" %
                       (field, "absent" if raw is None else "unreadable"))
            else:
                report(False, "live status accepted %s %s" % (field, raw))

# Exercise the real runner functions without its build/live preconditions.
# Text wiring alone cannot prove a stray 78 stays red, or that KNOWN_FAILING
# still refuses 126/127. No copy of the classifier lives in this harness.
print("\n-- #622 runner exit classification --")
marker = re.search(r'^PHASE_REFUSED_MARKER="([^"]*)"$', runner, re.MULTILINE)
report(marker is not None and marker.group(1) == tu.PHASE_REFUSED_MARKER,
       "runner marker literal equals testutil.PHASE_REFUSED_MARKER")
functions = [marker.group(0)] if marker else []
for name in ("known_failing_scope_matches", "known_failing_issue",
             "phase_dependent_test", "run_test", "undeclared_phase_test"):
    match = re.search(r"(?ms)^" + name + r"\(\) \{.*?^\}", runner)
    report(match is not None, "runner function %s is reachable" % name)
    if match:
        functions.append(match.group(0))
runner_harness = "\n".join(functions) + """
set -o pipefail
passed=0; failed=0; refused=0; known_failing=0; xpass=0
phase_expectation="$1"
KNOWN_FAILING=("known|622")
[[ "$6" == "1" ]] && KNOWN_FAILING+=("$2|622")
run_test "$2" bash -c '
    if [[ "$3" == "stderr" ]]; then
        printf "%s\\n" "$2" >&2
    else
        printf "%s\\n" "$2"
    fi
    exit "$1"
' unused "$3" "$4" "$5"
printf 'COUNTS %s %s %s %s %s\n' "$passed" "$failed" "$refused" "$known_failing" "$xpass"
"""


def runner_case(declared, name, rc, want, output=tu.PHASE_REFUSED_MARKER,
                stream="stdout", known=False, diagnostic=""):
    result = subprocess.run(["bash", "-uc", runner_harness, "runner-test",
                             declared, name, str(rc), output, stream,
                             "1" if known else "0"], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, universal_newlines=True)
    report(result.returncode == 0 and "COUNTS " + want in result.stdout
           and diagnostic in result.stdout,
           "declared=%r %s exit %s, %s on %s%s: %s" %
           (declared, name, rc, "marker" if output else "no marker", stream,
            ", known-failing" if known else "",
            result.stdout.strip().split("\n")[-1]))


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
        ("0", "unrelated", 0, "1 0 0 0 0"),
        ("0", "unrelated", 1, "0 1 0 0 0")):
    diagnostic = ""
    if rc == 78 and declared != "0":
        diagnostic = 'declaration not "0"'
    elif rc == 78 and name == "unrelated":
        diagnostic = "test is not phase-dependent"
    runner_case(declared, name, rc, want, diagnostic=diagnostic)

runner_case("0", "test_phase_absent", 78, "0 1 0 0 0",
            diagnostic="test is not phase-dependent")
# Also test the filename stem so a future classifier/registration rename cannot
# accidentally grant this internally asserted refusal an exit-78 exemption.
runner_case("0", "test_phase_absent_live", 78, "0 1 0 0 0",
            diagnostic="test is not phase-dependent")

runner_case("0", "test_execution_cardinality", 78, "0 1 0 0 0", output="",
            diagnostic="refusal marker absent")
runner_case("0", "test_execution_cardinality", 78, "0 0 1 0 0", stream="stderr")
runner_case("0", "test_execution_cardinality", 78, "0 0 0 1 0", known=True,
            diagnostic="KNOWN-FAILING (issue #622)")

print(f"\n{run - failed}/{run} checks passed")
sys.exit(0 if failed == 0 else 1)
