#!/usr/bin/env bash
# Astra r2 rule mutants: build + named REACHED + intended assertion, fail closed.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
exec python3 - "$repo" <<'PY'
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

repo = Path(sys.argv[1])
original = (repo / 'src/core/merge_adapter.c').read_text()
mutations = [
    ('R1', 'G1', [('record.identity_known = l != NULL;',
                   'record.identity_known = l != NULL || (e->cookie_present && !e->cookie);')]),
    ('R2', 'G2', [('record.q = e->q;', 'record.q = e->position;')]),
    ('R3', 'G3', [('if (!b) { *ok = false; return PGWT_MERGE_UNKNOWN_EVIDENCE; }',
                   'if (!b) { return PGWT_MERGE_UNKNOWN_EVIDENCE; }')]),
    ('R4', 'G4', [('record.rank = PGWT_MERGE_TRANS;',
                   '''record.rank = PGWT_MERGE_TRANS;
        if (le(e->bytes + 12, 4) == 0xfffffff6) {
            in->kind = PGWT_MERGE_BOUNDARY;
            in->source = e->source; in->p = e->position; in->w = record.t;
            return r;
        }''')]),
    ('R5', 'G5', [('record.fork_seed_proved = next.proof == MA_PROVED;',
                   'record.fork_seed_proved = false;')]),
    ('R6', 'G6', [('record.t = le(e->bytes + 16, 8);',
                   'record.t = record.rank == PGWT_MERGE_INIT ? d : le(e->bytes + 16, 8);')]),
    ('R7', 'G7', [('if (e->kind == MA_TEXT) {',
                   '''if (e->kind == MA_TEXT) {
        record.pid = le(e->bytes + 4, 4);
        record.t = le(e->bytes + 16, 8);
        record.rank = PGWT_MERGE_INIT;
        in->kind = PGWT_MERGE_DATA; in->record = record;
        return r;
    }
    if (e->kind == MA_TEXT) {''')]),
    ('R8', 'G8', [('c->sources[s].mechanism != expected',
                   '(c->sources[s].mechanism != MA_NONE && c->sources[s].mechanism != expected)')]),
    ('R9', 'G9', [('r.input[0].n = e->lost;', 'r.input[0].n = 0;')]),
    ('R10', 'G10', [('if (l->proof == MA_PENDING) { r.hold = true; return r; }',
                     'if (l->proof == MA_PENDING) { r.hold = true; r.count = 0; return r; }')]),
    ('R11', 'G11', [('r.input[0].kind = PGWT_MERGE_WRITER_REJECTED;',
                     'r.input[0].kind = PGWT_MERGE_WRITER_REJECTED;\n            r.input[0].observation.counters.value[14] = 0;')]),
    ('R12', 'G12', [('case MA_INTERRUPTED:', 'case MA_INTERRUPTED: return r;')]),
]
# Each mutation accepts a forbidden producer premise by omitting its obligation.
# Stale adopted state shares the fresh-fork bit; reuse and unsafe cleanup have
# their own independent guards. Cookie and q preservation are both exercised.
proofs = [
    ('P_nonfork_stale', 'proof_nonfork', 'MA_FRESH_FORK'),
    ('P_late_cookie', 'proof_late_cookie', 'MA_ATTACH_ORDER'),
    ('P_pid_only', 'proof_pid_only', 'MA_MATCHING_COOKIE'),
    ('P_reused', 'proof_reused', 'MA_ALLOCATOR'),
    ('P_cleanup', 'proof_unsafe_cleanup', 'MA_SAFE_CLEANUP'),
    ('P_cookie_overwrite', 'proof_overwrite_cookie', 'MA_PRESERVE_STATE'),
    ('P_q_overwrite', 'proof_overwrite_q', 'MA_SEQUENCE'),
    ('P_stale_marker', 'proof_stale_marker', 'MA_FRESH_FIRST'),
    ('P_emission_cookie', 'proof_other_facts', 'MA_EMISSION_COOKIE'),
    ('P_failure_accounting', 'proof_other_facts', 'MA_FAILURE_ACCOUNTING'),
    ('P_continuous', 'proof_other_facts', 'MA_CONTINUOUS'),
    ('P_idempotent_init', 'proof_other_facts', 'MA_IDEMPOTENT_INIT'),
]
for name, guard, bit in proofs:
    mutations.append((name, guard, [('uint64_t required = MA_ALL_FACTS;',
                                    f'uint64_t required = MA_ALL_FACTS & ~(uint64_t){bit};')]))
compiler = shlex.split(os.environ.get('CC', 'cc'))
flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-g']
flags += shlex.split(os.environ.get('CFLAGS', ''))
env = dict(os.environ, ASAN_OPTIONS='halt_on_error=1:exitcode=86',
           UBSAN_OPTIONS='halt_on_error=1:exitcode=87:print_stacktrace=1')
with tempfile.TemporaryDirectory(prefix='pgwt-merge-adapter-mutants-') as tmp:
    work = Path(tmp)
    for name in ['src/core/pgwt_merge.h', 'src/core/pgwt_merge_adapter.h', 'src/core/merge.c',
                 'tests/test_core_merge_adapter.c', 'tests/test_core_merge_contract.c',
                 'tests/merge_adapter_goldens.h']:
        dest = work / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo / name, dest)
    source = work / 'src/core/merge_adapter.c'
    binary = work / 'adapter-test'
    def build(text):
        source.write_text(text)
        r = subprocess.run(compiler + flags + ['-o', str(binary),
            str(work / 'tests/test_core_merge_adapter.c'), str(source), str(work / 'src/core/merge.c')],
            capture_output=True, text=True, env=env)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit('ERROR: build failure is not a mutant kill')
    build(original)
    base = subprocess.run([str(binary), str(repo / 'contracts/merge-v1/examples.json')],
                          capture_output=True, text=True, env=env)
    if base.returncode:
        print(base.stdout + base.stderr)
        raise SystemExit('ERROR: baseline/guard sweep failed')
    expected_sweep = 'adapter goldens: 12/12; guard sweep: 28/28'
    if expected_sweep not in base.stdout:
        raise SystemExit('ERROR: baseline did not reach every registered guard')
    print('adapter baseline: golden 12/12, guard sweep 28/28, end-to-end 78/78 PASS')
    killed = 0
    for name, guard, replacements in mutations:
        changed = original
        for before, after in replacements:
            if changed.count(before) != 1:
                raise SystemExit(f'ERROR: {name}: mutation anchor is not unique')
            changed = changed.replace(before, after)
        build(changed)
        result = subprocess.run([str(binary), '--guard', guard],
                                capture_output=True, text=True, env=env)
        # Exit 1 alone cannot classify a crash, setup failure or unrelated test.
        if (result.returncode != 1 or f'REACHED {guard}\n' not in result.stdout or
            f'adapter ASSERT {guard}:' not in result.stderr or
            any(x in result.stderr for x in ['AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'])):
            print(result.stdout + result.stderr)
            raise SystemExit(f'ERROR: {name}: survived or failed outside intended assertion')
        print(f'{name}: KILLED at {guard}')
        killed += 1
    print(f'adapter mutants: {killed}/{len(mutations)} killed; guard sweep 28/28 PASS')
PY
