#!/usr/bin/env python3
"""The selected libbpf must declare AND export every position API (no BPF)."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APIS = {
    "ring_buffer__ring": "struct ring *ring_buffer__ring(struct ring_buffer *r, unsigned int i)",
    "ring__producer_pos": "unsigned long ring__producer_pos(const struct ring *r)",
    "ring__consumer_pos": "unsigned long ring__consumer_pos(const struct ring *r)",
}


def main():
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="pgwt-libbpf-positions-") as tmp:
        tmp = Path(tmp)
        (tmp / "bpf").mkdir()
        for missing in [None, *APIS]:
            for surface in ["header", "library"]:
                header = "struct ring; struct ring_buffer;\n"
                code = "struct ring; struct ring_buffer;\n"
                for api, signature in APIS.items():
                    if missing != api or surface != "header":
                        header += signature + ";\n"
                    if missing != api or surface != "library":
                        code += signature + " { (void)r; " + (
                            "(void)i; return (struct ring *)0; }\n" if api == "ring_buffer__ring"
                            else "return 0; }\n")
                (tmp / "bpf/libbpf.h").write_text(header)
                (tmp / "library.c").write_text(code)
                subprocess.run([*cc, "-c", str(tmp / "library.c"), "-o", str(tmp / "library.o")], check=True)
                env = {**os.environ, "CC": shlex.join(cc), "CFLAGS": "-O2 -I" + str(tmp),
                       "CPPFLAGS": "", "LIBBPF_LIB": str(tmp / "library.o")}
                result = subprocess.run([
                    "make", "--no-print-directory", "check-libbpf-positions",
                    "SYSTEM_LIBBPF_HAS_USDT=yes", "LIBBPF_DEP=",
                    "CC=" + shlex.join(cc), "CFLAGS=" + env["CFLAGS"],
                    "LIBBPF_LIB=" + env["LIBBPF_LIB"]],
                    cwd=ROOT, env=env, capture_output=True, text=True)
                if missing:
                    assert result.returncode != 0 and "ERROR: coverage fences require libbpf >= 1.3" in result.stderr, result
                    print(f"libbpf guard: missing {surface} {missing}: REFUSED")
                else:
                    assert result.returncode == 0 and "compile and link" in result.stdout, result
        print("test_libbpf_positions: PASS (all APIs required in headers AND library)")


if __name__ == "__main__":
    main()
