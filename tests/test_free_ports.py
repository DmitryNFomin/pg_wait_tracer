#!/usr/bin/env python3
"""test_free_ports.py -- unit tests for tests/free_ports.py (issue: "make
check: allocate free ports per run so two agents can run it concurrently on
one Mac"). Pure Python + stdlib socket; no Playwright, no network beyond
localhost binds -- runs on the Mac as part of `make check`.

Usage: python3 tests/test_free_ports.py
"""
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import free_ports as fp

tests_run = 0
tests_passed = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


def test_is_free_detects_busy_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    s.listen(1)
    busy_port = s.getsockname()[1]
    try:
        check(not fp.is_free(busy_port),
              f"is_free() reports the bound port {busy_port} as busy")
    finally:
        s.close()
    # Released immediately after close (SO_REUSEADDR means no lingering
    # TIME_WAIT bind failure for a listen-only socket like this one).
    check(fp.is_free(busy_port),
          f"is_free() reports {busy_port} free again once released")


def test_find_free_base_returns_free_span():
    span = 10
    base = fp.find_free_base(span, lo=fp.LO, hi=fp.HI)
    check(fp.LO <= base < fp.HI - span,
          f"base {base} is inside [{fp.LO}, {fp.HI - span})")
    all_free = all(fp.is_free(base + i) for i in range(span))
    check(all_free, f"base {base}..{base + span - 1} are all still free")


def test_find_free_base_avoids_a_busy_port():
    # Occupy one port inside the candidate range and confirm find_free_base
    # never returns a base whose span would include it. Use a range well
    # outside free_ports.py's own default [LO, HI) allocation window (and
    # below macOS's ephemeral port range, 49152+) so this fixture's bind
    # can never collide with find_free_base()'s own default-range probing
    # elsewhere in this file, nor with an OS-assigned ephemeral port.
    lo, hi = 41000, 41100
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", lo + 5))
    s.listen(1)
    try:
        for _ in range(20):
            base = fp.find_free_base(8, lo=lo, hi=hi)
            check(not (base <= lo + 5 < base + 8),
                  f"base {base} (span 8) skips the busy port {lo + 5}")
    finally:
        s.close()


def test_find_free_base_rejects_span_too_large_for_range():
    threw = False
    try:
        fp.find_free_base(100, lo=100, hi=150)
    except ValueError:
        threw = True
    check(threw, "find_free_base() raises ValueError when span >= (hi - lo)")


def test_find_free_base_rejects_non_positive_span():
    for bad_span in (0, -1):
        threw = False
        try:
            fp.find_free_base(bad_span, lo=100, hi=200)
        except ValueError:
            threw = True
        check(threw, f"find_free_base(span={bad_span}) raises ValueError")


def test_find_free_base_raises_after_exhausting_tries():
    # hi - lo == span + 1, so randrange(lo, hi - span) can only ever return
    # `lo` -- occupy that one candidate base and every try must fail the
    # same way, so this deterministically exercises the "no lock, no
    # infinite retry" RuntimeError path check.sh's fail-loud step depends on.
    lo = 41200
    span = 2
    hi = lo + span + 1
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", lo))
    s.listen(1)
    try:
        threw = False
        try:
            fp.find_free_base(span, lo=lo, hi=hi, tries=3)
        except RuntimeError:
            threw = True
        check(threw,
              "find_free_base() raises RuntimeError once tries are exhausted "
              "against a busy-only range")
    finally:
        s.close()


def test_cli_prints_an_integer_in_range():
    import subprocess
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "free_ports.py")
    out = subprocess.run([sys.executable, script, "5"],
                          capture_output=True, text=True, check=True)
    base = int(out.stdout.strip())
    check(fp.LO <= base < fp.HI, f"CLI printed a base in range: {base}")


def main():
    test_is_free_detects_busy_port()
    test_find_free_base_returns_free_span()
    test_find_free_base_avoids_a_busy_port()
    test_find_free_base_rejects_span_too_large_for_range()
    test_find_free_base_rejects_non_positive_span()
    test_find_free_base_raises_after_exhausting_tries()
    test_cli_prints_an_integer_in_range()

    print(f"\n{tests_passed}/{tests_run} passed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
