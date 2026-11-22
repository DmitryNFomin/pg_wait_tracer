VERIFIED — References: P=`scratchpad/wt-pc`, M=`scratchpad/wt-discm`, MA=`origin/master`; S=`scratchpad/a2go/integ/astra-spec.md` (S:3; `a2go/integ/fable-challenge2.md:2`).

1. **INFERRED — REJECT sufficiency as proposed; ACCEPT kind 8 as the smaller carrier.**
VERIFIED — Unknown kinds produce BAD_KIND, and the block reader returns −2; the verdict model treats BAD as losing the remaining file (`MA/src/coverage_record.h:201–202`; `MA/src/event_reader.c:468–469`; `MA/src/coverage.h:31`; `MA/src/coverage.c:114–119`).
VERIFIED — That is suffix rejection, not necessarily boot-wide refusal: holes start at the last watermark’s floor (`MA/src/coverage.c:71–75`). The server skips COVERAGE entirely (`MA/src/server.c:2884–2890`).
INFERRED — ACCEPT the sidecar compatibility objection: decoding BOOT.d does not enforce its proposed capability bit; the existing verdict model has no mode field, and the server skips it (`MA/src/coverage_record.h:218`; `MA/src/coverage.h:46–56`; `MA/src/server.c:2889–2890`).
INFERRED — Counterexample: support commits at 6, startup closes at 6, then W=100 with floors=80; a later contradiction appends revocation and boot-wide GAP_OPEN, both lost in damaged tail. Existing holes cover [80,∞), while [20,30) can remain COMPLETE—violating revocation from boot (`MA/src/coverage.c:71–75,394–396,624–627`; `M/contracts/merge-v1/CONTRACT.md:484`). Fsync ordering does not repair later damage.
INFERRED — Kind 8 therefore needs explicit reader rules: uncertain revocation history invalidates affected coverage from boot; replay examines the complete required capture history; rotation/retention preserves applicable historical support and irreversible revocations. Re-emitting only live assertions is insufficient if the discarded history contains the sole revocation.
INFERRED — BOOT nonce plus `(pid,cookie)` supplies capture/lifetime identity; validate BOOT and conflicting duplicate assertions. The amendment must expressly qualify DECISION 42’s “add no per-record discovery storage carrier”; unchanged payload size alone does not resolve that prohibition (`MA/src/event_writer.c:903–918`; `M/contracts/merge-v1/CONTRACT.md:466,482`). No sidecar or new directory is inherently required.

2. **INFERRED — ACCEPT INVENTORY for the qualified pre-existing holder; move J13 to E.**
VERIFIED — INVENTORY preserves the census declaration for an unresolved number and resolves that number (`M/src/merge_capture.c:782–799`). J13 expects `U7[U] → F7`, `b_U=7`, `C_close=7`, `G(8)`, no pending obligations and known census (`M/contracts/merge-v1/oracle-discovery/EXPECTED.md:113`).
INFERRED — The conservative upper bound must establish pre-F0 existence, with pinned continuity, complete history, reliable accounting and J precedence before INVENTORY. A bound merely before a later boot_start is insufficient. These are DECISION 31/33 premises, not an exact-start requirement (`M/contracts/merge-v1/CONTRACT.md:304,312–330`). Emit INSTALLED separately; never store the tick upper bound as exact identity.

3. **INFERRED — ACCEPT the header reductions with these qualifications.**
VERIFIED — Unknown parent encoded as zero can classify as NONMEMBER and resolve the observation (`M/src/core/merge_adapter.c:233–234`; `M/src/merge_capture.c:868–869`).
INFERRED — Require all four identifiers known/nonzero, a retained binding and handle, plus existing reconciliation prerequisites before REBIND; use it only for FORK/NONMEMBER.
INFERRED — Drop the persisted qualification SHA/manifest requirement, but retain binary-specific qualification and executable/thread-history proof; major plus role alone cannot replace them (`M/contracts/merge-v1/CONTRACT.md:430–434`).
INFERRED — The coordinator persists a committed support’s revocation before delivering its contradicting POINTER, blocks certification on publication failure, and still processes the contradiction/fault. A contradiction before commitment must not manufacture a committed-bound revocation (`M/src/merge_capture.c:907–914`; S:254).
INFERRED — Cut `listed_at`, `holder`, `bound_at`; preserve any unchanged API slot with an explicitly unused value. Retain immutable pre-enable acknowledgements per generation and expose them through the binding accessor.
INFERRED — Record totals and reconciliation cut qualify that record’s census evidence; the fresh checkpoint gates present acceptance and supplies intervening invalidations. Drain faults and registered obligations before positive census; newer checkpoint totals cannot silently upgrade an older census cut.

4. **VERIFIED — ACCEPT: reaching release without a successful publisher faults WRITER_PUSH_FAIL** (`M/src/core/merge.c:933–937`; `M/src/merge_capture.c:399–407`).
INFERRED — Coordinator rule: withhold CONFIRM_NO_WAIT_WORD until a real checked publisher is available; retain pending inventory and pointer monitoring. An available publisher that fails must fault, not become ordinary waiting. Test both branches and a mutant removing the gate.

5. **INFERRED — REJECT 79/85 after accepting J13’s promotion: use 80/86.**
VERIFIED — The original table has 85 E and places J13 in P; the six identified rows require supporting publication (`S:133,235`; `M/contracts/merge-v1/oracle-discovery/ROWS.md:123,127–128,132,135–136`).
INFERRED — ACCEPT record-level read-back as E’s endpoint; retain assertions on merge outputs and persisted bytes. Do not claim verdict/display integration (`MA/src/coverage.h:5`; `MA/src/server.c:2887–2890`).

6. **INFERRED — ACCEPT U-P≈2 and U-I≈4–5 implementation slices plus four live cells as provisional sizing, not measured commitments. REJECT persistence≈1 as presently scoped.**
INFERRED — Persistence omits the reader/revocation-history semantics demonstrated in point 1; freeze those independent rows first, then budget writer/rotation and reader/recovery work separately.
INFERRED — Safe cuts: sidecar machinery, qualification manifests, unused fields and replicated acknowledgement arrays. Combine tests with their implementation slices. Preserve damaged-tail revocation, rotation/reopen, publication-failure and all four live qualification checks; cutting them would remove the evidence needed for the claimed behavior.