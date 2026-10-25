#!/usr/bin/env python3
"""Fail closed on foreign/uninspectable TCP listeners. No process is killed here.

No expected PID: require an unused port (preflight only, not a reservation).
Expected PID: require every listener on that port to belong to exactly it.
Linux uses ss; lsof supports the same real-socket regression tests on macOS.

Exit codes:
  0  the port is unused (no expected PID), or every listener on it is ours
  1  nothing is listening yet and an owner was expected -- the caller may retry
  2  REFUSE: a listener exists and is NOT ours, or the inspection tool itself
     failed (missing ss, non-zero ss, unparseable lsof)
  3  REFUSE: a listener demonstrably exists and NOTHING on this host can name
     its owner. Distinct from 2 on purpose: 2 says "that port is somebody
     else's", 3 says "this host cannot be inspected". The operator actions
     differ (find the other process vs fix the host), and conflating them is
     how #581 intermittently disabled the whole live-UI tier -- the 21:42
     ephemeral run refused while the 23:16 run on the SAME image passed.

Why the /proc fallback exists, measured (ubuntu 24.04, iproute2 6.1.0):
`ss -p` resolves a socket to a process by scanning /proc/*/fd for a
`socket:[<inode>]` symlink. It does that scan FIRST and only then opens
NETLINK_SOCK_DIAG to ask the kernel which sockets exist -- under strace, the
first openat("/proc/1/fd/") precedes the socket(AF_NETLINK, .., SOCK_DIAG) by
251.5 ms. So ss joins a STALE owner map against a FRESH socket list, and any
socket that entered LISTEN after the scan is dumped with no `pid=`. That is
exactly wait_bridge_ready's first poll, fired milliseconds after run_bridge.
Measured by sweeping when the listener binds, 5 trials per step: refused as
unverifiable in 5/5 at bind=+80ms and 3/5 at +70ms. It is a RACE, not an
image: a settled listener on the same image attributes fine.

This is NOT "the /proc link has not landed yet" -- that cannot happen. The fd
must already exist for listen() to be called on it, so /proc can never lag the
kernel's LISTEN table: across 1232 samples that saw the LISTEN row, the
`socket:[inode]` link was present in 1232 of them (0 absent).

Which is why reading in the OPPOSITE order is race-free rather than lucky:
listening_inodes() reads the inode table first, inode_owners() scans fds
second, so any socket in the table necessarily had its fd in place before the
scan began. Same evidence as ss, by a route whose ordering cannot lose. On the
sweep above the fallback was unverifiable in 0/45 trials against ss's 11/45.

A bounded retry on the inspection would also have worked -- the bare line does
not persist, the first ss issued after listen() attributes (25/25, 23.7-40.0 ms,
i.e. process spawn time) -- and it is deliberately NOT used: it is
probabilistic where this is structural, it would slow down every genuine #544
refusal, and when the retries ran out it would still face the same undecided
question. Re-reading a racy table until it is readable is legitimate; this
reads a table that was never racy.

What it must never do: report "no owner" as "no listener". An unattributable
listener raises, so main() can never fall through to the exit-0 "unused port"
branch over a live foreign process (#544).
"""
import os
import re
import subprocess
import sys

INSPECTION_IMPOSSIBLE = 3
PROC = "/proc"
TCP_LISTEN = "0A"  # /proc/net/tcp "st" column


class Unattributable(Exception):
    """A listener exists and no route on this host can name its owner.

    Deliberately NOT an OSError/RuntimeError subclass: the top-level handler
    must be able to tell this apart from a tool failure.
    """


def listening_inodes(port):
    """Socket inodes of LISTEN sockets on `port`, read from /proc/net/tcp*.

    A missing file means that address family is off (IPv6 disabled), which is
    fine. A file that exists but cannot be read, or a row that cannot be
    parsed, is an inspection failure and raises -- an unreadable family could
    be hiding the very listener we are being asked about.
    """
    inodes = set()
    read_any = False
    for name in ("net/tcp", "net/tcp6"):
        path = os.path.join(PROC, name)
        try:
            with open(path) as handle:
                rows = handle.read().splitlines()
        except FileNotFoundError:
            continue
        except OSError as exc:
            raise Unattributable(f"{path} exists but cannot be read: {exc}")
        read_any = True
        for row in rows[1:]:
            if not row.strip():
                continue
            fields = row.split()
            if len(fields) < 10:
                raise Unattributable(f"unparseable {path} row: {row!r}")
            if fields[3] != TCP_LISTEN:
                continue
            try:
                local_port = int(fields[1].rpartition(":")[2], 16)
                inode = int(fields[9])
            except ValueError:
                raise Unattributable(f"unparseable {path} row: {row!r}")
            if local_port == port:
                inodes.add(inode)
    if not read_any:
        raise Unattributable(f"no {PROC}/net/tcp or {PROC}/net/tcp6 to read")
    return inodes


def inode_owners(inodes):
    """PIDs holding `inodes` as open fds. Raises unless EVERY inode is placed.

    Returning the union over all of this port's LISTEN inodes can only widen
    the owner set, so it can only cause a refusal, never an approval: if the
    union equals the expected PID then every holder of every inode is the
    expected PID.
    """
    if not inodes:
        raise Unattributable("no socket inode to attribute")
    holders = {inode: set() for inode in inodes}
    wanted = {f"socket:[{inode}]": inode for inode in inodes}
    scanned = blocked = 0
    for entry in os.listdir(PROC):
        if not entry.isdigit():
            continue
        fds = os.path.join(PROC, entry, "fd")
        try:
            names = os.listdir(fds)
        except FileNotFoundError:
            continue  # exited mid-scan; a vanished process holds no listener
        except OSError:
            blocked += 1  # unreadable: this process COULD be the owner
            continue
        scanned += 1
        for name in names:
            try:
                target = os.readlink(os.path.join(fds, name))
            except OSError:
                continue
            if target in wanted:
                holders[wanted[target]].add(int(entry))
    missing = sorted(inode for inode, pids in holders.items() if not pids)
    if missing:
        raise Unattributable(
            f"socket inode(s) {missing} are listening but no process on this "
            f"host owns them ({scanned} readable /proc pid dir(s), {blocked} "
            f"unreadable)")
    return set().union(*holders.values())


def proc_attribution(port, line):
    """Attribute a listener ss could not, via /proc/net/tcp* + /proc/*/fd."""
    if sys.platform != "linux":
        raise Unattributable(f"no /proc attribution on {sys.platform}: {line}")
    inodes = listening_inodes(port)
    if not inodes:
        raise Unattributable(
            f"ss reported a listener on port {port} with no pid= and no LISTEN "
            f"row for that port exists in {PROC}/net/tcp*: {line}")
    pids = inode_owners(inodes)
    print(f"NOTE bridge ownership: ss gave no pid= for {line.strip()!r}; "
          f"{PROC} attributed inode(s) {sorted(inodes)} to PID(s) "
          f"{sorted(pids)}", file=sys.stderr, flush=True)
    return pids


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
                pids = proc_attribution(port, line)
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
        raise Unattributable("lsof returned listeners without owning PIDs")
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


def cli():
    try:
        return main()
    except Unattributable as exc:
        print(f"REFUSE bridge ownership: listener inspection impossible on "
              f"this host: {exc}", file=sys.stderr)
        return INSPECTION_IMPOSSIBLE
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"REFUSE bridge ownership: cannot verify: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(cli())
