# Fable's challenge of astra-redelivery-r3.md (2026-11-14), verbatim

1. Verdict: ACCEPT-WITH-CHANGES. Both rules are order-independent as written; the defects are precedence, a three-level failure signal that silently touches case 17, and untested branches. No evaluator was run; the case values below are my hand derivation.

2. Defects
D1 (2a last paragraph, precedence). A codec revision "supersedes CONTRACT §§1,5,6,7 and R3 Q5", while for #914 the same draft makes CONTRACT control. [Verified] C.md:4-5 "supplements ... without changing their replay semantics"; D:3 claims precedence over CODEC.md only. Multiset `A,A2`: K:268 says "the first boot's surviving prefix remains addressable", 2a says zero records. Fix: add a CONTRACT-side line (K:24-25 or a REVISION-r4 entry) saying "within one ordered input" stands and a codec delivery multiset follows REDELIVERY 2a. Also state that K §5 rule 5 stays reachable only from logical fixtures ([Verified] src/core/evidence_replay.c:102 still keeps the prefix), and add C:145-147 to D's superseded table.
D2 (2a paragraph 6 plus the D:76 edit). "Failed decode" now exists at delivery, capture and aggregate level, but only 2a says which. Case 34 says "failed aggregate decode"; case 42 `A,Z2` says "failed decode" while A replays. The frozen case 17 `A,Z` was answered as a per-delivery diagnostic ([Verified] redelivery-oracle/answers-sonnet.md:84 U1, RESULT.md "notational points"). The appended "unrelated captures' results remain available" pushes case 17 toward aggregate failure, so "1-31 unchanged" is not established for that cell. Fix: define the three levels once and give aggregate status explicitly for cases 17, 31, 32-35 and 42.
D3 (D:139 edit). "Extend the retraction exception" is under-specified: the existing sentence allows conflicts to "retract suffixes", but 2a retracts the whole capture, including validated prefix records and LIFE that K:158 and K:240 retain. Submultiset sequence `A` then `A,A2`: a0…a3, LIFE and coverage [0,20) are exposed, then all withdrawn. Fix: say "entire capture, including previously exposed prefix records, LIFE and certificates".
D4 (clauses 13/15, predecessor candidates). Case 33 `A,N2`: nothing says whether N2's candidates are the group (boot,capture,file_no-1) only, or every delivery of that capture and ordinal. The reason is PU either way ([Inferred] boot bytes change the digest), but the PU diagnostic differs: "absent" versus "A checked, boot mismatch". Fix: one sentence in 13 restricting candidates to the successor's own (boot,capture,file_no-1) group.
D5 (clause 14, meaning change not stated). Removing baseline from the static fields moves every pair that differs only in baseline from PU to SC. Case 40 `V,N1`: the old text gives PU with a0…a3,v4 and coverage [0,10); the new text gives SC, drops the validated revoking VOID and shows [0,20). [Inferred] This follows the case-18 precedent (D:118) and I accept it, but it must be written as a deliberate change, because SC now fires on a successor that would itself fail the baseline check.

Checks you asked for
- Incremental retraction is consistent with clause 1 ([Verified] D:38 "MUST equal fresh evaluation") and with live readers, who must incorporate later input before claiming an updated result. Your cite is stale: at 9b20b7f1 that text is C:562-564; C:554-556 is the integrity paragraph.
- The witness rule is monotone [Inferred]: the boot set and witness set only grow, and ambiguity is absorbing. No arrival dependence found.
- No new replay stop reason is needed. [Verified] src/core/evidence_replay.c:381-384 already faults at a successor descriptor whose baseline is not the running one. But no logical scenario exercises it ([Verified] R3:35: only scenario 22 has a successor, restating zeros), so cases 37 and 41 are its first independent answers.
- Cases 1-31: values hold except the case-17 status cell in D2. [Verified] Case 8 is the only admitted successor and it is B0 against B0; the draft's audit lists only the VOID-bearing cases and should name case 8.

3. New cases: I compute the same records, stops and replay effects for 32-42, with these notes.
- Case 40: by the case-18 convention ([Verified] D:118 `S(A,957,SC;{(A+,N)})`) the stop is `S(A,957,SC;{(V,N1)})`, since V's header is byte-identical to A's. The draft writes `S(V,…)`. Notation only, but two authors will split.
- Cases 36/39: correct. The fixture relies on v4's nonnull invalid_from causing lifecycle uncertainty at t=30 ([Verified] K:148-149), with Q's LIFE valid only because fork=40 ≥ 30 and start_known ([Verified] K:153-154). The "V replay" definition should say this, or authors will stumble as they did on case 22.
- Case 42: undetermined until D2 is fixed.

4. Missing for two independent authors
- Supply CONTRACT.md and REVISION-r3.md from round 1; cases 36-41 are replay cells, and round 1 of the 31 split on exactly that.
- Unknown baseline: clause 19's "both unknown" and "unknown differs from known zero" have no case. Add V with a VOID counters=null, then a successor declaring null (pass) and one declaring B0 (P).
- Records consumed as VOID: no case where a counter-differing FRONTIER or ATTACH_OK (K:278-288) moves the running baseline before a successor.
- Header-only proof of reuse: add `A,A2t0`. I expect U, records ∅, and `S(A2,193,T;{A2t0})` preserved [Inferred].
- Unqualified captures: the text does not say whether the running baseline is tracked when certificates are ignored (K:291). A writer and reader could disagree at every rotation [Inferred].
- Writer obligation: say that "running baseline" is what replay derives from files 0..n-1, not the live counters; a writer seeing changed counters at rotation must write a VOID before sealing [Inferred].

Not checked: REVISION-r2.md, WORKED.md, the fixture bytes and offsets of V/Q/N1/A2 (lengths taken on trust), and K §2 LIFE rules beyond lines 48 and 71-72.
