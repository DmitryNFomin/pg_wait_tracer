#!/usr/bin/env python3
"""test_server_alloc_tune.py -- pgwt-server actually applies the allocator
tuning, and the off switch actually switches it off.

tests/test_alloc_tune.c proves the property of src/alloc_tune.h. It cannot
prove that main() calls it: server.c has a main(), so no unit test links it.
That is the "the thing being checked is absent rather than wrong" case, and
it is the one that matters here -- a header nobody calls is indistinguishable
from no change at all, except in the 15-35% of response time it does not save.

So this walks the real binary: PGWT_ALLOC_TUNE_DEBUG=1 makes main() report the
bitmask mallopt() accepted, and the two environments must disagree. Both
halves are required: a constant line would satisfy the first check alone.

Every way this check could fail to see is a refusal, not a pass:
  - the binary is missing, not executable (126), or not found (127)
  - it prints nothing, or no alloc_tune line
  - the bitmask is unparseable
  - the libc is not one whose expected bitmask we know
and the refusal logic itself is exercised against stub binaries, because an
unreachable refusal is the same false negative in a different place.
"""
import os
import platform
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "..", "pgwt-server")
# Deliberately loose: the token is captured first and validated second, so a
# line that is present but malformed refuses as "unparseable" instead of
# silently matching the hex prefix of a nonsense value.
LINE_RE = re.compile(r"^alloc_tune: applied=(\S+)[ \t]*$", re.M)

failures = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        failures.append(msg)
        print("  FAIL: %s" % msg)


def refuse(msg):
    print("  REFUSE: %s" % msg)
    print("test_server_alloc_tune: cannot observe its own subject -- refusing")
    sys.exit(2)


def run_server(binary, env_extra, trace_dir):
    """Run <binary> --dump <trace_dir>. Returns (rc, stdout+stderr)."""
    env = dict(os.environ)
    env.pop("PGWT_ALLOC_TUNE", None)
    env.pop("PGWT_ALLOC_TUNE_DEBUG", None)
    env.update(env_extra)
    try:
        p = subprocess.run([binary, "--dump", trace_dir],
                           stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           env=env, timeout=60)
    except FileNotFoundError:
        return 127, ""
    except PermissionError:
        return 126, ""
    except subprocess.TimeoutExpired:
        return None, ""
    return p.returncode, p.stdout.decode("utf-8", "replace")


def applied_mask(binary, env_extra, trace_dir, what):
    """The bitmask <binary> reports, or a refusal. Never a default."""
    rc, out = run_server(binary, env_extra, trace_dir)
    if rc is None:
        refuse("%s: pgwt-server did not exit within 60 s; a stalled gate is "
               "not a passing gate" % what)
    if rc in (126, 127):
        refuse("%s: pgwt-server exited %d (not executable / not found) -- the "
               "tuning was never exercised" % (what, rc))
    if not out.strip():
        refuse("%s: pgwt-server produced no output at all" % what)
    m = LINE_RE.search(out)
    if not m:
        refuse("%s: no 'alloc_tune: applied=0x..' line in the output -- main() "
               "does not report the tuning, so this test cannot tell whether it "
               "applied it. Output was: %r" % (what, out[:400]))
    token = m.group(1)
    if not token.startswith("0x"):
        refuse("%s: bitmask %r is not in the documented 0x form" % (what, token))
    try:
        return int(token, 16)
    except ValueError:
        refuse("%s: unparseable bitmask %r" % (what, token))


def expected_full_mask():
    """0x7 on glibc; 0x0 where mallopt()/M_MMAP_MAX do not exist. Unknown libc
    is a refusal: guessing either way makes this test decorative."""
    if sys.platform == "darwin":
        return 0x0
    name, ver = platform.libc_ver()
    if name == "glibc":
        return 0x7
    if sys.platform == "linux":
        refuse("libc is %r (version %r), not glibc: the expected bitmask for "
               "this libc is unknown, so neither 0x7 nor 0x0 can be asserted"
               % (name, ver))
    refuse("unknown platform %r: no expected bitmask" % sys.platform)


def main():
    print("=== test_server_alloc_tune ===")

    for var in ("MALLOC_MMAP_MAX_", "MALLOC_TRIM_THRESHOLD_",
                "MALLOC_TOP_PAD_", "GLIBC_TUNABLES"):
        if os.environ.get(var):
            refuse("%s=%s is set: the allocator is already tuned from the "
                   "environment, so nothing observed here is attributable to "
                   "pgwt-server" % (var, os.environ[var]))

    if not os.path.exists(SERVER):
        refuse("%s does not exist -- run `make pgwt-server` first. A missing "
               "binary is a refusal, never a skip" % SERVER)

    full = expected_full_mask()

    with tempfile.TemporaryDirectory() as td:
        # 1. The real binary, default environment.
        got = applied_mask(SERVER, {"PGWT_ALLOC_TUNE_DEBUG": "1"}, td, "default env")
        check(got == full,
              "pgwt-server applied 0x%x, expected 0x%x: main() did not apply the "
              "full allocator tuning" % (got, full))

        # 2. The off switch must produce a DIFFERENT, empty mask. Checking only
        #    (1) would be satisfied by a hardcoded line.
        off = applied_mask(SERVER, {"PGWT_ALLOC_TUNE_DEBUG": "1",
                                    "PGWT_ALLOC_TUNE": "0"}, td, "PGWT_ALLOC_TUNE=0")
        check(off == 0x0,
              "PGWT_ALLOC_TUNE=0 still reported 0x%x: the off switch does not "
              "switch anything off" % off)
        if full != 0x0:
            check(off != got,
                  "PGWT_ALLOC_TUNE=0 and the default both reported 0x%x: the "
                  "reported mask is constant, so this test would pass against a "
                  "server that ignores the setting entirely" % got)

        # 3. The debug line is conditional: it must not appear in normal runs,
        #    where stderr belongs to the SSH transport.
        rc, out = run_server(SERVER, {}, td)
        if rc in (126, 127) or rc is None:
            refuse("quiet run: pgwt-server exited %r" % rc)
        check(LINE_RE.search(out) is None,
              "the alloc_tune line appeared without PGWT_ALLOC_TUNE_DEBUG: "
              "unconditional stderr noise on every server start")

        # 4. Bypass suite: the refusals above must actually be reachable.
        #    Each stub is a way the real check could have been blind.
        stubs = {
            "silent":      ("#!/bin/sh\nexit 0\n", 0o755),
            "no_line":     ("#!/bin/sh\necho 'summary: whatever'\n", 0o755),
            "unparseable": ("#!/bin/sh\necho 'alloc_tune: applied=0xbanana'\n", 0o755),
            "exit127":     ("#!/bin/sh\nexit 127\n", 0o755),
            "noexec":      ("#!/bin/sh\necho 'alloc_tune: applied=0x7'\n", 0o644),
        }
        for name, (body, mode) in stubs.items():
            path = os.path.join(td, "stub_" + name)
            with open(path, "w") as fh:
                fh.write(body)
            os.chmod(path, mode)
            # A refusal exits the process, so each stub runs in a child copy of
            # this test; the child must NOT come back 0.
            child = subprocess.run(
                [sys.executable, os.path.abspath(__file__), "--stub", path],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
            body_out = child.stdout.decode("utf-8", "replace")
            check(child.returncode != 0 and "REFUSE" in body_out,
                  "stub %r: the checker returned %d without refusing -- it would "
                  "approve a server it never observed. Output: %r"
                  % (name, child.returncode, body_out[-300:]))
            print("  stub %-12s -> rc=%d %s" % (
                name, child.returncode,
                "REFUSE" if "REFUSE" in body_out else "(no refusal!)"))

    print("\n=== %d/%d checks passed ===" % (checks - len(failures), checks))
    return 1 if failures else 0


def stub_mode(path):
    """Exercise the refusal path against one stub binary."""
    print("=== stub mode: %s ===" % os.path.basename(path))
    with tempfile.TemporaryDirectory() as td:
        got = applied_mask(path, {"PGWT_ALLOC_TUNE_DEBUG": "1"}, td, "stub")
    # Reaching here means no refusal fired. For every stub in the suite the
    # mask is either absent or a lie, so this is itself a failure.
    print("  FAIL: stub produced mask 0x%x without any refusal" % got)
    return 1


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--stub":
        sys.exit(stub_mode(sys.argv[2]))
    sys.exit(main())
