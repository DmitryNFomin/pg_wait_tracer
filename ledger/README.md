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
- `core-status`: `contract` names the record to read in both artifact modes
  (`wait_time-v1.json`, `merge-v1.json`, or `evidence-v1.json`).
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

The merge and evidence manifests add contract claim rows without changing the
existing release claims. Only their `broken_versions` cells have a producer;
empty attestation, platform, interface and retirement requirements remain red.
Their status files use the same claim/proof/run/provenance structure as wait_time.
Each per-id record must have its exact pass spelling; duplicate records fail,
missing records are NOT-RUN, and a changed tree discards all three contracts'
evidence. Mutation records bind the registered definition and actually compiled
source hashes and name their intended proof. Equivalent guard deletions are
reported separately and cannot satisfy the killed-mutant requirement.

`scripts/core-status.sh` runs all portable proof suites once. The regular proof stage in `scripts/check.sh`
uses these runs instead of repeating the merge, replay, codec, redelivery,
report, merge index, merge index mutant and merge queue mutant commands
(the last also prints the queue baseline). It still runs
`tests/test_core_merge_queue.py` for its symbol check and informational
timings. Proof suites are never skipped using stored status. Set
`PGWT_CORE_STATUS_DIR=/absolute/scratch/directory` to redirect this run's JSON
and logs outside the default `tests/results/core_status/` directory; this is an
output destination only, never an evidence input to the status producer.
`scripts/check.sh` creates a fresh output directory for its status step and
passes it to `bash tests/test_core_status.sh /absolute/output/directory`.
Before replay, the self-test requires all three reports to match the current
tree and contract hashes, contain the complete proof and runner inventories,
and have successful runs with intact logs matching their recorded SHA256s.
A different tree, missing report/proof/runner, unsuccessful run or changed log
is refused before replay. Only raw runner transcripts become parser fixtures;
the machinery derives proof and claim states again, including the controls
that remove or corrupt evidence and seed falsely green claim derivation.
Standalone `bash tests/test_core_status.sh` still makes its own real baseline.
The real wait-time controls and failing C baseline checks run in either mode.
The two CI status modes move and upload every top-level output file.

A failing or missing proof prints its id, affected claim ids and runner log
path on the console, followed by its first failing record when available.
The gate removes its temporary output directory on green and retains it on red
so those paths can be read after failure. If a runner could not start, its log explains that it did not run.

Local macOS measurements on 2026-11-14 of the commands in the CI `core-status`
job: **plain 588.8 s**, **ASan/UBSan 1260.2 s**, **1849.0 s serial total**
(checkout and artifact upload excluded). The sanitizer mode uses
`CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`.
The gate self-test took 308.1 s using its preceding status run; standalone,
including its own real plain baseline, it took 918.1 s. The plain timing
was measured around that standalone baseline subprocess on the invoking tree,
with output redirected to temporary storage. Hosted runner timings may differ.
