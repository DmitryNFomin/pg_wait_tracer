#!/usr/bin/env python3
"""orphan_guard_lib.py -- decision engine for scripts/hooks/orphan-guard.sh
(Claude Code PreToolUse hook on Bash).

Problem this guards against (see the spawn issue): background shells that
outlive the agent that started them, either via
  (a) a manually detached process (`nohup`, `setsid`, `disown`, or a shell
      `&` job that does not redirect both stdout and stderr), or
  (b) the Bash tool's own `run_in_background: true` wrapping a shell
      polling loop (`until grep -q X; do sleep 90; done`) -- the literal
      shape of three real orphans found on the owner's machine (ages
      22h43m, 11h04m, 10h29m; issue: PreToolUse Bash hook for orphan
      prevention).

decide(tool_name, command, run_in_background, session_id) returns a
Decision namedtuple: kind in {"allow", "mark", "refuse"}.
  - "allow"  : run as-is, nothing to track.
  - "mark"   : run, but the caller (orphan-guard.sh) prefixes the command
               so the reaper (orphan-reap.sh, SubagentStop/Stop) can find
               and kill it later if this session dies before it finishes.
  - "refuse" : block. `reason` is the human-readable message (WHY + the
               alternative), `code` is a short machine tag used by tests.

IMPORTANT: this module makes decisions from the literal command TEXT (and
the run_in_background flag). It cannot see inside a script it did not
write, so a command that merely CONTAINS the allowed shape as a substring
(e.g. a `grep` pattern mentioning "box-check") must not be treated as if
it WERE that shape -- every match below is anchored (^...$) or scoped to
whole words, never a bare substring search, specifically to close that
hole (see test_orphan_guard.sh case "substring-lookalike is not the real
command").
"""
from __future__ import annotations

import re
from collections import namedtuple

Decision = namedtuple("Decision", ["kind", "reason", "code"])


def _allow():
    return Decision("allow", None, None)


def _mark():
    return Decision("mark", None, None)


def _refuse(code, reason):
    return Decision("refuse", reason, code)


def _has_word(word, s):
    """Whole-word match, not a substring match. '..word..' inside a longer
    identifier or path does not count."""
    return re.search(r"(?<![\w./-])" + word + r"(?![\w-])", s) is not None


_DETACH_KEYWORDS = ("nohup", "setsid", "disown")

# make check | make check-fast | make box-check | make ui-gallery, optionally
# preceded by env assignments, followed by KEY=VAL args, and optionally
# followed by ONE redirect/tee clause capturing both stdout and stderr.
# fullmatch (^...$) -- deliberately does not allow anything else to ride
# along after the job (no `&&`, no `;`, no second command).
_ALLOWED_BG_JOB_RE = re.compile(
    r"^(?:[A-Za-z_][A-Za-z0-9_]*=\S+\s+)*"
    r"make\s+(?:check|check-fast|box-check|ui-gallery)"
    r"(?:\s+[A-Za-z_][A-Za-z0-9_]*=\S+)*"
    r"(?:\s*(?:>{1,2}\s*\S+\s*2>&1|2>&1\s*>{1,2}\s*\S+"
    r"|2>&1\s*\|\s*tee\s+(?:-a\s+)?\S+|\|\s*tee\s+(?:-a\s+)?\S+))?"
    r"\s*$"
)

_LOOP_KEYWORDS = ("until", "while", "do", "done")


def _looks_like_loop(cmd):
    if any(_has_word(k, cmd) for k in _LOOP_KEYWORDS):
        return True
    if re.search(r"\bfor\b\s+\S+\s+in\b", cmd):
        return True
    # a "bare repeated sleep" without an explicit loop keyword, e.g.
    # `sleep 30; sleep 30 &`
    sleep_count = len(re.findall(r"(?<![\w./-])sleep(?![\w-])", cmd))
    return sleep_count >= 2


def _manual_background(cmd):
    """True if the literal command text backgrounds itself with a bare `&`
    (not `&&`, not an `N>&M` fd-duplication like `2>&1`)."""
    norm = re.sub(r"\d>&\d", "", cmd)
    norm = norm.replace("&&", " ")
    return "&" in norm


def decide(tool_name, command, run_in_background, session_id):
    if tool_name != "Bash":
        return _allow()

    cmd = (command or "").strip()
    if not cmd:
        return _allow()

    for kw in _DETACH_KEYWORDS:
        if _has_word(kw, cmd):
            return _refuse(
                f"detach:{kw}",
                f"Refused: `{kw}` detaches a process from this shell -- it is "
                "exactly the mechanism behind the orphaned background shells "
                "found on this machine (some ran for over a day). "
                "Alternative: one blocking foreground wait inside your own "
                "command, never a detached process.",
            )

    manual_bg = _manual_background(cmd)
    rib = bool(run_in_background)

    if _has_word("ssh", cmd) and manual_bg:
        return _refuse(
            "ssh_bg",
            "Refused: this ssh command backgrounds a job on the remote "
            "side (`&`). That leaves the remote job attached to this ssh "
            "session's pipe even if the local command line has no trailing "
            "`&` of its own -- one of the three ways this class of orphan "
            "gets created. Alternative: run the remote command in the "
            "foreground and wait for it, or have the remote side write its "
            "own completion marker to a file you poll ONCE, blocking, "
            "in your own command.",
        )

    is_bg = rib or manual_bg

    if is_bg and _looks_like_loop(cmd):
        return _refuse(
            "loop_bg",
            "Refused: a backgrounded shell whose body is a poll loop "
            "(`until`/`while`/`for`, or a repeated `sleep`) is exactly the "
            "shape of the orphans found on this machine (up to 23h old) -- "
            "the poll outlives the agent that started it. Alternative: one "
            "blocking foreground wait inside your own command; if you "
            "cannot wait that long, stop and report back so the lead can "
            "re-invoke you instead of leaving a process behind.",
        )

    if rib:
        if _ALLOWED_BG_JOB_RE.match(cmd):
            return _mark()
        return _refuse(
            "rib_not_allowed",
            "Refused: run_in_background is restricted to `make check`, "
            "`make check-fast`, `make box-check`, and `make ui-gallery` "
            "(optionally with an env-var prefix and a trailing redirect or "
            "`| tee`). Alternative: one blocking foreground wait inside "
            "your own command, or report back and let the lead re-invoke "
            "you rather than leaving an unrecognised background job "
            "running.",
        )

    if manual_bg:
        stripped = cmd.rstrip()
        if not stripped.endswith("&"):
            return _refuse(
                "bg_unclear",
                "Refused: a background `&` appears somewhere other than "
                "the very end of the command, so this hook cannot verify "
                "both file descriptors are redirected. Alternative: put "
                "the `&` at the end of a single `cmd >log 2>&1 &`, or wait "
                "for it in the foreground.",
            )
        job = stripped[:-1].rstrip()
        has_stdout = re.search(r"(?:^|[\s;|&])(>>?|&>)\s*\S+", job) is not None
        has_stderr = ("2>&1" in job) or (re.search(r"2>\s*\S+", job) is not None) or ("&>" in job)
        if not (has_stdout and has_stderr):
            return _refuse(
                "unredirected_bg",
                "Refused: a background job (`&`) must redirect BOTH stdout "
                "and stderr in the same command (e.g. `cmd >log 2>&1 &`) -- "
                "an unredirected pipe is exactly what keeps a background "
                "shell listed as running long after it is actually done. "
                "Alternative: add `>log 2>&1` before the `&`, or wait for "
                "it in the foreground with one blocking call.",
            )
        return _mark()

    return _allow()
