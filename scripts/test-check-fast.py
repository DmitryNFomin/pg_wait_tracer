#!/usr/bin/env python3
"""Cheap slow-step coverage guard and hermetic selector proofs; stdlib only.

Only the marked selector definitions are evaluated, never check.sh as a whole.
Git objects/indexes below belong exclusively to temporary repositories.
"""
import fnmatch
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
BEGIN = '# BEGIN fast slow-step selector'
END = '# END fast slow-step selector'


def coverage(source, tracked):
    functions = dict(re.findall(r'(?ms)^(check_slow_\d+)\(\) \{\n(.*?)^\}', source))
    calls = [shlex.split(line) for line in source.splitlines() if re.match(r'^\s*slow_step\s', line)]
    assert calls, 'no slow steps inspected'
    for _, name, patterns, *command in calls:
        globs = patterns.split()
        assert globs, f'{name}: empty glob list'
        for glob in globs:
            assert any(fnmatch.fnmatchcase(path, glob) for path in tracked), f'{name}: unmatched glob {glob}'
        assert command, f'{name}: missing command'
        if command[0] in functions:
            body = functions[command[0]]
        else:
            body = ' '.join(command)
        scripts = set(re.findall(r'\b(?:tests|scripts)/[\w./-]+\.(?:py|sh)\b', body))
        assert scripts, f'{name}: own script path could not be inspected'
        for script in scripts:
            assert any(fnmatch.fnmatchcase(script, glob) for glob in globs), f'{name}: own script uncovered: {script}'
    return calls


def git(repo, *args, data=None):
    env = {**os.environ, 'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': os.devnull,
           'GIT_AUTHOR_NAME': 'Fast check fixture', 'GIT_AUTHOR_EMAIL': 'fixture@example.invalid',
           'GIT_COMMITTER_NAME': 'Fast check fixture', 'GIT_COMMITTER_EMAIL': 'fixture@example.invalid'}
    # Do not let a caller's repository/index override the throwaway repository.
    for name in list(env):
        if name.startswith('GIT_') and name not in {
            'GIT_CONFIG_NOSYSTEM', 'GIT_CONFIG_GLOBAL', 'GIT_AUTHOR_NAME',
            'GIT_AUTHOR_EMAIL', 'GIT_COMMITTER_NAME', 'GIT_COMMITTER_EMAIL'}:
            del env[name]
    result = subprocess.run(['git', '-C', str(repo), *args], input=data,
                            capture_output=True, env=env, check=True)
    return result.stdout.decode().strip()


def commit(repo, files, parent=None):
    entries = []
    for name, value in sorted(files.items()):
        oid = git(repo, 'hash-object', '-w', '--stdin', data=value.encode())
        entries.append(f'100644 blob {oid}\t{name}\n')
    tree = git(repo, 'mktree', data=''.join(entries).encode())
    args = ['commit-tree', tree]
    if parent:
        args += ['-p', parent]
    oid = git(repo, *args, data=b'fixture\n')
    git(repo, 'update-ref', 'HEAD', oid)
    git(repo, 'read-tree', '--reset', '-u', 'HEAD')
    return oid


def fixtures(root):
    repos = {}
    for case in ('clean', 'committed', 'staged', 'unstaged', 'untracked', 'ignored',
                 'deleted', 'renamed', 'special', 'combined', 'missing-base', 'unrelated-base', 'shallow'):
        repo = root / case
        repo.mkdir()
        git(repo, 'init', '-q')
        files = {name + '.txt': 'base\n' for name in ('committed', 'staged', 'unstaged', 'untouched', 'deleted', 'renamed')}
        files['.gitignore'] = 'ignored.txt\n'
        base = commit(repo, files)
        if case != 'missing-base':
            git(repo, 'update-ref', 'refs/remotes/origin/master', base)
        if case in ('committed', 'combined'):
            commit(repo, {**files, 'committed.txt': 'branch edit\n'}, base)
        elif case == 'staged':
            (repo / 'staged.txt').write_text('index edit\n')
            oid = git(repo, 'hash-object', '-w', 'staged.txt')
            git(repo, 'update-index', '--cacheinfo', f'100644,{oid},staged.txt')
        elif case == 'unstaged':
            (repo / 'unstaged.txt').write_text('working edit\n')
        elif case in ('untracked', 'ignored'):
            (repo / (case + '.txt')).write_text('new\n')
        elif case == 'deleted':
            (repo / 'deleted.txt').unlink()
        elif case == 'renamed':
            (repo / 'renamed.txt').rename(repo / 'new-name.txt')
        elif case == 'special':
            (repo / 'special [*]\nfile.txt').write_text('new\n')
        elif case == 'unrelated-base':
            other = git(repo, 'commit-tree', git(repo, 'rev-parse', 'HEAD^{tree}'), data=b'unrelated\n')
            git(repo, 'update-ref', 'refs/remotes/origin/master', other)
        elif case == 'shallow':
            (repo / '.git/shallow').write_text(base + '\n')
        if case == 'combined':
            (repo / 'staged.txt').write_text('index edit\n')
            oid = git(repo, 'hash-object', '-w', 'staged.txt')
            git(repo, 'update-index', '--cacheinfo', f'100644,{oid},staged.txt')
            (repo / 'unstaged.txt').write_text('working edit\n')
            (repo / 'untracked.txt').write_text('new\n')
        repos[case] = repo
    return repos


NAMES = ('committed', 'staged', 'unstaged', 'untracked', 'untouched', 'deleted', 'renamed', 'new-name', 'special')


def exercise(selector, repo, bash, *, fast=True, override=False, empty_globs=False):
    harness = 'set -uo pipefail\nFAST=' + str(int(fast)) + '\n'
    # Timing is stubbed only to keep suite output deterministic; selection and
    # command execution use the exact production functions.
    harness += 'finish_step() { :; }; step() { :; }; step_name=\n'
    harness += selector + '\n'
    for name in NAMES:
        pattern = '' if empty_globs else ('special*' if name == 'special' else name + '.txt')
        harness += f'slow_step {shlex.quote(name)} {shlex.quote(pattern)} printf "RUN: {name}\\n"\n'
    harness += 'print_fast_summary\n'
    env = dict(os.environ)
    env.pop('PGWT_CHECK_FAST_ALL', None)
    if override:
        env['PGWT_CHECK_FAST_ALL'] = '1'
    for name in list(env):
        if name.startswith('GIT_'):
            del env[name]
    result = subprocess.run([bash], cwd=repo, input=harness, text=True,
                            capture_output=True, env=env, timeout=15)
    assert result.returncode == 0, f'{repo.name}: selector execution failed under {bash}: {result.stderr}'
    assert not result.stderr, f'{repo.name}: selector diagnostic under {bash}: {result.stderr}'
    return result.stdout


def proofs(selector, repos, bash):
    # clean is exactly the merge base, with no changed or untracked paths.
    # Keep it first so the empty-array mutant reaches this specific case.
    expectations = {case: set() for case in ('clean', 'ignored')}
    expectations.update({case: {case} for case in ('committed', 'staged', 'unstaged', 'untracked', 'deleted', 'special')})
    expectations['renamed'] = {'renamed', 'new-name'}
    expectations['combined'] = {'committed', 'staged', 'unstaged', 'untracked'}
    expectations.update({case: set(NAMES) for case in ('missing-base', 'unrelated-base', 'shallow')})
    for case, expected in expectations.items():
        out = exercise(selector, repos[case], bash)
        actual = set(re.findall(r'^RUN: (.*)$', out, re.M))
        assert actual == expected, f'{case}: matching slow steps {actual}, expected {expected}'
        skipped = set(re.findall(r'^SKIP \(fast\): (.*)$', out, re.M))
        assert skipped == set(NAMES) - expected, f'{case}: wrong skip lines'
        assert f'Fast slow steps: {len(expected)} ran, {len(NAMES) - len(expected)} skipped' in out, f'{case}: incorrect counts'
        assert "CI's full-check runs everything" in out, f'{case}: missing CI note'
        if case in ('missing-base', 'unrelated-base', 'shallow'):
            assert 'fast: running every slow step:' in out, f'{case}: missing fail-safe explanation'
    for case in ('clean', 'committed', 'missing-base'):
        out = exercise(selector, repos[case], bash, fast=False)
        assert re.findall(r'^RUN: (.*)$', out, re.M) == list(NAMES), f'full mode {case}: skipped or reordered commands'
        assert 'SKIP' not in out and 'fast:' not in out and 'Fast slow steps:' not in out, 'full mode output changed'
    out = exercise(selector, repos['clean'], bash, override=True)
    assert set(re.findall(r'^RUN: (.*)$', out, re.M)) == set(NAMES), 'override: skipped slow steps'
    assert 'PGWT_CHECK_FAST_ALL=1' in out, 'override: missing explanation'
    out = exercise(selector, repos['committed'], bash, empty_globs=True)
    assert not re.search(r'^RUN:', out, re.M), 'empty globs: selected a slow step'
    assert f'Fast slow steps: 0 ran, {len(NAMES)} skipped' in out, 'empty globs: wrong counts'


def bash_shells():
    path_bash = shutil.which('bash')
    assert path_bash, 'bash not found on PATH'
    shells = [path_bash]
    system_bash = Path('/bin/bash')
    if system_bash.is_file() and not os.path.samefile(path_bash, system_bash):
        shells.append(str(system_bash))
    return shells


def main():
    source = (ROOT / 'scripts/check.sh').read_text()
    tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')[:-1]
    calls = coverage(source, tracked)
    print(f'coverage: PASS ({len(calls)} slow steps; nonempty, tracked globs; own scripts covered)', flush=True)
    # Drop the entire marker line, including its explanatory comment suffix.
    selector = source.split(BEGIN, 1)[1].split('\n', 1)[1].split(END, 1)[0]
    # Mutants must remain valid shell and reach the named assertion.
    mutations = (
        ('matched step skipped', 'if [[ "$path" == $pattern ]]; then\n                            selected=1',
         'if [[ "$path" == $pattern ]]; then\n                            selected=0', 'committed: matching slow steps'),
        ('untracked ignored', 'git ls-files --others --exclude-standard -z', 'printf \'\'', 'untracked: matching slow steps'),
        ('missing merge base skips', "fast_all_reason='cannot compute merge base with origin/master'", "fast_all_reason=", 'missing-base: matching slow steps'),
        ('full mode skips', 'if [[ $FAST -eq 0 ]]; then\n        selected=1', 'if [[ $FAST -eq 0 ]]; then\n        selected=0', 'full mode clean: skipped'),
    )
    with tempfile.TemporaryDirectory(prefix='pgwt-fast-selftest-') as temp:
        repos = fixtures(Path(temp))
        for bash in bash_shells():
            version = subprocess.check_output([bash, '-c', 'printf "%s" "$BASH_VERSION"'], text=True)
            print(f'bash: {bash} ({version})', flush=True)
            proofs(selector, repos, bash)
            print('selector: PASS (committed/staged/unstaged/untracked isolation + combined, empty changed-path list, ignored, delete/rename, NUL paths, missing/unrelated/shallow base, full mode, override, counts, empty globs)', flush=True)
            shell_mutations = list(mutations)
            # Newer Bash permits empty array expansion with nounset. Only
            # require this mutant to fail on shells exhibiting the defect.
            probe = subprocess.run([bash], input='set -u; empty=(); printf "%s" "${empty[@]}"\n',
                                   text=True, capture_output=True)
            if probe.returncode:
                assert 'empty[@]: unbound variable' in probe.stderr, probe.stderr
                shell_mutations.append((
                    'unguarded empty changed-path list',
                    'if [[ ${#fast_changed_paths[@]} -gt 0 && ${#globs[@]} -gt 0 ]]; then',
                    'if [[ ${#globs[@]} -gt 0 ]]; then',
                    'clean: selector execution failed'))
            else:
                print('empty-array regression mutant: N/A (shell permits empty arrays under set -u)', flush=True)
            for name, old, new, intended in shell_mutations:
                assert selector.count(old) == 1, f'{name}: mutation site drift'
                mutant = selector.replace(old, new)
                syntax = subprocess.run([bash, '-n'], input=mutant, text=True, capture_output=True)
                assert syntax.returncode == 0, f'{name}: mutant syntax error'
                try:
                    proofs(mutant, repos, bash)
                except AssertionError as exc:
                    assert intended in str(exc), f'{name}: wrong failure: {exc}'
                    if name == 'unguarded empty changed-path list':
                        assert 'fast_changed_paths[@]: unbound variable' in str(exc), str(exc)
                    print(f'mutant RED: {name} (valid shell; {exc})', flush=True)
                else:
                    raise AssertionError(f'{name}: SURVIVED')
    # The declaration guard must itself reject each required failure mode.
    first = calls[0]
    for name, bad in (('empty globs', ''), ('unmatched glob', 'no-such-tracked-path/*'),
                      ('own script uncovered', 'CLAUDE.md')):
        mutant = source.replace(shlex.quote(first[2]), shlex.quote(bad), 1)
        assert mutant != source, 'declaration mutation site drift'
        try:
            coverage(mutant, tracked)
        except AssertionError as exc:
            assert {'empty globs': 'empty glob list', 'unmatched glob': 'unmatched glob',
                    'own script uncovered': 'own script uncovered'}[name] in str(exc), str(exc)
            print(f'guard mutant RED: {name}', flush=True)
        else:
            raise AssertionError(f'{name}: SURVIVED')
    print('fast slow-step guard and mutants: PASS', flush=True)


if __name__ == '__main__':
    main()
