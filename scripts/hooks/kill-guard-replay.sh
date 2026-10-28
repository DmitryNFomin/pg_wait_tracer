#!/bin/bash
# Replay historical Bash inputs through the sibling guard; NEVER execute them.
# Optional argument overrides the transcript directory (also useful for fixtures).
# Denials are reported in full for review; only replay/protocol errors fail.
set -uo pipefail
guard_dir=$(cd -- "$(dirname -- "$0")" && pwd) || exit 1
command python3 -I -u - "$guard_dir/kill-guard.sh" "${1:-$HOME/.claude/projects/-Users-dmitryfomin-work-git-pg-wait-tracer}" <<'PY'
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys


guard, directory = sys.argv[1], Path(sys.argv[2])
if not directory.is_dir():
    print(f"SKIP kill-guard replay: transcript directory absent: {directory}")
    sys.exit(0)


def commands():
    for path in sorted(directory.glob('*.jsonl')):
        with path.open(encoding='utf-8') as stream:
            for number, line in enumerate(stream, 1):
                record = json.loads(line)
                # Hook envelopes and Claude session tool_use records carry the
                # same tool_input.command under different container keys.
                if record.get('tool_name') == 'Bash':
                    yield str(path), number, record['tool_input']['command']
                message = record.get('message')
                content = message.get('content') if isinstance(message, dict) else None
                if isinstance(content, list):
                    for block in content:
                        if isinstance(block, dict) and block.get('type') == 'tool_use' and block.get('name') == 'Bash':
                            yield str(path), number, block['input']['command']


def replay(item):
    path, number, command = item
    if not isinstance(command, str):
        raise ValueError(f"non-string Bash command at {path}:{number}")
    result = subprocess.run(
        [guard], input=json.dumps({'tool_name': 'Bash', 'tool_input': {'command': command}}),
        text=True, capture_output=True, timeout=15,
    )
    if result.returncode not in {0, 2} or result.stdout or (result.returncode == 0 and result.stderr):
        raise RuntimeError(f"guard protocol failure at {path}:{number}: {result!r}")
    return item, result


total = denied = 0
try:
    # Materialize only extracted commands, never transcript output or tool results.
    inputs = list(commands())
    with ThreadPoolExecutor(max_workers=8) as pool:
        for (path, number, command), result in pool.map(replay, inputs):
            total += 1
            if result.returncode == 2:
                denied += 1
                print(f"DENIED {denied} ({path}:{number})\n--- command ---")
                print(command)
                print('--- diagnostic ---')
                print(result.stderr, end='' if result.stderr.endswith('\n') else '\n')
                print('--- end denial ---', flush=True)
except Exception as exc:
    print(f"ERROR kill-guard replay: {exc}", file=sys.stderr)
    sys.exit(1)
print(f"Replay totals: {total} commands replayed; {denied} DENIED; {total - denied} ALLOWED.")
if not total:
    print(f"SKIP kill-guard replay: no historical Bash commands in {directory}")
PY
