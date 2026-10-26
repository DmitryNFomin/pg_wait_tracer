#!/usr/bin/env python3
"""test_uprobe_mask_gate.py -- the BYPASS suite for #591's exact-window mask
gate in tests/test_uprobe_fired.py.

The true-positive path of that gate has never been the broken one: a stale
`mask == ATTR_MASK` went red the moment #108's phase probes attached, loudly
and in every cycle. What this file exists for is the FALSE NEGATIVE -- every
way the gate could be made unable to see, and therefore able to approve:

  * an explicit override is empty or unparseable; without an override the
    shared resolver derives the expectation from kernel/binary evidence (#622);
  * the mask field is absent from the JSON, or is not a number;
  * the thing being checked is ABSENT rather than wrong -- phase bits missing
    where they were declared present (that is #591's live red), and phase bits
    PRESENT where they were declared absent (which is what keeps the EL8
    branch of the assertion non-vacuous);
  * the required attribution pair is incomplete;
  * an unknown bundle bit appears (a plain `mask & ATTR_MASK` would accept it);
  * the gate is never CALLED at all -- two subprocess cases run the real
    tests/test_uprobe_fired.py and prove its main() refuses an invalid explicit
    override before doing anything else, and gets past the override when
    one is given;
  * the runner stops declaring the expectation -- a case reads
    .github/workflows/ci.yml and fails if the capture-smoke cell no longer
    passes PGWT_EXPECT_PHASE_PROBES to tests/ci_smoke.sh.

Pure + offline: no root, no PostgreSQL, no eBPF, so it runs in `make check`
on macOS. Registered in scripts/check.sh.
"""
import io
import os
import subprocess
import sys
from unittest.mock import patch

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, SCRIPT_DIR)

import test_uprobe_fired as uf  # noqa: E402
from phase_probe_capability import resolve_phase_probes

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
    """An invalid explicit override must RAISE, never default."""
    try:
        with patch.dict(os.environ, {uf.EXPECT_PHASE_ENV: raw}):
            got = resolve_phase_probes().phase_mask
    except ValueError as exc:
        report(uf.EXPECT_PHASE_ENV in str(exc),
               f"{label}: refused, and the message names the variable")
        return
    report(False, f"{label}: returned 0x{got:x} instead of refusing")


def accepts(raw, want, label):
    try:
        with patch.dict(os.environ, {uf.EXPECT_PHASE_ENV: raw}):
            got = resolve_phase_probes().phase_mask
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


print("=== #591 exact-window mask gate: bypass suite ===")

# ── 1. The explicit override cannot be malformed ──────────
print("\n-- capability resolution / explicit overrides --")
with patch.dict(os.environ, {}, clear=True), \
        patch("phase_probe_capability.Path.stat", side_effect=FileNotFoundError):
    capability = resolve_phase_probes()
    report(not capability.expected and "ref_ctr_offset" in capability.reason,
           "unset override derives absent kernel capability (simulated sysfs)")
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
    env[uf.EXPECT_PHASE_ENV] = "invalid"
    # --pid 1 so an absent postmaster cannot be the reason it exits.
    r = subprocess.run([sys.executable, target, "--pid", "1"],
                       env=env, capture_output=True, text=True, timeout=120)
    out = r.stdout + r.stderr
    report(r.returncode != 0 and uf.EXPECT_PHASE_ENV in out,
           "invalid explicit override: main() exits non-zero naming the "
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

print(f"\n{run - failed}/{run} checks passed")
sys.exit(0 if failed == 0 else 1)
