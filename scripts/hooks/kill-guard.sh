#!/bin/bash
# Claude Code PreToolUse hook for Bash; deliberately not registered in settings.
# Over-denying costs an agent one reformulated command; under-denying costs
# every concurrent agent its test run. Fail CLOSED on malformed input, missing
# interpreters, unsupported syntax, and internal errors (including Python failure).
# This is a static command guard, not a sandbox for scripts, inherited aliases,
# shell functions, or arbitrary programs that can themselves send signals.
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
    consumed by line, not tokenized as commands. Unsupported constructs deny.
    """

    operators = re.compile(r"&&|\|\||;;&|;;|;&|&>>|&>|<<<|<<-|<<|>>|<>|>&|<&|>\||[;|&()<>\n]")
    redirects = {"<", ">", ">>", "<<", "<<-", "<<<", "<>", ">&", "<&", "&>", "&>>", ">|"}

    def __init__(self, source):
        self.source = source
        self.i = 0

    def substitution(self):
        if self.source.startswith("$((", self.i):
            raise Denied("arithmetic expansion cannot be resolved statically")
        self.i += 2
        self.scan(nested=True)

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
                # ANSI-C/locale quoting can construct an executable name.
                # Refuse to guess at its decoding, even when it looks harmless.
                if self.source[self.i + 1:self.i + 2] in {"\x27", "\""}:
                    raise Denied("unsupported ANSI-C or locale quoting")
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

    def heredocs(self, pending):
        for delimiter, strip_tabs in pending:
            body = []
            while self.i < len(self.source):
                end = self.source.find("\n", self.i)
                end = len(self.source) if end < 0 else end
                line = self.source[self.i:end]
                self.i = min(end + 1, len(self.source))
                if (line.lstrip("\t") if strip_tabs else line) == delimiter.text:
                    break
                body.append(line + "\n")
            else:
                raise Denied("unterminated heredoc")
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

        def finish():
            nonlocal words, input_redirect
            invocation(words, input_redirect)
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
            match = self.operators.match(self.source, self.i)
            if not match:
                words.append(self.word())
                continue
            op = match[0]
            start = self.i
            self.i = match.end()
            if op in {";;", ";&", ";;&"}:
                raise Denied("case syntax is not supported")
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
                self.heredocs(pending)
                pending = []
        finish()
        if nested or groups or pending:
            raise Denied("unterminated shell construct")


def literal(word):
    if word.dynamic:
        raise Denied("expanded executable, option, or kill target")
    return word.text


def numeric_kill(args):
    # Jobs (%1), expansions, PID 0 and negative process groups are denied:
    # none identifies a literal individual process whose cwd can be checked.
    values = [literal(arg) for arg in args]
    if values and values[0] in {"-l", "-L", "--list", "--table"}:
        return  # Signal listing sends no signal.
    if values and values[0] in {"-s", "-n", "--signal"}:
        if len(values) < 2 or not re.fullmatch(r"(?:SIG)?[A-Za-z0-9]+", values[1]):
            raise Denied("unresolved kill signal")
        values = values[2:]
    elif values and (re.fullmatch(r"-(?:SIG)?[A-Za-z0-9]+", values[0])
                     or re.fullmatch(r"--signal=(?:SIG)?[A-Za-z0-9]+", values[0])):
        values = values[1:]
    if values and values[0] == "--":
        values = values[1:]
    if not values or any(not re.fullmatch(r"[0-9]+", pid) or int(pid) == 0 for pid in values):
        raise Denied("kill requires literal positive numeric PIDs")


def invocation(words, input_redirect=False):
    while words and words[0].text in {"if", "then", "elif", "else", "while", "until", "do", "!", "{", "}"}:
        words = words[1:]
    while words and re.match(r"^[A-Za-z_][A-Za-z_0-9]*=", words[0].text):
        words = words[1:]
    if not words:
        return
    name = os.path.basename(literal(words[0]))
    args = words[1:]
    if name in {"case", "for", "select", "function", "coproc", "[[", "(("}:
        raise Denied("unsupported shell control syntax")
    if name in {"pkill", "killall"}:
        raise Denied(name + " selects processes by pattern")
    if name == "kill":
        numeric_kill(args)
    elif name == "fuser":
        for arg in args:
            value = literal(arg)
            if value == "--":
                break
            if value == "--kill" or (value.startswith("-") and not value.startswith("--") and "k" in value):
                raise Denied("fuser -k selects processes by file")
    elif name in {"command", "builtin", "exec", "env", "sudo", "nohup", "setsid", "nice", "timeout", "time", "xargs"}:
        index = 0
        flags = {
            "command": {"-p"}, "builtin": set(), "exec": {"-c", "-l"},
            "env": {"-i", "--ignore-environment"},
            "sudo": {"-n", "-E", "-H", "-S", "-b", "-k"},
            "nohup": set(), "setsid": {"-f", "-w"}, "nice": set(),
            "timeout": {"--preserve-status", "--foreground", "-v"},
            "time": {"-p"}, "xargs": {"-0", "-r", "-t", "-p", "-x", "--null", "--no-run-if-empty"},
        }
        options = {
            "exec": {"-a"}, "env": {"-u", "--unset", "-C", "--chdir"},
            "sudo": {"-u", "-g", "-h", "-p", "-C", "-T", "-R", "-D"},
            "nice": {"-n", "--adjustment"}, "timeout": {"-s", "--signal", "-k", "--kill-after"},
            "xargs": {"-I", "-n", "-P", "-L", "-s", "-E", "--replace", "--max-args", "--max-procs"},
        }
        while index < len(args):
            value = literal(args[index])
            if name == "command" and value in {"-v", "-V"}:
                return  # Inspection of a command name does not run it.
            if value == "--":
                index += 1
                break
            if name == "env" and re.match(r"^[A-Za-z_][A-Za-z_0-9]*=", value):
                index += 1
                continue
            if value in flags[name]:
                index += 1
            elif value in options.get(name, set()):
                if index + 1 >= len(args):
                    raise Denied("missing wrapper option value")
                literal(args[index + 1])
                index += 2
            elif value.startswith("-"):
                raise Denied("unsupported " + name + " option")
            else:
                break
        if name == "timeout":
            index += 1  # Duration precedes the executable.
        invocation(args[index:], input_redirect)
    elif name in {"bash", "sh", "dash", "zsh", "ksh"}:
        index = 0
        while index < len(args):
            value = literal(args[index])
            if value == "--" or not value.startswith("-"):
                break
            if value.startswith("--"):
                if value not in {"--noprofile", "--norc", "--posix"}:
                    raise Denied("unsupported shell option")
            elif "c" in value:
                if index + 1 >= len(args):
                    raise Denied("missing shell script")
                inspect(literal(args[index + 1]))
                return
            elif value in {"-o", "-O"}:
                index += 1
            index += 1
        if input_redirect:
            raise Denied("shell reading commands from a redirection")
        # Script contents and inherited startup state are outside this guard.
    elif name == "eval":
        inspect(" ".join(literal(arg) for arg in args))
    elif name == "find":
        for index, arg in enumerate(args):
            if arg.text in {"-exec", "-execdir", "-ok", "-okdir"}:
                end = index + 1
                while end < len(args) and args[end].text not in {";", "+"}:
                    end += 1
                invocation(args[index + 1:end])


def inspect(command):
    Shell(command).scan()


try:
    payload = json.load(sys.stdin)
    command = payload["tool_input"]["command"]
    if not isinstance(command, str) or "\x00" in command:
        raise Denied("command must be a string without NUL bytes")
    # Syntax check only: -n never executes the input, even substitutions.
    # Use an empty startup environment so BASH_ENV cannot run another script.
    syntax = subprocess.run(
        ["/bin/bash", "--noprofile", "--norc", "-n"], input=command,
        text=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"}, timeout=5,
    )
    if syntax.returncode:
        raise Denied("invalid shell syntax")
    inspect(command)
except BaseException as exc:
    print("kill blocked: " + str(exc), file=sys.stderr)
    sys.exit(2)
'; then
    exit 0
fi
printf '%s\n' "kill blocked: resolve the pattern with pgrep -fa <pattern>, confirm each pid's working directory belongs to this worktree, then kill <pid>." >&2
exit 2
