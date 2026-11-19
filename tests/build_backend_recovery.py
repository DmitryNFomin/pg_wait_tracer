#!/usr/bin/env python3
"""Compile unchanged recovery/FORK functions with deterministic process I/O.

Build products and mutation sources stay outside the repository. A mutant
counts only after it builds and reaches its distinguishing assertion.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_sampled_attr_io import ROOT, function


def build(output, backend=None):
    backend = backend or (ROOT / 'src/backend.c').read_text()
    names = ('pgwt_backend_init', 'pgwt_find_backend', 'alloc_backend',
             'recover_unattached_backend', 'pgwt_recover_unattached_backends', 'pgwt_handle_fork')
    with tempfile.TemporaryDirectory(prefix='pgwt-backend-recovery-') as tmp:
        source = Path(tmp) / 'recovery.h'
        source.write_text('\n\n'.join(function(backend, name) for name in names))
        flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'include'),
                 *shlex.split(os.environ.get('CFLAGS', ''))]
        if os.uname().sysname == 'Darwin':
            flags += ['-include', str(ROOT / 'demos/replay-compat.h'),
                      '-include', str(ROOT / 'tests/sample_weight_compat.h')]
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), *flags,
                        '-DPGWT_SERVER', f'-DPGWT_RECOVERY_SOURCE="{source}"',
                        str(ROOT / 'tests/test_backend_recovery.c'),
                        str(ROOT / 'src/backend_init.c'), '-o', str(output)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--mutants', action='store_true')
    args = parser.parse_args()
    if args.output:
        build(args.output.resolve())
        return
    with tempfile.TemporaryDirectory(prefix='pgwt-recovery-mutants-') as tmp:
        binary = Path(tmp) / 'test_backend_recovery'
        build(binary)
        subprocess.run([str(binary)], check=True)
        if args.mutants:
            backend = (ROOT / 'src/backend.c').read_text()
            mutations = [
                ('full_proc_walk', 'if (n >= 0 && (size_t)n < capacity)',
                 'if (0 && n >= 0 && (size_t)n < capacity)',
                 'stat opens bounded by children plus constant'),
                ('swallowed_fork', 'if (be && (!pgwt_mode_uses_watchpoints(d) || be->wp_fd >= 0 ||\n               be->bootstrap_fd >= 0))',
                 'if (be)', 'FORK after recovery must attempt attach'),
            ]
            for name, old, new, assertion in mutations:
                assert backend.count(old) == 1, name
                build(binary, backend.replace(old, new))
                result = subprocess.run([str(binary)], text=True, capture_output=True)
                assert (result.returncode == 1 and 'REACHED backend recovery' in result.stdout
                        and 'FAIL: ' + assertion in result.stderr), result.stdout + result.stderr
                print(f'{name}: KILLED at {assertion}')


if __name__ == '__main__':
    main()
