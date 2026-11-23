#!/usr/bin/env python3
"""test_timeline_assemble_diff.py -- C assembler vs the Python rule (#1006 S1).

S1 makes this the differential: the C assembler (driven through the
test_timeline_assemble binary) and tests/timeline_reconstruct_ref.py
(reconstruct_mark, verbatim from the kill test) must produce identical wait
lists on tests/fixtures/timeline/*.raw, fed in ring (unsorted) order.

S0 placeholder, skip-free: it pins the Python view of the wire records the
differential will read (struct formats) against the _Static_assert sizes in
src/timeline_wire.h, and runs the assembler binary it was pointed at.
Registered in scripts/check.sh, which passes --assembler-bin.

Usage: test_timeline_assemble_diff.py [--assembler-bin PATH]
       (default: test_timeline_assemble next to this file)
"""
import argparse
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Little-endian layouts of struct pgwt_tl_wire_* (header: size, version,
# kind, flags, reserved, pid, tid).
WIRE_HDR = "<HBBHHII"
WIRE = {
    "hdr": struct.Struct(WIRE_HDR),
    "call": struct.Struct(WIRE_HDR + "QQQQQIIII"),
    "mark": struct.Struct(WIRE_HDR + "QQQII"),
    "seed": struct.Struct(WIRE_HDR + "QQQII"),
    "exit": struct.Struct(WIRE_HDR + "Q"),
}

fails = 0


def check(cond, what):
    global fails
    if cond:
        print("  ok: %s" % what)
    else:
        fails += 1
        print("  FAIL: %s" % what)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--assembler-bin", default=os.path.join(HERE, "test_timeline_assemble"))
    args = ap.parse_args()

    with open(os.path.join(ROOT, "src", "timeline_wire.h")) as f:
        text = f.read()
    sizes = dict(re.findall(r"sizeof\(struct pgwt_tl_wire_(\w+)\) == (\d+)", text))
    check(set(sizes) == set(WIRE), "header asserts a size for every wire struct: %s" % sorted(sizes))
    for name, st in WIRE.items():
        check(int(sizes.get(name, -1)) == st.size,
              "wire %s: python %d bytes == C %s" % (name, st.size, sizes.get(name)))

    if not os.access(args.assembler_bin, os.X_OK):
        check(False, "assembler binary is executable: %s" % args.assembler_bin)
    else:
        rc = subprocess.run([args.assembler_bin], stdout=subprocess.DEVNULL).returncode
        check(rc == 0, "assembler binary %s exits 0 (got %d)" % (args.assembler_bin, rc))

    if fails:
        print("test_timeline_assemble_diff: %d check(s) failed" % fails)
        return 1
    print("test_timeline_assemble_diff: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
