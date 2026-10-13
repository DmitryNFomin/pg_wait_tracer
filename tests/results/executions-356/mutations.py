"""Prove the differential/provenance/refusal checks reject four broken lookups."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
root = Path(__file__).resolve().parents[3]
src = (root/'src/compute.c').read_text()
find = '''    int found = pgwt_pid_index_find(ix, pid);
    if (found >= 0)
        return found;'''
put = '''    if (pgwt_pid_index_put(ix, pid, idx) != 0)
        return -1;'''
mutants = {
    'linear_fallback': [(find, '''    for (int i = 0; i < *n_states; i++)
        if ((*states)[i].pid == pid) return i;'''), (put, '')],
    'truncated_pid': [(put, put.replace('ix, pid, idx', 'ix, (uint16_t)pid, idx'))],
    'wrong_slot': [(find, find.replace('return found;', 'return found + 1;'))],
    'ignored_allocation_failure': [(put, '    (void)pgwt_pid_index_put(ix, pid, idx);')],
}
with tempfile.TemporaryDirectory(prefix='pgwt-356-mutations-') as temp:
    dest = Path(temp)
    shutil.copytree(root/'src', dest/'src')
    (dest/'include').mkdir()
    (dest/'tests').mkdir()
    for name in ('test_execution_pid_index.c', 'executions_linear_oracle.h'):
        shutil.copy(root/'tests'/name, dest/'tests'/name)
    for name, replacements in ({'control': []} | mutants).items():
        changed = src
        for old, new in replacements:
            assert changed.count(old) == 1, (name, 'anchor mismatch')
            changed = changed.replace(old, new)
        (dest/'src/compute.c').write_text(changed)
        binary = dest/'test'
        command = ['cc','-g','-O2','-DPGWT_SERVER','-I'+str(dest/'src'),'-I'+str(dest/'include'),
            str(dest/'tests/test_execution_pid_index.c'), str(dest/'src/wait_event.c'),
            str(dest/'src/idle_rule.c'), str(dest/'src/cJSON.c'), '-lm','-o',str(binary)]
        command += ['-Wl,-dead_strip'] if sys.platform == 'darwin' else [str(dest/'src/spawn.c')]
        build = subprocess.run(command, capture_output=True, text=True)
        assert build.returncode == 0, (name, 'BUILD FAILED', build.stderr)
        run = subprocess.run([str(binary)], capture_output=True, text=True, cwd=dest)
        assert (run.returncode == 0) == (name == 'control'), (name, run.returncode, run.stdout, run.stderr)
        print(name, 'PASS' if name == 'control' else 'CAUGHT', flush=True)
