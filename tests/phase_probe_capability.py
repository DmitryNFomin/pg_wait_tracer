"""Shared phase-marker test contract (#622).

Derive prerequisites independently of the daemon: kernel semaphore refcounts
AND the postgresql SDT notes in the traced postmaster's executable, resolved
PER PROBE (execute pair, plan pair) because they attach and fail
independently (src/exact_probe.c pgwt_exact_probe_core_acquire: each
optional bundle member is attempted on its own, best effort, after the
window is granted). PGWT_EXPECT_PHASE_PROBES=0|1 explicitly overrides that
derivation for BOTH probes at once. Passing these prerequisites is an
expectation -- an UPPER BOUND on what could attach -- never proof of
attachment or firing: the execution-site allowlist, attach, mask and data
assertions still gate it, and a predicted-capable probe can still fail to
attach at runtime for reasons this resolver cannot see (see
exact_window_mask_ok).
"""
import argparse
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import re
import subprocess
import sys

EXPECT_PHASE_ENV = "PGWT_EXPECT_PHASE_PROBES"
REF_CTR_OFFSET = Path("/sys/bus/event_source/devices/uprobe/format/ref_ctr_offset")
EXEC_NOTES = frozenset(("query__execute__start", "query__execute__done"))
PLAN_NOTES = frozenset(("query__plan__start", "query__plan__done"))
PHASE_NOTES = EXEC_NOTES | PLAN_NOTES
ATTR_MASK = 0x3
EXEC_BIT = 0x4
PLAN_BIT = 0x8
PHASE_MASK = EXEC_BIT | PLAN_BIT
ALL_MASK = ATTR_MASK | PHASE_MASK
# Deliberately different from success, failure, and conventional skip (77).
CAPABILITY_REFUSAL = 78


def _phase_bit_names(bits):
    names = []
    if bits & EXEC_BIT:
        names.append("EXEC")
    if bits & PLAN_BIT:
        names.append("PLAN")
    return tuple(names)


@dataclass(frozen=True)
class PhaseCapability:
    exec_expected: bool
    plan_expected: bool
    reason: str

    @property
    def expected(self):
        """Both halves of the bundle derived capable. Waterfall/Scatter
        identify executions from the combined exec+plan bundle; a platform
        capable of only one probe is not capable of the contract those
        tests need, so this is the all-or-nothing gate require() and
        require_phase_probes() use."""
        return self.exec_expected and self.plan_expected

    @property
    def phase_mask(self):
        """The UPPER BOUND on exact_probe_attached_mask's phase bits this
        platform/binary could carry -- necessary, never sufficient. See
        exact_window_mask_ok for why a bit in this mask is not a promise
        that bit will actually attach."""
        mask = 0
        if self.exec_expected:
            mask |= EXEC_BIT
        if self.plan_expected:
            mask |= PLAN_BIT
        return mask

    def require(self, caller):
        if not self.expected:
            print(f"REFUSED (phase capability): {caller}: {self.reason}")
            raise SystemExit(CAPABILITY_REFUSAL)
        print(f"Phase markers expected: {self.reason}")


def resolve_phase_probes(pm_pid=None):
    raw = os.environ.get(EXPECT_PHASE_ENV)
    if raw is not None:
        if raw.strip() not in ("0", "1"):
            raise ValueError(f"{EXPECT_PHASE_ENV}={raw!r} is neither '0' nor '1'")
        value = raw.strip() == "1"
        return PhaseCapability(value, value,
                               f"explicit override {EXPECT_PHASE_ENV}={raw.strip()}")

    # stat, rather than exists(): permission/inspection errors must fail,
    # never masquerade as evidence of an unavailable capability.
    try:
        REF_CTR_OFFSET.stat()
    except FileNotFoundError:
        return PhaseCapability(False, False, "not available on this kernel: "
                               f"{REF_CTR_OFFSET} is absent (no USDT semaphore "
                               "refcounting)")

    if not pm_pid:
        from testutil import find_postmaster
        pm_pid = find_postmaster()
    if not pm_pid:
        raise ValueError("cannot inspect phase SDT notes: no postmaster found")
    # Same target selection as pgwt_find_pg_binary / pgwt_discover, whose
    # pg_binary_saved is passed to bpf_program__attach_usdt (not PATH's PG).
    binary = os.readlink(f"/proc/{pm_pid}/exe")
    env = dict(os.environ, LC_ALL="C")
    result = subprocess.run(["readelf", "--notes", binary], env=env,
                            capture_output=True, text=True, timeout=10)
    if result.returncode:
        raise ValueError(f"cannot inspect SDT notes in {binary}: {result.stderr.strip()}")
    notes = set()
    for block in re.split(r"(?m)^\s*Provider:\s*", result.stdout)[1:]:
        provider = block.splitlines()[0].strip()
        name = re.search(r"(?m)^\s*Name:\s*(\S+)\s*$", block)
        if provider == "postgresql" and name:
            notes.add(name.group(1))
    exec_ok = EXEC_NOTES <= notes
    plan_ok = PLAN_NOTES <= notes
    evidence = f"{REF_CTR_OFFSET} is present; {binary}"
    if not exec_ok and not plan_ok:
        return PhaseCapability(False, False,
                               f"not available in this PostgreSQL build: "
                               f"{evidence} has none of the four postgresql "
                               "execute/plan SDT notes")
    if exec_ok and plan_ok:
        return PhaseCapability(True, True,
                               f"{evidence} carries all four postgresql phase SDT notes")
    # One pair's notes present, the other absent: each probe attaches
    # independently (src/exact_probe.c), so this is a real, nameable
    # partial capability -- not an error to raise past.
    present_name, missing_name = ("execute", "plan") if exec_ok else ("plan", "execute")
    return PhaseCapability(exec_ok, plan_ok,
                           f"{evidence} carries the {present_name} postgresql "
                           f"SDT notes but not {missing_name} -- partial bundle, "
                           "the combined contract needs both")


def exact_window_mask_ok(mask, expected_phase):
    """The contract on an OPEN exact window's exact_probe_attached_mask,
    given `expected_phase` (PhaseCapability.phase_mask): an UPPER BOUND on
    the phase bits this platform/binary could carry, never a promise.

    Asserted (returns ok=False on violation):
      1. the REQUIRED attribution pair is present   (mask & 0x3) == 0x3
      2. no bits outside the known bundle           (mask & ~0xf) == 0
      3. no phase bit attached beyond derived capable
                                   (mask & 0xc) & ~expected_phase == 0

    (2) is why this is not written as a plain `mask & ATTR_MASK`: a new,
    unknown bundle member appearing in the JSON must still fail here rather
    than be silently accepted. An absent or unparseable field fails too -- a
    check that cannot read what it is checking refuses, it never approves.

    NOT asserted, and why: a phase bit capability resolution said this
    platform/binary COULD carry is not required to actually be attached.
    #108's optional attach is independent per probe and best-effort
    (src/exact_probe.c pgwt_exact_probe_core_acquire): EXEC additionally
    needs canonical-site publication and symbol/site/load-base resolution
    (src/discovery.c) that PLAN does not, so the two fail independently. A
    measured Ubuntu box showed exactly that split -- EXEC attached, PLAN did
    not, with both predicted capable from kernel+notes -- and the attach
    wrapper collapses every libbpf failure to ENOTSUP
    (usdt_pair_attach_one), so this test cannot name why a predicted-capable
    bit failed to attach; #592 adds the per-probe diagnostics that can, and
    is not on this tree. Asserting exact equality here was wrong on the only
    platform this has been measured on, so it is reported, not asserted:
    returns (ok, reason, degraded) where `degraded` names any
    predicted-capable bit ("EXEC"/"PLAN") the window did not attach.
    Callers print it plainly; they must never fold it into the ok/fail verdict.
    """
    if isinstance(mask, bool) or not isinstance(mask, (int, float)):
        return False, "exact_probe_attached_mask is not a number (%r)" % (mask,), ()
    if isinstance(mask, float):
        if mask != int(mask):
            return False, ("exact_probe_attached_mask is not an integer (%r)"
                           % (mask,)), ()
        mask = int(mask)
    if mask < 0:
        return False, "exact_probe_attached_mask is negative (%r)" % (mask,), ()
    if (mask & ATTR_MASK) != ATTR_MASK:
        return False, ("required attribution pair incomplete: mask=0x%x, "
                       "(mask & 0x%x)=0x%x" % (mask, ATTR_MASK,
                                               mask & ATTR_MASK)), ()
    unknown = mask & ~ALL_MASK
    if unknown:
        return False, ("unknown bundle bits set: mask=0x%x, outside 0x%x: "
                       "0x%x" % (mask, ALL_MASK, unknown)), ()
    phase = mask & PHASE_MASK
    surprise = phase & ~expected_phase
    if surprise:
        return False, ("phase bit(s) attached that capability resolution said "
                       "this platform/binary could not carry: %s (mask=0x%x, "
                       "derived upper bound=0x%x)"
                       % (", ".join(_phase_bit_names(surprise)), mask,
                          expected_phase)), ()
    degraded = _phase_bit_names(expected_phase & ~phase)
    return True, ("mask=0x%x, phase=0x%x (derived upper bound 0x%x)"
                 % (mask, phase, expected_phase)), degraded


def require_phase_probes(caller, pm_pid=None):
    try:
        resolve_phase_probes(pm_pid).require(caller)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"FAIL: cannot resolve phase capability: {exc}")
        raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--caller", default="phase-marker test")
    args = parser.parse_args()
    try:
        capability = resolve_phase_probes(args.pid)
        if args.json:
            print(json.dumps(asdict(capability)))
        else:
            capability.require(args.caller)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"FAIL: cannot resolve phase capability: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
