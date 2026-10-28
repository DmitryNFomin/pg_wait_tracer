#!/bin/bash
# Claude Code PreToolUse hook for Bash; deliberately not registered in settings.
# Prefer misses over false denials: indirect execution already escapes this
# tripwire, while false denials cost registrability and an unwired hook protects
# nothing. Fail CLOSED on malformed input, missing
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
import shlex
import subprocess
import sys
from dataclasses import dataclass


class Denied(Exception):
    pass


class PatternKill(Denied):
    pass


KILLERS = {"kill", "pkill", "killall", "killall5", "skill", "fuser", "xargs"}
REMEDIATION = "resolve the pattern with pgrep -fa <pattern>, confirm each pid\x27s working directory belongs to this worktree, then kill <pid>."


@dataclass
class Word:
    text: str
    dynamic: bool = False
    quoted: bool = False
    start: int = 0
    end: int = 0
    reference: str = ""
    pattern_target: bool = False
    assignment: bool = False
    bound: str = None


class Shell:
    """Lex shell words without executing them; inspect executable positions.

    Quotes remain data, except for actual substitutions. Heredoc bodies are
    consumed by line, not tokenized as commands. Malformed constructs deny.
    """

    operators = re.compile(r"&&|\|\||;;&|;;|;&|&>>|&>|<<<|<<-|<<|>>|<>|>&|<&|>\||[;|&()<>\n]")
    redirects = {"<", ">", ">>", "<<", "<<-", "<<<", "<>", ">&", "<&", "&>", "&>>", ">|"}

    def __init__(self, source, bindings=None):
        self.source = source
        self.bindings = dict(bindings or {})
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
                inspect("".join(body), bindings=self.bindings)
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
                expansion_start = self.i
                dynamic = self.dollar()
                word.pattern_target |= bool(re.match(r"\$\(\s*(?:/[^\s()]*/)?pgrep\s", self.source[expansion_start:self.i]))
                word.dynamic |= dynamic
                word.text += "?" if dynamic else "$"
                continue
            if char == "`":
                expansion_start = self.i
                self.backtick()
                word.pattern_target |= bool(re.match(r"`\s*(?:/[^\s`]*/)?pgrep\s", self.source[expansion_start:self.i]))
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
        word.assignment = bool(re.match(r"[A-Za-z_][A-Za-z_0-9]*=", self.source[word.start:word.end]))
        reference = re.fullmatch(r"\"?\$(?:([A-Za-z_][A-Za-z_0-9]*)|\{([A-Za-z_][A-Za-z_0-9]*)\})\"?", self.source[word.start:word.end])
        if reference:
            word.reference = reference[1] or reference[2]
            word.bound = self.bindings.get(word.reference)
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
                inspect("".join(body), bindings=self.bindings)
            if not delimiter.quoted:
                # An unquoted heredoc is data, but substitutions in it run.
                expansion = Shell("".join(body), self.bindings)
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
        # Substitutions have a subshell environment; their assignments do not
        # bind a later command in the enclosing shell.
        outer_bindings = self.bindings
        if nested:
            self.bindings = dict(outer_bindings)
        words, pending = [], []
        input_redirect = False
        groups = 0
        group_bindings = []
        pipeline_bindings = None
        cases = []
        loop_header = False
        loop_variable = False
        heredoc_shell = False

        def finish():
            nonlocal words, input_redirect, heredoc_shell
            if pending:
                heredoc_shell |= any(w.text in {"bash", "sh", "dash", "zsh", "ksh"} for w in words)
            invocation(words, input_redirect, bindings=self.bindings, appended=self.appended)
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
                    if loop_variable:
                        self.bindings.pop(word.text, None)
                        loop_variable = False
                    continue
                if not word.quoted and (not words or all(w.text in {"then", "do", "else", "if", "while", "until", "elif", "!", "{"} for w in words)):
                    if word.text == "case":
                        finish()
                        cases.append("subject")
                        continue
                    if word.text in {"for", "select"}:
                        finish()
                        loop_header = True
                        loop_variable = True
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
            before_finish = dict(self.bindings)
            finish()
            if op == "|" and pipeline_bindings is None:
                pipeline_bindings = before_finish
            if pipeline_bindings is not None:
                self.bindings = dict(pipeline_bindings)
                if op != "|":
                    pipeline_bindings = None
            elif op == "&":
                self.bindings = before_finish
            if op == "(":
                group_bindings.append(self.bindings)
                self.bindings = dict(self.bindings)
                groups += 1
            elif op == ")":
                if groups:
                    groups -= 1
                    self.bindings = group_bindings.pop()
                elif nested:
                    if pending:
                        raise Denied("heredoc without a body")
                    self.bindings = outer_bindings
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


def numeric_kill(args):
    # Unknown expansions may be individual PIDs. Treating them as nonnumeric
    # caused false denials of PID-file cleanup and numeric for loops in replay.
    # Only direct pgrep substitution operands are matched; PID-variable and
    # general data-flow selection are out of scope per #708.
    values = [None if arg.dynamic else arg.text for arg in args]
    if values and values[0] in {"-l", "-L", "--list", "--table"}:
        return  # Signal listing sends no signal.
    signal = None
    if values and values[0] in {"-s", "-n", "--signal"}:
        if len(values) < 2 or (values[1] is not None and not re.fullmatch(r"(?:SIG)?[A-Za-z0-9]+", values[1])):
            raise PatternKill("unresolved kill signal")
        signal = values[1]
        values = values[2:]
        args = args[2:]
    elif values and values[0] is not None and (re.fullmatch(r"-(?:SIG)?[A-Za-z0-9]+", values[0])
                     or re.fullmatch(r"--signal=(?:SIG)?[A-Za-z0-9]+", values[0])):
        signal = values[0].split("=", 1)[-1].lstrip("-")
        values = values[1:]
        args = args[1:]
    if signal == "0":
        return  # Signal zero only probes liveness; it sends no signal.
    if values and values[0] == "--":
        values = values[1:]
        args = args[1:]
    if any(arg.pattern_target for arg in args):
        raise PatternKill("kill targets a pgrep process selection")
    if not values or any(pid is not None and (not re.fullmatch(r"[0-9]+", pid) or int(pid) == 0) for pid in values):
        raise PatternKill("kill requires literal positive numeric PIDs")


# This is deliberately a bounded list, not a general execution model. Each
# entry specifies options with operands and options without operands. Unknown
# options stop inspection rather than mistaking their data for a command.
WRAPPERS = {
    "flock": ("w E", "wait conflict-exit-code", "s x u n o F v", "shared exclusive unlock nonblock nonblocking close no-fork verbose"),
    "ssh": ("B b c D E e F I i J L l m O o p Q R S W w", "", "4 6 A a C f G g K k M N n q s T t V v X x Y y", ""),
    "su": ("s g G w", "shell group supp-group whitelist-environment", "m p l", "preserve-environment login"),
    "sudo": ("u g h p C T r t D R", "user group host prompt close-from command-timeout role type chdir chroot", "A b E H i k n P S s", "askpass background preserve-env set-home login reset-timestamp non-interactive preserve-groups stdin shell"),
    "doas": ("u C", "", "n s L", ""),
    "env": ("u C a", "unset chdir argv0", "i 0 v", "ignore-environment null debug"),
    "nice": ("n", "adjustment", "", ""),
    "ionice": ("c n t p P u", "class classdata timeout pid pgid uid", "i", "ignore"),
    "stdbuf": ("i o e", "input output error", "", ""),
    "timeout": ("k s", "kill-after signal", "v", "foreground preserve-status verbose"),
    "watch": ("n q", "interval equexit", "b c C d e g p r t w x", "beep color no-color differences errexit chgexit precise no-rerun no-title no-wrap exec"),
    "nohup": ("", "", "", ""),
    "setsid": ("", "", "c f w", "ctty fork wait"),
    "chrt": ("p T P D", "pid sched-runtime sched-period sched-deadline", "a b d f i m o r R v", "all-tasks batch deadline fifo idle max other rr reset-on-fork verbose"),
    "taskset": ("", "", "a p c", "all-tasks pid cpu-list"),
    "xargs": ("a d E I J L n P R S s", "arg-file delimiter eof replace max-lines max-args max-procs max-replace max-chars process-slot-var", "0 e i l o p r t x", "null open-tty interactive no-run-if-empty verbose exit"),
}


def wrapper_command(name, args, bindings, appended):
    takes_short, takes_long, flags_short, flags_long = (
        set(part.split()) for part in WRAPPERS[name])
    index = 0
    su_user = False
    shell_body = None
    split_body = None
    while index < len(args):
        arg = args[index]
        value = arg.text
        if not arg.dynamic and value in {"--", "-"}:
            index += 1
            if value == "--":
                break
            continue
        # env assignments precede its utility, including after options.
        if name == "env" and re.match(r"^[A-Za-z_][A-Za-z_0-9]*=", value):
            key, val = value.split("=", 1)
            bindings[key] = val if not arg.dynamic else None
            index += 1
            continue
        # su accepts options both before and after the login name.
        if not value.startswith("-"):
            if name == "su" and not su_user:
                su_user = True
                index += 1
                continue
            break
        if value in {"--help", "--version"}:
            return
        # These modes inspect credentials/commands or change an existing PID;
        # their remaining operands are not an executable command.
        if ((name == "sudo" and value in {"-l", "--list", "-v", "--validate", "-V", "-K", "--remove-timestamp"})
                or (name in {"chrt", "taskset"} and value.startswith("-") and not value.startswith("--") and "p" in value)
                or (name in {"chrt", "taskset"} and value == "--pid")
                or (name == "ionice" and (value in {"-p", "-P", "-u", "--pid", "--pgid", "--uid"}))):
            return
        if name == "nice" and re.fullmatch(r"-\d+", value):
            index += 1
            continue
        if value.startswith("--"):
            option, equals, operand = value[2:].partition("=")
            if name in {"su", "flock"} and option == "command":
                kind = "shell"
            elif name == "env" and option == "split-string":
                kind = "split"
            elif option in takes_long:
                kind = "operand"
            elif option in flags_long:
                index += 1
                continue
            else:
                return
            if equals:
                body = Word(operand, arg.dynamic)
            else:
                index += 1
                if index >= len(args):
                    return
                body = args[index]
        else:
            cluster = value[1:]
            kind = None
            body = None
            for offset, option in enumerate(cluster):
                if name in {"su", "flock"} and option == "c":
                    kind = "shell"
                elif name == "env" and option == "S":
                    kind = "split"
                elif option in takes_short:
                    kind = "operand"
                elif option in flags_short:
                    continue
                else:
                    return
                tail = cluster[offset + 1:]
                if tail:
                    body = Word(tail, arg.dynamic)
                else:
                    index += 1
                    if index >= len(args):
                        return
                    body = args[index]
                break
        if kind == "shell":
            shell_body = body
        elif kind == "split":
            split_body = body
        index += 1
    if shell_body is not None:
        if not shell_body.dynamic:
            inspect(shell_body.text, bindings=bindings)
        return
    if split_body is not None:
        if not split_body.dynamic:
            # env -S splits arguments; it does not interpret shell operators.
            try:
                parts = [Word(value) for value in shlex.split(split_body.text)]
            except ValueError:
                return
            wrapper_command("env", parts + args[index:], bindings, appended)
        return
    if name in {"flock", "ssh", "timeout", "chrt", "taskset"} or (name == "su" and not su_user):
        # lock file/fd, host, duration, priority, CPU set, or login name.
        index += 1
    # Shared command handoff for every wrapper: markers can follow its own
    # positional operand too (notably the flock lock file). Neither marker is
    # an executable name; inspect the command that follows them.
    while index < len(args) and not args[index].dynamic and args[index].text in {"--", "-"}:
        index += 1
    trailing = args[index:]
    if name == "flock" and trailing and trailing[0].text in {"-c", "--command"}:
        if len(trailing) > 1 and not trailing[1].dynamic:
            inspect(trailing[1].text, bindings=bindings)
        return
    if name == "su":
        # After -- and the login name, su forwards arguments to the shell.
        # Inspect an explicit -c there just as we do before the marker.
        invocation([Word("sh")] + trailing, bindings=bindings)
        return
    if name == "ssh":
        # ssh joins its remote argv into one shell command. Quoted remote
        # scripts therefore contain command positions, unlike ordinary argv.
        if all(not arg.dynamic for arg in trailing):
            inspect(" ".join(arg.text for arg in trailing), bindings=bindings)
    else:
        invocation(trailing, bindings=bindings, appended=appended or name == "xargs")


def invocation(words, input_redirect=False, bindings=None, appended=False):
    if bindings is None:
        bindings = {}
    while words and not words[0].quoted and words[0].text in {"if", "then", "elif", "else", "while", "until", "do", "done", "fi", "!", "{", "}"}:
        words = words[1:]
    assignments = {}
    while words and words[0].assignment:
        key, value = words[0].text.split("=", 1)
        assignments[key] = value if not words[0].dynamic else None
        words = words[1:]
    if not words:
        bindings.update(assignments)
        return
    # Only a standalone literal binding proves a dynamic executable is a
    # killer. Mentions elsewhere (including paths to this guard) are data.
    if words[0].dynamic and words[0].text != "[":
        bound = words[0].bound
        if bound not in KILLERS:
            return
        words = [Word(bound)] + words[1:]
    # Prefix assignments enter the invoked environment after the shell expands
    # its command name; k=pkill "$k" does not bind that command-name expansion.
    bindings = dict(bindings, **assignments)
    name = os.path.basename(words[0].text)
    args = words[1:]
    if name in {"pkill", "killall", "killall5", "skill"}:
        raise PatternKill(name + " selects processes by pattern")
    if name == "kill":
        if appended:
            raise PatternKill("xargs kill appends unresolved targets from stdin")
        # Bindings were captured while lexing, before prefix/env assignments.
        # Unquoted multiword or globbed values have unknown expanded targets.
        numeric_kill([Word(arg.bound) if arg.bound is not None and (arg.quoted or
                      (len(arg.bound.split()) == 1 and not any(c in arg.bound for c in "*?[]{}")))
                      else arg for arg in args])
        return
    if name == "fuser":
        for arg in args:
            if arg.dynamic:
                continue
            value = arg.text
            if value == "--":
                break
            if value == "--kill" or (value.startswith("-") and not value.startswith("--") and "k" in value):
                raise PatternKill("fuser -k selects processes by file")
        return
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
                    # Extra shell argv are positional parameters, not extra
                    # targets of every kill command inside its -c string.
                    inspect(script.text, bindings=bindings)
                break
            if value in {"-o", "-O"}:
                index += 1
            index += 1
        # Script files, redirected input and interpreter payloads are #708.
    elif name == "eval":
        if all(not arg.dynamic for arg in args):
            inspect(" ".join(arg.text for arg in args), bindings=bindings)
        else:
            invocation(args, bindings=bindings, appended=appended)
    elif name in {"command", "builtin", "exec"}:
        # Shell builtins that execute their trailing command. Query options
        # such as command -v take data and do not invoke it.
        index = 0
        while index < len(args) and args[index].text.startswith("-"):
            option = args[index].text
            if name == "command" and option in {"-v", "-V"}:
                return
            if option not in {"--", "-p", "-c", "-l", "-a"}:
                return
            index += 2 if name == "exec" and option == "-a" else 1
            if option == "--":
                break
        invocation(args[index:], bindings=bindings, appended=appended)
    elif name in WRAPPERS:
        wrapper_command(name, args, bindings, appended)


def inspect(command, appended=False, bindings=None):
    shell = Shell(command, bindings)
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
