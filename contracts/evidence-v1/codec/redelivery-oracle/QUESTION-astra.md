# Question (one answer): what record set must the v5 decoder produce for several deliveries of one physical file?

Read with `git show origin/master:<path>` (master 03694b8b) and the branch commit 7d36b3b5 (fetch it from the repo; it
is reachable as `agent/868-u7b-codec-dedup` in the lead's checkout, or read files under
/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/27cc85b4-040d-47d2-ba40-e0c59ac54ec1/scratchpad/wt-u7b2).
Contract: contracts/evidence-v1/codec/CODEC.md §7 (lines 408-483, physical identity and deduplication), §8 (file
chain, footer, predecessor digest), §9 (refusal; torn tail), and contracts/evidence-v1/REVISION-r3.md (faults are
ordered stops).

Context: unit U7b (#868) adds §7 deduplication to the in-memory codec. Two implementation rounds each failed review
because the decoded RECORD set depends on the order deliveries arrive in:
- round 1: a conflicting copy after a capture stop was never compared (fixed in round 2);
- round 2 (7d36b3b5): with copies of one file torn at different points, the result depends on order. A = 4 one-row
  blocks; AtK = A cut inside block K; Ag = A plus a partial block; N = A's successor; Acrc2 = A with block 2's CRC bad.
  `At3,At1` gives 3 records and `At1,At3` gives 1; `A,At1` gives 1; `Ag,At1` 4 vs `At1,Ag` 1; `A,Nt2,Ag` keeps N's
  2 records and `A,Ag,Nt2` keeps none. A reviewer's sweep: 6,326 of 42,465 chain-order-preserving orderings of 3,860
  delivery multisets differ in records; none differs in whether or where a conflict is reported.
  Harness and output: .../scratchpad/rv-fuzz/perm.c and sweep_all.txt.

The contract says only that "repeated delivery of the same validated physical position may be suppressed; conflicting
bytes at that position are an error", and requires an identical-file replay to add no chain member. It does not say
what a decoder outputs when deliveries of one file differ in how much of it is valid.

Answer:
1. Does the contract already determine the record set for these cases (cite lines), or is this a gap? Mark [Verified]
   only what you read.
2. Propose ONE order-independent rule for the record set and stops of a capture given a multiset of deliveries
   (identical, longer/shorter valid prefix, torn, CRC-bad block, conflicting block, successor before/after). For
   example: the union of validated positions across copies (a longer valid copy extends a shorter one), or the
   intersection (the shortest valid prefix wins), or "any disagreement in validity stops the capture at the first
   position where copies differ". Say why, with the §8 rule that a later file cannot repair a stopped capture and the
   §7 rule that conflicting bytes are an error.
3. Is this rule a contract change (needs a Contract-Change commit and independent answer sets) or a clarification the
   codec unit may implement directly?
4. The test that would prove it: is "every permutation of every multiset in a generated sweep gives identical records
   and stop set" the right acceptance gate, and what must the generator include?
At most 25 lines.

# Task (Astra, contract author, read-only draft): evidence-v1 codec revision "redelivery"

Read: issue #868 comment 6868500311 (`gh api repos/DmitryNFomin/pg_wait_tracer/issues/comments/6868500311 --jq .body`),
which records the gap, both advisers' rules and the lead's resolution (prefix union, from CODEC.md:554-556). Your own
earlier answer is
/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/27cc85b4-040d-47d2-ba40-e0c59ac54ec1/scratchpad/storage/astra-redelivery.md;
Fable's rule is quoted in that comment. Contract files at origin/master (03694b8b): contracts/evidence-v1/codec/CODEC.md
(§7 lines 408-483, §8 484-559, §9), WORKED.md, REVISION-codec-r2.md, contracts/evidence-v1/REVISION-r3.md, README.md.
Read them with `git show origin/master:<path>`.

Draft, as text in your answer (do not edit files):
1. **Normative text** for CODEC.md §7/§8, replacing or extending CODEC.md:458-468 and :536-538, defining the result of
   a multiset of deliveries of one capture, computed on the whole multiset and independent of order:
   - grouping by physical file identity; per-copy validation; the validated block prefix P(copy);
   - conflict: which blocks count (validated in both copies? one?), the reported position and reason, what is kept;
   - records: the longest validated prefix taken whole from one copy (prefix union), never spliced across copies;
   - successor admission when copies of the predecessor differ (Fable's clause: admit if at least one copy meets
     :529-531). Accept or reject it with a reason;
   - stops: at most one per physical file, its position and reason, the tie-break; how rejected tails of shorter
     copies are reported (diagnostics, not capture stops), and how a stop identifies its delivery without an
     input-array index;
   - identical, flag-only and footer-only differences.
   Say where your earlier common-prefix answer is superseded and why, or object with a specific contract line if you
   think the union breaks §8.
2. **Cases:** 12 to 16 new redelivery scenarios as an input/expected table. Cover: At1+At3; A+At1; Ag+At1; A+Acrc2;
   Ac2t1 and Ac2t3 against A; a header conflict; A,Ag,Nt2; a successor with only a torn predecessor copy; three copies;
   an interleaved independent capture; a type-1/type-2 block among the positions. Expected = records (by physical
   position) and the normalized stop set.
3. **Test obligations** for the implementation unit: the independent per-multiset oracle, all permutations (including
   successor-before-predecessor), monotonicity, one mutant per rule clause, and the generator's required variants.
Mark each claim about existing text [Verified] with file:line. At most 120 lines.

## Task

Your draft r1: /private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/27cc85b4-040d-47d2-ba40-e0c59ac54ec1/scratchpad/storage/astra-redelivery-draft.md
Fable's challenge (ACCEPT-WITH-CHANGES, 7 defects plus missing cases): fable-redelivery-challenge.md in the same folder.
Task text of r1: q-redelivery-draft.md there. Contract at origin/master (03694b8b): contracts/evidence-v1/codec/CODEC.md,
REVISION-r3.md (`git show origin/master:<path>`).

Write revision r2 as text in your answer (do not edit files):
1. For each of Fable's points 1-7 and each missing item in its section 4: ACCEPT or REJECT, with a reason and a contract
   line where you reject.
2. The full r2 normative text (all clauses, renumbered if needed), not a diff, including:
   - a closed reason enumeration with an explicit rank;
   - stop positions as byte offsets;
   - delivery identity without the collision branch;
   - the codec/replay layer split.
3. The full r2 case table (all cases, not only changed ones), with a diagnostics column. Define every fixture name
   (A, A+, Ag, AtK, AcrcK, AcK, M, N, X ...) once in a notation block, with block counts and which block is a VOID or
   type-1/type-2 block. Two independent authors will derive expected values from your text alone, without code, so
   every expected value must follow from the clauses.
4. The test obligations for the implementation unit.
Mark claims about existing text [Verified] file:line. At most 220 lines.
