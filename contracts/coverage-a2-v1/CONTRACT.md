# A2 bounded omissions, revision 3

Status: INFERRED normative design. This text does not assert implementation or empirical qualification.

## Scope and policy

This contract adds bounded omission handling to the base coverage behavior represented by tracked tests/fixtures/coverage/ and src/coverage.c. Existing hard coverage failures retain precedence; A2 cannot make an unavailable metric available.

A qualifying omission is a lifetime admitted by merge-v1's complete A2 proof. Its identity is (writer_boot,pid,g), birth is fork_ns, exit is exit_ns, and interval is [fork_ns,exit_ns). Count every admitted never-attached lifetime, irrespective of an older diagnostic's "lost" classification.

Reader policy revision 3 uses:
- H_A2: fixed cohort width in ns;
- K_A2: absolute omitted-birth count cap per cohort;
- L_A2: omitted-lifetime duration cap in ns;
- N_qual: largest measured cohort birth count in the applicable qualification cell.

The provisional values are H_A2=1,000,000,000 ns, K_A2=2 and L_A2=1,000,000,000 ns. They are design-test defaults, not qualified shipping parameters. Qualification supplies the frozen values and N_qual for each applicable platform/workload cell.

K_A2 is qualified only up to the largest N_k measured; above it the limit is unqualified. Do not scale K with birth count, extrapolate a qualification cell or interpret K as a percentage allowance.
Return policy revision, H_A2, K_A2, L_A2, N_qual and the applicable qualification identity with the core result.
There is no R parameter or receive-delay threshold in the verdict. Return receive_ns-fork_ns only as a labelled diagnostic.

## Population, cohorts and finality

boot_start is the recorded start of this writer boot in the event clock.
For integer k >= 0:
J_k = [boot_start+k*H_A2, boot_start+(k+1)*H_A2).

Selections do not re-anchor, resize or pool these cohorts. Every observed birth belongs to exactly one cohort according to fork_ns, including births received only after exit or after the daemon's first ring drain.

N_k counts distinct observed births in J_k.
U_k counts distinct qualifying omitted lifetimes born in J_k, including zero-duration lifetimes.
Pre-existing tasks discovered at capture start are not observed births. They require the ordinary attachment and base-coverage rules; discovery does not create an A2 omission.
An A2 declaration with fork_ns < boot_start is invalid. Do not clamp its birth, assign it to J_0 or invent a negative-index cohort.

A cohort finalizes only when qualified source boundaries establish complete birth accounting through J_k.end and every birth has either proved attachment or a reconciled terminal disposition.
A known birth with neither attachment proof nor terminal disposition remains unfinished. A frontier over attached backends alone does not finish it.
A final certificate must satisfy 0 <= U_k <= N_k. Its U_k must agree with the retained omitted-identity ledger; when complete retained FORK evidence exists, its N_k must agree with that evidence.
Missing evidence is never a zero.

## Limits and their affected windows

A finalized cohort fails the count limit exactly when U_k > K_A2.
An omitted lifetime fails the duration limit exactly when exit_ns-fork_ns > L_A2.
Equality passes. Use exact, checked integer arithmetic.

A cohort is limit-failing if its count limit fails or any omitted lifetime born in it fails the duration limit.
Thus a long omitted lifetime makes its whole birth cohort fail, even for a selection that does not intersect that lifetime.

Two half-open intervals overlap only when:
max(left_start,right_start) < min(left_end,right_end).
A zero-duration lifetime has no overlap with any selection. It still contributes one to its birth cohort's U_k.

For a nonempty selection W=[W.start,W.end), the required cohort set consists of:
1. every cohort whose interval overlaps W; and
2. the birth cohort of every omitted lifetime whose interval overlaps W.

Evaluate each required cohort and every omitted lifetime born in it, including pairs outside W needed to establish that cohort's duration-limit status.
A limit-failing required cohort makes W's whole-window numerical answers Unavailable.
This rule carries a failed cohort along the full lifetimes of its omissions, including into later cohorts. It does not recursively add unrelated cohorts.
An empty omitted lifetime propagates no failure beyond its own birth cohort.

Any number of passing cohorts may be combined. Never turn accumulated passing omissions into a long-window failure, and never dilute a failing cohort with passing cohorts.

For a required cohort with N_k > N_qual, or without an applicable qualified parameter set, return Unavailable with untraced_unqualified. This is a result for the requested window, not a refusal to start capture.

## Status, reasons and omission notes

Preserve the base coverage result and reason precedence first. If base coverage permits numbers, A2 reasons have this precedence:
1. untraced_invalid: contradictory counts, identities, timestamps, certificates or validly decoded fields;
2. untraced_incomplete: missing required retained pairs, certificates or other finality evidence;
3. untraced_pending: explicitly recorded unfinished reconciliation;
4. untraced_unqualified: no applicable qualification or a required N_k exceeds N_qual;
5. untraced_limit: a required count or duration cap is exceeded.

Absence of a certificate is pending only when an explicit live ledger records the corresponding unfinished reconciliation; otherwise it is incomplete.
A damaged or undecodable payload retains the base coverage-hole behavior rather than being interpreted as a valid A2 field error.

If base coverage and all required A2 checks pass, retain measured numerical values without imputation, renormalization or changes to attribution.

Omission-note computation is independent of whether numbers are withheld.
When the required omission inventory and its identities and timestamps are complete and consistent, compute:
affected_lifetimes =
  count of distinct omitted identities with nonempty overlap with W;
untraced_backend_ns_upper =
  SUM over those identities of
  max(0,min(exit_ns,W.end)-max(fork_ns,W.start)).

Overlapping lifetimes add; this is backend-time, not elapsed wall time.
Return the note for a positive affected_lifetimes even when status is Unavailable because of untraced_limit, untraced_unqualified or a separate base failure.
The bound covers the declared omitted lifetimes only; it does not bound other missing evidence responsible for a base failure.
If the required omission inventory is invalid, incomplete or pending, withhold these two note values; do not label a known subset as the complete upper bound.

When affected_lifetimes=0, omit the omission note. An Unavailable explanation may still name a failing birth cohort.
UI text for a complete positive note is:
“N backends not traced; at most X backend-time omitted.”

This is not a relative DB Time, wait-attribution, execution-count or latency-distribution error bound.
Label cohort diagnostics separately. SUM(U_k) is never the displayed count of lifetimes overlapping W.

One core result supplies status, reason, note and numerical withholding to every enabled raw, summary, server, CLI and UI route. Consumers cannot recompute these quantities.
Whole-window totals and chart buckets use this same rule for their respective windows. An unaffected bucket may retain numbers while a larger total is Unavailable. Identical windows and evidence must produce identical results on every enabled route.

Late-attached lifetimes retain merge-v1's pre-attachment restrictions; they are not A2 omissions.
A daemon pause with no births creates no A2 omission. Base coverage still applies. A2 is not a general stall detector.

## Carrier and retention

COVERAGE GAP_OPEN, reason 5 UNTRACED_LIFETIME:
a=fork_ns, b=pid, c=gap_id, d=detection_ns.

Its matching COVERAGE GAP_CLOSE, reason 0:
a=exit_ns, b=g, c=gap_id, d=receive_ns.

gap_id is unique within the writer boot across all gap types.
The pair binds (writer_boot,pid,g). Only a complete consistent pair receives A2 semantics.
detection_ns is the userspace time at which the completed omission was declared; it is not an interval endpoint.
Preserve the original FORK dequeue timestamp as receive_ns. Require fork_ns <= exit_ns and fork_ns <= receive_ns; exit before receive is permitted.
Do not expand an A2 interval to watermark floors.

COVERAGE kind 7 A2_BUCKET, reason 0:
a=J_k.start, b=J_k.end, c=N_k, d=U_k.

Emit every finalized cohort, including N_k=U_k=0, after its omission declarations are durable.
The writer's reconciled lifecycle ledger is the sole count authority. A2_BUCKET certifies finality, not a sample of currently known births.
There is no A2_POLICY record. Existing WATERMARK or SEAL records alone cannot replace an A2_BUCKET certificate.

The payload remains schema 1, 48 bytes, in trace v6.
Finalized certificates are immutable. Later contradictions invalidate cached answers.
Rotation and retention must preserve all pairs and certificates needed to evaluate the required cohorts, including omitted lifetimes outside W whose duration affects a required cohort. Otherwise return Unavailable.
Failure to persist either pair member or a certificate follows merge-v1's writer-failure rules.

## Clarifications after the oracle round (lead, 2026-11-19)
Two independent answer sets agreed on 73 of 77 variants. These sentences decide the remaining cases; none changes an agreed answer.
- The required omission inventory consists of the retained pairs and the final A2_BUCKET certificate of every required cohort. It is complete and consistent only when each such certificate exists, is valid and agrees with its pairs. Otherwise no omission note is returned. (C4b, C14c, C24)
- A certificate/ledger mismatch caused only by a pair member missing from retention is untraced_incomplete, not untraced_invalid. (C14b)
- In the UI text, X is untraced_backend_ns_upper rendered as a duration with its unit (for example "18 ms"); the remaining words are literal. (C18e)
- Return receive_ns-fork_ns as a diagnostic for each omitted lifetime in a required cohort.
