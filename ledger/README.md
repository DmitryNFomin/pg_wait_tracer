# Release ledger

LEDGER_ISSUE=890

Master push CI publishes the table to the job summary and uploads `ledger.json`
and `ledger.md` as `ledger-SHA-ATTEMPT`, after downloading that run's
`core-status-SHA-ATTEMPT` artifact. This job reports even when core-status fails;
ledger generation and publication failures do not gate CI.

The lead creates a dedicated issue with `<!-- pgwt-ledger-issue -->` in its
body, then sets `LEDGER_ISSUE` above to that issue's number. Until then, `0`
skips issue publication. Never use the work board or any issue cited by a brief;
keep the dedicated ledger issue out of dispatch briefs. The publisher requires
the issue's opt-in marker and refuses the work board. It creates one bot comment
marked `<!-- pgwt-release-ledger -->`, then edits that same comment on later
pushes. Write permission belongs only to the reporting job's default token.

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
  a GitHub issue comment URL. `contract` names the row's contract, and
  `contract_sha256` binds the statement to that contract directory at the
  evaluated commit. As in `scripts/core-status.sh`, files are sorted by relative
  path, and SHA256 receives each UTF-8 relative path plus a NUL, then the raw
  SHA256 digest of its bytes. Expected answers are included. A mismatch leaves
  the cell red with `stale attestation`; missing or malformed hashes also fail.
  These cells say **attested**: the generator checks link syntax and the hash,
  not the link's contents or approval. The initial manifest leaves links unset
  for human review; hashes alone cannot turn a cell green.
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
the old tree hash. Contract or expected-answer changes invalidate attestations
carrying the old contract hash. Attestation links are human statements.
The JSON includes each cell's
state, reason, evidence ids, and source paths or links; it stores no colours.

Run `python3 -B tests/test_ledger.py` for the self-test. It is also registered
in the normal deterministic runner, `scripts/check.sh`.
