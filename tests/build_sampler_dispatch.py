#!/usr/bin/env python3
"""Build the portable C dispatch regression from unchanged production functions.

--daemon-source accepts `git show ff5511a0:src/daemon.c` for the red control.
Only its inline fd loop needs wrapping on that revision; its body is unchanged.
Temporary generated sources stay outside the repository.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_sampled_attr_io import ROOT, function, registry_sources


def dispatch_function(daemon):
    if "static void dispatch_ready_fd(" in daemon:
        return function(daemon, "dispatch_ready_fd")
    loop = function(daemon, "pgwt_daemon_run")
    start = loop.index("        for (int i = 0; i < n; i++) {")
    end = loop.index("\n\n        /* Drain event ringbuf", start)
    body = loop[start:end]
    body = body[body.index("\n") + 1:body.rfind("\n")]
    body = body.replace("events[i].data.fd", "fd")
    return ("static void dispatch_ready_fd(struct pgwt_daemon *d, int fd, "
            "int rb_fd, int event_rb_fd)\n{\nuint64_t started_ns;\n" + body + "\n}")


def build(output, daemon_source=None):
    daemon = (daemon_source or ROOT / "src/daemon.c").read_text()
    backend = (ROOT / "src/backend.c").read_text()
    sampler = (ROOT / "src/sampler.c").read_text()
    lifecycle, policy, registry, recording, interval = registry_sources()
    # Pin the link from the real event loop to the tested dispatch function.
    if daemon_source is None:
        loop = function(daemon, "pgwt_daemon_run")
        assert "dispatch_ready_fd(d, events[i].data.fd, rb_fd, event_rb_fd);" in loop
    with tempfile.TemporaryDirectory(prefix="pgwt-sampler-dispatch-") as tmp:
        tmp = Path(tmp)
        injected = tmp / "lifecycle.h"
        injected.write_text(lifecycle + policy + interval + registry + recording)
        dispatch = tmp / "dispatch.h"
        delay_config = (function(daemon, "configure_sample_tick_delay")
                        if "static void configure_sample_tick_delay(" in daemon else "")
        dispatch.write_text("\n".join((
            function(backend, "pgwt_sweep_bootstrap_backends"),
            function(daemon, "pgwt_consume_lifecycle_ring"),
            delay_config,
            function(daemon, "handle_sample_timer"),
            dispatch_function(daemon))))
        portable_sampler = tmp / "sampler.c"
        portable_sampler.write_text(sampler.replace(
            "#include <sys/uio.h>", "#include <sys/uio.h>\n"
            "extern ssize_t pgwt_test_wait_readv(pid_t, const struct iovec *, unsigned long, "
            "const struct iovec *, unsigned long, unsigned long);\n"
            "#define process_vm_readv pgwt_test_wait_readv"))
        flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-I" + str(ROOT / "src"), "-I" + str(ROOT / "include"),
                 *shlex.split(os.environ.get("CFLAGS", ""))]
        if delay_config:
            flags += ["-DPGWT_SAMPLE_TICK_DELAY_TEST"]
        if os.uname().sysname == "Darwin":
            flags += ["-include", str(ROOT / "demos/replay-compat.h"),
                      "-include", str(ROOT / "tests/sample_weight_compat.h")]
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), *flags,
                        "-DPGWT_SERVER", f'-DPGWT_LIFECYCLE_SOURCE="{injected}"',
                        f'-DPGWT_DISPATCH_SOURCE="{dispatch}"',
                        str(ROOT / "tests/test_sampler_dispatch.c"),
                        str(ROOT / "src/spawn.c"), str(ROOT / "src/backend_init.c"),
                        str(portable_sampler), str(ROOT / "src/anomaly.c"),
                        str(ROOT / "src/idle_rule.c"), "-o", str(output)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--daemon-source", type=Path)
    args = parser.parse_args()
    build(args.output.resolve(), args.daemon_source)


if __name__ == "__main__":
    main()
