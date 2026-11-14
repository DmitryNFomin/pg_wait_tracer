# Independent answers for REDELIVERY.md's 31 cases (2026-11-14)

Two answer authors worked in isolated directories:
- gpt-6.1-sol via `codex exec --sandbox read-only`;
- Claude Sonnet as an agent told to read only its directory.

Neither saw REDELIVERY.md's expected columns, Astra's answer, Fable's challenge or any code.

**Round 1.** Inputs: CODEC.md, REDELIVERY-blank.md (as REDELIVERY.md), CASES-blank.md and TASK.md.
- All three sets agree in records, stops and diagnostics in 28 of 31 cases.
- Cases 22, 29 and 30 differed, and only in replay effects or record-fault eligibility. Both authors marked those
  cells undetermined because the replay contract was not supplied:
  - case 22: the VOID's effect on certificates;
  - case 29: whether an unknown framed record kind stops;
  - case 30: the replay effect of an overlapping ATTACH_OK.
- Cell-by-cell table: COMPARE-round1.md.

**Round 2.** Fresh sessions, the same inputs plus contracts/evidence-v1/CONTRACT.md and REVISION-r3.md. Only cases 22
and 27-30, the cases with replay effects, were asked. Both authors now agree with the reference on every cell:
- **22:** a0…a4 and no stop. Replay revokes frontier 20 and keeps frontier 10 with coverage [0,10); high-water stays 20.
- **27 and 28:** a0…a4, and PU at the successor header. There is no seal proof: a digest mismatch in 27, and a
  non-boundary D in 28.
- **29:** a0 only, and an RF stop at (305,8) with witnesses {Au,Auc3}. The BC at 747 is a diagnostic.
- **30:** r0-r2, and BC at 769. There is a separate replay stop at (537,8) for the overlapping ATTACH_OK. LIFE is
  retained, with no coverage.

The authors add some detail the reference states more briefly, for example the lifecycle uncertainty at t=30 in
cases 22, 27 and 28. None of it contradicts the reference.

**Notational points the authors raised; no value depends on them:**
- the format of PU link-check details;
- the provenance set when a shorter copy's retained prefix is byte-identical (cases 6 and 15);
- the label of a replay stop;
- whether an unassigned header failure (cases 17 and 31) is a per-delivery diagnostic. Both authors and the
  reference treat it as one.

The implementation's oracle may fix these notations. It must not change a value.

**Costs:**
- sol: 41,267 tokens (round 1) plus 45,460 (round 2);
- Sonnet: 89,358 plus 88,357 tokens;
- comparison (Sonnet): 76,583 tokens.
