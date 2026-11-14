#!/usr/bin/env python3
"""Portable attribution syscall budget and mutations; all outputs stay in /tmp."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def run(args):
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def perf_gate_cases():
    from sampler_batch_perf import assess, parse_trace
    clean = "1000.000 ( 0.001 ms): 77 process_vm_readv(pid: 42) = 24\n"
    counts = parse_trace(clean, 999, 1001)
    for major, bound in ((13, 5), (17, 5), (18, 5)):
        result = assess(counts, major, 512, 200, 0)
        assert result["vm_calls_per_tick_bound"] == bound and not result["errors"]
    legacy = ("1000.000 ( 0.001 ms): 77 openat(dfd: CWD, filename: /proc/123/mem) = 9\n"
              "1000.001 ( 0.001 ms): 77 pread64(fd: 9, buf: 0) = 4\n"
              "1000.002 ( 0.001 ms): 77 close(fd: 9) = 0\n") + clean
    counts = parse_trace(legacy, 999, 1001)
    assert counts["mem_open"] == counts["mem_pread"] == counts["mem_close"] == 1
    assert len(assess(counts, 18, 512, 200, 0)["errors"]) == 4
    interrupted = ("1000.000 (         ): 77 process_vm_readv(pid: 42) ...\n"
                   "1000.000 ( 0.001 ms): 77 ... [continued]: process_vm_readv()) = 24\n")
    assert parse_trace(interrupted, 999, 1001)["process_vm_readv"] == 1
    for invalid in ("", clean + "LOST 2 events!", "1000.000 ( 0.001 ms): 77 close(fd: 3) ..."):
        try:
            parse_trace(invalid, 999, 1001)
        except AssertionError:
            pass
        else:
            raise AssertionError("incomplete perf evidence accepted")
    counts = parse_trace(clean, 999, 1001)
    counts["process_vm_readv"] = 1201
    assert assess(counts, 18, 512, 200, 0)["errors"]
    assert assess(counts, 18, 512, 200, 103)["errors"]
    print("perf gate: clean, legacy RED, continued, missing/lost/incomplete, call/failure bounds PASS")


def main():
    perf_gate_cases()
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             f"-I{ROOT / 'src'}"] + shlex.split(os.environ.get("CFLAGS", ""))
    if os.uname().sysname == "Darwin":
        flags += ["-include", str(ROOT / "demos/replay-compat.h")]
    source = (ROOT / "src/backend_status_layout.c").read_text()
    with tempfile.TemporaryDirectory(prefix="pgwt-sampled-io-") as directory:
        tmp = Path(directory)

        def build(label, text):
            implementation = tmp / f"{label}.c"
            implementation.write_text(text)
            binary = tmp / label
            result = run(cc + flags + [
                f'-DPGWT_PGBS_SOURCE="{implementation}"',
                str(ROOT / "tests/test_sampled_attr_io.c"),
                str(ROOT / "src/spawn.c"), "-o", str(binary)])
            assert result.returncode == 0, result.stdout + result.stderr
            return binary

        result = run([str(build("baseline", source))])
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout, end="")
        mutations = [
            ("drop_pid_compare",
             "snapshot->procpid == (uint32_t)expected_pid &&",
             "snapshot->procpid != 0 &&",
             "cross-wired row buffers fail PID identity"),
            ("accept_torn",
             "return before == after && (after & 1u) == 0;",
             "(void)before; return (after & 1u) == 0;",
             "persistent tear fails once after two retry calls"),
            ("parse_untransferred",
             "if (!row->complete) continue;",
             "if (!row->complete) { targets[i].rc = 0; continue; }",
             "untransferred rows cannot be parsed as valid"),
            ("iovec_owner_off_by_one",
             "owner[count++] = i;",
             "owner[count++] = (i + 1) % n;",
             "short byte count rejects partial row and resumes later rows"),
            ("no_iov_max_chunking",
             "if (want > PGWT_PGBS_IOV_MAX) want = PGWT_PGBS_IOV_MAX;",
             "if (want > PGWT_PGBS_IOV_MAX) want = count - base;",
             "batched syscall budget over 500 ticks"),
            ("skip_previously_idle",
             "if (!gate_only) memset(&targets[i].attr, 0, sizeof(targets[i].attr));",
             "if (targets[i].rc == 0 && !targets[i].attr.cmd_open) targets[i].reader = NULL;\n"
             "        if (!gate_only) memset(&targets[i].attr, 0, sizeof(targets[i].attr));",
             "idle row changes on the next tick"),
            ("counter_after_before_span",
             "ADD_RANGE(row->span, r->status_addr + lo, hi - lo);\n"
             "            if (text_len) ADD_RANGE(row->activity, r->activity_addr, text_len);\n"
             "            ADD_RANGE(&row->after, r->status_addr + layout->st_changecount.offset, 4);",
             "ADD_RANGE(&row->after, r->status_addr + layout->st_changecount.offset, 4);\n"
             "            if (text_len) ADD_RANGE(row->activity, r->activity_addr, text_len);\n"
             "            ADD_RANGE(row->span, r->status_addr + lo, hi - lo);",
             "counter after payload detects concurrent writer"),
            ("batch_unproven_address",
             "if (!r || !r->shared || !r->status_addr ||",
             "if (!r || !r->status_addr ||",
             "unproven shared address is never batched"),
        ]
        for label, before, after, assertion in mutations:
            assert source.count(before) == 1, (label, "mutation anchor changed")
            binary = build(label, source.replace(before, after))
            result = run([str(binary)])
            assert result.returncode == 1, (label, result.returncode, result.stdout, result.stderr)
            assert f"FAIL: {assertion}" in result.stderr, (label, result.stderr)
            print(f"MUTANT {label}: built, reached case, rejected by '{assertion}'")


if __name__ == "__main__":
    main()
