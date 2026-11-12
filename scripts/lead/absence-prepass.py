#!/usr/bin/env python3
"""absence-prepass.py <draft-file>

Deterministic pre-pass for audit-report.sh. Runs BEFORE any codex call.

RULE 0 requires a pasted command and its output for any ABSENCE claim
("exists nowhere", "not merged", "unreachable", "no mechanical check", "not
implemented", ...). This has been violated in both directions: claims of
absence asserted with nothing pasted, and real evidence buried where a
grader can't find it. This script catches the first kind mechanically, so
a BLOCK does not have to wait on codex noticing the same thing.

Vocabulary comes from CLAUDE.md RULE 0, docs/agent-procedures/reporting.md and the specific
phrases this session's drafts were caught using: "no mechanical check",
"nothing checks", "not implemented", "implemented nowhere", "exists
nowhere", "has no", "never", "zero", "absent", "missing", "unreachable",
"not on master", "no path", "does not exist", "is not present".

A hit needs evidence within WINDOW lines:
  - evidence: paired ``` fences with at least one content line, or an
    indented code block of two or more consecutive lines.
Non-existence-by-search phrasings additionally need, within WINDOW lines:
  - a SEARCHED: line with a non-vacuous value;
  - a NOT SEARCHED: line with a non-vacuous value.
Values must have at least eight characters after stripping whitespace and
trailing punctuation, and must not start (case-insensitively) with none,
nothing, n/a, na, nil, no, all, everything, or nowhere. Optional '-' or '*'
bullets are accepted. SEARCHED:, NOT SEARCHED:, and UNVERIFIED: lines are
not claims themselves; UNVERIFIED: never excuses a different claim.
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
import string
import sys
import unicodedata

WINDOW = 8  # lines of context searched on each side of a hit

PATTERNS = [
    r"\bno mechanical check\b",
    r"\bnothing (?:checks|references)\b",
    r"\bnot implemented\b",
    r"\bnowhere\b",
    r"\bhas no\b",
    r"\bnever\b",
    r"\bzero\b",
    r"\babsent\b",
    r"\bmissing\b",
    r"\bunreachable\b",
    r"\bnot on (?:master|(?:the )?remote)\b",
    r"\bno path\b",
    r"\bdoes(?: not|n't) exist\b",
    r"\bis not present\b",
    r"\bno\b[^.\n]{0,60}\banywhere\b",
    r"\bdo(?:es)? not work\b",
    r"\bnot found\b",
    r"\bno such\b",
    r"\bnone exist\b",
    r"\bisn't there\b",
    r"\bnot there\b",
    r"\bnot (?:been )?merged\b",
    r"\b(?:I|we) (?:can't|cannot|could not|couldn't)\b",
    r"^\s*cannot (?:access|find|read|write|reach|run|open)\b",
    r"\bno caller\b",
    r"\b(?:contains|there (?:is|are)) no\b",
    r"\b(?:isn't|not) (?:wired|called)\b",
    r"\blacks\b",
    r"\bnobody has\b",
    r"\bno guard\b",
    r"\b(?:grep|rg) returned (?:0|no) matches\b",
]
COMPILED = [re.compile(p, re.IGNORECASE) for p in PATTERNS]

# Keep existential/search claims separate from operational claims such as
# "zero failures", "never runs", "lacks a guard" or "missing a trailer".
SEARCH_ABSENCE_RE = re.compile(
    r"\b(?:does(?: not|n't) exist|not found|nowhere|none exist|no such|"
    r"is not present|isn't there|not there|"
    r"absent (?:from|in|on|under|within|at)|"
    r"no mechanical check|nothing (?:checks|references)|no caller|no guard|"
    r"(?:contains|there (?:is|are)) no|"
    r"no\b[^.\n]{0,60}\banywhere|"
    r"(?:grep|rg) returned (?:0|no) matches)\b", re.IGNORECASE)

FENCE_RE = re.compile(r"^\s*```")
INDENT_RE = re.compile(r"^(?: {4,}|\t)\S")
SCOPE_RES = {
    label: re.compile(r"^\s*(?:[-*]\s*)?" + label + r":\s*(.*)", re.IGNORECASE)
    for label in ("SEARCHED", "NOT SEARCHED")
}
UNVERIFIED_RE = re.compile(r"^\s*(?:[-*]\s*)?UNVERIFIED:", re.IGNORECASE)
VACUOUS_RE = re.compile(r"^(?:none|nothing|n/a|na|nil|no|all|everything|nowhere)\b", re.IGNORECASE)


def classify_lines(lines):
    """Return a set of 0-based line indices that are "evidence lines":
    inside a fenced block of >=2 lines, part of an indented run of >=2
    consecutive lines. UNVERIFIED: markers are not evidence."""
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

    return evidence


def find_hits(lines):
    """Return list of (1-based line number, matched phrase, line text) for
    every absence-shaped assertion, skipping lines that are themselves
    inside an evidence block (a command's own output saying "0 rows" is
    not a claim)."""
    evidence = classify_lines(lines)
    hits = []
    for idx, line in enumerate(lines):
        if (idx in evidence or UNVERIFIED_RE.match(line)
                or any(pat.match(line) for pat in SCOPE_RES.values())):
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


def non_vacuous(value):
    value = value.strip()
    while value and (value[-1].isspace() or value[-1] in string.punctuation
                     or unicodedata.category(value[-1]).startswith("P")):
        value = value[:-1]
    return len(value) >= 8 and not VACUOUS_RE.match(value)


def missing_requirements(idx, lines, evidence):
    missing = []
    if not has_nearby_evidence(idx, evidence, len(lines)):
        missing.append("evidence block missing")
    if not SEARCH_ABSENCE_RE.search(lines[idx]):
        return missing
    nearby = lines[max(0, idx - WINDOW):idx + WINDOW + 1]
    for label, pattern in SCOPE_RES.items():
        values = [m.group(1).strip() for line in nearby if (m := pattern.match(line))]
        if not values:
            missing.append(f"{label} line missing")
        elif not any(non_vacuous(value) for value in values):
            missing.append(f"{label} value is vacuous: '{values[0]}'")
    return missing


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

    unevidenced = []
    for idx, lineno, phrase, content in hits:
        missing = missing_requirements(idx, lines, evidence)
        if missing:
            unevidenced.append((lineno, phrase, content, missing))

    if not unevidenced:
        print("PREPASS: no unevidenced absence claim found")
        return 0

    print(f"PREPASS BLOCK: {len(unevidenced)} unevidenced absence claim(s) in {path}")
    for lineno, phrase, content, missing in unevidenced:
        print(f'  line {lineno} ("{phrase}"): {content} -- {"; ".join(missing)}')
    print(
        "An absence claim needs a pasted command+output (fenced or indented "
        "block) within "
        f"{WINDOW} lines. Non-existence-by-search claims also need non-vacuous "
        "SEARCHED: and NOT SEARCHED: lines within that window."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
