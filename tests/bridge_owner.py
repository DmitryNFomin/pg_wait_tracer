#!/usr/bin/env python3
"""Fail closed on foreign/uninspectable TCP listeners. No process is killed here.

No expected PID: require an unused port (preflight only, not a reservation).
Expected PID: require every listener on that port to belong to exactly it.
Exit 1 means not listening yet; 2 means refuse, including inspection failures.
Linux uses ss; lsof supports the same real-socket regression tests on macOS.
"""
import re
import subprocess
import sys


def listeners(port):
    if sys.platform == "linux":
        result = subprocess.run(["ss", "-H", "-ltnp", f"sport = :{port}"],
                                capture_output=True, text=True, timeout=2)
        if result.returncode or result.stderr:
            raise RuntimeError(result.stderr or f"ss exited {result.returncode}")
        owners = []
        for line in result.stdout.splitlines():
            pids = set(map(int, re.findall(r"pid=(\d+)", line)))
            if not pids:
                raise RuntimeError(f"listener ownership unavailable: {line}")
            owners.append(pids)
        return owners
    result = subprocess.run(["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN", "-Fp"],
                            capture_output=True, text=True, timeout=2)
    if result.stderr or result.returncode not in (0, 1):
        raise RuntimeError(result.stderr or f"lsof exited {result.returncode}")
    if result.stdout and not re.fullmatch(r"(?:[pf]\d+\n)+", result.stdout):
        raise RuntimeError(f"unparseable lsof output: {result.stdout!r}")
    pids = re.findall(r"^p(\d+)$", result.stdout, re.M)
    if result.stdout and not pids:
        raise RuntimeError("lsof returned listeners without owning PIDs")
    return [{int(p)} for p in pids]


def main():
    port = int(sys.argv[1])
    expected = int(sys.argv[2]) if len(sys.argv) > 2 else None
    if not 0 < port < 65536:
        raise ValueError(f"invalid bridge port {port}")
    owners = listeners(port)
    if not owners:
        return 1 if expected else 0
    if expected and all(pids == {expected} for pids in owners):
        return 0
    print(f"REFUSE bridge port {port}: occupied by listener PID(s) "
          f"{sorted(set.union(*owners))}; expected "
          f"{expected if expected else 'an unused port'}. No foreign process was killed.",
          file=sys.stderr, flush=True)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"REFUSE bridge ownership: cannot verify: {exc}", file=sys.stderr)
        sys.exit(2)
