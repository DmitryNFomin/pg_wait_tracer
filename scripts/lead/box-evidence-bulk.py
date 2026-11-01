#!/usr/bin/env python3
"""box-evidence-bulk.py <open-prs-json> <open-issues-json> <worktree-porcelain-file>

The sweep.sh-side equivalent of check-box-evidence.sh, across EVERY open
agent/* PR in ONE pass over data sweep.sh already fetched in bulk -- never
one `gh pr view` / `gh issue view` call per PR. That per-branch-call shape
(302 branches x 0.52s = 2.6 min) is the exact mistake sweep.sh's own header
comment warns about, and check-box-evidence.sh's single-PR CLI shape would
reintroduce it if called once per open PR from here. check-box-evidence.sh
itself stays the manual, one-PR-at-a-time tool a human or reviewer runs; this
script is sweep's bulk view across all of them at once, fed by data sweep.sh
already has lying around (issues.json from the "three bulk calls" section,
the worktree porcelain dump, and ONE extra `gh pr list` call with the fields
this check needs that the existing open-PR queries do not carry).

Inputs (all pre-fetched by the CALLER -- this script makes no gh/git calls
of its own, so it needs no stubbing to test and cannot itself suffer a
gh/git dependency failure):
  open-prs-json       gh pr list --state open --json
                       number,headRefOid,headRefName,closingIssuesReferences
  open-issues-json     gh issue list --state open --json number
  worktree-porcelain   git worktree list --porcelain, OR the literal text
                       "WORKTREE-LIST-FAILED" if that command itself failed
                       (the caller writes this sentinel on a nonzero exit --
                       an empty-but-valid porcelain dump, e.g. the single
                       main worktree with no linked ones, must never be
                       confused with "the git call itself failed").

Same vocabulary as check-box-evidence.sh, with the same reasoning: an empty
result and a failed lookup must never look alike.
  CANDIDATE: PR #N (branch) EVIDENCE-MISSING -- no box-check log reachable
  CANDIDATE: PR #N (branch) EVIDENCE-STALE <log-sha> != <head-sha>
  CANDIDATE: PR #N -> issue #M NO-CLOSING-REF (open; verify body ...)
  UNKNOWN: <reason>                      -- per-PR when only that PR's data
                                              is bad; once, globally, when an
                                              entire INPUT file is bad

Exit codes: 0 nothing to report, 1 only CANDIDATE findings, 3 at least one
UNKNOWN was printed (same "a finding and 'we could not tell' are not the
same code" reasoning as check-box-evidence.sh and held.sh).
"""
import glob
import json
import os
import re
import sys


def parse_porcelain(text):
    """branch (no refs/heads/ prefix) -> worktree path, last entry wins --
    which matches `git worktree list`'s own one-branch-one-worktree
    invariant (a branch can be checked out in at most one worktree)."""
    wt_map = {}
    cur_path = None
    for line in text.splitlines():
        if line.startswith("worktree "):
            cur_path = line[len("worktree "):]
        elif line.startswith("branch ") and cur_path is not None:
            b = line[len("branch "):]
            if b.startswith("refs/heads/"):
                b = b[len("refs/heads/"):]
            wt_map[b] = cur_path
    return wt_map


def branch_issue_nums(branch):
    """agent/507-490-v2 -> ["507","490"]; agent/230-pre-push-hook -> ["230"];
    stops at the first non-numeric dash-separated token, same rule as
    check-box-evidence.sh."""
    if not branch.startswith("agent/"):
        return []
    slug = branch[len("agent/"):]
    nums = []
    for tok in slug.split("-"):
        if tok.isdigit():
            nums.append(tok)
        else:
            break
    return nums


def evidence_for(pr, wt_map):
    """Returns (kind, detail) where kind is one of OK, MISSING, STALE, UNKNOWN."""
    branch = pr.get("headRefName") or ""
    head = pr.get("headRefOid") or ""
    wt = wt_map.get(branch)
    if not wt:
        return ("MISSING", None)
    results_dir = os.path.join(wt, "tests", "results")
    if not os.path.isdir(results_dir):
        return ("MISSING", None)
    logs = sorted(glob.glob(os.path.join(results_dir, "box-check-*.log")))
    if not logs:
        return ("MISSING", None)
    commits = []
    for log in logs:
        try:
            with open(log, "r", encoding="utf-8", errors="replace") as f:
                lines = [l for l in f if l.startswith("box-check: commit=")]
        except OSError as e:
            return ("UNKNOWN", f"{log} unreadable ({e})")
        if not lines:
            return ("UNKNOWN", f"{log} has no commit= header")
        m = re.search(r"commit=([0-9a-f]+)", lines[-1])
        if not m:
            return ("UNKNOWN", f"{log} has no parseable commit=")
        commits.append(m.group(1))
    if head in commits:
        return ("OK", None)
    return ("STALE", commits[-1])


def main(argv):
    if len(argv) != 4:
        print("usage: box-evidence-bulk.py <open-prs-json> <open-issues-json> "
              "<worktree-porcelain-file>", file=sys.stderr)
        return 2

    prs_path, issues_path, wt_path = argv[1], argv[2], argv[3]
    unknown = 0
    finding = 0

    try:
        with open(prs_path) as f:
            prs = json.load(f)
        if not isinstance(prs, list):
            raise TypeError("open-prs-json is not a list")
    except Exception as e:
        print(f"UNKNOWN: cannot read open-PR data ({e})")
        return 3

    try:
        with open(issues_path) as f:
            open_issue_nums = {str(i["number"]) for i in json.load(f)}
        issues_ok = True
    except Exception as e:
        print(f"UNKNOWN: cannot read open-issue data ({e})")
        issues_ok = False
        unknown += 1

    try:
        with open(wt_path, "r", encoding="utf-8", errors="replace") as f:
            wt_text = f.read()
        if wt_text.strip() == "WORKTREE-LIST-FAILED":
            print("UNKNOWN: git worktree list failed -- evidence check skipped for all PRs")
            wt_map = None
            unknown += 1
        else:
            wt_map = parse_porcelain(wt_text)
    except OSError as e:
        print(f"UNKNOWN: cannot read worktree porcelain dump ({e})")
        wt_map = None
        unknown += 1

    for pr in prs:
        branch = pr.get("headRefName") or ""
        if not branch.startswith("agent/"):
            continue
        num = pr.get("number")

        if wt_map is not None:
            kind, detail = evidence_for(pr, wt_map)
            if kind == "MISSING":
                print(f"   CANDIDATE: PR #{num} ({branch}) EVIDENCE-MISSING -- no box-check log reachable")
                finding += 1
            elif kind == "STALE":
                print(f"   CANDIDATE: PR #{num} ({branch}) EVIDENCE-STALE {detail} != {pr.get('headRefOid')}")
                finding += 1
            elif kind == "UNKNOWN":
                print(f"   UNKNOWN: PR #{num} ({branch}) box-check log {detail}")
                unknown += 1
            # OK: nothing to print

        nums = branch_issue_nums(branch)
        if not nums:
            continue
        if not issues_ok:
            continue  # the single global UNKNOWN above already covers this
        refs = {str(r.get("number")) for r in (pr.get("closingIssuesReferences") or [])}
        for n in nums:
            if n in refs:
                continue
            if n in open_issue_nums:
                print(f"   CANDIDATE: PR #{num} -> issue #{n} NO-CLOSING-REF "
                      f"(open; verify the PR body before treating as a bug)")
                finding += 1

    if unknown:
        return 3
    if finding:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
