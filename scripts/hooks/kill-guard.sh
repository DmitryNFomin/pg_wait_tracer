#!/bin/bash
# Claude Code PreToolUse hook for Bash; deliberately not registered in settings.
# Over-denying costs an agent one reformulated command; under-denying costs
# every concurrent agent its test run. Fail CLOSED on malformed input, missing
# interpreters, genuinely malformed syntax, and internal errors (including Python failure).
# Tripwire against the natural spelling of a pattern kill, not a security boundary.
# Indirect execution through interpreter payloads, script files, or functions defined
# earlier is out of scope, tracked in #708; inherited aliases are not resolved either.
# Register under matcher "Bash" EXACTLY: tool_name is unchecked, so a non-Bash
# payload denies with a bare diagnostic. Registration remains a separate change.
set -uo pipefail

# Keep stdin for json.load, and keep the denial path entirely in shell builtins.
# In particular, do not exec Python: even failure to start it must exit 2.
# Shell metacharacters below are Python source, not shell expansions.
# shellcheck disable=SC2016
if command python3 -I -c '
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass


class Denied(Exception):
    pass


class PatternKill(Denied):
    pass


KILLERS = {"kill", "pkill", "killall", "killall5", "skill", "fuser", "xargs"}
KILL_TOKEN = re.compile(r"(?<![A-Za-z0-9_])(?:" + "|".join(KILLERS) + r")(?![A-Za-z0-9_])")
REMEDIATION = "resolve the pattern with pgrep -fa <pattern>, confirm each pid\x27s working directory belongs to this worktree, then kill <pid>."


@dataclass
class Word:
    text: str
    dynamic: bool = False
    quoted: bool = False
    start: int = 0
    end: int = 0


class Shell:
    """Lex shell words without executing them; inspect executable positions.

    Quotes remain data, except for actual substitutions. Heredoc bodies are
    consumed by line, not tokenized as commands. Malformed constructs deny.
    """

    operators = re.compile(r"&&|\|\||;;&|;;|;&|&>>|&>|<<<|<<-|<<|>>|<>|>&|<&|>\||[;|&()<>\n]")
    redirects = {"<", ">", ">>", "<<", "<<-", "<<<", "<>", ">&", "<&", "&>", "&>>", ">|"}

    def __init__(self, source):
        self.source = source
        self.appended = False
        self.heredoc_spans = []
        self.case_separators = []
        self.i = 0

    def substitution(self):
        if self.source.startswith("$((", self.i):
            self.i += 3
            self.arithmetic()
            return
        self.i += 2
        self.scan(nested=True)

    def arithmetic(self):
        # Arithmetic is data, but command substitutions inside it execute.
        depth = 0
        while self.i < len(self.source):
            if not depth and self.source.startswith("))", self.i):
                self.i += 2
                return
            char = self.source[self.i]
            if char == "$":
                self.dollar()
            elif char == "`":
                self.backtick()
            elif char == "\\":
                self.i += 2
            else:
                depth += (char == "(") - (char == ")")
                self.i += 1
        raise Denied("unterminated arithmetic")

    def conditional(self):
        # [[ expressions ]] are data too; word() still walks substitutions.
        while self.i < len(self.source):
            if self.source.startswith("]]", self.i):
                self.i += 2
                return
            if self.source[self.i] in " \t\r\n;|&()<>":
                self.i += 1
            else:
                self.word()
        raise Denied("unterminated conditional")

    def ansi_quote(self):
        self.i += 2
        result = ""
        escapes = {"a": "\a", "b": "\b", "e": "\x1b", "E": "\x1b",
                   "f": "\f", "n": "\n", "r": "\r", "t": "\t", "v": "\v"}
        while self.i < len(self.source):
            char = self.source[self.i]
            self.i += 1
            if char == "\x27":
                # Bash words end at the first decoded NUL byte.
                return result.split("\x00", 1)[0]
            if char != "\\":
                result += char
                continue
            if self.i == len(self.source):
                break
            char = self.source[self.i]
            self.i += 1
            if char in escapes:
                result += escapes[char]
            elif char in "\\\x27\"?":
                result += char
            elif char == "c" and self.i < len(self.source):
                result += chr(ord(self.source[self.i].upper()) ^ 64)
                self.i += 1
            elif char in "01234567xuU":
                digits, width = ("01234567", 2) if char in "01234567" else ("0123456789abcdefABCDEF", {"x": 2, "u": 4, "U": 8}[char])
                value = char if char in "01234567" else ""
                for _ in range(width):
                    if self.i == len(self.source) or self.source[self.i] not in digits:
                        break
                    value += self.source[self.i]
                    self.i += 1
                result += chr(int(value, 8 if char in "01234567" else 16)) if value else "\\" + char
            else:
                result += "\\" + char
        raise Denied("unterminated ANSI-C quote")

    def backtick(self):
        self.i += 1
        body = []
        while self.i < len(self.source):
            char = self.source[self.i]
            self.i += 1
            if char == "`":
                inspect("".join(body))
                return
            if char == "\\" and self.i < len(self.source):
                following = self.source[self.i]
                if following in "`$\\\n":
                    self.i += 1
                    if following != "\n":
                        body.append(following)
                    continue
            body.append(char)
        raise Denied("unterminated backtick substitution")

    def dollar(self):
        if self.source.startswith("$(", self.i):
            self.substitution()
        elif self.source.startswith("${", self.i):
            self.i += 2
            depth = 1
            while self.i < len(self.source) and depth:
                if self.source.startswith("$(", self.i):
                    self.substitution()
                elif self.source[self.i] == "`":
                    self.backtick()
                else:
                    char = self.source[self.i]
                    self.i += 1
                    depth += (char == "{") - (char == "}")
            if depth:
                raise Denied("unterminated parameter expansion")
        else:
            match = re.match(r"\$(?:[A-Za-z_][A-Za-z_0-9]*|[0-9@*#?$!_-])", self.source[self.i:])
            if not match:
                self.i += 1
                return False
            self.i += len(match[0])
        return True

    def word(self):
        word = Word("", start=self.i)
        quote = None
        while self.i < len(self.source):
            char = self.source[self.i]
            if quote is None and self.source[self.i:self.i + 2] in {"<(", ">("}:
                self.i += 2
                self.scan(nested=True)
                word.dynamic = True
                word.text += "?"
                continue
            if quote is None and (char in " \t\r\n;|&()<>" ):
                break
            if char == "\x27" and quote is None:
                word.quoted = True
                end = self.source.find("\x27", self.i + 1)
                if end < 0:
                    raise Denied("unterminated single quote")
                word.text += self.source[self.i + 1:end]
                self.i = end + 1
                continue
            if char == "\"":
                word.quoted = True
                quote = None if quote else "\""
                self.i += 1
                continue
            if char == "\\":
                word.quoted = True
                self.i += 1
                if self.i == len(self.source):
                    raise Denied("trailing escape")
                following = self.source[self.i]
                if quote and following not in "$`\"\\\n":
                    word.text += "\\"
                if following != "\n":
                    word.text += following
                self.i += 1
                continue
            if char == "$":
                if quote is None and self.source.startswith("$\x27", self.i):
                    word.quoted = True
                    word.text += self.ansi_quote()
                    continue
                if quote is None and self.source.startswith("$\"", self.i):
                    # Locale strings use the usual double-quote rules. No catalog
                    # is loaded by this static guard.
                    self.i += 1
                    continue
                dynamic = self.dollar()
                word.dynamic |= dynamic
                word.text += "?" if dynamic else "$"
                continue
            if char == "`":
                self.backtick()
                word.dynamic = True
                word.text += "?"
                continue
            if quote is None and char in "*?[]{}~":
                word.dynamic = True
            word.text += char
            self.i += 1
        if quote:
            raise Denied("unterminated double quote")
        word.end = self.i
        return word

    def heredocs(self, pending, execute=False):
        for delimiter, strip_tabs in pending:
            body = []
            body_start = self.i
            while self.i < len(self.source):
                end = self.source.find("\n", self.i)
                end = len(self.source) if end < 0 else end
                line_start = self.i
                line = self.source[self.i:end]
                self.i = min(end + 1, len(self.source))
                if (line.lstrip("\t") if strip_tabs else line) == delimiter.text:
                    break
                body.append(line + "\n")
            else:
                raise Denied("unterminated heredoc")
            self.heredoc_spans.append((body_start, line_start))
            if execute:
                inspect("".join(body))
            if not delimiter.quoted:
                # An unquoted heredoc is data, but substitutions in it run.
                expansion = Shell("".join(body))
                while expansion.i < len(expansion.source):
                    if expansion.source.startswith("\\", expansion.i):
                        expansion.i += 2
                    elif expansion.source.startswith("$(", expansion.i):
                        expansion.substitution()
                    elif expansion.source[expansion.i] == "`":
                        expansion.backtick()
                    else:
                        expansion.i += 1

    def scan(self, nested=False):
        words, pending = [], []
        input_redirect = False
        groups = 0
        cases = []
        loop_header = False
        heredoc_shell = False

        def finish():
            nonlocal words, input_redirect, heredoc_shell
            if pending:
                heredoc_shell |= any(w.text in {"bash", "sh", "dash", "zsh", "ksh"} for w in words)
            invocation(words, input_redirect, source=self.source, appended=self.appended)
            words = []
            input_redirect = False

        while self.i < len(self.source):
            char = self.source[self.i]
            if char in " \t\r":
                self.i += 1
                continue
            if self.source.startswith("\\\n", self.i):
                self.i += 2
                continue
            if char == "#":
                end = self.source.find("\n", self.i)
                self.i = len(self.source) if end < 0 else end
                continue
            if self.source[self.i:self.i + 2] in {"<(", ">("}:
                words.append(self.word())
                continue
            match = self.operators.match(self.source, self.i)
            if not match:
                word = self.word()
                if cases and cases[-1] == "subject":
                    if word.text == "in" and not word.quoted:
                        cases[-1] = "pattern"
                    continue
                if cases and cases[-1] == "pattern":
                    if word.text == "esac" and not word.quoted:
                        cases.pop()
                    continue
                if loop_header:
                    continue
                if not word.quoted and (not words or all(w.text in {"then", "do", "else", "if", "while", "until", "elif", "!", "{"} for w in words)):
                    if word.text == "case":
                        finish()
                        cases.append("subject")
                        continue
                    if word.text in {"for", "select"}:
                        finish()
                        loop_header = True
                        continue
                    if word.text == "esac" and cases:
                        finish()
                        cases.pop()
                        continue
                    if word.text == "[[":
                        self.conditional()
                        continue
                words.append(word)
                continue
            op = match[0]
            start = self.i
            self.i = match.end()
            if op == "(" and self.source.startswith("(", self.i) and all(
                    not w.quoted and w.text in {"if", "then", "elif", "else", "while", "until", "do", "!", "{"}
                    for w in words):
                finish()
                self.i += 1
                self.arithmetic()
                continue
            if cases and cases[-1] == "pattern":
                if op == ")":
                    cases[-1] = "body"
                continue
            if op in {";;", ";&", ";;&"}:
                self.case_separators.append((start, self.i))
                finish()
                if cases:
                    cases[-1] = "pattern"
                continue
            if loop_header:
                if op in {";", "\n"}:
                    loop_header = False
                continue
            if op in self.redirects:
                # Only an adjacent digit word is an IO descriptor, not a PID.
                if words and words[-1].end == start and words[-1].text.isdigit():
                    words.pop()
                while self.i < len(self.source) and self.source[self.i] in " \t":
                    self.i += 1
                target = self.word()
                if target.end == target.start:
                    raise Denied("redirection without a target")
                if op in {"<<", "<<-"}:
                    if target.dynamic:
                        raise Denied("nonliteral heredoc delimiter")
                    pending.append((target, op == "<<-"))
                input_redirect |= op in {"<", "<<", "<<-", "<<<", "<>"}
                continue
            finish()
            if op == "(":
                groups += 1
            elif op == ")":
                if groups:
                    groups -= 1
                elif nested:
                    if pending:
                        raise Denied("heredoc without a body")
                    return
                else:
                    raise Denied("unmatched closing parenthesis")
            elif op == "\n" and pending:
                self.heredocs(pending, execute=heredoc_shell)
                pending = []
                heredoc_shell = False
        finish()
        if nested or groups or pending or cases:
            raise Denied("unterminated shell construct")


def literal(word):
    if word.dynamic:
        raise PatternKill("expanded executable, option, or kill target")
    return word.text


def numeric_kill(args):
    # Jobs (%1), expansions, PID 0 and negative process groups are denied:
    # none identifies a literal individual process whose cwd can be checked.
    values = [literal(arg) for arg in args]
    if values and values[0] in {"-l", "-L", "--list", "--table"}:
        return  # Signal listing sends no signal.
    if values and values[0] in {"-s", "-n", "--signal"}:
        if len(values) < 2 or not re.fullmatch(r"(?:SIG)?[A-Za-z0-9]+", values[1]):
            raise PatternKill("unresolved kill signal")
        values = values[2:]
    elif values and (re.fullmatch(r"-(?:SIG)?[A-Za-z0-9]+", values[0])
                     or re.fullmatch(r"--signal=(?:SIG)?[A-Za-z0-9]+", values[0])):
        values = values[1:]
    if values and values[0] == "--":
        values = values[1:]
    if not values or any(not re.fullmatch(r"[0-9]+", pid) or int(pid) == 0 for pid in values):
        raise PatternKill("kill requires literal positive numeric PIDs")


def invocation(words, input_redirect=False, source="", appended=False):
    while words and not words[0].quoted and words[0].text in {"if", "then", "elif", "else", "while", "until", "do", "done", "fi", "!", "{", "}"}:
        words = words[1:]
    while words and re.match(r"^[A-Za-z_][A-Za-z_0-9]*=", words[0].text):
        words = words[1:]
    if not words:
        return
    # A literal test builtin is not a globbed executable name.
    if words[0].dynamic and words[0].text != "[":
        if KILL_TOKEN.search(source):
            raise PatternKill("dynamic executable in command containing a kill-ish token")
        return
    name = os.path.basename(words[0].text)
    args = words[1:]
    if name in {"pkill", "killall", "killall5", "skill"}:
        raise PatternKill(name + " selects processes by pattern")
    if name == "kill":
        if appended:
            raise PatternKill("xargs kill appends unresolved targets from stdin")
        numeric_kill(args)
        return
    if name == "fuser":
        for arg in args:
            value = literal(arg)
            if value == "--":
                break
            if value == "--kill" or (value.startswith("-") and not value.startswith("--") and "k" in value):
                raise PatternKill("fuser -k selects processes by file")
    if name in {"bash", "sh", "dash", "zsh", "ksh"}:
        index = 0
        while index < len(args):
            value = args[index].text
            if value == "--" or not value.startswith("-"):
                break
            if not value.startswith("--") and "c" in value:
                if index + 1 >= len(args):
                    raise Denied("missing shell script")
                script = args[index + 1]
                if not script.dynamic:
                    inspect(script.text)
                elif KILL_TOKEN.search(source):
                    raise PatternKill("dynamic shell script containing a kill-ish token")
                break
            if value in {"-o", "-O"}:
                index += 1
            index += 1
        # Script files, redirected input and interpreter payloads are #708.
    elif name == "eval":
        if all(not arg.dynamic for arg in args):
            inspect(" ".join(arg.text for arg in args))
        else:
            invocation(args, source=source, appended=appended)

    # Bounded rule: look at each literal argv word and the first word of
    # literal argv strings, regardless of the enclosing executable. Recurse
    # only when that word is a killer; ordinary prose remains data.
    appended |= name == "xargs"
    for index, arg in enumerate(args):
        if arg.dynamic:
            continue
        value = arg.text
        first = value.split(None, 1)[0] if value.split() else ""
        # The string rule uses whitespace-delimited words; punctuation in a
        # literal regex such as "pkill|killall" must remain ordinary data.
        first = first.strip("\x27\"")
        if os.path.basename(first) not in KILLERS:
            continue
        if first == value:
            invocation(args[index:], source=source, appended=appended)
        else:
            inspect(value, appended=appended)



def inspect(command, appended=False):
    shell = Shell(command)
    shell.appended = appended
    shell.scan()
    # Bash 3.2 misparses quotes/backticks in quoted heredocs inside $(...).
    # Their bodies are data, already walked above where expansions execute.
    syntax_source = command
    for start, end in sorted(shell.heredoc_spans, reverse=True):
        syntax_source = syntax_source[:start] + re.sub(r"[^\n]", " ", syntax_source[start:end]) + syntax_source[end:]
    # Bash 3.2 lacks case fallthrough operators. Their grammar is otherwise
    # identical to ;;, so validate that shape without changing what we inspect.
    for start, end in shell.case_separators:
        syntax_source = syntax_source[:start] + ";;" + " " * (end - start - 2) + syntax_source[end:]
    return syntax_source


try:
    payload = json.load(sys.stdin)
    command = payload["tool_input"]["command"]
    if not isinstance(command, str) or "\x00" in command:
        raise Denied("command must be a string without NUL bytes")
    syntax_source = inspect(command)
    # Syntax check only: -n never executes the input, even substitutions.
    # Use an empty startup environment so BASH_ENV cannot run another script.
    syntax = subprocess.run(
        ["/bin/bash", "--noprofile", "--norc", "-n"], input=syntax_source,
        text=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"}, timeout=5,
    )
    if syntax.returncode:
        raise Denied("invalid shell syntax")
except BaseException as exc:
    print("kill blocked: " + str(exc), file=sys.stderr)
    if isinstance(exc, PatternKill):
        print("kill blocked: " + REMEDIATION, file=sys.stderr)
    sys.exit(2)
'; then
    exit 0
else
    status=$?
fi
if [[ $status -ne 2 ]]; then
    printf '%s\n' "kill blocked: guard interpreter failed; command denied." >&2
fi
exit 2
