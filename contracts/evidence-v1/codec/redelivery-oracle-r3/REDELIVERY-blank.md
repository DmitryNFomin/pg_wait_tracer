# Evidence-v1 codec redelivery revision r3b.

Normative reconciliation rule for CODEC.md; this revision takes precedence in the respects specified here. CONTRACT §1 explicitly delegates unordered cross-boot capture ambiguity to clause 2a; other logical replay rules remain controlling. The byte layout is unchanged. The previously published independent answers for cases 1–31 are recorded in redelivery-oracle/RESULT.md. Those answers cover the pre-r3b cells; the aggregate-status amendment and new cases require fresh independent answers. Refs #868, controlling scope decision comment 6868500311; Refs #913, #914.

## Superseded CODEC.md wording

Line numbers below refer to CODEC.md at `03694b8b`, before this revision's
insertions. Only the conflicting effects or interpretations listed here are
superseded; byte validation, writer obligations and the unqualified-certificate
and readable/null-counter exceptions remain applicable.

| Original CODEC.md lines | Superseded effect or interpretation | Controlling clauses |
|---|---|---|
| 14–18 | Every delivery's framing failure stops the capture; a malformed logical record permits continued replay under the old global-VOID path. | 7, 12, 17–19 |
| 140–142 | Cross-boot reuse is called ambiguous without defining the unordered-multiset outcome. No boot or prefix wins for that capture ID. | K §1; D 2a |
| 146–148 | A successor repeats the original baseline. It instead restates the replay-derived boundary baseline. | K §1; C §2; D 19 |
| 479–481 | The adapter uses the old §9 ordered-loss path for record faults. | 7, 18–19 |
| 529–538 | One chosen predecessor decides admission, any delivery difference is ambiguous, or original-baseline equality is a static link requirement. Actual clean witnesses and sealing proofs govern linkage; running-baseline agreement is checked during replay. | D 8–15,19 |
| 540–544 | A corrupt delivery automatically stops the capture despite another compatible delivery; inspection ends at that capture cutoff. Missing-file/root and cross-capture prohibitions remain. | 12, 15–18 |
| 572 | Each bad delivery CRC or link automatically stops the chain without reconciliation or candidate normalization. | 6, 12–17 |
| 574 | An unknown framed record kind permits continued replay after global loss. | 7, 18–19 |
| 579–582 | Every rejected segment is an unsuppressible capture stop and makes the whole multiset decode fail. | 12, 17–20 |
| 587–596 | Ordinary records after a stopping structural fault are delivered to replay, which may recover later in the same capture under the old synthetic-VOID path. Independent validation still continues. | 7, 16, 18–19 |
| 603–608 | State-dependent replay failures use the old continued global-VOID path rather than separate reachable replay stops; the counter exceptions remain. | 7, 19 |
| 623–625 | Replay must implement the old ordered fault/global-loss continuation path. | 7, 18–19 |

## Source references

References are at `origin/master`, verified as `03694b8b`: `C` = `contracts/evidence-v1/codec/CODEC.md`; `R3` = `contracts/evidence-v1/REVISION-r3.md`; `K` = `contracts/evidence-v1/CONTRACT.md`; `D` = `contracts/evidence-v1/codec/REDELIVERY.md`.
[Verified] Header CRC excludes flag bytes `[6,8)` (`C:125–128`); predecessor sealing and linkage are specified at `C:510–535`; physical identities and omission rules are at `C:453–469`; r3 distinguishes physical stopping from dependency-dependent decisions (`R3:7–15`).


## Normative rule, fixtures and obligations

The following is the complete replacement redelivery rule; unaffected byte-format and replay requirements remain applicable.

0. **Failure levels.** Delivery-level failure reports an independently rejected header, tail, structural record or local link; pairwise conflicts retain their proving-pair diagnostics. Such diagnostics alone do not establish failure of the reconciled capture. A capture is FAILED if it has a normalized codec stop, a reachable replay stop, or CAPTURE_BOOT_AMBIGUITY; otherwise it passes reconciliation/replay. Failed captures may retain only the usable evidence permitted by their applicable cutoff or ambiguity rule. The aggregate is FAILED if any capture is FAILED or any delivery has an unassigned header failure; otherwise it is PASS. Suppressed delivery failures alone do not fail the aggregate. Aggregate failure MUST coexist with separately returned usable capture results. PASS describes decoding/replay, not completeness of coverage or qualification; an empty record array is never a substitute for an explicit failure status.

1. **Whole-multiset result.** Reconcile the complete delivery multiset before exposing a capture’s logical input. Records, provenance, link decisions, diagnostics and stops MUST be independent of arrival order, filenames and input-array indices. Incremental evaluation MUST equal fresh evaluation of all deliveries accumulated so far.

2. **Identity.** A delivery is its complete byte string. Its compact name is `id(d)=(byte_length,SHA256(all bytes))`, including flags, footer and rejected tail. Identical byte strings are one delivery for reconciliation and diagnostics. Independently validate each header; only validated headers establish physical file identity `(boot,capture,file_no)`. An invalid header supplies no trusted capture identity or blocks.

**2a. Cross-boot capture ambiguity.** For each capture ID, collect distinct boot IDs from every independently validated header in the complete delivery multiset. More than one boot produces exactly one CAPTURE_BOOT_AMBIGUITY outcome for that capture ID, classified under clause 0. This outcome belongs to the separate capture/replay result domain, outside clause 3’s codec reasons; it has no codec rank and does not participate in codec-stop normalization.
Emit no logical records for the affected capture ID under any boot, including otherwise valid earlier prefixes. Expose no LIFE, certificates, coverage or metrics derived from that capture. Other capture IDs retain their independently evaluated records and outcomes.
The outcome identifies the capture ID, the complete observed boot set, and every distinct header-valid delivery naming that capture as witnesses. Each witness supplies its delivery identity, boot, file ordinal and header position `(0,null)`. Identical byte strings contribute once; distinct flag/footer/tail variants remain distinct witnesses. Canonically order boots and delivery witnesses by their byte identities, never arrival order.
A validated non-root header establishes reuse even if its root or predecessor is absent, linkage fails, or another cutoff prevents admission. Neither file 0, an admitted chain, lexical boot order nor first arrival selects a winner. An invalid header establishes no trusted boot/capture identity and cannot prove reuse.
Continue independent codec inspection, local linkage classification and normalization for all deliveries. Preserve their diagnostics and normalized stops alongside ambiguity; do not manufacture HC or another codec reason for reuse. Do not replay the ambiguous capture or invent semantic stops within it.
Incremental evaluation MUST retract the entire affected capture, including previously exposed prefix records, LIFE, certificates, coverage and metrics, when a newly validated header establishes reuse. This is the unordered-input exception expressly authorized by CONTRACT §1.

3. **Closed codec reasons.** The complete enumeration, with lower rank winning a same-position tie, is: HEADER_CONFLICT(HC)=0, BLOCK_CONFLICT(BC)=1, SEAL_CONFLICT(SC)=2, PREDECESSOR_UNAVAILABLE(PU)=3, BAD_CRC(CRC)=4, TRUNCATED(T)=5, INVALID_HEADER(IH)=6, INVALID_BLOCK(IB)=7, RECORD_FAULT(RF)=8. Diagnostic details are evidence, not additional reason codes. Capture ambiguity and replay failures belong to a separate result domain and do not compete within this enumeration.

4. **Positions.** A codec position is `(file_byte_offset,payload_byte_offset?)`. Header failures/conflicts and incoming-link failures use `(0,null)`. Block rejection/conflict uses `(block_start,null)`; sealing conflict uses `(D,null)`. A framed-record fault uses `(block_start,decoded_payload_record_offset)`. These are byte offsets, never block or record indices. Sort by file ordinal, file-byte offset, then payload-byte offset, with `null` before every payload offset. A decoded offset MUST NOT be presented as an absolute compressed-file offset.

5. **Independent prefix validation.** For each header-valid delivery, scan sequentially from its own H. `P(d)` contains its maximal contiguous sequence of wholly validated blocks of every type; `E(d)` is their exclusive end, or H when empty. Validate complete framing, schema, bounds, decompression, offsets, CRC chain and block CRC before accepting a block. A record-local fault inside trustworthy framing does not remove its enclosing block from P. Never resynchronize after a rejected block, borrow another delivery’s bytes, or skip sample blocks.

6. **Terminal classification.** Invalid headers receive IH. At each subsequent boundary, first accept EOF or an exactly matching complete footer. Otherwise: an incomplete block header receives T; invalid header-level bounds/type/codec/reserved/offset/preceding-CRC fields receive IB; insufficient bytes for a safely bounded declared block receive T; a complete block with bad CRC receives CRC; remaining whole-block decoding/framing/schema failures receive IB. Stop that delivery at this first rejected unit. Check whole-block validity before classifying any contained record fault. All rejected bytes begin at E(d), regardless of the interior byte revealing the failure.

7. **Structural versus semantic faults.** Record-local malformed fields or unknown kinds in trustworthy framing produce RF diagnostics and, where the governing contract requires stopping, RF candidates at their original record offsets. Validation and conflict inspection MUST continue through all of P, including after these faults. Counter comparison, generation/run assignment, delayed dependencies, frontier advancement and other replay-state checks do not produce codec RF candidates. Preserve the unqualified-certificate exception and r3’s readable/null-counter loss handling; do not turn those exceptions into structural stops.

8. **Header conflict.** For every pair in one physical-file group, compare their complete validated headers after omitting only bytes `[6,8)`. Different immutable header byte strings produce HC at offset 0. Retain no records from that group. Do not compare their blocks as if their headers were compatible.

9. **Block conflict.** For each compatible-header pair, compare their validated block-prefix byte strings. If neither is a prefix of the other, produce BC at the start of their first differing block. This is a common block boundary because all earlier blocks agree. Compare complete stored block bytes, including headers, lengths, payloads and CRCs; equal decoded values or equal CRCs are insufficient. Rejected or incomplete blocks supply no conflict evidence.

10. **Prefix union and provenance.** For a group without HC, let E be the maximum E(d). Source its prefix whole from one delivery attaining E, then cut before the earliest applicable conflict/stop. Never splice deliveries. Before the first conflict, all such source copies have identical retained bytes. Report the set of all deliveries attaining E as source provenance; which member supplies those identical bytes is not an observable choice.

11. **Physical records.** Emit each retained physical record position exactly once, in file/block/payload order. Equal contents at distinct positions remain distinct. Omit valid samples and markers from the exact array without changing surviving offsets. Identical copies, flag-only differences and addition/removal of an exactly valid footer cannot change records, conflicts or admission; provenance may reflect the different deliveries.

12. **Terminal-stop eligibility.** Preserve every delivery’s rejected-tail diagnostic. A terminal failure from E(d)<E is never a capture-stop candidate. If any delivery attaining E ends at clean EOF or an exactly valid footer, no terminal failure at E becomes a stop candidate. Otherwise choose the lowest-ranked terminal reason among deliveries attaining E; its witness set is **all** such deliveries with that reason. This creates one terminal candidate at E. Torn/bad footers have no special clean status.

13. **Actual predecessor witness.** For a header-valid successor, predecessor candidates are exclusively the header-valid deliveries in its own `(boot,capture,file_no−1)` group. A witness must independently validate exactly through the successor’s declared D, have no remaining bytes except an absent/exact footer, and satisfy the digest and static descriptor/link requirements: immediate ordinal, boot/capture, qualification and mapping. Baseline equality is excluded. A witness may differ from the record-source copy; partial deliveries cannot collectively construct one. Determine this local fact even when an earlier cutoff or capture ambiguity prevents admission.

14. **Sealing contradiction.** A header-valid successor and candidate predecessor prove SC only when clause 13’s static fields match, D is a validated block boundary, the successor digest matches the predecessor’s immutable bytes through D, and that predecessor has a complete validated block beyond D. Baseline equality is neither required nor evaluated. No clean predecessor ending at D is required. Place SC in the predecessor at D; retain only its blocks before D and exclude its extension and the successor. Rejected tail bytes alone prove nothing about sealing.
Removing baseline equality deliberately changes a former PU into SC whenever the remaining sealing-proof conditions hold. This applies even if the successor’s declaration would fail a reachable replay-baseline check. SC excludes that successor, so no such replay check is reached.

15. **Link classification and admission.** If seal proofs exist for a successor delivery, generate their SC candidates and reject that link; do not also generate PU merely because no clean witness exists. Otherwise generate PU at the successor header exactly when no witness exists. Missing, short, dirty, wrong-digest and statically mismatching predecessor alternatives use PU. Baseline differences do not affect this classification. An actual witness permits codec continuation only if no earlier cutoff, group conflict or capture ambiguity prohibits it. Exclusion solely for those reasons creates no additional local link failure. Codec admission does not establish replay-baseline agreement.

16. **Complete inspection and witnesses.** Evaluate every header-valid delivery and all relevant pairs, including files beyond an earlier cutoff. Generate each compatible pair’s earliest BC, every HC pair, authenticated SC pairs, structural faults and local link failures independently of arrival order. Conflict witnesses are the complete set of proving pairs at the reported position/reason; preserve predecessor/successor roles for SC. Other witnesses are sets of offending deliveries. Identical deliveries contribute once.

17. **Codec normalization.** From eligible codec candidates, return at most one stop per verified physical file: earliest position first, then lowest reason rank. Union the witnesses of candidates tied on both position and reason. Preserve all other failures as diagnostics. Invalid-header diagnostics cannot become stops attributed using unvalidated identity fields.

18. **Codec cutoff.** Subject to clause 2a’s whole-capture withholding, a capture’s earliest normalized codec stop excludes the offending unit and every later record/file. A block stop excludes the entire block; a structural record stop preserves preceding eligible records in that validated block. Start from file 0 and require each immediate link; never manufacture a missing file or reinterpret a successor as a root. Independent captures remain separate. Later intrinsic codec stops remain reportable.

19. **Replay boundary.** Replay receives only the codec-admitted stream of captures without clause 2a ambiguity. Resolve dependencies within that stream before declaring semantic faults. At each reachable successor descriptor, compare loss_baseline with the running baseline after preceding replay, including explicit VOID and applicable readable/null-counter loss handling. Equality requires both values unknown, or both known with all fifteen components equal; unknown differs from known zero. Apply CONTRACT’s unqualified-certificate exception: ignored certificates cannot move the baseline, but explicit VOID and successor checks still apply.
A mismatch produces a separate replay stop at that descriptor `(0,null)`, before any of its records, reporting expected and declared baselines. Do not reset the baseline, synthesize VOID, report counter loss, or alter codec admission, codec records, witnesses or normalization. Retain the preceding replay prefix, including prior revocations, and ignore every later record/file of that capture.
The effective capture cutoff is the earlier of its codec cutoff and reachable replay stop. Do not evaluate baseline mismatches or other semantic failures beyond that cutoff. Independent codec inspection continues. Reconstruct surviving certificates and LIFE under r3 without feeding semantic stops into codec normalization.

20. **Diagnostic and update contract.** Diagnostics include every independently observed header failure, first rejected tail, structural fault, pairwise conflict and PU, including suppressed candidates. PU carries the declared predecessor reference and validation/link-check results for clause 13’s candidate group; an empty group is reported as absent. Deduplicate and canonically order diagnostics, stops and witnesses using physical positions, reason ranks and delivery byte strings; arrival indices are forbidden. Signal failures at the levels defined in clause 0 while returning independently usable results. Updated replay MUST incorporate every retained later VOID; shorter deliveries cannot suppress it.

[Verified] The physical-stop and dependency-resolution basis is `R3:7–15`; explicit null counters report loss rather than malformed evidence at `K:278–288`. Retroactive VOID revocation is specified at `K:189–196`. Footer recognition and rejected tails are specified at `C:500–507`. The ranked normalization and multiset rules above are normative requirements.

The fixture notation below defines the entire case table. Derive codec record arrays and replay effects from these inputs and the normative rules.

- Unless explicitly overridden, fixtures use raw blocks, one row/record per block, valid integrity fields, zero flags/creation metadata, qualified=true and fifteen zero baseline counters. Boot is `"b"`; captures are `"a"` for A/N/L/R and their variants, `"m"` for M and `"x"` for X. M alone maps event ID 1 to wait name `"w"`; other mappings are empty. Type-2 timestamp hints are zero. Unmentioned query IDs and CPU values are zero.
- `LIFE(p)` means `t=0,pid=p,fork=0,start=0,start_known=true,exit=absent`. `ATTACH(t)` means pid=1, fork=0, reread, seed/state=null and zero counters. `FRONTIER(t)` has zero counters. All record contents named below include these fields.
- `A` is clean file 0 with four **type-2** blocks: `a0=LIFE(1)`, `a1=ATTACH(0)`, `a2=FRONTIER(10)`, `a3=FRONTIER(20)`. H=193; block starts are `[193,305,537,747]`; D=957.
- `A+` appends a fifth type-2 block to A at 957 containing `a4=VOID(t=30,counters=B0,reason="x",invalid_from=20)`, ending at 1179. This is the only VOID in cases 1–31.
- `N` is clean file 1, referencing A’s D=957 and digest, with four type-2 records `n0…n3=LIFE(2)…LIFE(5)`. `L` is clean file 2, referencing N, with `l0…l3=LIFE(6)…LIFE(9)`. Both have H=241, starts `[241,353,465,577]`, D=689.
- `X` is independent clean file 0 with four type-2 records `x0…x3=LIFE(10)…LIFE(13)`, H=193, starts `[193,305,417,529]`, D=641.
- `M` is clean file 0 with four blocks: type 0 `m0=TRANS(t=1,pid=1,old=null,new="w",duration=0)`; type 1 one sample `(t=2,pid=1,event="w",weight=1)`; type 2 `m2=LIFE(1)`; type 0 `m3=TRANS(t=3,pid=1,old="w",new=null,duration=2)`. The sample period and maximum weight are both 1. H=201; starts `[201,292,377,489]`; D=580. There is no attachment.
- For a base F, `FtK` truncates it exactly one byte into zero-based block K. `FcrcK` flips one bit of block K’s stored CRC, preserving all other bytes.
- `FcJ` changes block J validly: increment its type-2 record’s t by 1, or its type-0/1 row’s query_id by 1; repair that block’s CRC and all subsequent CRC chaining. Lengths remain unchanged. `FcJtK` then applies the truncation. Thus `Ac2t1` is byte-identical to `At1`; `Mt3`, `Mc1t3` and `Lc2t3` are defined.
- `Ag` appends byte `00` to A; `Ag′` appends byte `01`. `Ao` adds A’s exact 68-byte footer. `Aft` appends only its first 17 bytes.
- `Af` changes only A’s flags to 1. `Ah` changes A’s immutable start_time_ns from 0 to 1 and repairs header/block integrity. `Z` instead flips one bit of A’s stored header CRC without repair.
- `Nb` changes N’s predecessor digest by flipping its first bit; `Ni` changes N’s referenced D to 956 and stores the digest through that byte offset. Both repair their own header/block integrity; neither modification changes block sizes.
- `Au` changes A’s block-1 record kind to 255, preserving its bounded body/common t and repairing CRC chaining. `Auc3` additionally applies the valid block-3 change.
- `R` has A’s header and four type-2 records `r0=LIFE(1)`, `r1=ATTACH(0)`, `r2=ATTACH(1)`, `r3=FRONTIER(20)`. Starts are `[193,305,537,769]`; D=979. `Rc3` changes its last frontier to 21.
- A named record denotes its full physical identity **and the contents above**. Single type-2 record offsets are 8; single type-0/1 row offsets are 0. `a0…a3` means four ordered positions, not a timestamp range.
- `S(F,o,r;W)` is a normalized codec stop; scalar o abbreviates `(o,null)`. W is a delivery set, or a set of proving pairs for conflicts. `d(delivery,o,r)` is an independent failure diagnostic. In the diagnostics column, **S** expands to diagnostics underlying every listed stop, including its witnesses and required link evidence; `+` adds suppressed failures. Names denote their content-derived identifiers, so no hash must be guessed.

- `B0` is fifteen zeros; `B1` differs only at zero-based slot 4, `query_text_drop=1`.
- `A2` is A with boot `"c"` and repaired header/block integrity; all lengths and offsets remain unchanged. `A2t0` retains its complete valid header plus one byte of block 0: length 194.
- `N2` is N with boot `"c"`, referencing A2’s D=957 and digest, with repaired integrity. A2 is not implicitly supplied.
- `Z2` flips one bit of A2’s stored header CRC without repair.
- `V` is A+ with its final VOID counters changed to B1 and repaired integrity. Records are `a0…a3,v4`; `v4=VOID(30,B1,"x",20)` at `(957,8)`, ending at 1179. Header and first four blocks equal A’s.
- `Q` is clean file 1 referencing V’s D=1179 and digest, declaring B1. Its four type-2 records `q0…q3` are LIFE for pid=2…5 respectively, each with t=40, fork=40, start=40, start_known=true, exit=null. H=241; starts `[241,353,465,577]`; D=689.
- `Q0` differs from Q only by declaring B0, with integrity repaired; records remain `q0…q3`.
- `N1` differs from N only by declaring B1, with repaired integrity; it still references A at D=957 and contains `n0…n3`.
- `Vu` is V with final VOID counters=null: records `a0…a3,vu4`, where `vu4=VOID(30,null,"x",20)` at `(957,8)`. That block is 102 bytes; D=1059. Header and preceding blocks equal A’s.
- `Tu` is clean, header-only file 1 referencing Vu at D=1059 with its digest, declaring null; H=D=121. `T0` instead declares B0; H=D=241. Both contain no records.
- `W` is A followed by one type-2 block `w4=FRONTIER(t=30,counters=B1)` at `(957,8)`. Its final block is 210 bytes; D=1167.
- `Qw` is Q with predecessor reference changed to W at D=1167 and its digest. `Qw0` also changes baseline to B0. Their records and offsets remain `q0…q3`.
- `Wi` is W with qualified=false and repaired header/block integrity. `Qwi0` is Qw0 with qualified=false and predecessor digest/reference recomputed for Wi. `Qwi1` differs from Qwi0 only by declaring B1. All retain their corresponding record contents and offsets.
- `Vi` is V with qualified=false and repaired integrity. `Qvi` is Q with qualified=false and predecessor digest/reference recomputed for Vi.
- All modified fixtures repair dependent integrity fields unless expressly stated otherwise. No fixture implicitly adds a predecessor or footer.

`U(c;W)` denotes CAPTURE_BOOT_AMBIGUITY for capture c and complete delivery-witness set W; report the observed boot set as well. Each witness supplies its delivery identity, boot, file ordinal and header position `(0,null)`.
`P(F;expected,declared)` denotes the separate baseline-mismatch replay stop at F’s descriptor `(0,null)`. `S` and `d` retain their meanings above.

For replay descriptions, `HW` means publication high-water, distinct from header length H.

| # | Input multiset |
|---:|---|
| 1 | `At1,At3` |
| 2 | `A,At1` |
| 3 | `Ag,At1` |
| 4 | `A,Acrc2` |
| 5 | `A,Ac2t1` |
| 6 | `A,Ac2t3` |
| 7 | `A,Ah` |
| 8 | `A,Ag,Nt2` |
| 9 | `At3,N` |
| 10 | `At1,At3,A` |
| 11 | `At1,X,At3` |
| 12 | `A,A,Af,Ao` |
| 13 | `At3,Acrc3` |
| 14 | `M,Mt3` |
| 15 | `M,Mc1t3` |
| 16 | `A,A+,N` |
| 17 | `A,Z` |
| 18 | `A+,N` |
| 19 | `Ag,N` |
| 20 | `At1,N,L,Lc2t3` |
| 21 | `Ag,Ag′` |
| 22 | `A,A+` |
| 23 | `A,L` |
| 24 | `N` |
| 25 | `Aft` |
| 26 | `A,Aft` |
| 27 | `A+,Nb` |
| 28 | `A+,Ni` |
| 29 | `Au,Auc3` |
| 30 | `R,Rc3` |
| 31 | `Z` |
| 32 | `A,A2` |
| 33 | `A,N2` |
| 34 | `A,A2,X` |
| 35 | `A,A2,A2` |
| 36 | `V,Q` |
| 37 | `V,Q0` |
| 38 | `A,N` |
| 39 | `A,V,V,Q` |
| 40 | `V,N1` |
| 41 | `A,N1` |
| 42 | `A,Z2` |
| 43 | `Vu,Tu` |
| 44 | `Vu,T0` |
| 45 | `W,Qw` |
| 46 | `W,Qw0` |
| 47 | `A,A2t0` |
| 48 | `Wi,Qwi0` |
| 49 | `Wi,Qwi1` |
| 50 | `Vi,Qvi` |

The implementation unit must satisfy these obligations:

- Before implementation, freeze two fresh isolated answer sets derived from the revised byte/logical contracts. Supply CONTRACT.md and REVISION-r3.md from the first dispatch, together with CODEC.md, REDELIVERY’s normative rules and input-only fixture/case definitions. Withhold expected columns and previous answers. Compare full record identities and contents, source-provenance sets, witness/admission decisions, codec stops, diagnostics, capture outcomes, aggregate status and specified replay effects. Independently derive the new and amended cells and revalidate cases 1–31. Neither oracle may use product parsing, normalization or output.
- Enumerate every unique permutation of each table multiset and every bounded generated multiset, including successor-before-predecessor, reversed chains and interleaved captures. Compare each directly with its independent expected answer. Test incremental accumulation against the oracle for **every accumulated submultiset**, including required retractions.
- Check duplicate idempotence and conflict-free monotonicity of validated storage prefixes. Shorter/torn/CRC-bad copies cannot remove earlier validated positions; compatible longer copies extend them. Header/block/sealing conflicts may retract suffixes. CAPTURE_BOOT_AMBIGUITY retracts the entire capture, including previously exposed prefix records, LIFE and certificates and their derived results. Test semantic cutoffs and VOID revocation separately; certificates and metrics are not monotone.
- Require a targeted mutant for every numbered clause, splitting compound requirements into separate mutants where needed. Each mutant must build, reach its designated case and fail its intended independent assertion. No collision mutant is required because no collision branch exists.
- Generate cuts throughout headers, block headers, payloads, CRCs and footers; clean boundary snapshots; malformed/exact footers; multiple simultaneous defects; repaired and unrepaired CRCs; unsupported schemas; bad offsets/chaining; raw/LZ4 forms; differing block lengths; and disagreements validated in one versus both copies.
- Generate three-plus copies, duplicate multiplicities, all maximum-prefix/clean-tail/reason/witness ties, flags, immutable-header conflicts, all block types, omitted markers/samples, identical rows at distinct positions and zero-duration waits. Preserve original record offsets through every omission.
- Generate empty predecessors, missing roots/middle files, invalid ordinal headers, wrong digest/descriptor/D, nonboundary D, clean versus dirty witness alternatives, authenticated extra blocks, multiple successor claims and conflicts beyond earlier cutoffs. Exercise structural faults before later conflicts, replay faults before codec stops, delayed dependencies, counter exceptions and unqualified captures.
- Test the retroactive VOID with a query ending before its detection time, proving the updated result still incorporates it. Record generator bounds and actual rounds, tokens and gate-box cost; a permutation sweep alone is never the oracle.
