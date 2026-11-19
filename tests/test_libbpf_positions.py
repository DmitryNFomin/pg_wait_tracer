#!/usr/bin/env python3
"""Probe system USDT/positions, select fallback, and guard the selected library.

Only compile/link host C; no BPF or bundled downloads. USDT_HEADER and
LIBBPF_LIB override the probe's system paths with synthetic headers/libraries.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APIS = {
    "bpf_program__attach_usdt": "void *bpf_program__attach_usdt(void)",
    "ring_buffer__ring": "struct ring *ring_buffer__ring(struct ring_buffer *r, unsigned int i)",
    "ring__producer_pos": "unsigned long ring__producer_pos(const struct ring *r)",
    "ring__consumer_pos": "unsigned long ring__consumer_pos(const struct ring *r)",
}


def main():
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="pgwt-libbpf-positions-") as tmp:
        tmp = Path(tmp)
        (tmp / "bpf").mkdir()
        usdt_header = tmp / "bpf/usdt.bpf.h"
        usdt_header.touch()
        for missing in [None, *APIS, "usdt.bpf.h"]:
            for surface in ["header", "library"]:
                header = "struct ring; struct ring_buffer;\n"
                code = "struct ring; struct ring_buffer;\n"
                for api, signature in APIS.items():
                    if missing != api or surface != "header":
                        header += signature + ";\n"
                    if missing != api or surface != "library":
                        body = ("return (void *)0; }\n" if api == "bpf_program__attach_usdt" else
                                "(void)r; (void)i; return (struct ring *)0; }\n" if api == "ring_buffer__ring" else
                                "(void)r; return 0; }\n")
                        code += signature + " { " + body
                (tmp / "bpf/libbpf.h").write_text(header)
                (tmp / "library.c").write_text(code)
                subprocess.run([*cc, "-c", str(tmp / "library.c"), "-o", str(tmp / "library.o")], check=True)
                env = {**os.environ, "CC": shlex.join(cc), "CFLAGS": "-O2 -I" + str(tmp),
                       "CPPFLAGS": "", "LIBBPF_LIB": str(tmp / "library.o"),
                       "USDT_HEADER": str(tmp / "missing" if missing == "usdt.bpf.h" else usdt_header)}
                probe = subprocess.run(["sh", "scripts/detect_libbpf_usdt.sh"], cwd=ROOT,
                                       env=env, capture_output=True, text=True, check=True)
                selected = probe.stdout.strip()
                assert selected == ("no" if missing else "yes"), (missing, surface, probe)
                # Exercise the Makefile's own probe and selection, without forcing
                # SYSTEM_LIBBPF_HAS_USDT. The script overrides select the fixture.
                selection = subprocess.run([
                    "make", "--no-print-directory", "-f", "Makefile", "-f", "-", "inspect-libbpf"],
                    input="inspect-libbpf:\n\t@echo '$(LIBBPF_LIB)|$(LIBBPF_DEP)|$(LIBBPF_VERSION)'\n",
                    cwd=ROOT, env=env, capture_output=True, text=True, check=True).stdout.strip()
                if missing:
                    assert selection == "build/libbpf/root/usr/lib64/libbpf.a -lelf -lz|build/libbpf/root/usr/lib64/libbpf.a|v1.4.7", selection
                else:
                    assert selection == "-lbpf||v1.4.7", selection
                print(f"system probe: missing {surface} {missing}: {selected}; "
                      f"{'bundled v1.4.7' if missing else 'system'} selected")
                # A broken bundled build must fail loudly on any missing ring
                # API; the USDT probe cases above test fallback separately.
                result = subprocess.run([
                    "make", "--no-print-directory", "check-libbpf-positions",
                    "SYSTEM_LIBBPF_HAS_USDT=" + selected, "LIBBPF_DEP=",
                    "CC=" + shlex.join(cc), "CFLAGS=" + env["CFLAGS"],
                    "LIBBPF_LIB=" + env["LIBBPF_LIB"]],
                    cwd=ROOT, env=env, capture_output=True, text=True)
                if missing in set(APIS) - {"bpf_program__attach_usdt"}:
                    assert result.returncode != 0 and "ERROR: coverage fences require libbpf >= 1.3" in result.stderr, result
                    print(f"bundled guard: missing {surface} {missing}: REFUSED")
                else:
                    assert result.returncode == 0 and "compile and link" in result.stdout, result
        print("test_libbpf_positions: PASS (system USDT + positions, pinned fallback, bundled guard)")


if __name__ == "__main__":
    main()
