#!/usr/bin/env python3
"""Recompute the release ledger from tree-bound evidence (stdlib only)."""
import argparse
import csv
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
COLUMNS = {
    "contract_accepted": "Contract accepted",
    "independent_answer": "Independent answer exists",
    "real_interface": "Correct in the real interface",
    "broken_versions": "Broken versions caught",
    "binary_identified": "Binary identified",
    "load_platform": "Load and platform exercised",
    "old_code_deleted": "Old code deleted",
    "cost_next": "Cost so far / next action",
}
ATTESTED = {"contract_accepted", "independent_answer", "cost_next"}
NO_PRODUCER = {"real_interface", "load_platform", "old_code_deleted"}
HASH = r"[0-9a-f]{40}(?:[0-9a-f]{24})?"
REPOSITORY = "https://github.com/DmitryNFomin/pg_wait_tracer"


class EvidenceError(ValueError):
    pass


def require(condition, reason):
    if not condition:
        raise EvidenceError(reason)


def read_json(path):
    # Duplicate fields are ambiguous evidence, even if the last value is PASS.
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "malformed: duplicate JSON field")
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True,
                                   stderr=subprocess.PIPE).strip()


def contract_claims(commit, contract):
    require(re.fullmatch(r"[A-Za-z0-9_-]+", contract), "malformed contract id")
    text = git("show", f"{commit}:contracts/{contract}/CLAIMS.tsv")
    rows = csv.DictReader((line for line in text.splitlines()
                           if line and not line.startswith("#")), delimiter="\t")
    require(rows.fieldnames == ["claim_id", "claim", "proof_ids"],
            "malformed contract claims")
    result = {}
    for row in rows:
        ident, title, proofs = row["claim_id"], row["claim"], row["proof_ids"]
        require(None not in row and ident and ident not in result and title and proofs,
                "malformed contract claim")
        refs = proofs.split()
        require(len(refs) == len(set(refs)), "malformed contract proof ids")
        result[ident] = {"claim": title, "proof_ids": refs}
    require(result, "missing contract claims")
    return result


def same_tree(record, tree):
    require(isinstance(record, dict), "malformed record")
    value = record.get("tree_hash")
    require(isinstance(value, str) and re.fullmatch(HASH, value), "missing tree hash")
    require(value == tree, "stale")


def attestation(record):
    link = record.get("link")
    if type(link) is int and link > 0:
        link = f"#{link}"
    require(isinstance(link, str) and link, "missing attestation link")
    if re.fullmatch(r"#[1-9][0-9]*", link):
        link = f"{REPOSITORY}/pull/{link[1:]}"
    else:
        require(re.fullmatch(
            re.escape(REPOSITORY) + r"/"
            r"(?:pull/[1-9][0-9]*|issues/[1-9][0-9]*#issuecomment-[1-9][0-9]*)", link),
            "malformed attestation link")
    return [link]


def box_log(record, tree, base):
    value = record.get("path")
    require(isinstance(value, str) and value, "missing box-check log")
    path = base / value
    headers = [line for line in path.read_text(encoding="utf-8").splitlines()
               if line.startswith("box-check: commit=")]
    require(len(headers) == 1, "missing or ambiguous build provenance header")
    match = re.fullmatch(
        rf"box-check: commit=({HASH}) tree-hash=({HASH}) built-from=({HASH})", headers[0])
    require(match, "malformed build provenance header")
    require(match[2] == tree, "stale")
    require(match[3] == match[2], "built-from differs from tree-hash")
    return [str(path.resolve())]


def passing_proof(proofs, ident):
    proof = proofs.get(ident)
    require(isinstance(proof, dict) and proof.get("state") == "PASS",
            f"missing or non-PASS proof: {ident}")
    lines = proof.get("lines")
    require(isinstance(lines, list) and len(lines) == 1 and isinstance(lines[0], str),
            f"malformed proof: {ident}")
    line = lines[0]
    if ident.startswith("case:"):
        require(line == ident[5:] + " PASS", f"malformed case proof: {ident}")
    elif ident.startswith("diff:"):
        require(line == f"NAMED {ident[5:]} MATCH", f"malformed diff proof: {ident}")
    elif ident.startswith("mutant:"):
        match = re.fullmatch(
            re.escape(ident[7:]) + r" KILLED \((contract|differential) ([A-Za-z0-9_]+), "
            r"build OK, (case mismatch|reference mismatch)\) "
            r"definition-sha256=[0-9a-f]{64} source-sha256=[0-9a-f]{64}", line)
        require(match and ((match[1], match[3]) in (
            ("contract", "case mismatch"), ("differential", "reference mismatch"))),
            f"malformed mutant proof: {ident}")
        return (("case:" + match[2] + ".json") if match[1] == "contract"
                else "diff:" + match[2])
    else:
        raise EvidenceError(f"unknown proof type: {ident}")
    return None


def core_status(record, tree, artifact, reference, expected):
    require(artifact is not None, "missing artifact directory")
    contract, claim_id = reference.split(":")
    require(record.get("contract") == contract, "wrong evidence contract")
    sources = []
    for mode in ("plain", "sanitizers"):
        directory = artifact / mode
        provenance = read_json(directory / "provenance.json")
        same_tree(provenance, tree)
        # L1 records github.sha (a PR merge commit for PR runs), NOT head_sha.
        github = provenance.get("github")
        require(provenance.get("job") == "core-status" and isinstance(github, dict),
                "malformed CI provenance")
        require(isinstance(github.get("sha"), str) and re.fullmatch(HASH, github["sha"]),
                "malformed CI sha")
        require(all(isinstance(github.get(key), str) and
                    re.fullmatch(r"[1-9][0-9]*", github[key])
                    for key in ("run_id", "run_attempt")), "malformed CI run")
        path = directory / f"{contract}.json"
        status = read_json(path)
        same_tree(status, tree)
        require(status.get("contract") == contract, "wrong status contract")
        require(status.get("state") == "PASS" and status.get("errors") == [],
                "core-status overall is not PASS with empty errors")
        claims, proofs = status.get("claims"), status.get("proofs")
        require(isinstance(claims, list) and claims and isinstance(proofs, dict) and proofs,
                "malformed core-status inventory")
        found = {}
        for claim in claims:
            require(isinstance(claim, dict) and isinstance(claim.get("id"), str)
                    and claim["id"] not in found, "malformed or duplicate claim row")
            require(claim.get("state") == "PASS", "core-status claim is not PASS")
            found[claim["id"]] = claim
        claim = found.get(claim_id)
        require(claim is not None, "missing claim row")
        refs = claim.get("proof_ids")
        require(isinstance(refs, list) and refs and all(isinstance(p, str) for p in refs)
                and len(refs) == len(set(refs)), "malformed claim proof ids")
        require(set(refs) == set(expected["proof_ids"]), "claim proof ids differ from contract")
        targets = []
        for ref in refs:
            target = passing_proof(proofs, ref)
            if target is not None:
                passing_proof(proofs, target)
                targets.append(target)
        require(any(ref.startswith("case:") for ref in refs), "diff-only claim")
        require(any(target.startswith("case:") for target in targets),
                "no mutant with independent intended case")
        sources.extend([str(path.resolve()), str((directory / "provenance.json").resolve()),
                        f"{REPOSITORY}/actions/runs/{github['run_id']}/attempts/{github['run_attempt']}"])
    return sources


def evaluate(manifest, commit, tree, artifact, base, load_contract):
    require(isinstance(manifest, dict) and manifest.get("version") == 1,
            "malformed manifest version")
    evidence, claims = manifest.get("evidence"), manifest.get("claims")
    require(isinstance(evidence, dict) and isinstance(claims, list) and claims,
            "malformed manifest inventory")
    rows, seen = [], set()
    for claim in claims:
        require(isinstance(claim, dict) and isinstance(claim.get("id"), str)
                and claim["id"] and claim["id"] not in seen, "malformed or duplicate ledger id")
        seen.add(claim["id"])
        reference = claim.get("contract_claim")
        expected, contract_error = None, None
        if reference is not None:
            try:
                require(isinstance(reference, str) and reference.count(":") == 1,
                        "malformed contract reference")
                contract, ident = reference.split(":")
                expected = load_contract(contract).get(ident)
                require(expected is not None, "missing contract claim")
            except (ValueError, OSError, subprocess.SubprocessError) as error:
                contract_error = str(error)
        title = expected["claim"] if expected else claim.get("title", claim["id"])
        require(isinstance(title, str), "malformed claim title")
        cells = {}
        declared = claim.get("cells")
        for column in COLUMNS:
            refs = declared.get(column) if isinstance(declared, dict) else None
            cell = {"state": "FAIL", "kind": "attested" if column in ATTESTED else "measured",
                    "evidence_ids": refs if isinstance(refs, list) else [], "sources": []}
            try:
                require(column not in NO_PRODUCER, "no evidence producer")
                require(not contract_error, contract_error)
                if column == "contract_accepted":
                    require(reference is not None, "no contract")
                if column == "broken_versions":
                    require(expected is not None, "no contract")
                require(isinstance(refs, list) and refs and all(isinstance(r, str) for r in refs)
                        and len(refs) == len(set(refs)), "missing or malformed evidence ids")
                for ident in refs:
                    record = evidence.get(ident)
                    require(isinstance(record, dict), f"missing evidence: {ident}")
                    kind = ("attestation" if column in ATTESTED else
                            "core-status" if column == "broken_versions" else "box-check")
                    require(record.get("kind") == kind, "wrong evidence kind")
                    if kind == "attestation":
                        sources = attestation(record)
                    elif kind == "core-status":
                        sources = core_status(record, tree, artifact, reference, expected)
                    else:
                        sources = box_log(record, tree, base)
                    cell["sources"].extend(sources)
                cell.update(state="PASS", reason="attested" if column in ATTESTED else "measured")
            except (ValueError, OSError, TypeError, KeyError, subprocess.SubprocessError) as error:
                cell["reason"] = str(error) or "malformed evidence"
            cells[column] = cell
        rows.append({"id": claim["id"], "contract_claim": reference, "claim": title, "cells": cells})
    return {"commit": commit, "tree_hash": tree, "columns": COLUMNS, "claims": rows}


def markdown(ledger):
    def escape(value):
        return str(value).replace("|", "\\|").replace("\n", " ").replace("\r", " ")
    lines = ["| Claim | " + " | ".join(COLUMNS.values()) + " |",
             "|---|" + "---|" * len(COLUMNS)]
    for row in ledger["claims"]:
        label = row["id"] if row["contract_claim"] else row["claim"]
        cells = []
        for cell in row["cells"].values():
            if cell["state"] == "PASS":
                # Keep the table small; ledger.json retains every local record
                # and provenance path, while CI cells link to the run attempt.
                remote = [s for s in cell["sources"] if s.startswith("https://")]
                sources = list(dict.fromkeys(remote or cell["sources"]))
                links = ", ".join(f"[evidence](<{escape(source)}>)" for source in sources)
                cells.append(f"🟢 {cell['kind']} {links}")
            else:
                cells.append("🔴 " + escape(cell["reason"]))
        lines.append("| " + escape(label) + " | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()
    try:
        commit = git("rev-parse", "--verify", "--end-of-options", args.commit + "^{commit}")
        tree = git("rev-parse", commit + "^{tree}")
        manifest = read_json(ROOT / "ledger/claims.json")
        cache = {}

        def load_contract(contract):
            if contract not in cache:
                cache[contract] = contract_claims(commit, contract)
            return cache[contract]

        ledger = evaluate(manifest, commit, tree, args.artifact_dir.resolve()
                          if args.artifact_dir else None, ROOT, load_contract)
        Path("ledger.json").write_text(json.dumps(ledger, indent=2) + "\n", encoding="utf-8")
        print(markdown(ledger))
    except (ValueError, OSError, TypeError, KeyError, subprocess.SubprocessError) as error:
        print(f"ledger: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
