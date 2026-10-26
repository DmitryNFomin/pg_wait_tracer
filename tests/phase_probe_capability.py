"""Shared phase-marker test contract (#622).

Derive prerequisites independently of the daemon: kernel semaphore refcounts
AND the four postgresql SDT notes in the traced postmaster's executable.
PGWT_EXPECT_PHASE_PROBES=0|1 explicitly overrides that derivation. Passing
these prerequisites is an expectation, never proof of attachment or firing:
the execution-site allowlist, attach, mask and data assertions still gate it.
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
PHASE_NOTES = frozenset(("query__execute__start", "query__execute__done",
                         "query__plan__start", "query__plan__done"))
ATTR_MASK = 0x3
PHASE_MASK = 0xC
ALL_MASK = ATTR_MASK | PHASE_MASK
# Deliberately different from success, failure, and conventional skip (77).
CAPABILITY_REFUSAL = 78


@dataclass(frozen=True)
class PhaseCapability:
    expected: bool
    reason: str

    @property
    def phase_mask(self):
        return PHASE_MASK if self.expected else 0

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
        return PhaseCapability(raw.strip() == "1",
                               f"explicit override {EXPECT_PHASE_ENV}={raw.strip()}")

    # stat, rather than exists(): permission/inspection errors must fail,
    # never masquerade as evidence of an unavailable capability.
    try:
        REF_CTR_OFFSET.stat()
    except FileNotFoundError:
        return PhaseCapability(False, "not available on this kernel: "
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
    present = PHASE_NOTES & notes
    evidence = f"{REF_CTR_OFFSET} is present; {binary}"
    if not present:
        return PhaseCapability(False, f"not available in this PostgreSQL build: "
                               f"{evidence} has none of the four postgresql "
                               "execute/plan SDT notes")
    if present != PHASE_NOTES:
        # A partial bundle cannot honestly promise mask 0x3 OR 0xf.
        raise ValueError(f"incomplete phase SDT bundle: {evidence} lacks "
                         + ", ".join(sorted(PHASE_NOTES - present)))
    return PhaseCapability(True, f"{evidence} carries all four postgresql phase SDT notes")



def exact_window_mask_ok(mask, expected_phase):
    """The three-part contract on an OPEN exact window's
    exact_probe_attached_mask. Returns (ok, reason-for-the-check-line).

      1. the REQUIRED attribution pair is present   (mask & 0x3) == 0x3
      2. no bits outside the known bundle           (mask & ~0xf) == 0
      3. the phase markers match the resolved capability    (mask & 0xc) == expected

    (2) is why this is not written as a plain `mask & ATTR_MASK`: a new,
    unknown bundle member appearing in the JSON must still fail here rather
    than be silently accepted. An absent or unparseable field fails too -- a
    check that cannot read what it is checking refuses, it never approves.
    """
    if isinstance(mask, bool) or not isinstance(mask, (int, float)):
        return False, "exact_probe_attached_mask is not a number (%r)" % (mask,)
    if isinstance(mask, float):
        if mask != int(mask):
            return False, ("exact_probe_attached_mask is not an integer (%r)"
                           % (mask,))
        mask = int(mask)
    if mask < 0:
        return False, "exact_probe_attached_mask is negative (%r)" % (mask,)
    if (mask & ATTR_MASK) != ATTR_MASK:
        return False, ("required attribution pair incomplete: mask=0x%x, "
                       "(mask & 0x%x)=0x%x" % (mask, ATTR_MASK,
                                               mask & ATTR_MASK))
    unknown = mask & ~ALL_MASK
    if unknown:
        return False, ("unknown bundle bits set: mask=0x%x, outside 0x%x: "
                       "0x%x" % (mask, ALL_MASK, unknown))
    phase = mask & PHASE_MASK
    if phase != expected_phase:
        return False, ("phase markers 0x%x but resolved capability requires 0x%x (mask=0x%x)"
                       % (phase, expected_phase, mask))
    return True, "mask=0x%x, phase=0x%x as expected" % (mask, phase)

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
