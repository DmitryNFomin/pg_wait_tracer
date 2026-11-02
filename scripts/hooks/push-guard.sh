#!/usr/bin/env bash
# Claude Code PreToolUse hook (Bash matcher only): five disarming rules.
# scripts/git-hooks/pre-push is the SOLE stamp/tree enforcement. Keep that
# enforcement at the real git invocation: no shell evaluation, repository
# resolution, or stamp checking belongs in this text guard (#643).
#
# All five rules examine invocation/operand boundaries. Quoted prose, grep
# patterns, --body/--body-file payloads and data heredocs are data. Explicit
# executable contexts (shell -c, shell heredocs and substitutions) are checked.
# This is a bounded lexical check, not a general shell parser or a security
# boundary against hidden wrapper scripts. No payload files are opened.
#
# Under-denying can let someone disable the real pre-push hook, the dangerous
# direction. Over-denying makes the guard undiscussable, which happened with
# substring rules (#719). Prefer precision; when precision is impossible, deny.
# Fed Claude Code JSON on stdin ({"tool_input":{"command":...}}).
set -uo pipefail
input=$(cat)

deny() {
    echo "blocked: $1 (push-guard denylist, #230)" >&2
    exit 2
}

# Python source is intentionally literal; the command is JSON data, never eval.
# shellcheck disable=SC2016
if reason=$(printf '%s' "$input" | python3 -c '
import json, re, sys

class Ambiguous(Exception):
    pass

class Forbidden(Exception):
    pass

TERMS = ("PGWT_SKIP_PUSH_GUARD", "core.hookspath", "--no-v", "send-pack", "git-hooks", "git-push", "git-receive-pack")

def relevant(text):
    # This only avoids parsing unrelated, possibly oddly quoted commands.
    # Include split/escaped spellings; the decision still uses lexical argv.
    text = text.lower().replace(chr(39), "").replace(chr(34), "").replace("\\\n", "").replace("\\", "")
    return any(term.lower() in text for term in TERMS)


# A bounded shell lexer, not a shell evaluator. Quotes group one operand;
# heredoc bodies are data; operators delimit invocations and redirects.
# Unsupported executable expansion involving denylist terms fails closed.
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
                    if not quoted:
                        # Here quotes are literal data; only substitutions execute.
                        pos = 0
                        while pos < len(body):
                            if body[pos] == "\\":
                                pos += 2
                            elif body.startswith("$(", pos) or body[pos] == "`":
                                pos = substitution(body, pos+1 if body[pos] == "`" else pos+2,
                                                   "`" if body[pos] == "`" else ")")
                            else:
                                pos += 1
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
READERS = {"echo", "printf", "cat", "head", "tail", "grep", "rg", "sed", "awk", "less"}
GIT_PLUMBING = {"git-send-pack", "git-push", "git-receive-pack"}
REDIRECTS = {">", ">>", ">|", "&>", "&>>", ">&", "<>", "<", "<&", "<<", "<<-", "<<<"}

def plumbing_name(word):
    # Exact binary name only (any path prefix), never a substring or pattern —
    # a general git-* pattern also matches the git-hooks path operand and
    # reintroduces the #719 false positive. git-push and git-receive-pack are
    # the obvious symmetry with the proven git-send-pack case (#687).
    return word.rsplit("/", 1)[-1] in GIT_PLUMBING

def skip_assignment(word):
    if word.startswith("PGWT_SKIP_PUSH_GUARD="):
        value = word.split("=", 1)[1]
        # Expansion can resolve to 1; known inert values need no denial.
        return value == "1" or "$" in value or "`" in value
    return None

def git_config(operands):
    # Parse the key position, keeping query patterns, file paths and values as
    # data. A bare key reads; a key plus value writes. Both legacy flags and
    # the newer config subcommands can select a write explicitly.
    reads = {"--get", "--get-all", "--get-regexp", "--get-urlmatch", "--list", "-l"}
    writes = {"--add", "--replace-all", "--unset", "--unset-all"}
    read, write, positional, index, options = False, False, [], 0, True
    if operands and operands[0] in ("get", "list", "set", "unset"):
        read = operands[0] in ("get", "list")
        write = not read
        operands = operands[1:]
    while index < len(operands):
        arg = operands[index]
        if options and arg == "--":
            options = False
        elif options and arg.startswith("-"):
            read = read or arg in reads
            write = write or arg in writes
            if arg in ("-f", "--file", "--blob", "-t", "--type", "--default", "--value", "--comment"):
                index += 1
        else:
            positional.append(arg)
        index += 1
    if positional and not read and (write or len(positional) > 1):
        key = positional[0]
        if "$" in key:
            raise Ambiguous("expanded git config write key")
        if key.lower() == "core.hookspath":
            raise Forbidden("core.hooksPath: config write can permanently disable the real hook")

def git_invocation(args, skip):
    if skip:
        raise Forbidden("PGWT_SKIP_PUSH_GUARD=1: assignment enables the humans-only escape hatch")
    index = 0
    while index < len(args):
        arg = args[index]
        if arg == "-c" or (arg.startswith("-c") and len(arg) > 2):
            if arg == "-c":
                index += 1
                if index == len(args):
                    raise Ambiguous("missing git -c value")
                config = args[index]
            else:
                config = arg[2:]
            key, separator, _ = config.partition("=")
            if key.lower() == "core.hookspath":
                if not separator:
                    raise Ambiguous("git hooksPath override without a value")
                raise Forbidden("core.hooksPath: git -c overrides the real hook directory")
            if "$" in key:
                raise Ambiguous("expanded git config key")
        elif arg in ("-C", "--git-dir", "--work-tree", "--namespace", "--config-env"):
            index += 1
            if index == len(args):
                raise Ambiguous("missing git global option value")
            if arg == "--config-env":
                key = args[index].split("=", 1)[0]
                if key.lower() == "core.hookspath" or "$" in key:
                    raise Ambiguous("environment-supplied hook directory")
        elif arg.startswith("--config-env="):
            key = arg.split("=", 2)[1]
            if key.lower() == "core.hookspath" or "$" in key:
                raise Ambiguous("environment-supplied hook directory")
        elif arg == "--":
            index += 1
            break
        elif not arg.startswith("-"):
            break
        index += 1
    if index == len(args):
        return False
    subcommand, operands = args[index], args[index+1:]
    if "$" in subcommand and any(relevant(a) for a in operands):
        raise Ambiguous("expanded git subcommand")
    if subcommand == "send-pack":
        raise Forbidden("git send-pack: plumbing bypasses the pre-push hook entirely (#687)")
    if subcommand == "config":
        return git_config(operands)
    if subcommand != "push":
        return False
    index = 0
    while index < len(operands):
        arg = operands[index]
        if arg == "--":
            break
        if "$" in arg:
            raise Ambiguous("expanded git push flag or operand")
        if len(arg) >= len("--no-v") and "--no-verify".startswith(arg):
            raise Forbidden("--no-verify: git push flag bypasses the pre-push hook")
        # These push options take data operands, even if those spell a flag.
        if arg in ("--repo", "--receive-pack", "--exec", "-o", "--push-option", "--recurse-submodules"):
            index += 1
        index += 1
    return False

def invocation(words, heredocs=(), state=None):
    state = {} if state is None else state
    skip = state.get("skip", False)
    hook_config = state.get("hook_config", False)
    wrapped = False
    while words:
        if re.match(r"GIT_CONFIG_KEY_[0-9]+=(?i:core\.hookspath)$", words[0]):
            hook_config = True
        assignment = skip_assignment(words[0])
        if assignment is not None:
            skip = assignment
        if re.match(r"[A-Za-z_][A-Za-z_0-9]*=", words[0]) or words[0] in ("!", "{", "}", "then", "do", "else", "elif", "if", "while", "until"):
            words = words[1:]
        # Normalize shell/env prefixes for assignment scope and non-git
        # mutations too. Launcher coverage below does not depend on this list.
        elif words[0].rsplit("/", 1)[-1] in ("command", "builtin", "exec", "env", "sudo", "time"):
            wrapper = words[0].rsplit("/", 1)[-1]
            wrapped = True
            words = words[1:]
            while words and words[0].startswith("-"):
                option = words[0]
                if wrapper == "env" and option.startswith(("-S", "--split-string")):
                    raise Ambiguous("env executes a split command string")
                if wrapper == "env" and option in ("--unset=PGWT_SKIP_PUSH_GUARD", "-i", "--ignore-environment"):
                    skip = False
                if wrapper == "env" and option in ("-i", "--ignore-environment"):
                    hook_config = False
                if option in ("-u", "-g", "-C", "--user", "--group", "--chdir", "--unset"):
                    if len(words) < 2:
                        raise Ambiguous("missing wrapper option value")
                    if wrapper == "env" and option in ("-u", "--unset") and words[1] == "PGWT_SKIP_PUSH_GUARD":
                        skip = False
                    words = words[2:]
                else:
                    words = words[1:]
        else:
            break
    if not words:
        if not wrapped:
            state["skip"] = skip
            state["hook_config"] = hook_config
        return False
    verb, args = words[0].rsplit("/", 1)[-1], words[1:]
    if "$" in verb and any(relevant(w) for w in args):
        raise Ambiguous("expanded command name")
    if verb in ("eval", "source", ".") and any(relevant(w) for w in args):
        raise Ambiguous("indirect executable payload")
    if verb == "export":
        for arg in args:
            if re.match(r"GIT_CONFIG_KEY_[0-9]+=(?i:core\.hookspath)$", arg):
                state["hook_config"] = True
            assignment = skip_assignment(arg)
            if assignment is not None:
                state["skip"] = assignment
        return False
    if verb == "unset" and "PGWT_SKIP_PUSH_GUARD" in args:
        state["skip"] = False
        return False
    if verb == "git":
        if hook_config:
            raise Forbidden("core.hooksPath: GIT_CONFIG_KEY environment overrides the real hook directory")
        return git_invocation(args, skip)
    if plumbing_name(verb):
        raise Forbidden(f"{verb}: plumbing bypasses the pre-push hook entirely (#687)")
    if verb in ("sh", "bash", "zsh", "dash", "ksh"):
        index, stdin_script = 0, True
        while index < len(args):
            arg = args[index]
            if arg == "--":
                stdin_script = index+1 == len(args)
                break
            if not arg.startswith(("-", "+")) or arg == "-":
                stdin_script = arg == "-"
                break
            if arg.startswith("-") and not arg.startswith("--") and "c" in arg:
                if index+1 >= len(args):
                    raise Ambiguous("missing shell command")
                return mutates(args[index+1], {"skip": skip, "hook_config": hook_config})
            if arg in ("-o", "+o", "-O", "+O", "--rcfile", "--init-file"):
                index += 1
            if arg.startswith("-") and not arg.startswith("--") and "s" in arg:
                break
            index += 1
        if stdin_script:
            for body in heredocs:
                if mutates(body, {"skip": skip, "hook_config": hook_config}):
                    return True
    # Shell command strings/heredocs above are executable; script arguments
    # are data, just as gh body/comment payloads are data. Preserve those
    # established boundaries before applying the unknown-launcher fallback.
    if verb in ("sh", "bash", "zsh", "dash", "ksh"):
        return False
    scan = []
    index = 0
    while index < len(args):
        arg = args[index]
        if verb == "gh" and arg in ("--body", "--body-file", "--comment", "--title", "-b", "-t"):
            index += 2
            continue
        if not (verb == "gh" and arg.startswith(("--body=", "--body-file=", "--comment=", "--title="))):
            scan.append(arg)
        index += 1
    # Invert launcher recognition over operands: a reader argv is data to this
    # check. Unknown verbs are denied when git and a denylist term occur together,
    # or when an operand names a known plumbing binary.
    # Interpreters evaluating program-string arguments (awk, GNU sed e, perl -e,
    # python3 -c, and the like) are outside this check entirely. READERS membership
    # does not change that limit: the gap is string evaluation, not launcher names.
    # Like #688 and #708, this is a lexical tripwire over Claude Code command text,
    # not a boundary against an interpreter, wrapper script or binary executing
    # its own code; wrapper file contents are never inspected. See #688, #708.
    git_token = any(re.match(r"(?:[^\s]+/)?git(?:\s|$)", arg) for arg in scan)
    if verb not in READERS and git_token and (skip or hook_config or any(relevant(arg) for arg in scan)):
        raise Forbidden("unknown executable context contains git and a push-guard denylist term")
    plumbing_hit = any(plumbing_name(arg) for arg in scan)
    if verb not in READERS and plumbing_hit:
        raise Forbidden("plumbing binary reached through an unrecognized launcher: bypasses the pre-push hook entirely (#687)")
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

def mutates(command, state=None):
    return mutates_tokens(tokens(command)[0], state)

def mutates_tokens(ts, state=None):
    state = {} if state is None else state
    words, heredocs, i = [], [], 0
    scopes, pipeline = [], None
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
            before = state.copy()
            if invocation(words, heredocs, state):
                return True
            # Environment changes in a subshell/pipeline cannot arm a later
            # invocation in its parent shell. Braced groups share state.
            if value in ("|", "|&"):
                pipeline = before if pipeline is None else pipeline
                state = pipeline.copy()
            elif pipeline is not None:
                state, pipeline = pipeline, None
            if value == "(":
                scopes.append(state)
                state = state.copy()
            elif value == ")" and scopes:
                state = scopes.pop()
            words, heredocs = [], []
        else:
            words.append(value)
        i += 1
    return invocation(words, heredocs, state)

try:
    command = json.load(sys.stdin)["tool_input"]["command"]
    if not isinstance(command, str):
        raise Ambiguous("command is not text")
    # Keep unrelated commands untouched, including deliberately odd quoting.
    if relevant(command) and mutates(command):
        raise Forbidden("write-shaped operation ... git-hooks: could remove, overwrite, or disable the pre-push hook")
except Forbidden as exc:
    print(str(exc))
    sys.exit(2)
except (Ambiguous, ValueError, KeyError, TypeError, RecursionError) as exc:
    print("ambiguous invocation: " + str(exc))
    sys.exit(2)
'); then
    exit 0
else
    deny "${reason:-ambiguous invocation: parser unavailable}"
fi
