# Report audit contract

The printed `VERDICT SEND for draft=<full SHA-256>` authorizes that exact draft.
An exit code is not authorization. `AUDIT MALFORMED`, `AUDIT AMBIGUOUS`, and
`AUDIT INCONCLUSIVE` withhold authorization; they do not assert a contradiction.
Only a checked contradiction in the current draft (or the unchanged deterministic
absence pre-pass) produces `VERDICT BLOCK`.

Properties exercised by `audit-report-selftest.py`:

1. **One completed derivation per exact byte sequence of draft content.** A private
   state directory outside the repo stores a decision atomically after the audit,
   under a process lock. Only a written `decision.json` consumes that hash's pass;
   crashes and other incomplete attempts can be retried on the same draft bytes.
   A retry discards any incomplete attempt's final answer before invoking Codex.
   Repeating a completed hash reads only its decision, never its transcript or
   final-answer file. Replay validates the digest, repository tree and age, then
   independently re-derives the verdict using the same counts-based contradiction
   backstop as a fresh pass: any `counts.WRONG > 0` forces BLOCK even if the stored
   verdict says SEND. Repeating BLOCK preserves BLOCK. Any byte change, including
   appending a blank line, produces a new hash and a fresh pass; this is not a
   one-pass-per-report guarantee.
   Decisions are scoped to the checkout and gate implementation; after ten minutes
   or any repository content change, replay withholds authorization and requests a
   revised draft. A replay explicitly identifies its original observation time;
   it is not evidence of freshly checked processes or GitHub state. Include a fresh
   observation timestamp in a draft when a fresh check is needed.
2. **Previous transcripts are not an input.** The auditor receives only a private,
   immutable temporary snapshot's path, not the source draft's directory. Its
   transcript and final answer are saved separately, outside the repository and
   outside the snapshot directory. It starts a new `exec --ephemeral`, with memory
   injection and generation explicitly disabled. It is instructed to search the
   repo and the supplied GitHub snapshot, not scratchpad trees, archives or session histories; individually
   cited scratchpad evidence remains available. A recursive search of the supplied
   draft directory cannot reach earlier audits. This is a data-flow and instruction
   boundary, not an OS prohibition on reading arbitrary files: the read-only CLI
   sandbox does not promise that. Exact-quote validation and final-channel extraction
   provide independent backstops if irrelevant material nevertheless appears.
3. **Contradiction, contamination, and access failure stay distinct.** Only the CLI's
   `--output-last-message` file is parsed. Prompt/tool/earlier-turn text in stdout or
   stderr has no vote. Each final `CLAIM` JSON record identifies a line and quote
   in the current snapshot, plus evidence and status. Matching collapses whitespace,
   strips surrounding whitespace and folds curly quotes/apostrophes to ASCII on
   both sides. A structured WRONG that still fails to match its stated line is
   MALFORMED, including a quote found only on a different line. Out-of-draft
   VERIFIED/UNVERIFIABLE records and status-word narration or prompt echoes count
   as contamination; non-WRONG quotes found at the wrong line still withhold
   authorization. Draft-bound WRONG forces BLOCK even alongside SEND.
   UNVERIFIABLE means attempted checks failed for access reasons. If its
   draft-bound quote matches `#[0-9]+` or a whole word `PR`, `merged`, `issue`,
   `CI`, `check`, `review`, or `comment` (case insensitive), it withholds SEND as
   `AUDIT INCONCLUSIVE (GitHub claim not checkable from the snapshot: line N)`.
   This applies even without an explicit issue/PR number, on fresh and replayed
   decisions, and to legacy findings. Only non-GitHub access failures alone
   permit SEND, including process-list and session claims in a draft that also
   contains verified GitHub claims. Draft-bound WRONG still forces BLOCK.
   The auditor may finish with `VERDICT: INCONCLUSIVE` for an uncheckable GitHub
   claim; that verdict without such a finding is malformed. The parser applies
   the GitHub refusal even if the auditor says SEND.
   Legacy exact-quote findings also work, including
   `WRONG:`. Attempted legacy findings with ` -> ` and a status word still fail
   closed when they cannot bind to the draft. The brief permits only well-formed
   CLAIM JSON lines, blank lines and one VERDICT line, with no narration.
   Invalid JSON, empty audits, missing verdicts,
   conflicting final verdicts and unsubstantiated BLOCK withhold authorization.
4. **The absence pre-pass is unchanged.** It runs on the exact bytes to be audited,
   before Codex or decision replay. `never`, `does not exist`, and `exists nowhere`
   without nearby evidence still BLOCK without an auditor invocation. A change to
   the source draft or repo during an audit withholds authorization.
5. **A fresh auditor invocation first captures GitHub evidence on the host.**
   The auditor retains `--sandbox read-only` with no network. When the draft
   contains `#` followed by digits or a whole word from `PR`, `merged`, `issue`,
   `CI`, `check`, `review`, `comment` (case insensitive), the wrapper fetches each distinct `#number`
   with `gh pr view`. Only an explicit response that the number is not a PR
   triggers `gh issue view`, including comments. Each referenced PR also gets
   `gh pr view N --json comments,reviews`. It also fetches
   `gh pr list --state all --limit 200 --json number,title,state,mergedAt,closedAt,headRefName`
   and `gh run list --branch master --limit 10 --json databaseId,headSha,status,conclusion,workflowName,createdAt`.
   Each call has a 30-second timeout.
   A fresh `github-snapshot-*` directory inside the attempt holds JSON files
   named `pr-N.json` and `pr-N-comments-reviews.json`, or `issue-N.json`, plus
   `pr-list.json` and `run-list.json`. Every file records
   `fetched_at_utc`, the exact `command`, and its result in `data`. Files have
   mode 0400 and the completed directory has mode 0500.
   The brief names that directory and its snapshot files and requires GitHub claims to be checked
   against these primary observations at the recorded times; contradictions
   are WRONG. The PR list's 200-result limit does not establish unbounded absence;
   the run list's 10-result limit does not establish every workflow's state for
   a commit. CI findings must identify the run and commit checked.
   Failed calls, invalid JSON or snapshot write failures withhold authorization
   as `AUDIT INCONCLUSIVE (GitHub snapshot failed: <what>)` before the auditor
   starts. They do not publish a decision; retries fetch a fresh directory.
   Drafts without the markers skip GitHub entirely. Existing replay semantics
   remain as described above.

The tests use a deterministic PATH stub that models recursive searches, intermediate
WRONG/tool-result output, prompt echoes, unavailable access, and an absurd count
contradicted by evidence. They run through the actual executable wrapper, are wired
into full `make check`, and can retain all fixtures with `--output-dir DIR`.
They prove wrapper behavior, not the truthfulness of a language model's evidence.

Investigation of #761: the old wrapper demonstrably wrote beside the source draft,
parsed all stdout/stderr, and treated a wrapped prompt example as WRONG. The single
unwrapped example in the owner's comment matches VERIFIED first in this checkout's
awk parser; splitting at its display wrap reproduces WRONG. The historical deleted
files and their originating sessions were not supplied, so their byte identity,
tool IDs and mechanism of recall have not been independently reproduced. The old
wrapper used a fresh `codex exec`, not `resume`; persistence alone does not establish
that history was injected. Fresh exec, ephemeral storage, explicit memory settings,
and a final-answer-only boundary remove those channels from the new contract.

CLI semantics: [non-interactive mode](https://developers.openai.com/codex/noninteractive)
(`--ephemeral`, `--output-last-message`) and
[configuration reference](https://developers.openai.com/codex/config-reference)
(`memories.use_memories`, `memories.generate_memories`). Local `codex exec --help`
and `codex features list` were also checked; older CLIs that reject these flags
withhold authorization rather than falling back to whole-transcript parsing.
