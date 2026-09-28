#!/usr/bin/env python3
"""Resolve the working directory of a git push in Claude's Bash tool input.

Exit 0 for no push, 1 with its directory on stdout for a resolved push,
and 2 when a push cannot be resolved safely.
"""

import json
import os
import re
import shlex
import sys


SEPARATORS = {";", "&&", "||", "|", "&", "\n", "(", ")"}
VALUE_OPTIONS = {"-c", "--config-env", "--git-dir", "--work-tree", "--namespace", "--exec-path"}
UNSAFE_OPTIONS = {"--git-dir", "--work-tree", "--namespace"}
FLAG_OPTIONS = {"-p", "-P", "--paginate", "--no-pager", "--no-replace-objects",
                "--no-optional-locks", "--literal-pathspecs", "--glob-pathspecs",
                "--noglob-pathspecs", "--icase-pathspecs"}
REPO_ENV = {"GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_CEILING_DIRECTORIES"}


def git_target(args, cwd):
    safe = True
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--":
            i += 1
            break
        if arg == "-C" or arg in VALUE_OPTIONS:
            if i + 1 >= len(args):
                return False, None
            if arg == "-C":
                cwd = os.path.abspath(os.path.join(cwd, args[i + 1]))
            elif arg in UNSAFE_OPTIONS:
                safe = False
            i += 2
        elif arg.startswith("-C") and len(arg) > 2:
            cwd = os.path.abspath(os.path.join(cwd, arg[2:]))
            i += 1
        elif any(arg.startswith(option + "=") for option in VALUE_OPTIONS):
            if any(arg.startswith(option + "=") for option in UNSAFE_OPTIONS):
                safe = False
            i += 1
        elif arg in FLAG_OPTIONS:
            i += 1
        elif arg.startswith("-"):
            safe = False
            i += 1
        else:
            return arg == "push", cwd if safe else None
    return i < len(args) and args[i] == "push", cwd if safe else None


def targets(command, start=None):
    lexer = shlex.shlex(command, posix=True, punctuation_chars=";&|()<>")
    lexer.whitespace = " \t\r"
    lexer.commenters = "#"
    raw = list(lexer)
    tokens = []
    i = 0
    while i < len(raw):
        if raw[i] != "<<" or i + 1 >= len(raw):
            tokens.append(raw[i])
            i += 1
            continue
        delimiter = raw[i + 1]
        i += 2
        while i < len(raw) and raw[i] != "\n":
            tokens.append(raw[i])
            i += 1
        if i == len(raw):
            raise ValueError("unterminated here-document")
        tokens.append("\n")
        i += 1
        while i < len(raw):
            line = []
            while i < len(raw) and raw[i] != "\n":
                line.append(raw[i])
                i += 1
            i += 1
            if line == [delimiter]:
                break
        else:
            raise ValueError("unterminated here-document")
    cwd = start or os.getcwd()
    found = []
    segment = []
    uncertain = any(name in os.environ for name in REPO_ENV)

    def process(parts):
        nonlocal cwd, uncertain
        while parts and re.match(r"^[A-Za-z_][A-Za-z_0-9]*=", parts[0]):
            uncertain = uncertain or parts[0].split("=", 1)[0] in REPO_ENV
            parts = parts[1:]
        if parts and parts[0] in {"env", "command"}:
            parts = parts[1:]
            while parts and re.match(r"^[A-Za-z_][A-Za-z_0-9]*=", parts[0]):
                uncertain = uncertain or parts[0].split("=", 1)[0] in REPO_ENV
                parts = parts[1:]
        if not parts:
            return
        for part in parts:
            if "$(" in part or "`" in part:
                inner = re.findall(r"\$\((.*?)\)|`(.*?)`", part)
                for dollar, backtick in inner:
                    found.extend(None if uncertain else target
                                 for target in targets(dollar or backtick, cwd))
        if parts[0] in {"pushd", "popd", "export"}:
            uncertain = True
        elif parts[0] == "cd":
            if len(parts) != 2 or parts[1].startswith("-"):
                uncertain = True
            else:
                cwd = os.path.abspath(os.path.join(cwd, parts[1]))
                if not os.path.isdir(cwd):
                    uncertain = True
        elif parts[0] == "git" or parts[0].endswith("/git"):
            push, target = git_target(parts[1:], cwd)
            if push:
                found.append(None if uncertain else target)
        elif (parts[0] in {"bash", "sh", "zsh"} and len(parts) >= 3
              and parts[1].startswith("-") and "c" in parts[1]):
            found.extend(None if uncertain else target for target in targets(parts[2], cwd))
        elif parts[0] == "eval":
            found.extend(None if uncertain else target
                         for target in targets(" ".join(parts[1:]), cwd))
        else:
            # Unknown wrappers (for example sudo) can execute a git argument.
            # Detect its subcommand but refuse to guess the wrapper's cwd.
            for i, word in enumerate(parts[1:], 1):
                if word == "git" or word.endswith("/git"):
                    push, _ = git_target(parts[i + 1:], cwd)
                    if push:
                        found.append(None)

    i = 0
    while i < len(tokens):
        token = tokens[i]
        if token in {"<", ">", ">>"}:
            i += 2
            continue
        if token in SEPARATORS:
            process(segment)
            if segment and segment[0] == "cd" and token != "&&":
                uncertain = True
            segment = []
        else:
            segment.append(token)
        i += 1
    process(segment)
    return found


def main():
    try:
        command = json.load(sys.stdin)["tool_input"]["command"]
        if not isinstance(command, str):
            raise ValueError("command is not a string")
        matches = targets(command)
    except (ValueError, KeyError, TypeError) as exc:
        print(f"invalid or unparseable tool command: {exc}")
        return 2
    if not matches:
        return 0
    if len(matches) != 1 or matches[0] is None:
        print("ambiguous push command or repository")
        return 2
    print(matches[0])
    return 1


if __name__ == "__main__":
    sys.exit(main())
