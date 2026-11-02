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

Inputs (pre-fetched by the CALLER; no gh calls here). Evidence comparisons
also use local, read-only git rev-parse, cat-file and diff-tree calls in the
same fixed repository as check-box-evidence.sh. Query failures degrade to
UNKNOWN for the affected PR without stopping the remaining checks.
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
  CANDIDATE: PR #N (branch) EVIDENCE-TREE-DRIFT tip=<tree> logged=<tree>
    FINDING(src/tests) or NOTE(other); run the single-PR tool for file details
  CANDIDATE: PR #N (branch) EVIDENCE-UNIDENTIFIED logged tree <tree> unresolvable
  CANDIDATE: PR #N -> issue #M NO-CLOSING-REF (open; verify body ...)
  UNKNOWN: <reason>                      -- per-PR when only that PR's data
                                              is bad; once, globally, when an
                                              entire INPUT file is bad

Matching tree hashes are OK and print nothing, regardless of commit=.
Exit codes: 0 no findings (NOTE drift may be printed), 1 CANDIDATE findings
(src/tests drift, unidentified/missing evidence, closing refs), 3 at least one
UNKNOWN was printed (same "a finding and 'we could not tell' are not the
same code" reasoning as check-box-evidence.sh and held.sh).
"""
import glob
import json
import os
import re
import subprocess
import sys


REPO_ROOT = "/Users/dmitryfomin/work/git/pg_wait_tracer"


def git_read(*args):
    """Run local git plumbing; callers handle per-PR query failures."""
    return subprocess.run(
        ["git", "-C", REPO_ROOT, *args], check=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    ).stdout.strip()


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
    """Return (kind, detail, finding); UNKNOWN takes precedence over findings."""
    branch = pr.get("headRefName") or ""
    head = pr.get("headRefOid") or ""
    wt = wt_map.get(branch)
    if not wt:
        return ("MISSING", None, True)
    results_dir = os.path.join(wt, "tests", "results")
    if not os.path.isdir(results_dir):
        return ("MISSING", None, True)
    logs = sorted(glob.glob(os.path.join(results_dir, "box-check-*.log")))
    if not logs:
        return ("MISSING", None, True)
    try:
        tip_tree = git_read("rev-parse", f"{head}^{{tree}}")
    except (OSError, subprocess.CalledProcessError) as e:
        return ("UNKNOWN", f"cannot resolve tip tree for {head} ({e})", False)
    if not tip_tree:
        return ("UNKNOWN", f"empty tip tree for {head}", False)
    trees = []
    for log in logs:
        try:
            with open(log, "r", encoding="utf-8", errors="replace") as f:
                lines = [l for l in f if l.startswith("box-check: commit=")]
        except OSError as e:
            return ("UNKNOWN", f"{log} unreadable ({e})", False)
        if not lines:
            return ("UNKNOWN", f"{log} has no commit= header", False)
        m = re.match(r"box-check: commit=([0-9a-f]+)(?:\s|$)", lines[-1])
        if not m:
            return ("UNKNOWN", f"{log} has no parseable commit=", False)
        t = re.match(r"box-check: commit=[0-9a-f]+ tree-hash=([0-9a-f]+)(?:\s|$)", lines[-1])
        if not t:
            return ("UNKNOWN", f"{log} has no parseable tree-hash=", False)
        trees.append(t.group(1))
    if tip_tree in trees:
        return ("OK", None, False)
    logged_tree = trees[-1]  # filename order, same newest-log rule as the CLI
    try:
        git_read("cat-file", "-e", logged_tree)
    except subprocess.CalledProcessError:
        return ("UNIDENTIFIED", f"logged tree {logged_tree} unresolvable", True)
    except OSError as e:
        return ("UNKNOWN", f"cannot query logged tree {logged_tree} ({e})", False)
    try:
        diff = git_read("diff-tree", "-r", "--name-status", tip_tree, logged_tree)
    except (OSError, subprocess.CalledProcessError) as e:
        return ("UNKNOWN", f"cannot compare tip/logged trees ({e})", False)
    touches_tests_or_src = any(
        path.lstrip('"').startswith(("src/", "tests/"))
        for line in diff.splitlines() for path in line.split("\t")[1:]
    )
    label = "FINDING(src/tests)" if touches_tests_or_src else "NOTE(other)"
    return ("TREE-DRIFT", f"tip={tip_tree} logged={logged_tree} {label} "
            f"(diff: tip -> logged) -- run check-box-evidence.sh {pr.get('number')} for the file list",
            touches_tests_or_src)


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
            kind, detail, evidence_finding = evidence_for(pr, wt_map)
            if kind == "MISSING":
                print(f"   CANDIDATE: PR #{num} ({branch}) EVIDENCE-MISSING -- no box-check log reachable")
            elif kind in ("TREE-DRIFT", "UNIDENTIFIED"):
                print(f"   CANDIDATE: PR #{num} ({branch}) EVIDENCE-{kind} {detail}")
            elif kind == "UNKNOWN":
                print(f"   UNKNOWN: PR #{num} ({branch}) {detail}")
                unknown += 1
            # OK: nothing to print
            finding += int(evidence_finding)

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
