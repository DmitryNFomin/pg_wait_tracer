"""testutil.py — Shared test helpers for pg_wait_tracer Python tests.

Usage:
    from testutil import find_postmaster
    pm_pid = find_postmaster()          # highest PG version available
    pm_pid = find_postmaster(pg_version=16)  # specific version
"""
import os
import re
import subprocess
import sys


def pg_wait_sampling_available():
    """Return True if the pg_wait_sampling extension is loaded and queryable.

    Tests that cross-check against pg_wait_sampling (Extension-class waits,
    sample-count cross-validation) must skip cleanly when it is absent —
    e.g. on a stock PostgreSQL or in CI. Note: a per-test psql() helper
    that returns "" on error cannot be used to detect this, since the
    failure is silent; we must inspect the process return code here.
    """
    r = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc",
         "SELECT 1 FROM pg_wait_sampling_profile LIMIT 1"],
        capture_output=True, text=True)
    return r.returncode == 0


def find_postmaster(pg_version=None):
    """Find the PostgreSQL postmaster PID.

    If pg_version is specified, find that exact version.
    Otherwise find the highest available version.

    A postmaster is a postgres process whose parent is NOT also postgres.

    Returns PID (int) or None.
    """
    result = subprocess.run(["pgrep", "-x", "postgres"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return None

    best_pid = None
    best_ver = 0

    for pid_str in result.stdout.strip().split('\n'):
        if not pid_str:
            continue
        pid = int(pid_str)

        # Skip children — a postmaster's parent is not postgres
        try:
            ppid = None
            with open(f"/proc/{pid}/status") as f:
                for line in f:
                    if line.startswith("PPid:"):
                        ppid = int(line.split()[1])
                        break
            if ppid is None:
                continue
            parent_comm = open(f"/proc/{ppid}/comm").read().strip()
            if parent_comm == "postgres":
                continue
        except (FileNotFoundError, PermissionError, ValueError):
            continue

        # Get version from exe path: /usr/lib/postgresql/18/bin/postgres
        try:
            exe = os.readlink(f"/proc/{pid}/exe")
        except OSError:
            continue

        m = re.search(r'postgresql/(\d+)/', exe)
        if m:
            ver = int(m.group(1))
        else:
            # Fallback: run postgres --version
            try:
                out = subprocess.run([exe, "--version"],
                                     capture_output=True, text=True, timeout=5)
                m2 = re.search(r'PostgreSQL\)?\s+(\d+)', out.stdout)
                ver = int(m2.group(1)) if m2 else 0
            except Exception:
                continue

        if pg_version is not None:
            if ver == pg_version:
                return pid
        else:
            if ver > best_ver:
                best_ver = ver
                best_pid = pid

    return best_pid


# exact_probe_attached_mask bundle bits, mirroring src/exact_probe.h.
#   ATTR_MASK  = QUERY_ID|ACTIVITY, the REQUIRED attribution pair: a window
#                missing either one hands out half-exact answers.
#   PHASE_MASK = EXEC|PLAN, #108's OPTIONAL execute/plan USDT phase markers,
#                attached best effort inside pgwt_exact_probe_core_acquire()
#                AFTER the window is granted (src/exact_probe.c).
ATTR_MASK = 0x3
PHASE_MASK = 0xC
ALL_MASK = ATTR_MASK | PHASE_MASK

# #591: this file predates #108 and asserted `mask == ATTR_MASK`, which went
# deterministically red everywhere the phase markers attach (0xf). Widening it
# to "required bits present" alone is not enough -- the interesting half of
# #108 is then unasserted on a live kernel, and the check can no longer tell a
# window that got both markers from one that got neither.
#
# The expectation cannot be derived from the product:
#   * from the mask itself -- circular; the assertion could not fail.
#   * from the daemon -- src/exact_probe.c discards the optional attach's
#     result (`(void)attach_missing(...)`), so the bit IS the only state.
#   * from the kernel's /sys/.../uprobe/format/ref_ctr_offset -- that says the
#     kernel supports USDT semaphore refcounting, not that THESE probes
#     attach, which also needs the SDT notes in the postgres binary, the #464
#     execution-site allowlist, and neither --skip-usdt nor --lightweight. It
#     would redden a legitimate capable-kernel/no-notes configuration.
#   * from /etc/os-release -- a guess about a kernel capability.
# So whatever PROVISIONED the tier declares it, via the environment, and an
# UNDECLARED tier is refused rather than defaulted: a default is exactly how
# this assertion would go quietly vacuous again the next time a tier is added.
EXPECT_PHASE_ENV = "PGWT_EXPECT_PHASE_PROBES"


def resolve_expected_phase_mask(raw):
    """Map $PGWT_EXPECT_PHASE_PROBES to the phase bits an open exact window
    must carry: PHASE_MASK for "1", 0 for "0".

    Raises ValueError for everything else -- unset (None), empty, or
    unparseable -- so a tier that never declared an expectation fails loudly
    instead of being approved by a silent default.
    """
    if raw is None:
        raise ValueError(
            "%s is not set. This tier must DECLARE whether #108's "
            "execute/plan USDT phase markers are expected to attach in an "
            "exact window: 1 = must attach (kernel with USDT semaphore "
            "refcounting, postgres built with the SDT notes), 0 = must not "
            "(e.g. EL8). There is deliberately no default -- one would make "
            "the phase-bit assertion vacuous. Declare it next to the PG "
            "version in the runner that invokes this test." % EXPECT_PHASE_ENV)
    value = raw.strip()
    if value == "1":
        return PHASE_MASK
    if value == "0":
        return 0
    raise ValueError(
        "%s=%r is neither '0' nor '1'; refusing rather than guessing which "
        "phase-marker contract this tier has." % (EXPECT_PHASE_ENV, raw))


def exact_window_mask_ok(mask, expected_phase):
    """The three-part contract on an OPEN exact window's
    exact_probe_attached_mask. Returns (ok, reason-for-the-check-line).

      1. the REQUIRED attribution pair is present   (mask & 0x3) == 0x3
      2. no bits outside the known bundle           (mask & ~0xf) == 0
      3. the phase markers match the declaration    (mask & 0xc) == expected

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
        return False, ("phase markers 0x%x but %s requires 0x%x (mask=0x%x)"
                       % (phase, EXPECT_PHASE_ENV, expected_phase, mask))
    return True, "mask=0x%x, phase=0x%x as declared" % (mask, phase)


# #622: a refusal is an executed negative check, never a silent skip.
PHASE_REFUSED_EXIT = 78
PHASE_REFUSED_MARKER = "PHASE-CONTRACT-REFUSED"
PHASE_DEPENDENT_TESTS = ("test_execution_cardinality", "test_execution_span_live",
                         "test_escalation", "test_ui_live_smoke")


def resolve_declared_phase_mask(env=None):
    """Resolve the provisioner's mandatory declaration, without a default."""
    if env is None:
        env = os.environ
    return resolve_expected_phase_mask(env.get(EXPECT_PHASE_ENV))


def phase_contract_verdict(expected_phase, observed_masks, artifacts):
    """PURE. Check the declared phase markers against an OPEN window's masks
    and measured marker-derived artifacts. Union the masks because the daemon
    assembles the bundle one bit at a time (#622); a torn read must not decide
    the verdict, but any phase bit ever seen contradicts a declaration of 0.
    None is an explicit exact-data refusal (UNAVAIL), and counts as zero.
    """
    if expected_phase not in (0, PHASE_MASK):
        return "fail", "invalid expected phase mask: %r" % (expected_phase,)
    if not isinstance(observed_masks, (list, tuple)) or not observed_masks:
        return "fail", "no mid-window mask was observed (need a non-empty list/tuple)"
    union = 0
    for mask in observed_masks:
        # Only type/readability is checked per sample: required/phase bits
        # can still be assembling. The union gets the complete bundle check.
        try:
            ok, reason = exact_window_mask_ok(mask, expected_phase)
            readable = (not isinstance(mask, bool)
                        and isinstance(mask, (int, float))
                        and mask >= 0 and mask == int(mask))
        except (ValueError, OverflowError):
            readable = False
            reason = "exact_probe_attached_mask is unreadable (%r)" % (mask,)
        if not readable:
            return "fail", reason
        union |= int(mask)
    ok, reason = exact_window_mask_ok(union, expected_phase)
    if not ok:
        return "fail", reason
    if not isinstance(artifacts, dict) or not artifacts:
        return "fail", "no marker-derived artifact was counted"
    for name, count in artifacts.items():
        if count is not None and (isinstance(count, bool)
                                  or not isinstance(count, int) or count < 0):
            return "fail", "%s has unreadable marker-derived count %r" % (name, count)
    for name, count in artifacts.items():
        if expected_phase == PHASE_MASK and (count is None or count == 0):
            return "fail", ("%s is 0/UNAVAIL while %s=1 declares the phase "
                            "markers attach here" % (name, EXPECT_PHASE_ENV))
        if expected_phase == 0 and count is not None and count > 0:
            return "fail", ("%s=%s while %s=0 declares the phase markers cannot "
                            "attach here" % (name, count, EXPECT_PHASE_ENV))
    if expected_phase == PHASE_MASK:
        return "pass", "%s=1: %s; %s are present" % (EXPECT_PHASE_ENV, reason, artifacts)
    return "refuse", ("%s=0 declares no phase markers; mask union 0x3 carries "
                      "the attribution pair and no phase markers; %s are absent "
                      "as declared" % (EXPECT_PHASE_ENV, artifacts))


def phase_contract_apply(expected_phase, observed_masks, artifacts, label,
                         fail=None):
    """Print the shared verdict; refusal exits 78, failure calls fail or exits 1."""
    verdict, reason = phase_contract_verdict(expected_phase, observed_masks, artifacts)
    print("  %s: %s: %s" % (verdict.upper(), label, reason))
    if verdict == "refuse":
        print("%s: %s" % (PHASE_REFUSED_MARKER, reason))
        raise SystemExit(PHASE_REFUSED_EXIT)
    if verdict == "fail":
        if fail is not None:
            fail(reason)
        else:
            raise SystemExit(1)
    return verdict


def _phase_cli():
    if sys.argv[1:] != ["phase-expectation"]:
        print("Usage: python3 tests/testutil.py phase-expectation", file=sys.stderr)
        return 2
    try:
        expected = resolve_declared_phase_mask()
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print("1" if expected == PHASE_MASK else "0")
    return 0


if __name__ == "__main__":
    sys.exit(_phase_cli())
