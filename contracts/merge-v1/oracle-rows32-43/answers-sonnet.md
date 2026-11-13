Notation: K = FORK_D10{fork_seed_proved=true}. No row requires a feed-call status; there is no REARM. No row exits the daemon or invalidates an epoch. After a VOID the epoch is suspended, so later inputs produce no output.

## Row 32
Output N: `K → D12[U] → F12`
Output Q: `K → D12[U] → F12` (q1 is the first value, matching next_q=1)
Exit: no
Reasoning: The qualified fork-seed FORK is valid creation proof (25b). It precedes D's first TRANS in output order, and D has no prior history.
Reasoning: W stays 0 until the last boundary of S(13). Then W=13, so FORK@10 and D12 are eligible and F=12.
Reasoning: D12 is the first released TRANS of a 25b generation, so it is [U]. The FORK keeps its proof bit. Q initialises next_q=1, so q1 is consecutive.

## Row 33
Output N: `VOID20[C0,unknown_evidence]`
Output Q: `VOID20[C0,unknown_evidence]`
Exit: no
Reasoning: The FORK has proof bit false and there is no INIT, so neither creation route (FORK+INIT or a qualified fork-seed FORK) is satisfied.
Reasoning: Validation of the eligible batch runs when the last boundary of S(13) makes W=13. D12 has no creation proof, so the result is unknown_evidence, which gives VOID in both modes.
Reasoning: Nothing was released before the VOID, so there is no output and no frontier.

## Row 34
Output N: `VOID20[C0,unknown_evidence]`
Output Q: `VOID20[C0,unknown_evidence]`
Exit: no
Reasoning: D is (pid=10,g=2), but A=(10,1) is a previously known generation of pid 10 and has no EXIT.
Reasoning: The contract requires every previously known generation of the PID to have its EXIT ordered before the FORK. That is violated, so the direct attachment is unproved.
Reasoning: The failure is found at eligible-batch validation at W=13. Result: unknown_evidence VOID with d=20, and nothing is released.

## Row 35
Output N: `VOID20[C0,unknown_evidence]`
Output Q: `VOID20[C0,unknown_evidence]`
Exit: no
Reasoning: D12 has no FORK and no INIT, so no creation proof precedes its first TRANS. The first arrival never establishes the sequence baseline.
Reasoning: This is missing identity, generation or sequence initialization, i.e. unknown_evidence in both modes (see row 29's precedent). The VOID comes at validation when W=13.

## Row 36
Output N: `K → INIT_D11 → D12[U] → F12`
Output Q: `K → INIT_D11 → D12[U] → F12`
Exit: no
Reasoning: A qualified fork-seed FORK selects 25b even when a genuine INIT exists. The INIT does not reinitialise sequence state and does not remove 25c's restriction.
Reasoning: The INIT at t=11 is released normally in key order (t=10, 11, 12). INIT does not consume q, so D12 with q1 is consecutive from next_q=1.
Reasoning: D12 is still the first TRANS of the 25b generation, so it is [U]. F=12 at W=13.

## Row 37
Output N: `K → D12[U] → F12`
Output Q: `VOID20[C0,sequence_gap]`
Exit: no
Reasoning: N omits q, so the row is identical to row 32.
Reasoning: In Q, the fork-seed FORK initialises next_q=1. D's first record in output order has q2, so q1 is missing, which is a sequence_gap at batch validation.
Reasoning: In Q nothing is released, because validation is of the whole eligible batch.

## Row 38
Output N: `VOID20[C0,unknown_evidence]`
Output Q: `VOID20[C0,unknown_evidence]`
Exit: no
Reasoning: This is the row 29 pattern. S(10) gives W=10, so only D9 (t<10) is eligible. The FORK at t=10 is not eligible, so no creation proof precedes D's first TRANS.
Reasoning: D9 has an unknown generation start, so the result is unknown_evidence VOID at the last boundary of S(10). The later S(13) is suppressed because the epoch is suspended.

## Row 39
Output N: `K → D12[U] → F12`
Output Q: `K → D12[U] → F12`
Exit: no
Reasoning: Arrival order does not matter. Release sorts by (t,...), so FORK@10 precedes D12 in validated output order, and the creation proof precedes the first TRANS.
Reasoning: The first arrival does not set the baseline, since K initialises next_q=1. Otherwise this is the same as row 32.

## Row 40
Output N: `K → D12[U] → F12 → INIT_D14 → D15[T] → F15`
Output Q: `K → D12[U] → F12 → INIT_D14 → D15[T] → F15`
Exit: no
Reasoning: The first part is row 32. The genuine INIT at t=14 for an already directly attached generation is handled idempotently (no reseed, no coverage break), and is released normally.
Reasoning: D15 is the second TRANS, so it is [T]. In Q, the q2 after the released q1 is consecutive because sequence state advanced on the successful release.
Reasoning: S(16) gives W=16, so INIT14 and D15 are eligible and F=15, which advances past F12. D15 and INIT_D14 are at or above the L and P0 boundaries of 13, so there is no seal_violation or clock_regression.

## Row 41
Output N: `K → D12[U] → F12 → VOID20[C0,unknown_evidence]`
Output Q: `K → D12[U] → F12 → VOID20[C0,unknown_evidence]`
Exit: no
Reasoning: The first three inputs release as in row 32. The FAULT then immediately emits VOID with its given d=20 and C0, and the epoch is suspended.
Reasoning: The final S(16) is suppressed because the suspended epoch emits no trusted DATA/TEXT or FRONTIER. The FAULT is not late DATA, so there is no invalidation and F12 stands.

## Row 42
Output N: `K → MARKER_D12[U] → D14[T] → F14`
Output Q: `K → MARKER_D12[U] → D14[T] → F14`
Exit: no
Reasoning: The marker is a valid first TRANS/MARKER and, per the row, satisfies 25c, so there is no unknown_evidence. The FORK, which precedes it, is the creation proof.
Reasoning: The marker is the first released TRANS/MARKER, so it is [U]. D14 is the next one, so it is [T].
Reasoning: In Q, the q1 on the marker and q2 on D14 are consecutive in output order across sources E and P0. S(15) gives W=15, so F=14.

## Row 43
Output N: `VOID20[Ci,counter_change]`
Output Q: `VOID20[Ci,counter_change]`
Exit: no
Reasoning: The first input is CHECK(20,Ci). The vector differs from the baseline and the last observation C0 (lifecycle_init_drop=1), so it reports counter_change VOID with the observed Ci. The epoch is suspended.
Reasoning: The remaining inputs (K, D12, S(13)) observe the same Ci, so there is no new report. Suspension suppresses DATA/FRONTIER, and the baseline would never match Ci anyway.

## Ambiguities
- Row 40 (mild): the contract does not say whether an INIT with t=14, ordered after the same generation's t=12 TRANS, creates any validation problem (lifecycle order is by t first). I read INIT as released normally and idempotently, as §3 says. The other reading is a coverage-break FAULT. I did not mark the row AMBIGUOUS because §3 says handling for an already directly attached generation must be idempotent.
- Rows 33, 34, 35, 38 (timing only): the VOID could be read as raised at arrival of the offending record or at eligible-batch validation. Both give the same output, because the d is fixed at 20 and nothing was released earlier.
- Row 36: INIT_D11 is released as its own output record. The contract does not say whether INIT carries any prior_interval marking. I assumed it carries none, since marking is only on TRANS/MARKER.
