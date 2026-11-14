Revision **r3b**, against `origin/master` **9b20b7f1307cab74895c6eff877c71cc31018657**. Refs #868, #913, #914.
References: `K=contracts/evidence-v1/CONTRACT.md`; `R3=contracts/evidence-v1/REVISION-r3.md`; `C=contracts/evidence-v1/codec/CODEC.md`; `D=contracts/evidence-v1/codec/REDELIVERY.md`. Line references use that commit unless explicitly marked otherwise. Text below is proposed normative wording; expected results are author-derived, not an evaluator run.

**1. Challenge dispositions**

| Item | Decision and reason |
|---|---|
| D1 | **ACCEPT.** Put the ordered-input limitation and unordered-multiset precedence in K itself; align R3 Q5. [Verified] K:24–25,267–269 and R3:11 currently preserve the first boot’s prefix; C:4–5 promises unchanged replay semantics. Add the reuse and original-baseline entries to D’s superseded table, translating its historical line coordinates. |
| D2 | **ACCEPT.** Define delivery, capture and aggregate failure once. Case 17 gains an explicit failed aggregate status while A remains usable; case 31 explicitly fails at aggregate level. [Verified] The previous interpretation was per-delivery: `codec/redelivery-oracle/RESULT.md:30–35`; ambiguity was identified in `codec/redelivery-oracle/answers-sonnet.md:84`. |
| D3 | **ACCEPT.** Ambiguity retracts the entire affected capture, including previously exposed prefix records, LIFE and certificates. Ordinary prefix retention cannot describe this exception. [Verified] K:158–159,235–243; D:139. |
| D4 | **ACCEPT.** Restrict predecessor candidates to the successor’s own `(boot,capture,file_no−1)` group, making case 33’s PU evidence “absent”. |
| D5 | **ACCEPT.** Explicitly acknowledge the PU-to-SC change when all sealing conditions except the former baseline comparison hold. SC excludes the successor before its replay-baseline check. |
| §3, case 40 | **ACCEPT.** Use `S(A,957,SC;{(V,N1)})`; V retains A’s header. [Verified] The existing convention is D:118. |
| §3, cases 36/39 | **ACCEPT.** State the lifecycle uncertainty caused by nonnull `invalid_from`, and Q’s fresh-fork/readable-baseline conditions. [Verified] K:148–159. |
| §3, case 42 | **ACCEPT.** Aggregate FAILED, A usable, unassigned IH, no capture ambiguity. |
| §4, oracle inputs | **ACCEPT.** Supply revised CONTRACT and REVISION-r3 from the first independent-answer dispatch. [Verified] Their omission caused the earlier replay-cell disagreement: `codec/redelivery-oracle/RESULT.md:9–25`. |
| §4, unknown baseline | **ACCEPT.** Cases 43–44 distinguish unknown=unknown from unknown≠known zero. |
| §4, records consumed as VOID | **ACCEPT.** Cases 45–46 exercise a counter-differing FRONTIER and its successor. [Verified] This handling is required by K:278–288. |
| §4, header-only reuse proof | **ACCEPT.** Case 47 uses `A,A2t0`; retain its T stop alongside ambiguity. |
| §4, unqualified captures | **ACCEPT.** Track descriptor continuity and explicit VOID; ignored certificate-shaped records do not update baseline. Cases 48–50 distinguish these paths. [Verified] K:291–296 and C:618–624 preserve the certificate exception and explicit VOID. |
| §4, writer obligation | **ACCEPT.** Specify a replay-derived boundary baseline and explicit loss publication before sealing, rather than copying live counters into the successor header. |

[Verified] The live-reader citation is **C:562–564**, not C:554–556. Case 8’s admitted successor must also appear in the unchanged-case audit: D:108.

**2. Complete normative text for changed provisions**

**K §1, replacing K:24–25:**
Within one ordered logical input, a capture ID belongs to the boot named by its first validated descriptor; reuse by another boot is a chain fault under §6. The first-boot/prefix rules in §§5–7 and REVISION-r3 Q5 apply to ordered logical fixtures. A codec delivery multiset instead follows REDELIVERY.md clause 2a, irrespective of the order in which deliveries were supplied. For an ambiguously reused capture ID, clause 2a’s whole-capture withholding overrides ordinary prefix, LIFE and certificate retention, including §§4–6 and §7’s first-descriptor target binding. For unambiguous captures, this contract continues to govern logical replay.

**R3 Q5, replacing R3:11:**
Q5 ACCEPT (§§1,5,6,7) — for ordered logical fixtures, the first descriptor binds capture to boot; a conflicting descriptor stops that ID, preserving its earlier prefix. Codec delivery multisets follow CONTRACT §1’s precedence statement and REDELIVERY clause 2a instead.

**K §5, additional paragraph after K:296:**
Qualification does not exempt descriptor continuity checks. Every capture initializes its running baseline from file 0, updates it through applicable explicit VOID records, and checks each reachable successor descriptor against it. In an unqualified capture, ignored ATTACH_OK, FRONTIER and END records neither update the baseline nor become counter-loss VOID records. Explicit VOID retains its ordinary baseline and lifecycle effects. A successor-baseline mismatch stops the capture even when qualification is false; ignoring certificates never creates coverage.

**C introductory statement, replacing C:4–5:**
This is a design, not a claim that the daemon emits or qualifies v5. It supplements CONTRACT.md, REVISION-r2.md and REVISION-r3.md, subject to CONTRACT §1’s explicit precedence for codec delivery multisets; other logical replay semantics remain controlling.

**C §2, replacing C:151–153 and adding the writer obligation there:**
Every successor repeats boot, capture, qualified and the identical mapping entries. Its loss_baseline MUST restate the running baseline at that file boundary, including null when unknown. File 0 initializes the baseline; a successor descriptor never resets it or reports loss. Replay compares the declaration with its state after preceding replay, including applicable explicit VOID and records consumed as VOID, with the unqualified-certificate exception preserved. A mismatch stops replay at the successor descriptor under CONTRACT §6.
For successor file n, “running baseline” means the value derived by contract replay of files 0 through n−1, not a live-counter snapshot or the preceding header’s declaration. If rotation observes changed or unreadable counters, the writer MUST persist the required explicit VOID in the predecessor before sealing it, using the contract’s counters/null and invalid_from rules. It MUST then derive and declare the resulting baseline. A successor header MUST NOT carry an unrecorded counter change or substitute for a loss report.

**C §8, replacing C:536–542:**
For local codec linkage, require an actual predecessor delivery whose complete validated data region ends exactly at previous_data_end, with no remaining bytes except an absent or exactly valid footer. Verify the digest, immediate ordinal, matching boot/capture, qualification and mapping. These are the static link requirements; baseline equality is not one of them. A shorter validated prefix is insufficient. A validated extension beyond the referenced D is handled by REDELIVERY’s sealing-conflict rule. Removing referenced block bytes invalidates the link; rebuilding only the footer or changing flags does not.
A codec-admitted successor is separately checked during replay against the running baseline at its descriptor. A replay-baseline mismatch creates no PU, SC or RF and does not undo an independently established sealing contradiction. Baseline bytes remain immutable header bytes covered by header validation, header-conflict comparison and predecessor hashing.

**D title and opening, replacing D:1–6:**
Evidence-v1 codec redelivery revision r3b.
Normative reconciliation rule for CODEC.md; this revision takes precedence in the respects specified here. CONTRACT §1 explicitly delegates unordered cross-boot capture ambiguity to clause 2a; other logical replay rules remain controlling. The byte layout is unchanged. The previously published independent answers for cases 1–31 are recorded in redelivery-oracle/RESULT.md. Those answers cover the pre-r3b cells; the aggregate-status amendment and new cases require fresh independent answers. Refs #868, controlling scope decision comment 6868500311; Refs #913, #914.

**D superseded table: additional rows and complete replacement for its original 529–538 row.** The controlling-column heading is “Controlling clauses”.
[Verified] D:10 uses C at `03694b8b`; current C:145–147 maps to historical C:140–142, and current C:151–153 maps to historical C:146–148.

| Original C lines at `03694b8b` | Superseded effect or interpretation | Controlling clauses |
|---|---|---|
| 140–142 | Cross-boot reuse is called ambiguous without defining the unordered-multiset outcome. No boot or prefix wins for that capture ID. | K §1; D 2a |
| 146–148 | A successor repeats the original baseline. It instead restates the replay-derived boundary baseline. | K §1; C §2; D 19 |
| 529–538 | One chosen predecessor decides admission, any delivery difference is ambiguous, or original-baseline equality is a static link requirement. Actual clean witnesses and sealing proofs govern linkage; running-baseline agreement is checked during replay. | D 8–15,19 |

**D new clause 0, immediately before clause 1:**
**0. Failure levels.** Delivery-level failure reports an independently rejected header, tail, structural record or local link; pairwise conflicts retain their proving-pair diagnostics. Such diagnostics alone do not establish failure of the reconciled capture. A capture is FAILED if it has a normalized codec stop, a reachable replay stop, or CAPTURE_BOOT_AMBIGUITY; otherwise it passes reconciliation/replay. Failed captures may retain only the usable evidence permitted by their applicable cutoff or ambiguity rule. The aggregate is FAILED if any capture is FAILED or any delivery has an unassigned header failure; otherwise it is PASS. Suppressed delivery failures alone do not fail the aggregate. Aggregate failure MUST coexist with separately returned usable capture results. PASS describes decoding/replay, not completeness of coverage or qualification; an empty record array is never a substitute for an explicit failure status.

**D new clause 2a:**
**2a. Cross-boot capture ambiguity.** For each capture ID, collect distinct boot IDs from every independently validated header in the complete delivery multiset. More than one boot produces exactly one CAPTURE_BOOT_AMBIGUITY outcome for that capture ID, classified under clause 0. This outcome belongs to the separate capture/replay result domain, outside clause 3’s codec reasons; it has no codec rank and does not participate in codec-stop normalization.
Emit no logical records for the affected capture ID under any boot, including otherwise valid earlier prefixes. Expose no LIFE, certificates, coverage or metrics derived from that capture. Other capture IDs retain their independently evaluated records and outcomes.
The outcome identifies the capture ID, the complete observed boot set, and every distinct header-valid delivery naming that capture as witnesses. Each witness supplies its delivery identity, boot, file ordinal and header position `(0,null)`. Identical byte strings contribute once; distinct flag/footer/tail variants remain distinct witnesses. Canonically order boots and delivery witnesses by their byte identities, never arrival order.
A validated non-root header establishes reuse even if its root or predecessor is absent, linkage fails, or another cutoff prevents admission. Neither file 0, an admitted chain, lexical boot order nor first arrival selects a winner. An invalid header establishes no trusted boot/capture identity and cannot prove reuse.
Continue independent codec inspection, local linkage classification and normalization for all deliveries. Preserve their diagnostics and normalized stops alongside ambiguity; do not manufacture HC or another codec reason for reuse. Do not replay the ambiguous capture or invent semantic stops within it.
Incremental evaluation MUST retract the entire affected capture, including previously exposed prefix records, LIFE, certificates, coverage and metrics, when a newly validated header establishes reuse. This is the unordered-input exception expressly authorized by CONTRACT §1.

**D clause 3:**
**3. Closed codec reasons.** The complete enumeration, with lower rank winning a same-position tie, is: HEADER_CONFLICT(HC)=0, BLOCK_CONFLICT(BC)=1, SEAL_CONFLICT(SC)=2, PREDECESSOR_UNAVAILABLE(PU)=3, BAD_CRC(CRC)=4, TRUNCATED(T)=5, INVALID_HEADER(IH)=6, INVALID_BLOCK(IB)=7, RECORD_FAULT(RF)=8. Diagnostic details are evidence, not additional reason codes. Capture ambiguity and replay failures belong to a separate result domain and do not compete within this enumeration.

**D clauses 13–15:**
**13. Actual predecessor witness.** For a header-valid successor, predecessor candidates are exclusively the header-valid deliveries in its own `(boot,capture,file_no−1)` group. A witness must independently validate exactly through the successor’s declared D, have no remaining bytes except an absent/exact footer, and satisfy the digest and static descriptor/link requirements: immediate ordinal, boot/capture, qualification and mapping. Baseline equality is excluded. A witness may differ from the record-source copy; partial deliveries cannot collectively construct one. Determine this local fact even when an earlier cutoff or capture ambiguity prevents admission.
**14. Sealing contradiction.** A header-valid successor and candidate predecessor prove SC only when clause 13’s static fields match, D is a validated block boundary, the successor digest matches the predecessor’s immutable bytes through D, and that predecessor has a complete validated block beyond D. Baseline equality is neither required nor evaluated. No clean predecessor ending at D is required. Place SC in the predecessor at D; retain only its blocks before D and exclude its extension and the successor. Rejected tail bytes alone prove nothing about sealing.
Removing baseline equality deliberately changes a former PU into SC whenever the remaining sealing-proof conditions hold. This applies even if the successor’s declaration would fail a reachable replay-baseline check. SC excludes that successor, so no such replay check is reached.
**15. Link classification and admission.** If seal proofs exist for a successor delivery, generate their SC candidates and reject that link; do not also generate PU merely because no clean witness exists. Otherwise generate PU at the successor header exactly when no witness exists. Missing, short, dirty, wrong-digest and statically mismatching predecessor alternatives use PU. Baseline differences do not affect this classification. An actual witness permits codec continuation only if no earlier cutoff, group conflict or capture ambiguity prohibits it. Exclusion solely for those reasons creates no additional local link failure. Codec admission does not establish replay-baseline agreement.

**D clauses 18–20:**
**18. Codec cutoff.** Subject to clause 2a’s whole-capture withholding, a capture’s earliest normalized codec stop excludes the offending unit and every later record/file. A block stop excludes the entire block; a structural record stop preserves preceding eligible records in that validated block. Start from file 0 and require each immediate link; never manufacture a missing file or reinterpret a successor as a root. Independent captures remain separate. Later intrinsic codec stops remain reportable.
**19. Replay boundary.** Replay receives only the codec-admitted stream of captures without clause 2a ambiguity. Resolve dependencies within that stream before declaring semantic faults. At each reachable successor descriptor, compare loss_baseline with the running baseline after preceding replay, including explicit VOID and applicable readable/null-counter loss handling. Equality requires both values unknown, or both known with all fifteen components equal; unknown differs from known zero. Apply CONTRACT’s unqualified-certificate exception: ignored certificates cannot move the baseline, but explicit VOID and successor checks still apply.
A mismatch produces a separate replay stop at that descriptor `(0,null)`, before any of its records, reporting expected and declared baselines. Do not reset the baseline, synthesize VOID, report counter loss, or alter codec admission, codec records, witnesses or normalization. Retain the preceding replay prefix, including prior revocations, and ignore every later record/file of that capture.
The effective capture cutoff is the earlier of its codec cutoff and reachable replay stop. Do not evaluate baseline mismatches or other semantic failures beyond that cutoff. Independent codec inspection continues. Reconstruct surviving certificates and LIFE under r3 without feeding semantic stops into codec normalization.
**20. Diagnostic and update contract.** Diagnostics include every independently observed header failure, first rejected tail, structural fault, pairwise conflict and PU, including suppressed candidates. PU carries the declared predecessor reference and validation/link-check results for clause 13’s candidate group; an empty group is reported as absent. Deduplicate and canonically order diagnostics, stops and witnesses using physical positions, reason ranks and delivery byte strings; arrival indices are forbidden. Signal failures at the levels defined in clause 0 while returning independently usable results. Updated replay MUST incorporate every retained later VOID; shorter deliveries cannot suppress it.

**D:78, final normative sentence:**
The ranked normalization and multiset rules above are normative requirements.

**D:137, independent-answer obligation:**
Before implementation, freeze two fresh isolated answer sets derived from the revised byte/logical contracts. Supply CONTRACT.md and REVISION-r3.md from the first dispatch, together with CODEC.md, REDELIVERY’s normative rules and input-only fixture/case definitions. Withhold expected columns and previous answers. Compare full record identities and contents, source-provenance sets, witness/admission decisions, codec stops, diagnostics, capture outcomes, aggregate status and specified replay effects. Independently derive the new and amended cells and revalidate cases 1–31. Neither oracle may use product parsing, normalization or output.

**D:139, monotonicity obligation:**
Check duplicate idempotence and conflict-free monotonicity of validated storage prefixes. Shorter/torn/CRC-bad copies cannot remove earlier validated positions; compatible longer copies extend them. Header/block/sealing conflicts may retract suffixes. CAPTURE_BOOT_AMBIGUITY retracts the entire capture, including previously exposed prefix records, LIFE and certificates and their derived results. Test semantic cutoffs and VOID revocation separately; certificates and metrics are not monotone.

**3. Complete fixture additions and expected table**

**D:82, fixture defaults:**
Unless explicitly overridden, fixtures use raw blocks, one row/record per block, valid integrity fields, zero flags/creation metadata, qualified=true and fifteen zero baseline counters. Boot is `"b"`; captures are `"a"` for A/N/L/R and their variants, `"m"` for M and `"x"` for X. M alone maps event ID 1 to wait name `"w"`; other mappings are empty. Type-2 timestamp hints are zero. Unmentioned query IDs and CPU values are zero.

**D:85, A+ definition:**
`A+` appends a fifth type-2 block to A at 957 containing `a4=VOID(t=30,counters=B0,reason="x",invalid_from=20)`, ending at 1179. This is the only VOID in cases 1–31.

[Verified] Existing fixture conventions and offsets: D:83–97. Counter encodings are 121 bytes known and 1 byte unknown: C:46. Block/frame/body widths: C:196–198,279–294,318–324. The following new lengths are derived from those widths.

- `B0` is fifteen zeros; `B1` differs only at zero-based slot 4, `query_text_drop=1`.
- `A2` is A with boot `"c"` and repaired header/block integrity; all lengths and offsets remain unchanged. `A2t0` retains its complete valid header plus one byte of block 0: length 194, zero validated blocks, T at 193.
- `N2` is N with boot `"c"`, referencing A2’s D=957 and digest, with repaired integrity. A2 is not implicitly supplied.
- `Z2` flips one bit of A2’s stored header CRC without repair; its purported identity is untrusted.
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

`U(c;W)` denotes CAPTURE_BOOT_AMBIGUITY for capture c, observed boots `{"b","c"}`, and the complete delivery-witness set W; every witness has header position `(0,null)` and its defined ordinal/boot.
`P(F;expected,declared)` denotes the separate baseline-mismatch replay stop at F’s descriptor `(0,null)`. `S` and `d` retain D:97’s meanings. In case 40, the stop’s file label is A because V has A’s header; its witness is V.
For replay descriptions, `HW` means publication high-water, distinct from header length H. Every retained LIFE below has exit=null; retained projections have waits=[] and open=null. Fresh LIFE without an attachment has coverage=[].

- `RA`: baseline B0, F=HW=20; PID 1 LIFE `[0,null]`, coverage `[0,20)`.
- `RV`: baseline B1; frontier 20 revoked, F=10, HW=20; PID 1’s prior LIFE retained, coverage `[0,10)`. Nonnull invalid_from creates lifecycle uncertainty at t=30.
- `RU`: RV’s retained LIFE/certificates and lifecycle uncertainty, but baseline=null; no recovery.
- `RW`: w4 is consumed solely as VOID at t=30 with B1 and invalid_from=null; it publishes no frontier. Baseline B1, F=HW=20, PID 1 LIFE retained, coverage `[0,20)` frozen; no lifecycle taint from this slot-4 change.
- `LQ`: q0…q3 survive as LIFE `[40,null]` for PIDs 2…5. In RV, they are eligible because appended after the uncertainty with readable B1, start_known=true and fork=40≥30. They create no runs or coverage.
- `LN`: n0…n3 survive as LIFE `[0,null]` for PIDs 2…5, without coverage.
- `LX`: X retains LIFE `[0,null]` for PIDs 10…13, baseline B0, F/HW absent, no coverage.
- `RI`: unqualified Wi retains PID 1 LIFE, ignores its certificates including w4, baseline remains B0, F/HW absent, no coverage.
- `RVI`: unqualified Vi retains PID 1 LIFE; explicit v4 establishes B1 and lifecycle uncertainty at 30; F/HW remain absent, no coverage. LQ is eligible afterward.

| # | Input multiset | Codec records | Normalized codec stops | Diagnostics / explicit replay effect | Aggregate |
|---:|---|---|---|---|---|
| 17 | `A,Z` | `a0…a3` | ∅ | `d(Z,0,IH)` unassigned; A passes with RA | FAILED |
| 31 | `Z` | ∅ | ∅ | `d(Z,0,IH)` unassigned; no trusted capture | FAILED |
| 32 | `A,A2` | ∅ | ∅ | `U("a";{A,A2})`; no replay for `"a"` | FAILED |
| 33 | `A,N2` | ∅ | `S(N2,0,PU;{N2})` | S; predecessor group `("c","a",0)` absent; `U("a";{A,N2})` | FAILED |
| 34 | `A,A2,X` | `x0…x3` | ∅ | `U("a";{A,A2})`; X passes with LX | FAILED |
| 35 | `A,A2,A2` | ∅ | ∅ | `U("a";{A,A2})`; identical duplicate contributes once | FAILED |
| 36 | `V,Q` | `a0…a3,v4,q0…q3` | ∅ | No failure diagnostics; RV + LQ; Q baseline passes | PASS |
| 37 | `V,Q0` | `a0…a3,v4,q0…q3` | ∅ | `P(Q0;B1,B0)`; RV retained; no Q0 records survive replay | FAILED |
| 38 | `A,N` | `a0…a3,n0…n3` | ∅ | No failure diagnostics; RA + LN; successor B0 passes | PASS |
| 39 | `A,V,V,Q` | `a0…a3,v4,q0…q3` | ∅ | No failure diagnostics; RV + LQ; predecessor source and clean witness sets both `{V}` | PASS |
| 40 | `V,N1` | `a0…a3` | `S(A,957,SC;{(V,N1)})` | S; RA; v4 and N1 excluded; no reachable baseline check | FAILED |
| 41 | `A,N1` | `a0…a3,n0…n3` | ∅ | `P(N1;B0,B1)`; RA retained; no N1 records survive replay | FAILED |
| 42 | `A,Z2` | `a0…a3` | ∅ | `d(Z2,0,IH)` unassigned; no U; A passes with RA | FAILED |
| 43 | `Vu,Tu` | `a0…a3,vu4` | ∅ | No failure diagnostics; RU; Tu’s unknown baseline passes | PASS |
| 44 | `Vu,T0` | `a0…a3,vu4` | ∅ | `P(T0;null,B0)`; RU retained | FAILED |
| 45 | `W,Qw` | `a0…a3,w4,q0…q3` | ∅ | No codec failure diagnostics; RW + LQ; Qw’s B1 passes | PASS |
| 46 | `W,Qw0` | `a0…a3,w4,q0…q3` | ∅ | `P(Qw0;B1,B0)`; RW retained; no Qw0 records survive replay | FAILED |
| 47 | `A,A2t0` | ∅ | `S(A2,193,T;{A2t0})` | S; `U("a";{A,A2t0})`; no replay for `"a"` | FAILED |
| 48 | `Wi,Qwi0` | `a0…a3,w4,q0…q3` | ∅ | No codec failure diagnostics; RI + LQ; ignored w4 does not change B0; successor passes | PASS |
| 49 | `Wi,Qwi1` | `a0…a3,w4,q0…q3` | ∅ | `P(Qwi1;B0,B1)`; RI retained; no successor records survive replay | FAILED |
| 50 | `Vi,Qvi` | `a0…a3,v4,q0…q3` | ∅ | No failure diagnostics; RVI + LQ; explicit VOID changes baseline and successor B1 passes | PASS |

Counter-loss annotations in RW and ignored-certificate annotations in RI remain semantic diagnostics, not codec failures. Cases apply to every permutation, including successor-before-predecessor, and to incremental accumulation evaluated against each accumulated submultiset. Cases 36–37 share clean predecessor witness `{V}` despite different replay outcomes.

**D:133, complete replacement paragraph:**
For case 22, A alone has coverage `[0,20)`; adding A+ retains the later VOID and retracts only the revoked certification. For case 30, the codec array deliberately includes the structurally valid overlapping attachment; replay rejects it without replacing the codec’s later BC stop. Among cases 1–31, only case 30 introduces a replay stop.

**4. Cases 1–31: exact cell accounting**

The **aggregate-status cell for case 17 is newly specified as FAILED**, strengthening the prior per-delivery-only interpretation; its diagnostics/replay cell now explicitly says that A remains usable. The **aggregate-status cell for case 31 is newly explicit as FAILED**, consistent with its previous failed-decode wording. Neither amendment changes records, normalized stops or the IH diagnostic.

All other existing expected cells in cases 1–31 remain unchanged by this proposal; this is a contract audit, not a rerun claim. [Verified] Their fixtures use one boot and B0; the sole explicit VOID retains B0: D:82–95. Case 8 admits the successor prefix with B0 against running B0: D:108. Cases 16/18 cut before VOID; case 22 has no successor; cases 27/28 reject linkage: D:116–128. Case 30 retains its separate replay stop and codec BC: D:130,133. Consequently neither new ambiguity nor successor-baseline mismatch changes those existing value cells.