# TASK for independent answerers: merge-v1 delta 7c (selected executions and H11a-H11c)

Give each independent answerer only the amended contract, amended `ROWS.md` and this TASK. Do not supply this settlement, `EXPECTED.md`, mutants, implementation, review, triage, previous answer sets or product output.

Independently derive exactly the 34 executions listed below. Each suffixed ID and explicitly selected parameter variant is a separate execution. Expand referenced row sequences, abbreviations and S into their primitive inputs, and evaluate after every primitive input. Do not derive unlisted variants or perform an all-rows field sweep for this packet.

Delta 7c re-derivation packet:

- Nine previously non-unanimous executions: H1a, H2a, H3c, H3d, H5a, H5b, H6a, H6b, H6c.
- H1b and H1c.
- G4's first variant: the final control is TERM_D19 naming the same EXIT.
- Both R10 variants: qualified non-NULL pointer fire and demonstrated continuity break.
- H3b and N5.
- H7a.
- One R2 execution: p=empty, no K, and the order `TERM_D18, D20@P0, EXIT_D18@L, S(25)`.
- O1.
- N14, G5, G7 and V4.
- A18 and U15.
- H2b.
- H4a.
- H3a and G6.
- Two R5 executions: `(h=1,p=empty,q=empty)` and `(h=1,p=AB_D,q=K)`.
- H11a, H11b and H11c.

For each execution report:

- The complete combined publication timeline: startup OPEN, supporting TEXT/DATA, END, FRONTIER, revoked OPEN and VOID. Also retain the separate merge-only stream. Include exact endpoints, certification markings, detection times and counter vectors. Distinguish plain OPEN from revoked OPEN, successful publication from rejected writer calls, and publication from commitment.
- The exact primitive input and checkpoint establishing each fault or contradiction, with its diagnostic increment and any storage-counter change. A shared detection timestamp does not identify the primitive. Identify deliberate absence of a repeated contradiction report.
- After every primitive input: historical admitted status, admission-proof validity, recorded/refuted control claims, committed b and x, pending lifetime/numeric split, census completeness, certification latch, epoch status and startup-gap state. Report whether each resolving proof committed, separately from whether the gap closed.
- At entry to every VOID callback: the exact epoch status, public discovery state, applicable cache invalidation, persisted startup-gap history and the persisted reader's result compared with the public view. For V4, also evaluate the prefix saved at callback entry independently, with no later publications.
- Capture-cumulative abandonment and uncovered-consumption counts, distinguishing validated consumption from fault disposal, suspension disposal and rejected arrival.
- Every epoch invalidation and nonzero exit, or their absence.
- An explicit undecided list, empty when the contract determines every requested observable. State competing alternatives and the missing decision for any ambiguity; do not silently select an alternative or consult product behavior.

This packet replaces the delta 7b re-derivation list and its all-rows field sweep. Give each answerer only the amended contract, amended ROWS.md and this TASK; withhold EXPECTED.md, mutants, the settlement, implementation, review, triage, previous answers and product output. Freeze both fresh answer sets before comparison and before implementation. Compare every selected execution and requested observable; an unchosen alternative counts as a disagreement, not agreement.
