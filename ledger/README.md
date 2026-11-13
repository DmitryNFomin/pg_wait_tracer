# Release ledger

`claims.json` is the reviewed manifest, not a saved status. Contract rows refer
to `contract:claim_id`; their text and required proof ids are read from
`CLAIMS.tsv` at the evaluated commit. Rows awaiting a contract have a title
and a null `contract_claim`. Each of the eight cells lists all evidence ids
it requires. Empty lists and missing records leave cells red.

Run from the directory where the generated `ledger.json` should be written:

```sh
python3 /path/to/repo/scripts/ledger.py --commit origin/master --artifact-dir /path/to/download
```

The generator prints the Markdown table and replaces `ledger.json` in the
current directory. Generated results are disposable; do not use them as input
or commit them as reviewed evidence. The manifest is always read from this
checkout's `ledger/claims.json`. It can describe a commit predating the ledger.

Download a CI artifact with `gh run download RUN_ID --name
core-status-SHA-ATTEMPT --dir /path/to/download`. Both `plain/` and
`sanitizers/` must contain `provenance.json` and the contract's status JSON.
The existing L1 provenance (`github.sha`, `run_id`, `run_attempt`, `job`, and
`tree_hash`) is sufficient. A PR run's SHA may be a merge commit; a PR head SHA
is neither required nor inferred. Evidence qualifies by whole-tree equality,
even when two commits have different SHAs but the same tree.

Evidence kinds:

- `attestation`: `link` is a positive PR number, `#123`, a GitHub PR URL, or
  a GitHub issue comment URL. These cells say **attested**. As specified in
  L2, the generator checks only link presence and syntax, not the link's
  contents or approval. The initial manifest leaves these links unset for
  human review.
- `core-status`: `contract` names the record to read in both artifact modes.
  The overall state, every claim row, and every required proof must pass,
  with an empty errors list and matching tree hashes in status and provenance.
  Proof ids must match the frozen claim. At least one independent `case:`
  proof and one mutant killed on an independent intended case are required.
  The producer's canonical mutant proof line identifies its intended case;
  differential agreement and mutants aimed only at differential cases cannot
  qualify. The generator does not read or edit the mutant definitions.
- `box-check`: `path` is an absolute local log path, or relative to the repo.
  Exactly one certified `box-check: commit=... tree-hash=... built-from=...`
  header must identify the evaluated tree and a binary built from that tree.
  A source header saying `build provenance pending` does not qualify.

Real interface, load/platform, and old-code deletion always report
`no evidence producer` until a later step supplies one. There is no manual
status override. Changing any tree content invalidates measured records carrying
the old tree hash. Attestation links are human statements, not measured records.
The JSON includes each cell's
state, reason, evidence ids, and source paths or links; it stores no colours.

Run `python3 -B tests/test_ledger.py` for the self-test. It is also registered
in the normal deterministic runner, `scripts/check.sh`.
