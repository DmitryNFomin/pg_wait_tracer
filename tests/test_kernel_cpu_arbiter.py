#!/usr/bin/env python3
"""test_kernel_cpu_arbiter.py — regression tests for two issue #115 tooling
bugs in kernel_cpu_arbiter.py, both now required to fail loudly:

1. "bpftrace reprints every populated map at exit": `delays` parses the
   SDELAY_US / CDELAY_US / CBURST_US histograms a sched*.bt(.tmpl) script
   prints in its END block. Unless every one of those maps is also clear()d
   there, bpftrace's own default end-of-program dump reprints each map's
   buckets a second time, right after our explicit print() of the same
   histogram -- silently doubling every count and skewing every percentile
   computed from them (this is exactly how the pg_wait_sampling
   wakeup-delay numbers were first measured wrong; see the issue #115
   comment thread and docs/OBSERVER_BIAS.md).

2. "kernel.sched_schedstats=1 is a precondition, not a default": on a fresh
   machine /proc/<pid>/schedstat's run_delay field reads 0 until this sysctl
   is set, which would otherwise silently zero out the entire run-queue-delay
   side of every comparison. `sample` must refuse to run rather than collect
   zeros.

This test does not need bpftrace, the kernel, or Linux: it feeds `cmd_delays`
synthetic text shaped like bpftrace's own output, and monkeypatches
`cmd_sample`'s one sysctl read, so it runs anywhere including this Mac.

Runs standalone: python3 tests/test_kernel_cpu_arbiter.py
Also wired into tests/unit_tests.list for the Linux/box-check tier.
"""
import argparse
import builtins
import importlib.util
import io
import os
import sys
import tempfile
import contextlib

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "kernel_cpu_arbiter", os.path.join(HERE, "kernel_cpu_arbiter.py"))
kca = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kca)

CLEAN = """\
SDELAY_US (sampler wakeup -> on cpu)
@sdelay:
[8, 16)                3 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[16, 32)                1 |@@@@@@@@@@@@@@@@                                |

CDELAY_US (collector wakeup -> on cpu)
@cdelay:
[8, 16)                5 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
"""

# Shaped exactly like a real bpftrace run whose END block prints @sdelay via
# printf()+print() but never clear()s it: bpftrace's own default dump of
# every still-populated map follows immediately, with no "SDELAY_US" header
# of ours in front of it -- just its bare "@sdelay:" banner -- and the SAME
# buckets again.
DOUBLED = """\
SDELAY_US (sampler wakeup -> on cpu)
@sdelay:
[8, 16)                3 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[16, 32)                1 |@@@@@@@@@@@@@@@@                                |

@sdelay:
[8, 16)                3 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[16, 32)                1 |@@@@@@@@@@@@@@@@                                |
"""

# The other shape the same bug can take if bpftrace happens to interleave
# without an intervening blank line at all (still caught: 'seen' rejects the
# same textual header appearing twice).
DOUBLED_HEADER_TWICE = CLEAN + CLEAN


def _run(text, which="SDELAY_US,CDELAY_US,CBURST_US"):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(text)
        path = f.name
    try:
        args = argparse.Namespace(bt=path, which=which)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = kca.cmd_delays(args)
        return rc, out.getvalue()
    finally:
        os.unlink(path)


def test_clean_dump_parses_once():
    rc, out = _run(CLEAN)
    assert rc == 0, f"expected success, got rc={rc}"
    assert "SDELAY_US n=4 " in out, out
    assert "CDELAY_US n=5 " in out, out
    print("PASS: clean single dump -> n=4/n=5, not doubled")


def test_doubled_dump_fails_loud():
    for name, text in (("bare-redump", DOUBLED),
                        ("header-twice", DOUBLED_HEADER_TWICE)):
        try:
            _run(text)
        except SystemExit as e:
            msg = str(e)
            assert "doubl" in msg.lower() or "second time" in msg.lower() or \
                "printed twice" in msg.lower() or "appeared while" in msg.lower(), msg
            print(f"PASS: {name} -> loud failure: {msg.splitlines()[0]!r}")
            continue
        raise AssertionError(
            f"{name}: doubled bpftrace dump was silently accepted "
            "instead of failing loudly (regression of issue #115's "
            "'bpftrace reprints every populated map at exit' bug)")


def test_sample_refuses_when_schedstats_disabled():
    """cmd_sample must not silently collect an all-zero run_delay column on a
    fresh machine that never had `sysctl -w kernel.sched_schedstats=1` run.
    Monkeypatches only the one sysctl-file read cmd_sample does before
    touching anything else (postmaster, output file, /proc/<pid>/*), so this
    needs no real /proc/sys/kernel/sched_schedstats file."""
    real_open = builtins.open

    def fake_open(path, *a, **kw):
        if path == "/proc/sys/kernel/sched_schedstats":
            return io.StringIO("0\n")
        return real_open(path, *a, **kw)

    args = argparse.Namespace(hz=20, duration=1, out="/should/not/be/opened",
                               postmaster=1, extra_pid=[])
    kca.open = fake_open
    try:
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = kca.cmd_sample(args)
    finally:
        kca.open = real_open
    assert rc == 2, f"expected rc=2 (refuse), got {rc!r}"
    assert "sched_schedstats" in err.getvalue(), err.getvalue()
    print("PASS: cmd_sample refuses (rc=2) when kernel.sched_schedstats=0, "
          f"instead of silently sampling zeros: {err.getvalue().strip()!r}")


def main():
    test_clean_dump_parses_once()
    test_doubled_dump_fails_loud()
    test_sample_refuses_when_schedstats_disabled()
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
