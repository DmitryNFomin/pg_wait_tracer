#!/usr/bin/env python3
"""Evaluate the finite tag contract using exact-SHA Actions steps and blockers.

Offline fixtures: run-list.json maps workflow filenames to gh run-list arrays;
run-<databaseId>.json is gh run-view JSON (headSha, jobs, workflowName, status);
issues.json is gh issue-list JSON; history.json contains historical TSV versions;
proofs.json contains PR metadata, commit ancestry and changed-file observations.
No fixture or command accepts typed readiness verdicts.
"""
import argparse
from collections import Counter
import csv
import io
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
COLUMNS = "id area statement tolerance tests evidence platforms red_proof".split()
STATES = ("GREEN", "GREEN-UNPROVEN", "WEAK", "RED", "NOT-RUN", "NO-TEST", "RETIRED")
WEAK_TOLERANCES = {"substring", "presence", "ordering", "exit-code"}


def valid_tolerance(value):
    # A bound must contain a number and an explicit comparison/uncertainty.
    return value in {"exact", *WEAK_TOLERANCES} or bool(
        re.search(r"(?:±|<=|>=|<|>|≤|≥)\s*\d", value)
        or re.fullmatch(r"\d+(?:\.\d+)?–\d+(?:\.\d+)?(?:\s+\S.*)?", value))


class Refused(ValueError):
    pass


def command(argv, cwd=ROOT):
    result = subprocess.run(argv, cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise Refused(f"{' '.join(argv)} failed ({result.returncode}): {result.stderr.strip()}")
    return result.stdout


def parse_tsv(data, allow_empty=False, historical=False):
    if not data.strip() and allow_empty:
        return []
    reader = csv.DictReader(io.StringIO(data), delimiter="\t")
    if not reader.fieldnames or reader.fieldnames[:8] != COLUMNS:
        raise Refused("TSV must start with columns: " + " ".join(COLUMNS))
    if len(set(reader.fieldnames)) != len(reader.fieldnames) or set(reader.fieldnames) - set(COLUMNS + ["state", "reason"]):
        raise Refused("duplicate or unknown TSV columns")
    rows, ids = [], set()
    for row in reader:
        if None in row or any(value is None for value in row.values()):
            raise Refused(f"malformed TSV row {reader.line_num}")
        row = {key: value.strip() for key, value in row.items()}
        ident = row["id"]
        if not re.fullmatch(r"L[0-9]{3,}", ident) or ident in ids:
            raise Refused(f"invalid or duplicate id: {ident}")
        ids.add(ident)
        if (any(not row[key] for key in ("area", "statement", "platforms", "red_proof"))
                or (row["tests"] and row["evidence"] and not row["tolerance"])):
            raise Refused(f"{ident}: empty required field")
        if not historical and (not row["tests"] or not row["evidence"]) and row["tolerance"]:
            raise Refused(f"{ident}: NO-TEST row tolerance must be empty")
        if not historical and row["tolerance"] and not valid_tolerance(row["tolerance"]):
            raise Refused(f"{ident}: tolerance must be exact, a numeric bound, substring, presence, ordering or exit-code")
        if row["red_proof"] != "NONE" and not re.fullmatch(
                r"(?:PR #[1-9][0-9]*|#[1-9][0-9]*|[0-9a-f]{40}|https://github\.com/[^/]+/[^/]+/(?:pull/[1-9][0-9]*|commit/[0-9a-f]{40})(?:#\S+)?)",
                row["red_proof"]):
            raise Refused(f"{ident}: red_proof must be NONE, PR number/URL or full sha")
        if row.get("state", "") not in ("", "active", "retired"):
            raise Refused(f"{ident}: invalid explicit state")
        if row.get("state") == "retired" and not row.get("reason"):
            raise Refused(f"{ident}: retired requires a reason column value")
        tests = next(csv.reader([row["tests"]], strict=True)) if row["tests"] else []
        if any("::" not in test or not all(test.split("::", 1)) for test in tests):
            raise Refused(f"{ident}: tests require file::name")
        evidence = [row["evidence"]] if row["evidence"] else []
        for item in evidence:
            if item != "box-check" and (len(item.split("/", 2)) != 3 or not all(item.split("/", 2))):
                raise Refused(f"{ident}: malformed evidence: {item}")
        rows.append(row)
    if not rows:
        raise Refused("empty checklist")
    return rows


def append_only(rows, baseline):
    missing = sorted({row["id"] for row in baseline} - {row["id"] for row in rows})
    if missing:
        raise Refused("removed TSV row(s): " + ", ".join(missing) + "; retain as retired with reason")
    current = {row["id"]: row for row in rows}
    for old in baseline:
        if old.get("state") == "retired" and current[old["id"]].get("state") != "retired":
            raise Refused(f"retired id reused: {old['id']}")


def history_from_git(path, sha):
    """Every version on the evaluated commit's first-parent history, including it.

    Do not compare only a parent/merge-base: that forgives a deletion one commit
    later. Refuse shallow history even when the path's visible log looks complete.
    """
    relative = path.resolve().relative_to(ROOT.resolve()).as_posix()
    if command(["git", "rev-parse", "--is-shallow-repository"]).strip() != "false":
        raise Refused("complete first-parent history required; fetch-depth: 0")
    command(["git", "cat-file", "-e", f"{sha}^{{commit}}"])
    revisions = command(["git", "log", "--first-parent", "--format=%H", sha, "--", relative]).splitlines()
    baseline = []
    for revision in revisions:
        entry = command(["git", "ls-tree", revision, "--", relative]).strip()
        if entry:  # The deletion itself has no blob; earlier versions still do.
            baseline.extend(parse_tsv(command(["git", "show", f"{revision}:{relative}"]), historical=True))
    return baseline


def check_history(rows, path, sha, offline=None):
    if offline:
        versions = read_json(offline / "history.json")
        if not isinstance(versions, list) or any(not isinstance(data, str) for data in versions):
            raise Refused("history.json must contain historical TSV versions")
        baseline = [row for data in versions for row in parse_tsv(data, historical=True)]
    else:
        baseline = history_from_git(path, sha)
    append_only(rows, baseline)


def verify_red_proofs(rows, sha, offline=None):
    proofs, changed_files = None, {}
    for row in rows:
        proof = row["red_proof"]
        if proof == "NONE":
            continue
        test_files = {test.split("::", 1)[0] for test in next(csv.reader([row["tests"]]))} if row["tests"] else set()
        try:
            if proof not in changed_files:
                if offline and proofs is None:
                    proofs = read_json(offline / "proofs.json")
                pr = re.fullmatch(r"(?:PR #|#)([1-9][0-9]*)", proof)
                commit = re.fullmatch(r"[0-9a-f]{40}", proof)
                # URLs accepted by the parser are resolved, never trusted as prose.
                if not pr and not commit:
                    pr = re.fullmatch(r"https://github\.com/[^/]+/[^/]+/pull/([1-9][0-9]*)(?:#\S+)?", proof)
                    commit = re.fullmatch(r"https://github\.com/[^/]+/[^/]+/commit/([0-9a-f]{40})(?:#\S+)?", proof)
                if pr:
                    number = int(pr[1])
                    target = proof if proof.startswith("https://") else str(number)
                    data = proofs.get(proof) if offline and isinstance(proofs, dict) else (
                        gh_json(["pr", "view", target, "--json", "number,state,mergedAt,files"]) if not offline else None)
                    if (not isinstance(data, dict) or type(data.get("number")) is not int or data.get("number") != number
                            or data.get("state") != "MERGED" or not isinstance(data.get("mergedAt"), str) or not data["mergedAt"]):
                        raise Refused("PR must exist and be merged")
                    files = data.get("files")
                    if (not isinstance(files, list) or any(not isinstance(item, dict)
                            or not isinstance(item.get("path"), str) or not item["path"] for item in files)):
                        raise Refused("missing/invalid PR changed files")
                    paths = [item["path"] for item in files]
                elif commit:
                    ancestor = proof if len(proof) == 40 else commit[1]
                    if offline:
                        data = proofs.get(proof) if isinstance(proofs, dict) else None
                        if not isinstance(data, dict) or data.get("ancestor") != ancestor or data.get("descendant") != sha or data.get("isAncestor") is not True:
                            raise Refused(f"not a verified ancestor of {sha}")
                        paths = data.get("files")
                    else:
                        command(["git", "merge-base", "--is-ancestor", ancestor, sha])
                        # Compare merges to their first parent; include root commits.
                        paths = command(["git", "diff-tree", "--root", "--diff-merges=first-parent", "--no-commit-id",
                                         "--name-only", "-r", "-z", ancestor]).rstrip("\0").split("\0")
                    if not isinstance(paths, list) or any(not isinstance(path, str) or not path for path in paths):
                        raise Refused("missing/invalid commit changed files")
                else:
                    raise Refused("unverifiable proof")
                changed_files[proof] = set(paths)
            if not test_files.intersection(changed_files[proof]):
                raise Refused("proof does not touch any row test file")
        except (Refused, OSError) as exc:
            files = ", ".join(sorted(test_files)) or "(none)"
            raise Refused(f"{row['id']}: red_proof {proof}; row test files: {files}: {exc}") from exc


def read_json(path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError) as exc:
        raise Refused(f"invalid/unavailable JSON {path}: {exc}") from exc


def gh_json(args):
    try:
        return json.loads(command(["gh", *args]))
    except ValueError as exc:
        raise Refused(f"invalid gh JSON: {exc}") from exc


def load_evidence(rows, sha, offline=None, run_id=None):
    workflows = sorted({item.split("/", 1)[0] for row in rows
                        for item in [row["evidence"]] if item and item != "box-check"}) or ["ci.yml"]
    lists = read_json(offline / "run-list.json") if offline else None
    if offline and not isinstance(lists, dict):
        raise Refused("run-list.json must map workflow filenames to run arrays")
    steps, sources = {}, []
    for workflow in workflows:
        runs = lists.get(workflow) if offline else gh_json([
            "run", "list", "--workflow", workflow, "--commit", sha, "--branch", "master",
            "--event", "push", "--limit", "100", "--json",
            "databaseId,headSha,status,workflowName,headBranch,event"])
        if not isinstance(runs, list):
            raise Refused(f"{workflow}: invalid run list")
        for run in runs:
            if not isinstance(run, dict) or run.get("headSha") != sha:
                raise Refused(f"{workflow}: run for wrong sha (wanted {sha})")
            if (type(run.get("databaseId")) is not int or not isinstance(run.get("workflowName"), str) or not run["workflowName"]
                    or run.get("headBranch") != "master" or run.get("event") != "push"
                    or run.get("status") not in ("completed", "queued", "in_progress", "waiting", "pending", "requested")):
                raise Refused(f"{workflow}: malformed run metadata (requires master push)")
        if run_id is not None:
            runs = [run for run in runs if run["databaseId"] == run_id]
            if not runs:
                raise Refused(f"{workflow}: requested run {run_id} absent for {sha}")
        if not runs:
            raise Refused(f"{workflow}: no CI run for {sha}")
        run = max(runs, key=lambda entry: entry["databaseId"])
        ident = run["databaseId"]
        view = read_json(offline / f"run-{ident}.json") if offline else gh_json([
            "run", "view", str(ident), "--json", "headSha,jobs,workflowName,status,databaseId,conclusion,attempt"])
        if not isinstance(view, dict) or view.get("headSha") != sha:
            raise Refused(f"run {ident}: wrong sha (wanted {sha})")
        if (view.get("databaseId") != ident or view.get("workflowName") != run["workflowName"]
                or view.get("status") != run["status"]):
            raise Refused(f"run {ident}: run-list/view identity or status mismatch")
        if (view.get("conclusion") not in (None, "", "success", "failure", "skipped", "cancelled", "timed_out", "action_required", "neutral", "stale", "startup_failure")
                or (view["status"] == "completed" and not view.get("conclusion"))):
            raise Refused(f"run {ident}: missing/invalid conclusion")
        jobs = view.get("jobs")
        if not isinstance(jobs, list) or not jobs:
            raise Refused(f"run {ident}: empty/void jobs JSON")
        job_names = set()
        for job in jobs:
            if (not isinstance(job, dict) or not isinstance(job.get("name"), str) or not job["name"] or job["name"] in job_names
                    or not isinstance(job.get("steps"), list)):
                raise Refused(f"run {ident}: malformed/duplicate jobs")
            job_names.add(job["name"])
            step_names = set()
            for step in job["steps"]:
                if (not isinstance(step, dict) or not isinstance(step.get("name"), str) or not step["name"] or step["name"] in step_names
                        or step.get("conclusion") not in (None, "", "success", "failure", "skipped", "cancelled", "timed_out", "action_required", "neutral", "stale", "startup_failure")):
                    raise Refused(f"run {ident}: malformed/duplicate steps")
                step_names.add(step["name"])
                key = (workflow, job["name"], step["name"])
                # In-progress runs can never grant proof, even if an early
                # step is already successful.
                steps[key] = step.get("conclusion") if run["status"] == "completed" else "in_progress"
        sources.append({"workflow": workflow, "run": ident, "status": run["status"],
                        "conclusion": view.get("conclusion"), "sha": sha, "attempt": view.get("attempt")})
    issues = read_json(offline / "issues.json") if offline else gh_json([
        "issue", "list", "--state", "open", "--label", "tag-blocker", "--limit", "10000",
        "--json", "number,title,labels"])
    if not isinstance(issues, list) or len(issues) >= 10000:
        raise Refused("invalid or potentially truncated blocker issue list")
    return steps, sources, issues


def evaluate(rows, steps, issues):
    ids = {row["id"] for row in rows}
    blockers, unmapped, numbers = {}, [], set()
    for issue in issues:
        if (not isinstance(issue, dict) or type(issue.get("number")) is not int
                or issue["number"] in numbers or not isinstance(issue.get("title"), str)
                or not isinstance(issue.get("labels"), list)
                or any(not isinstance(label, dict) or not isinstance(label.get("name"), str) for label in issue["labels"])):
            raise Refused("malformed/duplicate blocker issue")
        numbers.add(issue["number"])
        labels = {label["name"] for label in issue["labels"]}
        if "tag-blocker" not in labels:
            raise Refused(f"issue #{issue['number']}: missing tag-blocker label")
        mapped = {label[5:] for label in labels if label.startswith("line:")}
        if mapped - ids:
            raise Refused(f"issue #{issue['number']}: unknown line label(s): " + ", ".join(sorted(mapped - ids)))
        if not mapped:
            unmapped.append(issue)
        for ident in mapped:
            blockers.setdefault(ident, []).append(issue["number"])
    results = []
    for row in rows:
        evidence = [row["evidence"]] if row["evidence"] else []
        conclusions = [steps.get(tuple(item.split("/", 2))) if item != "box-check" else None for item in evidence]
        all_success = bool(row["tests"] and evidence) and all(value == "success" for value in conclusions)
        failed = any(value in ("failure", "timed_out", "startup_failure") for value in conclusions)
        gaps = blockers.get(row["id"], [])
        if gaps or failed:
            state = "RED"
            reason = ("open tag-blocker " + ", ".join(f"#{number}" for number in gaps)) if gaps else "evidence step failed"
            if gaps and all_success:
                reason += "; test gap"
        elif row.get("state") == "retired":
            state, reason = "RETIRED", row["reason"]
        elif not row["tests"] or not evidence:
            state, reason = "NO-TEST", "no tests or no evidence step"
        elif not all_success:
            state, reason = "NOT-RUN", "box-only, absent, skipped, unfinished or non-success step"
        elif row["tolerance"] in WEAK_TOLERANCES:
            state, reason = "WEAK", "asserts only " + row["tolerance"] + "; cannot certify numerical correctness"
        elif row["red_proof"] == "NONE":
            state, reason = "GREEN-UNPROVEN", "red_proof NONE"
        else:
            state, reason = "GREEN", "all evidence steps success; red proof recorded; no open blocker"
        results.append({**row, "state": state, "reason": reason})
    for issue in sorted(unmapped, key=lambda entry: entry["number"]):
        results.append({"id": f"issue:{issue['number']}", "area": "Unmapped blockers", "statement": issue["title"],
                        "state": "RED", "reason": f"unmapped tag-blocker #{issue['number']}"})
    return results


def report(sha, rows, sources):
    counts = {state: 0 for state in STATES}
    counts.update(Counter(row["state"] for row in rows))
    return {"sha": sha, "header": f"GREEN {counts['GREEN']} / total {len(rows)}",
            "total": len(rows), "counts": counts, "sources": sources, "lines": rows}


def markdown(result):
    def cell(value):
        return str(value).replace("&", "&amp;").replace("<", "&lt;").replace("|", "&#124;").replace("\n", " ").replace("\r", " ")
    out = [f"# {result['header']}", "", f"Commit: `{result['sha']}`", "",
           " · ".join(f"{state} {count}" for state, count in result["counts"].items()), ""]
    for source in result["sources"]:
        conclusion = source.get("conclusion") or "pending"
        out.append(f"CI `{cell(source['workflow'])}` run `{source['run']}` attempt `{source.get('attempt')}`: "
                   f"{cell(source['status'])}, conclusion `{cell(conclusion)}`, SHA `{source['sha']}`")
    out.append("")
    for area in dict.fromkeys(row["area"] for row in result["lines"]):
        out.extend([f"## {cell(area)}", "", "| ID | Guarantee | State | Tolerance | Platforms | Reason |",
                    "|---|---|---|---|---|---|"])
        for row in result["lines"]:
            if row["area"] == area:
                out.append("| " + " | ".join(cell(row.get(key, "")) for key in ("id", "statement", "state", "tolerance", "platforms", "reason")) + " |")
        out.append("")
    return "\n".join(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sha", help="full 40-character commit SHA")
    parser.add_argument("--tsv", type=Path, default=ROOT / "tests/tag_checklist.tsv")
    parser.add_argument("--offline", type=Path)
    parser.add_argument("--check-history", action="store_true", help="validate the real TSV against HEAD history without querying CI")
    parser.add_argument("--run-id", type=int, help="pin to the workflow_run ID")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--markdown-output", type=Path, help="also write Markdown from the same evidence snapshot")
    args = parser.parse_args(argv)
    try:
        if not re.fullmatch(r"[0-9a-f]{40}", args.sha):
            raise Refused("requires a full lowercase 40-character commit sha")
        rows = parse_tsv(args.tsv.read_text())
        check_history(rows, args.tsv, args.sha, args.offline)
        if args.check_history:
            print(f"OK: {len(rows)} checklist rows retain every historical first-parent ID")
            return 0
        verify_red_proofs(rows, args.sha, args.offline)
        steps, sources, issues = load_evidence(rows, args.sha, args.offline, args.run_id)
        result = report(args.sha, evaluate(rows, steps, issues), sources)
        if args.markdown_output:
            args.markdown_output.write_text(markdown(result) + "\n")
        print(json.dumps(result, indent=2, ensure_ascii=False) if args.json else markdown(result))
        return 0
    except (Refused, OSError, ValueError, csv.Error) as exc:
        print(f"REFUSED: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
