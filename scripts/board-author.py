#!/usr/bin/env python3
"""Derive a by:* label from `gh pr view --json commits` on stdin."""
import json
import re
import subprocess
import sys


def derive(commits):
    authors = set()
    for commit in commits:
        # Parse each COMPLETE message separately: concatenating bodies can turn
        # another commit's prose into a trailer. Git enforces the trailer block
        # boundary and unfolds continuations, unlike a grep of arbitrary lines.
        message = commit["messageHeadline"] + "\n\n" + commit["messageBody"]
        trailers = subprocess.check_output(
            ["git", "interpret-trailers", "--parse"], input=message, text=True
        )
        for trailer in trailers.splitlines():
            key, _, value = trailer.partition(":")
            key, value = key.strip().lower(), value.split("<", 1)[0].strip().lower()
            if key not in {"co-authored-by", "generated-with", "claude-session", "signed-off-by"}:
                continue
            if re.search(r"\bcodex\b|\bgpt-", value):
                authors.add("by:codex")
            if re.search(r"\bclaude\s+opus\b", value):
                authors.add("by:claude-opus")
            if re.search(r"\bclaude\s+sonnet\b", value):
                authors.add("by:claude-sonnet")
            # An ordinary human name/signoff is not proof of human authorship.
            # Human requires an explicit declaration, agents still take priority.
            if key == "generated-with" and value == "human":
                authors.add("by:human")
    # Preserve Codex precedence for joint work, then Opus, then Sonnet.
    return next((label for label in (
        "by:codex", "by:claude-opus", "by:claude-sonnet", "by:human"
    ) if label in authors), "by:unknown")


if __name__ == "__main__":
    print(derive(json.load(sys.stdin)["commits"]))
