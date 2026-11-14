Revision r2 adopts **prefix union** and supersedes r1 below.

References are at `origin/master`, verified as `03694b8b`: `C` = `contracts/evidence-v1/codec/CODEC.md`; `R3` = `contracts/evidence-v1/REVISION-r3.md`; `K` = `contracts/evidence-v1/CONTRACT.md`.
[Verified] Header CRC excludes flag bytes `[6,8)` (`C:125–128`); predecessor sealing and linkage are specified at `C:510–535`; physical identities and omission rules are at `C:453–469`; r3 distinguishes physical stopping from dependency-dependent decisions (`R3:7–15`).

The dispositions are:

| Challenge | Decision and reason |
|---|---|
| 1. Sealing conflict lets the longer predecessor win | **ACCEPT.** Stop at the predecessor’s declared end D. Exclude both its extension and the successor. Neither contradictory continuation wins. |
| 2. Sealing precondition missing | **ACCEPT.** Require a validated boundary at D, a matching digest through D, and matching static link fields. A seal proof takes precedence over ordinary no-witness classification. |
| 3. Delivery-identifier collision branch | **ACCEPT.** Delivery identity is its byte string; the digest is a compact name. Remove the collision branch. |
| 4. Undefined reason ordering and hash-selected witness | **ACCEPT.** Define a closed codec reason enumeration and fixed ranks. Terminal witnesses include every maximum-prefix delivery with the selected reason. |
| 5. Codec/replay layer mixing | **ACCEPT.** Normalize only codec failures in the codec. Report dependency/state-dependent replay stops separately; never feed them back into codec normalization. |
| 6. Optional inspection after faults | **ACCEPT.** Independent validation and conflict inspection MUST continue through trustworthy blocks, irrespective of replay eligibility. |
| 7. Unnamed no-witness failures | **ACCEPT.** Every header-valid successor lacking a witness or seal proof gets `PREDECESSOR_UNAVAILABLE`; diagnostics retain the failed-link evidence. Locally invalid ordinal fields remain header failures. |
| §4: diagnostics column | **ACCEPT.** Every case below specifies diagnostics, including failures suppressed as stops. |
| §4: invalid header | **ACCEPT.** Cases 17 and 31 distinguish unassigned header failure from a capture stop. |
| §4: `A+,N` and `Ag,N` | **ACCEPT.** Cases 18–19 distinguish authenticated seal contradiction from an unavailable predecessor. |
| §4: conflict beyond an earlier cutoff | **ACCEPT.** Case 20 requires reporting the later intrinsic conflict. |
| §4: same-position tie | **ACCEPT.** Cases 13 and 21 cover ranked reasons and multiple witnesses. |
| §4: later retroactive VOID | **ACCEPT.** Case 22 retains the VOID and specifies its revocation. |
| §4: missing middle/root | **ACCEPT.** Cases 23–24 forbid admitting a later file as a root. |
| §4: torn footer at maximum prefix | **ACCEPT.** Cases 25–26 distinguish an unsuppressed terminal stop from a diagnostic suppressed by a clean alternative. |
| §4: byte positions | **ACCEPT.** All positions below use absolute file-byte offsets, with decoded-payload byte offsets for records. |
| §4: flag exclusion from header CRC | **ACCEPT.** Explicitly confirmed by `[Verified] C:125–128`, rather than inferred from predecessor hashing. |
| §3: undefined `Mt3` | **ACCEPT.** The generic fixture transformations below define it. |

The following is the complete replacement redelivery rule; unaffected byte-format and replay requirements remain applicable.

1. **Whole-multiset result.** Reconcile the complete delivery multiset before exposing a capture’s logical input. Records, provenance, link decisions, diagnostics and stops MUST be independent of arrival order, filenames and input-array indices. Incremental evaluation MUST equal fresh evaluation of all deliveries accumulated so far.

2. **Identity.** A delivery is its complete byte string. Its compact name is `id(d)=(byte_length,SHA256(all bytes))`, including flags, footer and rejected tail. Identical byte strings are one delivery for reconciliation and diagnostics. Independently validate each header; only validated headers establish physical file identity `(boot,capture,file_no)`. An invalid header supplies no trusted capture identity or blocks.

3. **Closed codec reasons.** The complete enumeration, with lower rank winning a same-position tie, is: `HEADER_CONFLICT(HC)=0`, `BLOCK_CONFLICT(BC)=1`, `SEAL_CONFLICT(SC)=2`, `PREDECESSOR_UNAVAILABLE(PU)=3`, `BAD_CRC(CRC)=4`, `TRUNCATED(T)=5`, `INVALID_HEADER(IH)=6`, `INVALID_BLOCK(IB)=7`, `RECORD_FAULT(RF)=8`. Diagnostic details are evidence, not additional reason codes. Replay failures belong to a separate result domain.

4. **Positions.** A codec position is `(file_byte_offset,payload_byte_offset?)`. Header failures/conflicts and incoming-link failures use `(0,null)`. Block rejection/conflict uses `(block_start,null)`; sealing conflict uses `(D,null)`. A framed-record fault uses `(block_start,decoded_payload_record_offset)`. These are byte offsets, never block or record indices. Sort by file ordinal, file-byte offset, then payload-byte offset, with `null` before every payload offset. A decoded offset MUST NOT be presented as an absolute compressed-file offset.

5. **Independent prefix validation.** For each header-valid delivery, scan sequentially from its own H. `P(d)` contains its maximal contiguous sequence of wholly validated blocks of every type; `E(d)` is their exclusive end, or H when empty. Validate complete framing, schema, bounds, decompression, offsets, CRC chain and block CRC before accepting a block. A record-local fault inside trustworthy framing does not remove its enclosing block from P. Never resynchronize after a rejected block, borrow another delivery’s bytes, or skip sample blocks.

6. **Terminal classification.** Invalid headers receive IH. At each subsequent boundary, first accept EOF or an exactly matching complete footer. Otherwise: an incomplete block header receives T; invalid header-level bounds/type/codec/reserved/offset/preceding-CRC fields receive IB; insufficient bytes for a safely bounded declared block receive T; a complete block with bad CRC receives CRC; remaining whole-block decoding/framing/schema failures receive IB. Stop that delivery at this first rejected unit. Check whole-block validity before classifying any contained record fault. All rejected bytes begin at E(d), regardless of the interior byte revealing the failure.

7. **Structural versus semantic faults.** Record-local malformed fields or unknown kinds in trustworthy framing produce RF diagnostics and, where the governing contract requires stopping, RF candidates at their original record offsets. Validation and conflict inspection MUST continue through all of P, including after these faults. Counter comparison, generation/run assignment, delayed dependencies, frontier advancement and other replay-state checks do not produce codec RF candidates. Preserve the unqualified-certificate exception and r3’s readable/null-counter loss handling; do not turn those exceptions into structural stops.

8. **Header conflict.** For every pair in one physical-file group, compare their complete validated headers after omitting only bytes `[6,8)`. Different immutable header byte strings produce HC at offset 0. Retain no records from that group. Do not compare their blocks as if their headers were compatible.

9. **Block conflict.** For each compatible-header pair, compare their validated block-prefix byte strings. If neither is a prefix of the other, produce BC at the start of their first differing block. This is a common block boundary because all earlier blocks agree. Compare complete stored block bytes, including headers, lengths, payloads and CRCs; equal decoded values or equal CRCs are insufficient. Rejected or incomplete blocks supply no conflict evidence.

10. **Prefix union and provenance.** For a group without HC, let E be the maximum E(d). Source its prefix whole from one delivery attaining E, then cut before the earliest applicable conflict/stop. Never splice deliveries. Before the first conflict, all such source copies have identical retained bytes. Report the set of all deliveries attaining E as source provenance; which member supplies those identical bytes is not an observable choice.

11. **Physical records.** Emit each retained physical record position exactly once, in file/block/payload order. Equal contents at distinct positions remain distinct. Omit valid samples and markers from the exact array without changing surviving offsets. Identical copies, flag-only differences and addition/removal of an exactly valid footer cannot change records, conflicts or admission; provenance may reflect the different deliveries.

12. **Terminal-stop eligibility.** Preserve every delivery’s rejected-tail diagnostic. A terminal failure from E(d)<E is never a capture-stop candidate. If any delivery attaining E ends at clean EOF or an exactly valid footer, no terminal failure at E becomes a stop candidate. Otherwise choose the lowest-ranked terminal reason among deliveries attaining E; its witness set is **all** such deliveries with that reason. This creates one terminal candidate at E. Torn/bad footers have no special clean status.

13. **Actual predecessor witness.** A header-valid successor has a witness only if an actual predecessor delivery independently validates exactly through its declared D, has no remaining bytes except an absent/exact footer, and satisfies the digest and static descriptor/link requirements. A witness may differ from the record-source copy. Partial deliveries cannot collectively construct one. Determine this local link fact even when an earlier capture stop prevents eventual admission.

14. **Sealing contradiction.** A pair consisting of a header-valid successor and a predecessor delivery proves SC only when all static link fields match, D is a validated block boundary of that predecessor, the successor’s digest matches that predecessor’s immutable bytes through D, and the predecessor has a complete validated block beyond D. No clean predecessor ending at D is required for this proof. Place SC in the **predecessor** at D; retain only its blocks before D and exclude its extension and the successor. Rejected tail bytes alone prove nothing about sealing.

15. **Link classification and admission.** If seal proofs exist for a successor delivery, generate their SC candidates and reject that link; do not also generate PU merely because no clean witness exists. Otherwise, generate PU at the successor header exactly when no witness exists. Missing, short, dirty, wrong-digest and mismatching predecessor alternatives all use PU. An actual witness permits continuation only if no earlier cutoff or group conflict prohibits it. Exclusion solely because of an earlier cutoff creates no additional stop.

16. **Complete inspection and witnesses.** Evaluate every header-valid delivery and all relevant pairs, including files beyond an earlier cutoff. Generate each compatible pair’s earliest BC, every HC pair, authenticated SC pairs, structural faults and local link failures independently of arrival order. Conflict witnesses are the complete set of proving pairs at the reported position/reason; preserve predecessor/successor roles for SC. Other witnesses are sets of offending deliveries. Identical deliveries contribute once.

17. **Codec normalization.** From eligible codec candidates, return at most one stop per verified physical file: earliest position first, then lowest reason rank. Union the witnesses of candidates tied on both position and reason. Preserve all other failures as diagnostics. Invalid-header diagnostics cannot become stops attributed using unvalidated identity fields.

18. **Codec cutoff.** Within a capture, its earliest normalized codec stop excludes the offending unit and every later record/file. A block stop excludes the entire block; a structural record stop preserves preceding eligible records in that validated block. Start from file 0 and require each immediate link; never manufacture a missing file or reinterpret a successor as a root. Independent captures remain separate. Later intrinsic codec stops remain reportable.

19. **Replay boundary.** Replay receives only the codec-admitted stream. It resolves dependencies within that available stream before declaring semantic faults and reports its physical stop separately. The effective capture cutoff is the earlier of its codec cutoff and its reachable replay stop. After that cutoff, do not replay or invent additional semantic failures; independent codec inspection still supplies its diagnostics/stops. Reconstruct surviving certificates and LIFE under r3 without feeding semantic stops back into codec normalization.

20. **Diagnostic and update contract.** Diagnostics include every independently observed header failure, first rejected tail, structural fault, pairwise conflict and PU, including suppressed candidates. PU carries the declared predecessor reference and the available predecessor validation/link-check results, so its cause is inspectable without a new reason code. Deduplicate and canonically order diagnostics, stops and witness sets using physical positions, reason ranks and delivery byte strings; arrival indices are forbidden. An unassigned header failure remains a failed decode, not an empty success. Updated replay must incorporate every retained later VOID; shorter deliveries cannot suppress it.

[Verified] The physical-stop and dependency-resolution basis is `R3:7–15`; explicit null counters report loss rather than malformed evidence at `K:278–288`. Retroactive VOID revocation is specified at `K:189–196`. Footer recognition and rejected tails are specified at `C:500–507`. The ranked normalization and multiset rules above are proposed requirements.

The fixture notation below defines the entire case table. Cases describe **codec record arrays**, with replay effects stated explicitly where tested.

- All fixtures use raw blocks, one row/record per block, valid integrity fields unless expressly changed, zero flags/creation metadata, qualified=true and fifteen zero baseline counters. Boot is `"b"`; captures are `"a"` for A/N/L/R, `"m"` for M and `"x"` for X. M alone maps event ID 1 to wait name `"w"`; other mappings are empty. Type-2 timestamp hints are zero. Unmentioned query IDs and CPU values are zero.
- `LIFE(p)` means `t=0,pid=p,fork=0,start=0,start_known=true,exit=absent`. `ATTACH(t)` means pid=1, fork=0, reread, seed/state=null and zero counters. `FRONTIER(t)` has zero counters. All record contents named below include these fields.
- `A` is clean file 0 with four **type-2** blocks: `a0=LIFE(1)`, `a1=ATTACH(0)`, `a2=FRONTIER(10)`, `a3=FRONTIER(20)`. H=193; block starts are `[193,305,537,747]`; D=957.
- `A+` appends a fifth **type-2** block at 957 containing `a4=VOID(t=30,counters=zero,reason="x",invalid_from=20)`. Its end is 1179. This is the only VOID in the fixtures.
- `N` is clean file 1, referencing A’s D=957 and digest, with four type-2 records `n0…n3=LIFE(2)…LIFE(5)`. `L` is clean file 2, referencing N, with `l0…l3=LIFE(6)…LIFE(9)`. Both have H=241, starts `[241,353,465,577]`, D=689.
- `X` is independent clean file 0 with four type-2 records `x0…x3=LIFE(10)…LIFE(13)`, H=193, starts `[193,305,417,529]`, D=641.
- `M` is clean file 0 with four blocks: type 0 `m0=TRANS(t=1,pid=1,old=null,new="w",duration=0)`; type 1 one sample `(t=2,pid=1,event="w",weight=1)`; type 2 `m2=LIFE(1)`; type 0 `m3=TRANS(t=3,pid=1,old="w",new=null,duration=2)`. The sample period and maximum weight are both 1. H=201; starts `[201,292,377,489]`; D=580. There is no attachment.
- For a base F, `FtK` truncates it exactly one byte into zero-based block K; exactly K blocks remain validated. `FcrcK` flips one bit of block K’s stored CRC, preserving all other bytes.
- `FcJ` changes block J validly: increment its type-2 record’s t by 1, or its type-0/1 row’s query_id by 1; repair that block’s CRC and all subsequent CRC chaining. Lengths remain unchanged. `FcJtK` then applies the truncation. Thus `Ac2t1` is byte-identical to `At1`; `Mt3`, `Mc1t3` and `Lc2t3` are defined.
- `Ag` appends byte `00` to A; `Ag′` appends byte `01`. Both have four validated blocks and a truncated tail at 957. `Ao` adds A’s exact 68-byte footer. `Aft` appends only its first 17 bytes, yielding a truncated tail at 957.
- `Af` changes only A’s flags to 1. `Ah` changes A’s immutable start_time_ns from 0 to 1 and repairs header/block integrity. `Z` instead flips one bit of A’s stored header CRC without repair; it has no trusted physical-file identity.
- `Nb` changes N’s predecessor digest by flipping its first bit; `Ni` changes N’s referenced D to 956 and stores the digest through that byte offset. Both repair their own header/block integrity; neither modification changes block sizes.
- `Au` changes A’s block-1 record kind to 255, preserving its bounded body/common t and repairing CRC chaining. `Auc3` additionally applies the valid block-3 change. Both have four validated blocks and an RF at `(305,8)`.
- `R` has A’s header and four type-2 records `r0=LIFE(1)`, `r1=ATTACH(0)`, `r2=ATTACH(1)`, `r3=FRONTIER(20)`. Starts are `[193,305,537,769]`; D=979. `Rc3` changes its last frontier to 21. The overlapping attachment is a replay fault at `(537,8)`.
- A named record denotes its full physical identity **and the contents above**. Single type-2 record offsets are 8; single type-0/1 row offsets are 0. `a0…a3` means four ordered positions, not a timestamp range.
- `S(F,o,r;W)` is a normalized codec stop; scalar o abbreviates `(o,null)`. W is a delivery set, or a set of proving pairs for conflicts. `d(delivery,o,r)` is an independent failure diagnostic. In the diagnostics column, **S** expands to diagnostics underlying every listed stop, including its witnesses and required link evidence; `+` adds suppressed failures. Names denote their content-derived identifiers, so no hash must be guessed.

| # | Input multiset | Codec records | Normalized codec stops | Diagnostics / explicit replay effect |
|---:|---|---|---|---|
| 1 | `At1,At3` | `a0…a2` | `S(A,747,T;{At3})` | S + `d(At1,305,T)` |
| 2 | `A,At1` | `a0…a3` | ∅ | `d(At1,305,T)` |
| 3 | `Ag,At1` | `a0…a3` | `S(A,957,T;{Ag})` | S + `d(At1,305,T)` |
| 4 | `A,Acrc2` | `a0…a3` | ∅ | `d(Acrc2,537,CRC)` |
| 5 | `A,Ac2t1` | `a0…a3` | ∅ | `d(Ac2t1,305,T)` |
| 6 | `A,Ac2t3` | `a0,a1` | `S(A,537,BC;{(A,Ac2t3)})` | S + `d(Ac2t3,747,T)` |
| 7 | `A,Ah` | ∅ | `S(A,0,HC;{(A,Ah)})` | S |
| 8 | `A,Ag,Nt2` | `a0…a3,n0,n1` | `S(N,465,T;{Nt2})` | S + `d(Ag,957,T)` |
| 9 | `At3,N` | `a0…a2` | `S(A,747,T;{At3}); S(N,0,PU;{N})` | S; predecessor is short and dirty |
| 10 | `At1,At3,A` | `a0…a3` | ∅ | `d(At1,305,T); d(At3,747,T)` |
| 11 | `At1,X,At3` | A: `a0…a2`; X: `x0…x3` | `S(A,747,T;{At3})` | S + `d(At1,305,T)` |
| 12 | `A,A,Af,Ao` | `a0…a3` | ∅ | ∅ |
| 13 | `At3,Acrc3` | `a0…a2` | `S(A,747,CRC;{Acrc3})` | S + `d(At3,747,T)` |
| 14 | `M,Mt3` | `m0,m2,m3` | ∅ | `d(Mt3,489,T)` |
| 15 | `M,Mc1t3` | `m0` | `S(M,292,BC;{(M,Mc1t3)})` | S + `d(Mc1t3,489,T)` |
| 16 | `A,A+,N` | `a0…a3` | `S(A,957,SC;{(A+,N)})` | S |
| 17 | `A,Z` | `a0…a3` | ∅ | `d(Z,0,IH)`; header failure remains unassigned |
| 18 | `A+,N` | `a0…a3` | `S(A,957,SC;{(A+,N)})` | S; seal proof exists without a clean exact predecessor |
| 19 | `Ag,N` | `a0…a3` | `S(A,957,T;{Ag}); S(N,0,PU;{N})` | S; predecessor has a rejected tail |
| 20 | `At1,N,L,Lc2t3` | `a0` | `S(A,305,T;{At1}); S(N,0,PU;{N}); S(L,465,BC;{(L,Lc2t3)})` | S + `d(Lc2t3,577,T)`; L’s conflict remains reportable |
| 21 | `Ag,Ag′` | `a0…a3` | `S(A,957,T;{Ag,Ag′})` | S; both terminal witnesses retained |
| 22 | `A,A+` | `a0…a4` | ∅ | ∅; replay revokes frontier 20, retains frontier 10 and coverage `[0,10)`; publication high-water remains 20 |
| 23 | `A,L` | `a0…a3` | `S(L,0,PU;{L})` | S; missing file 1 |
| 24 | `N` | ∅ | `S(N,0,PU;{N})` | S; missing file 0 |
| 25 | `Aft` | `a0…a3` | `S(A,957,T;{Aft})` | S |
| 26 | `A,Aft` | `a0…a3` | ∅ | `d(Aft,957,T)` |
| 27 | `A+,Nb` | `a0…a4` | `S(N,0,PU;{Nb})` | S; no exact clean predecessor, and digest mismatch prevents SC |
| 28 | `A+,Ni` | `a0…a4` | `S(N,0,PU;{Ni})` | S; 956 is not a validated boundary, so no SC |
| 29 | `Au,Auc3` | `a0` | `S(A,(305,8),RF;{Au,Auc3})` | S + BC diagnostic at 747 with pair `(Au,Auc3)` |
| 30 | `R,Rc3` | `r0,r1,r2` | `S(R,769,BC;{(R,Rc3)})` | S; separate replay stop at `(537,8)` for overlapping ATTACH_OK; LIFE retained, no coverage |
| 31 | `Z` | ∅ | ∅ | `d(Z,0,IH)`; failed header decode, not empty success |

For case 22, A alone has coverage `[0,20)`; adding A+ retains the later VOID and retracts only the revoked certification. For case 30, the codec array deliberately includes the structurally valid overlapping attachment; replay rejects it without replacing the codec’s later BC stop. No other table case introduces a replay stop.

The implementation unit must satisfy these obligations:

- Freeze two isolated answer sets derived from this text and the byte/logical contracts before implementation. Compare full record identities **and contents**, source-provenance sets, witness/admission decisions, codec stops, diagnostics and specified replay effects. Neither oracle may use product parsing, normalization or output.
- Enumerate every unique permutation of each table multiset and every bounded generated multiset, including successor-before-predecessor, reversed chains and interleaved captures. Compare each directly with its independent expected answer. Test incremental accumulation against the oracle for **every accumulated submultiset**, including required retractions.
- Check duplicate idempotence and conflict-free monotonicity of validated storage prefixes. Shorter/torn/CRC-bad copies cannot remove earlier validated positions; compatible longer copies extend them. Header/block/sealing conflicts may retract suffixes. Test semantic cutoffs and VOID revocation separately; certificates and metrics are not monotone.
- Require a targeted mutant for every numbered clause, splitting compound requirements into separate mutants where needed. Each mutant must build, reach its designated case and fail its intended independent assertion. No collision mutant is required because no collision branch exists.
- Generate cuts throughout headers, block headers, payloads, CRCs and footers; clean boundary snapshots; malformed/exact footers; multiple simultaneous defects; repaired and unrepaired CRCs; unsupported schemas; bad offsets/chaining; raw/LZ4 forms; differing block lengths; and disagreements validated in one versus both copies.
- Generate three-plus copies, duplicate multiplicities, all maximum-prefix/clean-tail/reason/witness ties, flags, immutable-header conflicts, all block types, omitted markers/samples, identical rows at distinct positions and zero-duration waits. Preserve original record offsets through every omission.
- Generate empty predecessors, missing roots/middle files, invalid ordinal headers, wrong digest/descriptor/D, nonboundary D, clean versus dirty witness alternatives, authenticated extra blocks, multiple successor claims and conflicts beyond earlier cutoffs. Exercise structural faults before later conflicts, replay faults before codec stops, delayed dependencies, counter exceptions and unqualified captures.
- Test the retroactive VOID with a query ending before its detection time, proving the updated result still incorporates it. Record generator bounds and actual rounds, tokens and gate-box cost; a permutation sweep alone is never the oracle.