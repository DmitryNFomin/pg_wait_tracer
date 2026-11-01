#!/usr/bin/env bash
# Claude Code PreToolUse hook (Bash matcher only): five disarming rules.
# scripts/git-hooks/pre-push is the SOLE stamp/tree enforcement. Keep that
# enforcement at the real git invocation: no push detection, shell evaluation,
# repository resolution, or stamp checking belongs in this text guard (#643).
#
# Rules 1, 2, 4, 5 remain RAW-stdin substring tests, including their accepted
# false positives in documentation: PGWT_SKIP_PUSH_GUARD=1, core.hooksPath,
# --no-verify, git send-pack. Rule 3 alone examines command/operand boundaries
# to deny hook-path mutations, rather than mentioning a path near prose or an
# unrelated redirect. Quoted arguments and heredoc bodies are data; shell -c
# explicitly executes its operand. This is a bounded lexical check, not a
# general shell parser or a security boundary against hidden wrapper scripts.
#
# Under-denying can let someone disable the real pre-push hook, the dangerous
# direction. Over-denying costs a reformulation and makes this guard hard to
# discuss. Prefer precision over breadth; when precision is impossible, deny.
# Fed Claude Code JSON on stdin ({"tool_input":{"command":...}}).
set -uo pipefail
input=$(cat)

deny() {
    echo "blocked: command contains '$1' (push-guard denylist, #230) -- $2" >&2
    exit 2
}

case "$input" in
    *'PGWT_SKIP_PUSH_GUARD=1'*)
        deny "PGWT_SKIP_PUSH_GUARD=1" \
            "humans-only escape hatch (CLAUDE.md); an agent must not set it" ;;
esac

case "$input" in
    *'core.hooksPath'*)
        deny "core.hooksPath" \
            "this is the git config that arms scripts/git-hooks/pre-push" ;;
esac

case "$input" in
    *'git-hooks'*)
        # Only rule 3 needs lexical invocation/operand boundaries. This is
        # not push detection: no repository resolution or stamp enforcement.
        # Python source is intentionally literal.
        # shellcheck disable=SC2016
        if ! printf '%s' "$input" | python3 -c '
import json, re, sys

class Ambiguous(Exception):
    pass

# A bounded shell lexer, not a shell evaluator. Quotes group one operand;
# heredoc bodies are data; operators delimit invocations and redirects.
# Unsupported executable expansion involving the protected path fails closed.
def substitution(text, start, closing):
    ts, end = tokens(text, start, closing)
    if mutates_tokens(ts):
        raise Ambiguous("mutating command substitution")
    return end

def tokens(text, i=0, closing=None):
    out, word, active, pending, depth = [], "", False, [], 0
    def flush():
        nonlocal word, active
        if active:
            out.append(("word", word))
            word, active = "", False
    while i < len(text):
        c = text[i]
        if c == closing and depth == 0:
            flush()
            return out, i+1
        if c == "\\":
            if i + 1 == len(text):
                raise Ambiguous("unfinished escape")
            if text[i+1] != "\n":
                word += text[i+1]
                active = True
            i += 2
        elif c in (chr(39), chr(34)):
            quote = c
            active = True
            i += 1
            while i < len(text) and text[i] != quote:
                if quote == chr(34) and text[i] == "\\" and i+1 < len(text) and text[i+1] in "\\\"$`\n":
                    i += 1
                    if text[i] != "\n":
                        word += text[i]
                elif quote == chr(34) and (text.startswith("$(", i) or text[i] == "`"):
                    i = substitution(text, i+2 if text.startswith("$(", i) else i+1,
                                     ")" if text.startswith("$(", i) else "`")
                    word += "${expansion}"
                    continue
                else:
                    word += text[i]
                i += 1
            if i == len(text):
                raise Ambiguous("unfinished quote")
            i += 1
        elif text.startswith(("$(", "<(", ">("), i) or c == "`":
            i = substitution(text, i+1 if c == "`" else i+2, "`" if c == "`" else ")")
            word += "${expansion}"
            active = True
        elif c == "#" and not active:
            j = text.find("\n", i)
            i = len(text) if j < 0 else j
        elif c in " \t\r":
            flush()
            i += 1
        elif c in "\n;&|()<>":
            if c in "<>" and active and word.isdigit():
                # An adjacent IO number is part of the redirect, not argv[0].
                word, active = "", False
            flush()
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            op = next((op for op in ("<<-", "<<<", "&>>", "&>", ">>", ">|", ">&", "<&", "<>", "<<", "&&", "||", ";;", "|&") if text.startswith(op, i)), c)
            out.append(("op", op))
            i += len(op)
            if op in ("<<", "<<-"):
                # Read the delimiter as one shell word, preserving quote removal.
                match = re.match(r"[ \t]*(?:([\"\x27])(.*?)\1|([^\s;&|<>]+))", text[i:])
                if not match:
                    raise Ambiguous("heredoc delimiter")
                delimiter = match.group(2) if match.group(1) else match.group(3)
                out.append(("word", delimiter))
                pending.append((delimiter, op == "<<-", bool(match.group(1)), len(out)-1))
                i += match.end()
            if c == "\n" and pending:
                for delimiter, tabs, quoted, target_index in pending:
                    body = []
                    while True:
                        j = text.find("\n", i)
                        line = text[i:] if j < 0 else text[i:j]
                        i = len(text) if j < 0 else j+1
                        if (line.lstrip("\t") if tabs else line) == delimiter:
                            break
                        body.append(line)
                        if j < 0:
                            raise Ambiguous("unfinished heredoc")
                    body = "\n".join(body)
                    if not quoted and "git-hooks" in body and ("$(" in body or "`" in body):
                        raise Ambiguous("executable heredoc expansion")
                    out[target_index] = ("heredoc", body)
                pending = []
        else:
            active = True
            word += c
            i += 1
    flush()
    if pending:
        raise Ambiguous("unfinished heredoc")
    if closing is not None:
        raise Ambiguous("unfinished substitution")
    return out, i

def protected(word):
    # A path operand, not prose or a blob of source code. Keep the historical
    # git-hooks spelling coverage, including absolute paths and descendants.
    return "git-hooks" in word

MUTATORS = {"rm", "unlink", "mv", "cp", "install", "truncate", "tee", "chmod", "ln", "touch", "dd"}
REDIRECTS = {">", ">>", ">|", "&>", "&>>", ">&", "<>", "<", "<&", "<<", "<<-", "<<<"}

def invocation(words, heredocs=()):
    while words and (re.match(r"[A-Za-z_][A-Za-z_0-9]*=", words[0]) or words[0] in ("!", "{", "}", "then", "do", "else", "elif", "if", "while", "until")):
        words = words[1:]
    while words and words[0].rsplit("/", 1)[-1] in ("command", "builtin", "exec", "env", "sudo", "time"):
        words = words[1:]
        while words and (words[0].startswith("-") or re.match(r"[A-Za-z_][A-Za-z_0-9]*=", words[0])):
            if words[0] in ("-u", "-g", "-C", "--user", "--group", "--chdir", "--unset"):
                words = words[2:]
            else:
                words = words[1:]
    if not words:
        return False
    verb, args = words[0].rsplit("/", 1)[-1], words[1:]
    if "$" in verb and any(protected(w) for w in args):
        raise Ambiguous("expanded command name")
    if verb in ("eval", "source", ".") and any("git-hooks" in w for w in args):
        raise Ambiguous("indirect executable payload")
    if verb in ("sh", "bash", "zsh", "dash", "ksh"):
        if any("git-hooks" in body for body in heredocs):
            raise Ambiguous("shell executes hook-path heredoc")
        for index, arg in enumerate(args):
            if arg.startswith("-") and "c" in arg:
                if index+1 >= len(args):
                    raise Ambiguous("missing shell command")
                return mutates(args[index+1])
    if verb not in MUTATORS:
        return False
    if verb in ("cp", "install", "ln"):
        # Their source is read-only. -t/--target-directory changes which
        # operand is the destination; the usual form uses the final operand.
        operands, directory, index, options = [], None, 0, True
        while index < len(args):
            arg = args[index]
            if options and arg == "--":
                options = False
            elif options and arg in ("-t", "--target-directory"):
                if index+1 >= len(args):
                    raise Ambiguous("missing target directory")
                index += 1
                directory = args[index]
            elif options and (arg.startswith("--target-directory=") or arg.startswith("-t")):
                directory = arg.split("=", 1)[-1] if "=" in arg else arg[2:]
            elif options and arg in ("-m", "-o", "-g", "-S", "--mode", "--owner", "--group", "--suffix"):
                index += 1
            elif not options or not arg.startswith("-"):
                operands.append(arg)
            index += 1
        if verb == "install" and any(a in ("-d", "--directory") for a in args):
            targets = operands
        else:
            targets = [directory] if directory is not None else operands[-1:]
    elif verb == "dd":
        targets = [arg[3:] for arg in args if arg.startswith("of=")]
    else:
        targets = args
    return any(protected(w) for w in targets)

def mutates(command):
    return mutates_tokens(tokens(command)[0])

def mutates_tokens(ts):
    words, heredocs, i = [], [], 0
    while i < len(ts):
        kind, value = ts[i]
        if kind == "op" and value in REDIRECTS:
            if i+1 >= len(ts) or ts[i+1][0] not in ("word", "heredoc"):
                raise Ambiguous("redirect target")
            target = ts[i+1][1]
            if ts[i+1][0] == "heredoc":
                heredocs.append(target)
            if value in {">", ">>", ">|", "&>", "&>>", ">&", "<>"} and protected(target):
                return True
            i += 2
            continue
        if kind == "op":
            if invocation(words, heredocs):
                return True
            words, heredocs = [], []
        else:
            words.append(value)
        i += 1
    return invocation(words, heredocs)

try:
    command = json.load(sys.stdin)["tool_input"]["command"]
    if not isinstance(command, str):
        raise Ambiguous("command is not text")
    sys.exit(2 if mutates(command) else 0)
except (Ambiguous, ValueError, KeyError, TypeError) as exc:
    print("push-guard: ambiguous hook-path invocation: " + str(exc), file=sys.stderr)
    sys.exit(2)
'; then
            deny "write-shaped operation ... git-hooks" \
                "could remove, overwrite, or disable the pre-push hook (or is ambiguous)"
        fi
        ;;
esac

case "$input" in
    *'--no-verify'*)
        deny "--no-verify" \
            "the one flag that bypasses a pre-push hook outright" ;;
esac

case "$input" in
    *'git send-pack'*)
        deny "git send-pack" \
            "git plumbing bypasses the pre-push hook entirely (#687)" ;;
esac

exit 0
