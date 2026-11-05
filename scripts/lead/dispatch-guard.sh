#!/usr/bin/env bash
# PreToolUse (Agent|Bash): refuse dispatches with incomplete live-comment evidence.
set -u
command -v python3 >/dev/null 2>&1 || {
  echo 'DISPATCH GUARD ERROR: python3 unavailable; cannot inspect dispatch' >&2
  exit 2
}
HOOK_DIR=$(cd "$(dirname "$0")" && pwd) || exit 2
python3 /dev/fd/3 "$HOOK_DIR" 3<<'PY'
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

GUIDANCE = '''Paste the comments fetch command and its full output in a fence for every dispatched issue/PR:
```console
gh api repos/{owner}/{repo}/issues/N/comments --paginate
<paste the full output here, including every comment id and body>
```
Then name the controlling live comment and quote at least 20 characters from it:
CONTROLLING SCOPE DECISION: #N comment <id> "<quote>"
For an issue with no comments, paste [] or (no output) and use:
CONTROLLING SCOPE DECISION: #N none (0 comments)'''


def refuse(reason):
    print(reason, file=sys.stderr)
    print(GUIDANCE, file=sys.stderr)
    return 2


def brief_paths(command):
    """Inspect literal shell invocations; never execute or expand the command."""
    if "codex-task.sh" not in command:
        return []
    lexer = shlex.shlex(command, posix=True, punctuation_chars=";&|()<>\n")
    lexer.whitespace = " \t\r"
    lexer.whitespace_split = True
    lexer.commenters = "#"
    tokens = list(lexer)
    paths = []
    command_position = True
    shell_wrapper = False
    wrappers = {"bash", "sh", "zsh", "env", "command", "exec", "sudo"}
    for index, token in enumerate(tokens):
        if token and all(c in ";&|()\n" for c in token):
            command_position = True
            shell_wrapper = False
        elif command_position:
            name = os.path.basename(token)
            if shell_wrapper and re.fullmatch(r"-[a-zA-Z]*c[a-zA-Z]*", token):
                if index + 1 < len(tokens):
                    paths.extend(brief_paths(tokens[index + 1]))
                command_position = False
            elif name == "codex-task.sh":
                if index + 2 >= len(tokens) or tokens[index + 1] in (";", "&&", "||", "\n"):
                    raise ValueError("codex-task.sh needs a readable brief file and branch")
                paths.append(tokens[index + 1])
                command_position = False
            elif name in wrappers:
                shell_wrapper = name in {"bash", "sh", "zsh"}
            elif token.startswith("-") or re.match(r"^[A-Za-z_][A-Za-z_0-9]*=", token):
                continue
            else:
                command_position = False
    return paths


def main():
    try:
        event = json.load(sys.stdin)
        if not isinstance(event, dict) or not isinstance(event.get("tool_name"), str):
            raise ValueError("expected an event object with tool_name")
        tool = event["tool_name"]
        if tool not in ("Agent", "Bash"):
            return 0
        data = event.get("tool_input")
        if not isinstance(data, dict):
            raise ValueError("expected tool_input object")
        if tool == "Bash":
            command = data.get("command")
            if not isinstance(command, str):
                raise ValueError("expected Bash command string")
            paths = brief_paths(command)
            if not paths:
                return 0
            if event.get("cwd"):
                os.chdir(event["cwd"])
            briefs = [Path(path).read_text(encoding="utf-8") for path in paths]
        else:
            if (data.get("isolation") != "worktree"
                    and data.get("subagent_type", "") not in
                    (None, "", "codex", "claude", "general-purpose")):
                return 0
            prompt = data.get("prompt")
            if not isinstance(prompt, str):
                raise ValueError("expected Agent prompt string")
            # Quoted paths may contain spaces. Bare paths stop at prose delimiters.
            paths = re.findall(r'''["'`](/[^"'`\n]+\.(?:md|txt))["'`]|(?<![\w/])(/[^\s"'`<>]+?\.(?:md|txt))(?=$|[\s`"'.,;:)\]])''', prompt)
            contents = [prompt]
            for quoted, bare in paths:
                path = Path(quoted or bare)
                if path.is_file():
                    contents.append(path.read_text(encoding="utf-8"))
            briefs = ["\n".join(contents)]
        for brief in briefs:
            with tempfile.TemporaryDirectory(prefix="dispatch-guard-") as directory:
                path = Path(directory) / "brief.md"
                path.write_text(brief, encoding="utf-8")
                result = subprocess.run(
                    ["bash", str(Path(sys.argv[1]) / "check-brief.sh"), str(path)],
                    capture_output=True, text=True)
            if result.returncode:
                return refuse(result.stdout + result.stderr)
        return 0
    except (OSError, ValueError, TypeError, UnicodeError) as exc:
        return refuse(f"DISPATCH GUARD ERROR: cannot inspect dispatch: {exc}")


sys.exit(main())
PY
rc=$?
case "$rc" in
  0|2) exit "$rc" ;;
  *) echo "DISPATCH GUARD ERROR: python3 failed (exit $rc)" >&2; exit 2 ;;
esac
