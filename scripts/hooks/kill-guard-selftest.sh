#!/bin/bash
# Standalone protocol tests. Commands under test are strings, NEVER executed.
set -uo pipefail
guard_dir=$(cd -- "$(dirname -- "$0")" && pwd) || exit 1
command python3 -I -u - "$guard_dir/kill-guard.sh" <<'PY'
import json
import os
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


def check(label, command=None, deny=False, raw=None, env=None, show=False):
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
            good &= all(part in result.stderr for part in alternative)
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
check('R8 quoted mention', 'printf "%s\\n" "pkill -f scripts/check-lock"', show=True)
check('R8 quoted heredoc mention', "gh issue create --body-file - <<'EOF'\npkill -f scripts/check-lock\nEOF\n", show=True)
check('R9 ps -p 1; kill 1; pkill -f foo', 'ps -p 1; kill 1; pkill -f foo', deny=True, show=True)

allowed = [
    'kill 0012345', 'kill -9 12345', 'kill -s TERM 12345',
    'kill --signal=TERM 12345', 'kill -n 15 12345', 'kill -- 12345 12346',
    'kill -0 12345', 'kill -l', 'kill -l TERM',
    '/bin/kill -TERM 12345', 'builtin kill 12345', 'command kill 12345',
    'kill 12345; ps -p 12345', 'ps -p 1; kill -TERM 12345 12346',
    'kill 12345 2>/dev/null', 'kill 12345 >/dev/null',
    'ps aux', 'lsof -p 12345', 'fuser /tmp/example', 'fuser -v /tmp/example',
    'pgrep -fa "pkill|killall"', 'ps -p "$pid"',
    'echo pkill killall', 'echo "$(pgrep -fa check-lock)"',
    "printf %s '$(pkill -f foo)'", "printf %s '`pkill -f foo`'",
    'echo "a quoted ; pkill -f foo"', 'command -v pkill',
    'echo safe # pkill -f foo', 'echo safe\\\nword',
    "cat <<EOF\npkill -f foo\nEOF\n",
    "cat <<'EOF'\n$(pkill -f foo)\n`killall node`\nEOF\n",
    'cat <<"EOF"\npkill -f foo\nEOF\n',
    "cat <<E'OF'\npkill -f foo\nEOF\n",
    "cat <<-'EOF'\n\tpkill -f foo\n\tEOF\n",
    "cat <<'EOF'\nunmatched quote: '\nunmatched parenthesis: )\nEOF\n",
    "cat <<'ONE' <<'TWO'\npkill foo\nONE\nkillall node\nTWO\n",
    'gh issue create --body "$(cat <<\'EOF\'\npkill -f foo\nEOF\n)"',
    'bash -c "echo pkill"', 'bash -c "kill 12345"',
    'env MODE=test kill -TERM 12345', 'sudo -n kill 12345',
    'find . -name pkill -print', '',
]
for index, command in enumerate(allowed, 1):
    check(f"allow {index}: {command!r}", command)

denied = [
    'pkill', 'killall', '/usr/bin/pkill -f foo', '/usr/bin/killall node',
    '"pkill" -f foo', 'p"ki"ll -f foo', 'p\\kill -f foo', 'pki\\\nll -f foo',
    'kill $(pgrep -f foo)', 'kill `pgrep -f foo`', 'kill "$pid"',
    'kill %1', 'kill node', 'kill 12345 foo', 'kill 0', 'kill -- -12345',
    'kill -TERM 0', 'kill -TERM -- -12345', 'kill -s "$signal" 12345',
    'kill 12345 2 > /dev/null; pkill foo',
    'fuser -k /tmp/example', 'fuser -vk /tmp/example', 'fuser --kill /tmp/example',
    'ps -p 1 && pkill -f foo', 'ps -p 1 || killall node',
    'echo foo | pkill -f foo', '(ps -p 1; pkill -f foo)',
    'if true; then pkill -f foo; fi', '{ kill 12345; killall node; }',
    'echo "$(pkill -f foo)"', 'echo `killall node`',
    'echo "${unset:-$(pkill -f foo)}"',
    'cat <<EOF\n$(pkill -f foo)\nEOF\n',
    'cat <<EOF\n`killall node`\nEOF\n',
    "bash <<'EOF'\npkill -f foo\nEOF\n",
    "cat <<'EOF'\npkill prose\nEOF\npkill -f foo",
    'gh issue create --body "$(cat <<\'EOF\'\npkill prose\nEOF\n)"; killall node',
    'command -- pkill -f foo', 'builtin kill "$pid"', 'exec /usr/bin/pkill foo',
    'env MODE=test pkill -f foo', 'env -u UNUSED killall node',
    'sudo -n -u root pkill -f foo', 'nohup pkill -f foo',
    'timeout 10 pkill -f foo', 'nice -n 5 killall node',
    'xargs -n 1 pkill -f', 'find . -exec pkill -f foo \\;',
    'bash -lc "pkill -f foo"', 'sh -c "killall node"',
    'eval "pkill -f foo"', 'eval "$command"', '$killer -f foo',
    'kill 12345; "$(echo pkill)" -f foo',
    'echo "unterminated', "cat <<'EOF'\nunterminated\n",
]
for index, command in enumerate(denied, 1):
    check(f"deny {index}: {command!r}", command, deny=True)

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

print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
PY
