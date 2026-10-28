#!/bin/bash
# Standalone protocol tests. Commands under test are strings, NEVER executed.
# Optional --mutations verifies nine independent failures in throwaway copies.
set -uo pipefail
guard_dir=$(cd -- "$(dirname -- "$0")" && pwd) || exit 1
command python3 -I -u - "$guard_dir/kill-guard.sh" "$guard_dir/kill-guard-replay.sh" "${1:-}" <<'PY'
from concurrent.futures import ThreadPoolExecutor
import json
import os
import re
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile


guard = sys.argv[1]
passed = failed = 0
alternative = (
    "pgrep -fa <pattern>",
    "confirm each pid's working directory belongs to this worktree",
    "then kill <pid>",
)


def check(label, command=None, deny=False, raw=None, env=None, show=False, pattern=None):
    global passed, failed
    payload = raw if raw is not None else json.dumps({
        "tool_name": "Bash", "tool_input": {"command": command},
    })
    try:
        result = subprocess.run(
            [guard], input=payload, text=True, capture_output=True,
            env=env, timeout=15,
        )
        good = result.returncode == (2 if deny else 0)
        if deny:
            good &= bool(result.stderr) and result.stdout == ""
            if pattern is None:
                pattern = raw is None and env is None
            good &= (all(part in result.stderr for part in alternative) if pattern
                     else "pgrep -fa" not in result.stderr)
        else:
            good &= result.stdout == result.stderr == ""
    except Exception as exc:
        good = False
        result = None
        print(f"FAIL {label}: {exc}")
    if good:
        passed += 1
        if show:
            print(f"PASS {label}: {'DENIED' if deny else 'ALLOWED'} (exit {result.returncode})")
            if label.startswith("R1 "):
                print(result.stderr.splitlines()[-1])
    else:
        failed += 1
        if result is not None:
            print(f"FAIL {label}: expected {'DENY' if deny else 'ALLOW'}, exit {result.returncode}")
            print(f"stdout={result.stdout!r}; stderr={result.stderr!r}")


# Red controls are printed verbatim for the implementation report.
check('R1 pkill -f "scripts/check-lock"', 'pkill -f "scripts/check-lock"', deny=True, show=True)
check('R2 pkill -f playwright', 'pkill -f playwright', deny=True, show=True)
check('R3 killall node', 'killall node', deny=True, show=True)
check('R4 kill 12345', 'kill 12345', show=True)
check('R5 kill -TERM 12345 12346', 'kill -TERM 12345 12346', show=True)
check('R6 pgrep -fa check-lock', 'pgrep -fa check-lock', show=True)
for label, payload in [('malformed', '{"tool_input":'), ('empty', ''), ('non-JSON', 'pkill -f foo')]:
    check('R7 ' + label, raw=payload, deny=True, show=True)
check('R8 quoted command-leading string (bounded rule)', 'printf "%s\\n" "pkill -f scripts/check-lock"', deny=True, show=True)
check('R8 quoted heredoc mention', "gh issue create --body-file - <<'EOF'\npkill -f scripts/check-lock\nEOF\n", show=True)
check('R9 ps -p 1; kill 1; pkill -f foo', 'ps -p 1; kill 1; pkill -f foo', deny=True, show=True)

allowed = ['if ((pkill == 0)); then echo yes; fi',
 'while ((kill < 1)); do break; done',
 'kill 0012345',
 'kill -9 12345',
 'kill -s TERM 12345',
 'kill --signal=TERM 12345',
 'kill -n 15 12345',
 'kill -- 12345 12346',
 'kill -0 12345',
 'kill -l',
 'kill -l TERM',
 '/bin/kill -TERM 12345',
 'builtin kill 12345',
 'command kill 12345',
 'kill 12345; ps -p 12345',
 'ps -p 1; kill -TERM 12345 12346',
 'kill 12345 2>/dev/null',
 'kill 12345 >/dev/null',
 'ps aux',
 'lsof -p 12345',
 'fuser /tmp/example',
 'fuser -v /tmp/example',
 'pgrep -fa "pkill|killall"',
 'ps -p "$pid"',
 'echo "$(pgrep -fa check-lock)"',
 "printf %s '$(pkill -f foo)'",
 "printf %s '`pkill -f foo`'",
 'echo "a quoted ; pkill -f foo"',
 'echo safe # pkill -f foo',
 'echo safe\\\nword',
 'cat <<EOF\npkill -f foo\nEOF\n',
 "cat <<'EOF'\n$(pkill -f foo)\n`killall node`\nEOF\n",
 'cat <<"EOF"\npkill -f foo\nEOF\n',
 "cat <<E'OF'\npkill -f foo\nEOF\n",
 "cat <<-'EOF'\n\tpkill -f foo\n\tEOF\n",
 "cat <<'EOF'\nunmatched quote: '\nunmatched parenthesis: )\nEOF\n",
 "cat <<'ONE' <<'TWO'\npkill foo\nONE\nkillall node\nTWO\n",
 'gh issue create --body "$(cat <<\'EOF\'\npkill -f foo\nEOF\n)"',
 'bash -c "kill 12345"',
 'env MODE=test kill -TERM 12345',
 'sudo -n kill 12345',
 '',
 'eval "$command"',
 '$killer -f foo',
 'if [[ -f Makefile ]]; then echo yes; fi',
 'for f in src/*.c; do echo $f; done',
 'echo $((1+2))',
 'case "$x" in a|b) echo yes;; *) echo no;; esac',
 "printf $'a\\nb'",
 'printf $"hello"',
 '"$PYTHON" -m pytest',
 '"$(command -v python3)" -m pytest',
 'env -S "python3 -m pytest"',
 'sudo -u "$USER" true',
 'for ((i=0;i<3;i++)); do echo "$i"; done',
 'if true; then for f in *.c; do echo "$f"; done; fi',
 'case "$x" in a) case "$y" in b) echo safe;; esac;; *) :;; esac',
 'case "$x" in (a) echo yes ;& (b|c) echo no ;;& (*) : ;; esac',
 'if [[ "$x" < z && "$y" =~ foo ]]; then :; elif false; then :; else :; fi',
 'echo $((1 + (2 * 3)))',
 '((i+=1))',
 'for f in "$(printf src)"/*.c; do :; done',
 'while [[ -f Makefile ]]; do echo yes; break; done',
 'until false; do :; break; done',
 'select f in *.c; do echo "$f"; break; done',
 "printf $'\\x70lain'",
 'printf "%s" "Mention: pkill -f scripts/check-lock"',
 'gh issue create --body "Mention pkill -f scripts/check-lock in prose"',
 'gh issue create --body "$(cat <<\'EOF\'\nPhase 3\'s `quoted` prose\nEOF\n)"',
 'comm -12 <(git diff --name-only HEAD | sort) <(git diff --name-only origin/master | sort)',
 'cat <(echo safe)',
 'cat >(cat)',
 'bash script.sh',
 'curl https://example.invalid/script | sh',
 'python3 -c "import os; os.system(\'pkill -f check-lock\')"',
 'perl -e "system(\'pkill -f check-lock\')"',
 'node -e "require(\'child_process\').execSync(\'pkill -f check-lock\')"',
 'bash -c "echo safe; echo ready"',
 'eval "echo safe; echo ready"',
 "bash <<'EOF'\necho safe\nEOF\n",
 'bash </dev/null',
 'case x in pkill|killall) echo literal patterns;; esac',
 'printf \'%s\' "literal \\\\$\'quote\'"']

for index, command in enumerate(allowed, 1):
    check(f"allow {index}: {command!r}", command)

denied = ["ssh root@box \"'pkill' -f check-lock\"",
 "printf $'pkill\\0ignored'",
'pkill',
 'killall',
 '/usr/bin/pkill -f foo',
 '/usr/bin/killall node',
 '"pkill" -f foo',
 'p"ki"ll -f foo',
 'p\\kill -f foo',
 'pki\\\nll -f foo',
 'kill $(pgrep -f foo)',
 'kill `pgrep -f foo`',
 'kill "$pid"',
 'kill %1',
 'kill node',
 'kill 12345 foo',
 'kill 0',
 'kill -- -12345',
 'kill -TERM 0',
 'kill -TERM -- -12345',
 'kill -s "$signal" 12345',
 'kill 12345 2 > /dev/null; pkill foo',
 'fuser -k /tmp/example',
 'fuser -vk /tmp/example',
 'fuser --kill /tmp/example',
 'ps -p 1 && pkill -f foo',
 'ps -p 1 || killall node',
 'echo foo | pkill -f foo',
 '(ps -p 1; pkill -f foo)',
 'if true; then pkill -f foo; fi',
 '{ kill 12345; killall node; }',
 'echo "$(pkill -f foo)"',
 'echo `killall node`',
 'echo "${unset:-$(pkill -f foo)}"',
 'cat <<EOF\n$(pkill -f foo)\nEOF\n',
 'cat <<EOF\n`killall node`\nEOF\n',
 "bash <<'EOF'\npkill -f foo\nEOF\n",
 "cat <<'EOF'\npkill prose\nEOF\npkill -f foo",
 'gh issue create --body "$(cat <<\'EOF\'\npkill prose\nEOF\n)"; killall node',
 'command -- pkill -f foo',
 'builtin kill "$pid"',
 'exec /usr/bin/pkill foo',
 'env MODE=test pkill -f foo',
 'env -u UNUSED killall node',
 'sudo -n -u root pkill -f foo',
 'nohup pkill -f foo',
 'timeout 10 pkill -f foo',
 'nice -n 5 killall node',
 'xargs -n 1 pkill -f',
 'find . -exec pkill -f foo \\;',
 'bash -lc "pkill -f foo"',
 'sh -c "killall node"',
 'eval "pkill -f foo"',
 'kill 12345; "$(echo pkill)" -f foo',
 'echo pkill killall',
 'command -v pkill',
 'bash -c "echo pkill"',
 'find . -name pkill -print',
 'for f in src/*.c; do pkill -f check-lock; done',
 'case "$x" in a) pkill -f check-lock;; *) :;; esac',
 'case "$x" in a) case "$y" in b) killall node;; esac;; esac',
 'if [[ -f Makefile ]]; then pkill -f check-lock; fi',
 '[[ $(pkill -f check-lock) == x ]]',
 'echo $((1 + $(pkill -f foo)))',
 '((i=$(pkill -f foo)))',
 'for ((i=$(pkill -f foo);i<3;i++)); do :; done',
 'for f in $(pkill -f foo); do :; done',
 'case $(pkill -f foo) in a) :;; esac',
 'cat <(pkill -f check-lock)',
 "printf $'\\x70kill'",
 "$'\\160kill' -f check-lock",
 '$"pkill" -f foo',
 'k=pkill; "$k" -f check-lock',
 '"$PYTHON" -m pytest; echo "kill"',
 'flock /tmp/l pkill -f check-lock',
 'ssh root@box pkill -f pgwt',
 'su -c "pkill -f foo"',
 'doas pkill -f foo',
 'watch -n1 pkill -f foo',
 'stdbuf -oL pkill -f foo',
 'ionice -c3 pkill -f foo',
 'grep pkill file',
 'arbitrary-wrapper --unknown "pkill -f foo"',
 'killall5',
 'skill -c foo',
 'flock /tmp/l killall5',
 'ssh box "skill -c foo"',
 'pgrep -f check-lock | xargs kill 1',
 'xargs kill 12345 < pids.txt',
 'xargs kill',
 'xargs -n1 kill -TERM 12345',
 'flock /tmp/l xargs kill 1',
 'xargs "kill 12345"',
 'bash -c "echo safe; pkill -f foo"',
 'eval "echo safe; pkill -f foo"']

for index, command in enumerate(denied, 1):
    check(f"deny {index}: {command!r}", command, deny=True)

# Real parse errors stay closed and must not suggest pattern remediation.
syntax_errors = ['echo "unterminated',
 "cat <<'EOF'\nunterminated\n",
 'echo trailing\\',
 "printf $'unterminated",
 'echo $((1+2)',
 'if true; then echo yes']
for command in syntax_errors:
    check(f"syntax: {command!r}", command, deny=True, pattern=False)

for payload in ['null', '[]', '{}', '{"tool_input":null}', '{"tool_input":[]}',
                '{"tool_input":{"command":null}}', '{"tool_input":{"command":42}}',
                '{"tool_input":{"command":[]}}', '{"tool_input":{"command":{}}}',
                '{"tool_input":{"command":"ps\\u0000"}}', '{} {}']:
    check('invalid envelope ' + payload, raw=payload, deny=True)

with tempfile.TemporaryDirectory(prefix='kill-guard-selftest-') as directory:
    isolated = os.environ.copy()
    isolated['PATH'] = directory
    check('missing Python interpreter', 'ps aux', env=isolated, deny=True, show=True)
    fake = Path(directory) / 'python3'
    fake.write_text('#!/bin/bash\nprintf "%s\\n" "simulated interpreter failure" >&2\nexit 17\n')
    fake.chmod(0o755)
    check('internal interpreter failure', 'ps aux', env=isolated, deny=True, show=True)

with tempfile.TemporaryDirectory(prefix='kill-guard-replay-selftest-') as directory:
    replay = sys.argv[2]
    missing = subprocess.run([replay, str(Path(directory) / 'absent')], capture_output=True, text=True)
    if missing.returncode == 0 and 'SKIP' in missing.stdout and 'absent' in missing.stdout:
        passed += 1
    else:
        failed += 1
        print(f"FAIL replay absent directory: {missing!r}")
    marker = Path(directory) / 'must-not-execute'
    kill_command = 'pkill -f replay-fixture'
    records = [
        {'tool_name': 'Bash', 'tool_input': {'command': f'touch {marker}'}},
        {'message': {'content': [{'type': 'tool_use', 'name': 'Bash', 'input': {'command': kill_command}}]}},
        {'tool_name': 'Read', 'tool_input': {'command': 'killall ignored'}},
    ]
    (Path(directory) / 'fixture.jsonl').write_text(''.join(json.dumps(record) + '\n' for record in records))
    result = subprocess.run([replay, directory], capture_output=True, text=True, timeout=30)
    if (result.returncode == 0 and '2 commands replayed; 1 DENIED; 1 ALLOWED' in result.stdout
            and kill_command in result.stdout and not marker.exists()):
        passed += 1
    else:
        failed += 1
        print(f"FAIL replay fixture: {result!r}")

print(f"{passed} passed, {failed} failed")
if failed:
    sys.exit(1)

if sys.argv[3] == '--mutations':
    original = Path(guard).read_text()
    mutations = [
        ('deny exit allows', 'exit 2\n', 'exit 0\n'),
        ('pattern killers disabled', 'if name in {"pkill", "killall", "killall5", "skill"}:', 'if name in set():'),
        ('malformed envelopes allow', 'except BaseException as exc:',
         'except (json.JSONDecodeError, KeyError, TypeError, ValueError):\n    sys.exit(0)\nexcept BaseException as exc:'),
        ('numeric target validation disabled', 'def numeric_kill(args):', 'def numeric_kill(args):\n    return'),
        ('remediation removed', 'resolve the pattern with pgrep -fa <pattern>', 'remediation removed'),
        ('fuser kill detection disabled', 'raise PatternKill("fuser -k selects processes by file")', 'pass'),
        ('shell body inspection disabled', 'inspect(script.text)', 'pass'),
        ('eval body inspection disabled', 'inspect(" ".join(arg.text for arg in args))', 'pass'),
        ('dynamic executable check disabled',
         'if words[0].dynamic and words[0].text != "[":\n        if KILL_TOKEN.search(source):',
         'if words[0].dynamic and words[0].text != "[":\n        if False:'),
    ]

    def mutate(item):
        label, before, after = item
        if original.count(before) != 1:
            return f"FAIL mutation {label}: expected exactly one mutation site", False
        changed = original.replace(before, after, 1)
        with tempfile.TemporaryDirectory(prefix='kill-guard-mutation-') as directory:
            target = Path(directory) / 'kill-guard.sh'
            target.write_text(changed)
            target.chmod(0o755)
            test = Path(directory) / 'kill-guard-selftest.sh'
            shutil.copy2(Path(guard).with_name('kill-guard-selftest.sh'), test)
            shutil.copy2(sys.argv[2], Path(directory) / 'kill-guard-replay.sh')
            result = subprocess.run([str(test)], capture_output=True, text=True, timeout=300)
            summary = next((line for line in result.stdout.splitlines() if ' passed, ' in line), 'no test summary')
            # Valid source and a completed red suite, not a startup/syntax failure.
            valid = subprocess.run(['/bin/bash', '-n', str(target)], capture_output=True).returncode == 0
            python_source = changed[changed.index('import json'):changed.index("\n'; then")]
            compile(python_source, str(target), 'exec')
            counts = re.fullmatch(r'(\d+) passed, (\d+) failed', summary)
            good = valid and result.returncode == 1 and counts is not None and int(counts[2]) > 0
            return f"{'PASS' if good else 'FAIL'} mutation {label}: exit {result.returncode}; {summary}", good

    with ThreadPoolExecutor(max_workers=3) as pool:
        outcomes = list(pool.map(mutate, mutations))
    for message, good in outcomes:
        print(message)
    if not all(good for message, good in outcomes):
        sys.exit(1)
    print('9/9 mutations turned the self-test red')
sys.exit(0)
PY
