#!/usr/bin/env python3
"""Negative controls for the ledger; all evidence lives in a temporary directory."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("ledger", ROOT / "scripts/ledger.py")
ledger = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ledger)
TREE = "a" * 40
COMMIT = "b" * 40
OTHER = "c" * 40
REFERENCE = "example-v1:C01"
PROOFS = ["case:edge.json", "mutant:M1", "diff:agreement"]
DEFINITIONS = {"C01": {"claim": "An independent boundary answer", "proof_ids": PROOFS}}
CONTROLS = []


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data) + "\n")


def fixture(root):
    evidence = {
        "contract": {"kind": "attestation", "link": "#123"},
        "answer": {"kind": "attestation",
                   "link": "https://github.com/example/project/pull/124"},
        "cost": {"kind": "attestation",
                 "link": "https://github.com/example/project/issues/125#issuecomment-126"},
        "core": {"kind": "core-status", "contract": "example-v1"},
        "binary": {"kind": "box-check", "path": "box.log"},
    }
    manifest = {"version": 1, "evidence": evidence, "claims": [{
        "id": REFERENCE, "contract_claim": REFERENCE,
        "cells": {column: refs for column, refs in zip(ledger.COLUMNS, [
            ["contract"], ["answer"], [], ["core"], ["binary"], [], [], ["cost"]])},
    }]}
    status = {
        "contract": "example-v1", "commit": COMMIT, "tree_hash": TREE,
        "state": "PASS", "errors": [],
        "claims": [{"id": "C01", "state": "PASS", "proof_ids": PROOFS}],
        "proofs": {
            "case:edge.json": {"state": "PASS", "lines": ["edge.json PASS"]},
            "mutant:M1": {"state": "PASS", "lines": [
                "M1 KILLED (contract edge, build OK, case mismatch) "
                + "definition-sha256=" + "d" * 64 + " source-sha256=" + "e" * 64]},
            "diff:agreement": {"state": "PASS", "lines": ["NAMED agreement MATCH"]},
        },
    }
    provenance = {"github": {"sha": COMMIT, "run_id": "123", "run_attempt": "2"},
                  "job": "core-status", "tree_hash": TREE}
    for mode in ("plain", "sanitizers"):
        write(root / mode / "example-v1.json", status)
        write(root / mode / "provenance.json", provenance)
    (root / "box.log").write_text(
        f"box-check: source commit={COMMIT} tree-hash={TREE} (build provenance pending)\n"
        f"box-check: commit={COMMIT} tree-hash={TREE} built-from={TREE}\n")
    return manifest


def evaluate(manifest, root, definitions=None):
    result = ledger.evaluate(manifest, OTHER, TREE, root, root,
                             lambda contract: definitions or DEFINITIONS)
    return result["claims"][0]["cells"]


def change_record(root, filename, change, mode="plain"):
    path = root / mode / filename
    record = json.loads(path.read_text())
    change(record)
    write(path, record)


def red(cells, column, reason):
    cell = cells[column]
    assert cell["state"] == "FAIL" and reason in cell["reason"], cell


def control(name, run):
    with tempfile.TemporaryDirectory(prefix="pgwt-ledger-test-") as tmp:
        root = Path(tmp)
        manifest = fixture(root)
        run(root, manifest)
    CONTROLS.append(name)


def all_good(root, manifest):
    cells = evaluate(manifest, root)
    assert [key for key, value in cells.items() if value["state"] == "PASS"] == [
        "contract_accepted", "independent_answer", "broken_versions", "binary_identified", "cost_next"]
    for column in ledger.NO_PRODUCER:
        red(cells, column, "no evidence producer")
    for column in ledger.ATTESTED:
        assert cells[column]["kind"] == "attested"
    assert cells["broken_versions"]["kind"] == "measured"
    assert all(value["sources"] for value in cells.values() if value["state"] == "PASS")
    # Evaluation uses OTHER; CI's sha is COMMIT, and no PR head field exists.
    assert "head_sha" not in (root / "plain/provenance.json").read_text()


def status_control(root, manifest, change, reason, mode="plain"):
    change_record(root, "example-v1.json", change, mode)
    red(evaluate(manifest, root), "broken_versions", reason)


def diff_only(root, manifest):
    for mode in ("plain", "sanitizers"):
        change_record(root, "example-v1.json",
                      lambda s: s["claims"][0].update(proof_ids=["diff:agreement"]), mode)
    red(evaluate(manifest, root, {"C01": {"claim": "Agreement only",
                                         "proof_ids": ["diff:agreement"]}}),
        "broken_versions", "diff-only claim")


def diff_mutant(root, manifest):
    for mode in ("plain", "sanitizers"):
        change_record(root, "example-v1.json", lambda s: s["proofs"]["mutant:M1"].update(lines=[
            "M1 KILLED (differential agreement, build OK, reference mismatch) "
            + "definition-sha256=" + "d" * 64 + " source-sha256=" + "e" * 64]), mode)
    red(evaluate(manifest, root), "broken_versions", "no mutant with independent intended case")


def provenance_control(root, manifest, change, reason):
    change_record(root, "provenance.json", change)
    red(evaluate(manifest, root), "broken_versions", reason)


def box_control(root, manifest, content, reason):
    (root / "box.log").write_text(content)
    red(evaluate(manifest, root), "binary_identified", reason)


def attestation_control(root, manifest, change, reason):
    change(manifest["evidence"]["contract"])
    red(evaluate(manifest, root), "contract_accepted", reason)


def missing_mode(root, manifest):
    (root / "sanitizers/example-v1.json").unlink()
    red(evaluate(manifest, root), "broken_versions", "No such file")


def malformed(root, manifest):
    (root / "plain/example-v1.json").write_text('{"state":"PASS","state":"FAIL"}')
    red(evaluate(manifest, root), "broken_versions", "duplicate JSON field")


def no_contract(root, manifest):
    manifest["claims"][0]["contract_claim"] = None
    red(evaluate(manifest, root), "contract_accepted", "no contract")
    red(evaluate(manifest, root), "broken_versions", "no contract")


def evidence_ids(root, manifest):
    manifest["claims"][0]["cells"]["contract_accepted"] = ["contract", "unknown"]
    red(evaluate(manifest, root), "contract_accepted", "missing evidence: unknown")


def missing_attestations(root, manifest):
    for ident in ("contract", "answer", "cost"):
        manifest["evidence"][ident].pop("link")
    cells = evaluate(manifest, root)
    for column in ledger.ATTESTED:
        red(cells, column, "missing attestation link")


def no_producer_override(root, manifest):
    for column in ledger.NO_PRODUCER:
        manifest["claims"][0]["cells"][column] = ["contract"]
    cells = evaluate(manifest, root)
    for column in ledger.NO_PRODUCER:
        red(cells, column, "no evidence producer")


def no_reused_status(root, manifest):
    # A previous green output or a hand-entered state must not survive.
    manifest["claims"][0]["state"] = "PASS"
    (root / "ledger.json").write_text('{"state":"PASS"}')
    (root / "plain/example-v1.json").unlink()
    red(evaluate(manifest, root), "broken_versions", "No such file")


def real_cli(root, manifest):
    # Exercise revision lookup, frozen claim resolution, Markdown and JSON output.
    command = [sys.executable, "-B", str(ROOT / "scripts/ledger.py"), "--commit", "HEAD"]
    result = subprocess.run(command, cwd=root, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    report = json.loads((root / "ledger.json").read_text())
    assert len(report["claims"]) == 7
    assert len(report["columns"]) == 8
    assert report["tree_hash"] == ledger.git("rev-parse", "HEAD^{tree}")
    assert "missing artifact directory" in result.stdout
    assert len(result.stdout.splitlines()) == 9
    assert report["claims"][0]["claim"] == ledger.contract_claims(
        report["commit"], "wait_time-v1")["C01"]["claim"]
    assert all(c["state"] == "FAIL" for r in report["claims"] for c in r["cells"].values())


control("all-good / existing provenance / same tree, different commit", all_good)
control("stale status", lambda r, m: status_control(r, m, lambda s: s.update(tree_hash=OTHER), "stale"))
control("stale provenance", lambda r, m: provenance_control(r, m, lambda s: s.update(tree_hash=OTHER), "stale"))
control("overall FAIL, rows PASS", lambda r, m: status_control(r, m, lambda s: s.update(state="FAIL"), "overall"))
control("overall PASS, errors", lambda r, m: status_control(r, m, lambda s: s.update(errors=["failure"]), "overall"))
control("overall skipped", lambda r, m: status_control(r, m, lambda s: s.update(state="NOT-RUN"), "overall"))
control("claim FAIL", lambda r, m: status_control(r, m, lambda s: s["claims"][0].update(state="FAIL"), "claim is not PASS"))
control("diff-only claim", diff_only)
control("all mutants use differential intended cases", diff_mutant)
control("missing mutant", lambda r, m: status_control(r, m, lambda s: s["proofs"].pop("mutant:M1"), "missing or non-PASS"))
control("proof skipped", lambda r, m: status_control(r, m, lambda s: s["proofs"]["mutant:M1"].update(state="NOT-RUN"), "non-PASS"))
control("missing intended case", lambda r, m: status_control(r, m, lambda s: s["proofs"].pop("case:edge.json"), "missing or non-PASS"))
control("narrowed claim proofs", lambda r, m: status_control(r, m, lambda s: s["claims"][0].update(proof_ids=["diff:agreement"]), "differ from contract"))
control("malformed mutant", lambda r, m: status_control(r, m, lambda s: s["proofs"]["mutant:M1"].update(lines=["M1 KILLED"]), "malformed mutant"))
control("sanitizer failure", lambda r, m: status_control(r, m, lambda s: s.update(state="FAIL"), "overall", "sanitizers"))
control("missing sanitizer record", missing_mode)
control("malformed JSON", malformed)
control("missing CI metadata", lambda r, m: provenance_control(r, m, lambda s: s.pop("github"), "malformed CI"))
control("pending build provenance", lambda r, m: box_control(r, m, f"box-check: source commit={COMMIT} tree-hash={TREE} (build provenance pending)\n", "provenance header"))
control("stale binary", lambda r, m: box_control(r, m, f"box-check: commit={COMMIT} tree-hash={OTHER} built-from={OTHER}\n", "stale"))
control("wrong built-from", lambda r, m: box_control(r, m, f"box-check: commit={COMMIT} tree-hash={TREE} built-from={OTHER}\n", "built-from differs"))
control("ambiguous header", lambda r, m: box_control(r, m, (r / "box.log").read_text() * 2, "ambiguous"))
control("missing attestation", lambda r, m: attestation_control(r, m, lambda e: e.pop("link"), "missing attestation link"))
control("malformed attestation", lambda r, m: attestation_control(r, m, lambda e: e.update(link="javascript:alert(1)"), "malformed attestation link"))
control("numeric PR attestation", lambda r, m: (
    m["evidence"]["contract"].update(link=123),
    all_good(r, m)))
control("no contract", no_contract)
control("every evidence id required", evidence_ids)
control("missing link in each attested column", missing_attestations)
control("no manual producer override", no_producer_override)
control("previous green status ignored", no_reused_status)
control("real CLI", real_cli)
print(f"ledger self-test: PASS ({len(CONTROLS)} controls)")
