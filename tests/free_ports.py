#!/usr/bin/env python3
"""free_ports.py -- allocate a free base port for a Mac-tier test run.

Two agents can run `make check` (or `make ui-gallery`) at the same time in
different git worktrees on the same Mac. The Playwright suites behind those
targets (and the mock_server.py instances they drive) used to bind FIXED
ports, so the second run lost the race with "OSError: Address already in
use". Owner's direction (issue: "make check: allocate free ports per run so
two agents can run it concurrently on one Mac"): do NOT serialize with a
lock -- that would queue two ~4-minute runs one after another. Make the runs
port-independent instead.

This helper probes random bases in a high, rarely-used range and verifies
`base .. base+n-1` are ALL bindable on 127.0.0.1 right now before printing
`base`. Callers lay out every port a run needs at FIXED OFFSETS from that one
base (see the PORT MAP comments in scripts/check.sh and tests/ui_gallery.sh),
so a single allocation covers a whole run.

There is deliberately no lock and no retry-on-bind-failure inside the mock
servers themselves: if a suite's mock can't bind its assigned port anyway
(e.g. a genuine two-in-a-billion collision, or a stale leftover process), its
`start_mock_server` fails loudly with the port number in the message -- see
test_web_ui.py, test_web_ui_chaos.py, test_web_ui_snapshots.py,
ui_live_smoke.py. The allocation done here is what is supposed to prevent
that in the first place.

Usage:
  python3 tests/free_ports.py [N]     # print a free base for a span of N
                                       # consecutive ports (default 60)

As a library:
  from free_ports import find_free_base
  base = find_free_base(60)
"""
import random
import socket
import sys

LO = 20000
HI = 40000
DEFAULT_SPAN = 60
MAX_TRIES = 500


def is_free(port, host="127.0.0.1"):
    """True if `port` can be bound (and released) right now on `host`.

    Uses SO_REUSEADDR, matching how mock_server.py's own HTTP/WS listeners
    bind, so the probe reflects what the real caller will experience.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind((host, port))
    except OSError:
        return False
    finally:
        s.close()
    return True


def find_free_base(span=DEFAULT_SPAN, lo=LO, hi=HI, tries=MAX_TRIES):
    """Return a base such that base .. base+span-1 are all free right now.

    Random probing (not a sequential scan from `lo`) so two concurrent
    callers on the same Mac land on different bases almost always; a genuine
    collision is still possible (this is allocation, not a lock) and is left
    to the caller's own loud bind-failure message.
    """
    if span <= 0:
        raise ValueError(f"span must be positive, got {span}")
    if hi - span <= lo:
        raise ValueError(f"range [{lo}, {hi}) too small for span {span}")
    for _ in range(tries):
        base = random.randrange(lo, hi - span)
        if all(is_free(base + i) for i in range(span)):
            return base
    raise RuntimeError(
        f"could not find {span} consecutive free ports in [{lo}, {hi}) "
        f"after {tries} tries")


def main(argv):
    span = int(argv[1]) if len(argv) > 1 else DEFAULT_SPAN
    try:
        print(find_free_base(span))
    except (ValueError, RuntimeError) as exc:
        print(f"free_ports: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
