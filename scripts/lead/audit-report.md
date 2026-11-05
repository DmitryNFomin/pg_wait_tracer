# Report audit contract

The printed `VERDICT SEND for draft=<full SHA-256>` authorizes that exact draft.
An exit code is not authorization. `AUDIT MALFORMED`, `AUDIT AMBIGUOUS`, and
`AUDIT INCONCLUSIVE` withhold authorization; they do not assert a contradiction.
Only a checked contradiction in the current draft (or the unchanged deterministic
absence pre-pass) produces `VERDICT BLOCK`.

Properties exercised by `audit-report-selftest.py`:

1. **One derivation per draft hash.** A private state directory outside the repo
   records an attempt before invoking Codex, under a process lock. Repeating a hash
   reads only its validated decision, never its transcript or final-answer file.
   Repeating BLOCK preserves BLOCK. Revising the text produces a new hash and pass.
   Crashes leave an attempted hash, rather than allowing repeated tries until SEND.
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
   repo and GitHub, not scratchpad trees, archives or session histories; individually
   cited scratchpad evidence remains available. A recursive search of the supplied
   draft directory cannot reach earlier audits. This is a data-flow and instruction
   boundary, not an OS prohibition on reading arbitrary files: the read-only CLI
   sandbox does not promise that. Exact-quote validation and final-channel extraction
   provide independent backstops if irrelevant material nevertheless appears.
3. **Contradiction, contamination, and access failure stay distinct.** Only the CLI's
   `--output-last-message` file is parsed. Prompt/tool/earlier-turn text in stdout or
   stderr has no vote. Each final `CLAIM` JSON record identifies a line and exact
   quote in the current snapshot, plus evidence and status. Out-of-draft records
   and recognized prompt examples count as contamination. Draft-bound WRONG forces
   BLOCK even alongside SEND. UNVERIFIABLE means attempted checks failed for access
   reasons and alone permits SEND. Legacy exact-quote findings also work, including
   `WRONG:`. Unstructured findings, invalid JSON, empty audits, missing verdicts,
   conflicting final verdicts and unsubstantiated BLOCK withhold authorization.
4. **The absence pre-pass is unchanged.** It runs on the exact bytes to be audited,
   before Codex or decision replay. `never`, `does not exist`, and `exists nowhere`
   without nearby evidence still BLOCK without an auditor invocation. A change to
   the source draft or repo during an audit withholds authorization.

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
