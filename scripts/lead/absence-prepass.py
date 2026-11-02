#!/usr/bin/env python3
"""absence-prepass.py <draft-file>

Deterministic pre-pass for audit-report.sh. Runs BEFORE any codex call.

RULE 0 requires a pasted command and its output for any ABSENCE claim
("exists nowhere", "not merged", "unreachable", "no mechanical check", "not
implemented", ...). This has been violated in both directions: claims of
absence asserted with nothing pasted, and real evidence buried where a
grader can't find it. This script catches the first kind mechanically, so
a BLOCK does not have to wait on codex noticing the same thing.

Vocabulary is taken verbatim from CLAUDE.md's RULE 0 text and the specific
phrases this session's drafts were caught using: "no mechanical check",
"nothing checks", "not implemented", "implemented nowhere", "exists
nowhere", "has no", "never", "zero", "absent", "missing", "unreachable",
"not on master", "no path", "does not exist", "is not present".

A hit is EXCUSED only when, within a small window of lines around it, the
draft contains either:
  - a fenced code block (```...```) of at least two lines, or
  - an indented code block (two or more consecutive lines starting with
    4+ spaces or a tab), or
  - an explicit "UNVERIFIED:" marker.
Proximity (not "anywhere in the file") is deliberate: a single pasted
command near the top of a long report must not blanket-excuse an
unrelated absence claim near the bottom. That would make the gate pass
vacuously -- exactly the class of bug this project calls out by name.

Exit codes:
  0  no unevidenced absence claim found (pre-pass does not block)
  1  at least one unevidenced absence claim found (pre-pass BLOCKs)
  2  usage/IO error (missing file, unreadable, etc.) -- never silence
"""
import re
import sys

WINDOW = 8  # lines of context searched on each side of a hit

PATTERNS = [
    r"no mechanical check",
    r"nothing checks",
    r"not implemented",
    r"implemented nowhere",
    r"exists nowhere",
    r"has no\b",
    r"\bnever\b",
    r"\bzero\b",
    r"\babsent\b",
    r"\bmissing\b",
    r"\bunreachable\b",
    r"not on master",
    r"no path\b",
    r"does not exist",
    r"is not present",
]
COMPILED = [re.compile(p, re.IGNORECASE) for p in PATTERNS]

FENCE_RE = re.compile(r"^\s*```")
INDENT_RE = re.compile(r"^(?: {4,}|\t)\S")
UNVERIFIED_RE = re.compile(r"UNVERIFIED:", re.IGNORECASE)


def classify_lines(lines):
    """Return a set of 0-based line indices that are "evidence lines":
    inside a fenced block of >=2 lines, part of an indented run of >=2
    consecutive lines, or carrying an explicit UNVERIFIED: marker."""
    evidence = set()

    # Fenced blocks: pair up ``` markers; everything strictly between two
    # fence markers counts if the block has at least one content line.
    fence_starts = [i for i, l in enumerate(lines) if FENCE_RE.match(l)]
    i = 0
    while i + 1 < len(fence_starts):
        start, end = fence_starts[i], fence_starts[i + 1]
        if end - start >= 2:  # at least one content line between fences
            for j in range(start, end + 1):
                evidence.add(j)
        i += 2

    # Indented runs of >=2 consecutive non-blank indented lines.
    run = []
    for idx, l in enumerate(lines):
        if INDENT_RE.match(l):
            run.append(idx)
        else:
            if len(run) >= 2:
                evidence.update(run)
            run = []
    if len(run) >= 2:
        evidence.update(run)

    # Explicit UNVERIFIED: marker lines.
    for idx, l in enumerate(lines):
        if UNVERIFIED_RE.search(l):
            evidence.add(idx)

    return evidence


def find_hits(lines):
    """Return list of (1-based line number, matched phrase, line text) for
    every absence-shaped assertion, skipping lines that are themselves
    inside an evidence block (a command's own output saying "0 rows" is
    not a claim)."""
    evidence = classify_lines(lines)
    hits = []
    for idx, line in enumerate(lines):
        if idx in evidence:
            continue
        for pat in COMPILED:
            m = pat.search(line)
            if m:
                hits.append((idx, idx + 1, m.group(0), line.strip()))
                break
    return hits, evidence


def has_nearby_evidence(idx, evidence, total):
    lo = max(0, idx - WINDOW)
    hi = min(total - 1, idx + WINDOW)
    return any(j in evidence for j in range(lo, hi + 1))


def main(argv):
    if len(argv) != 2:
        print("usage: absence-prepass.py <draft-file>", file=sys.stderr)
        return 2
    path = argv[1]
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as e:
        print(f"PREPASS ERROR: cannot read {path}: {e}", file=sys.stderr)
        return 2

    lines = text.splitlines()
    hits, evidence = find_hits(lines)

    unevidenced = [
        (lineno, phrase, content)
        for (idx, lineno, phrase, content) in hits
        if not has_nearby_evidence(idx, evidence, len(lines))
    ]

    if not unevidenced:
        print("PREPASS: no unevidenced absence claim found")
        return 0

    print(f"PREPASS BLOCK: {len(unevidenced)} unevidenced absence claim(s) in {path}")
    for lineno, phrase, content in unevidenced:
        print(f'  line {lineno} ("{phrase}"): {content}')
    print(
        "An absence claim needs a pasted command+output (fenced or indented "
        "block) within "
        f"{WINDOW} lines, or an explicit UNVERIFIED: marker on that line."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
