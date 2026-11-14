# Fable's challenge of astra-redelivery-draft.md (2026-11-14), verbatim

**1. Verdict: ACCEPT-WITH-CHANGES.** The prefix-union core (clauses 1, 4, 7–9, 11, 13) is order-independent and matches the lead's resolution. Six changes are needed before answer sets are frozen. I read the draft, the #868 comment and CODEC.md 400–600 at origin/master; I ran nothing.

**2. Defects**

1. **Clause 14 (SEAL_CONFLICT) lets one side win.** Multiset `A, N` gives `a0…a3,n0…n3`; adding `A+` gives `a0…a4` and drops all of N.
   - The retraction itself is acceptable: it is the same non-monotonicity as BLOCK_CONFLICT, and the draft's line 73 allows it.
   - The side chosen is not. `a4` is the block that C:510–511 says cannot exist, yet the draft keeps and certifies it and rejects N, whose digest validates. This breaks the "neither wins" rule of clauses 6–7. [Verified] C:510–511, C:533–534 (the latter is per-copy wording). [Inferred] the fix below.
   - Change: stop at `(A, offset D)`, keep `A[H,D)`, exclude both the beyond-D blocks and N.
2. **Clause 14 precondition is missing.** For `A+, N` with no clean A, clauses 14 and 15 both fire at `N,H`. It is unstated whether SEAL_CONFLICT counts as a "conflict reason" for clause 17.
   - Clause 14 also never requires N's digest to match `A+[0,6)||[8,D)`, or D to be a block boundary of A+. Without that, an unrelated wrong-digest file reads as a sealing conflict.
   - Change: SEAL_CONFLICT only if the digest over the longer copy's `[..D)` verifies and D is one of its block boundaries; otherwise an ordinary link failure.
3. **Clause 3 collision branch: delete it.** Same `(length, SHA256)` with different bytes is a SHA-256 collision. No fixture can reach it, so the "one mutant per clause" obligation is unsatisfiable. A content-derived name is needed, since stops cannot use an input index.
   - Change: "a delivery is its byte string; the identifier is its name."
4. **Clauses 10, 12, 17 sort on reason-code spellings the contract does not define.** [Verified] /usr/bin/grep of CODEC.md finds no `TRUNCATED`, `BAD_CRC`, `*_CONFLICT` or `PREDECESSOR_UNAVAILABLE`. For `At3, Acrc3` the answer is BAD_CRC only because "B" sorts before "T"; renaming a code flips it.
   - Change: add a closed reason enumeration with an explicit rank.
   - Change: a terminal stop's witness is the sorted set of all max-length copies sharing the chosen reason. With `Ag, Ag'` (different garbage tails) the current witness is "min SHA", which an author cannot write without hashing.
5. **Clauses 16, 18, 19 mix layers.** Clause 16 folds "applicable r3 faults" into codec normalization, but r3 Q2–Q6 faults depend on replay state. [Verified] REVISION-r3.md:8–12.
   - Past a cutoff nothing is replayed, so "later intrinsic stops remain reportable" is not computable for semantic faults.
   - With a semantic fault in B1 and a BLOCK_CONFLICT in B3 of one file, "at most one stop per file" needs replay output fed back into the codec.
   - Change: the codec stop set contains only header/block/seal conflicts, terminal failures, link failures and §9 structural record faults. Replay stops are reported separately; cutoff is the minimum of the two. Past a cutoff, only the codec set is reported.
6. **Clause 5 "may" contradicts clause 8.** Clause 5 says conflict inspection past a logical fault "may continue"; clause 8 requires examining every delivery. An optional scan changes the stop set. Change to MUST.
7. **Clause 15 does not name the reason when copies exist but none is a witness** (wrong digest, ordinal or D). The test obligations at line 77 generate exactly these. Change: one reason, PREDECESSOR_UNAVAILABLE, for every no-witness case, with specifics as diagnostics; or enumerate them.
8. **Clauses 7/8: sound, no defect.** All copies start at H, so the first divergence is always at a common block start. A simpler equivalent wording: conflict iff neither validated prefix byte string is a prefix of the other, positioned at the first differing block. [Inferred]

**3. Cases.** All 16 are consistent with the clauses as written. Three I would change:
- `A, A+, N`: under defect 1, `a0…a3` and `{S(A,B4,SEAL_CONFLICT;A+,N)}`.
- `At3, Acrc3`: correct only through alphabetical luck (defect 4).
- `M, Mt3`: `Mt3` is never defined; the `AtK` notation is given for A only.

**4. Missing for two independent authors**
- A diagnostics column. Clauses 2 and 12 and the oracle obligation require diagnostics, but the table gives none, for example the At1 tail in `A, At1`.
- Cases that are absent:
  - a header-invalid delivery (clause 2);
  - `A+, N` and `Ag, N` (no witness);
  - a conflict in a file beyond an earlier cutoff (clause 8);
  - a same-position stop tie (clause 17);
  - a longer copy whose extra block is a retroactive VOID, which is the stated reason for prefix union and is currently untested;
  - a missing middle file and a missing file 0 (C:540–542);
  - a torn footer on a max-length copy, to settle whether it is a capture stop or only a diagnostic under C:505–507.
- The position vocabulary: is `B4` the byte offset D? State stops as byte offsets.
- Whether the header CRC excludes flag bytes [6,8). Clause 6 relies on it. [Inferred] from C:523–526, not checked in §2–§3.
