## Row 1
Output: `DATA A10@P0 → DATA B12@P1 → FRONTIER F12`  
Exit: no  
Reasoning: Sort by timestamp. W=min(13,…,13)=13; F=13−1=12.

## Row 2
Output: `DATA A10@P1 → DATA B10@P0 → FRONTIER F10`  
Exit: no  
Reasoning: At equal timestamps, PID 10 precedes PID 20. W=11; F=11−1=10.

## Row 3
Output: `FRONTIER F9 → DATA A10@P0 → FRONTIER F10`  
Exit: no  
Reasoning: At S(10), W=10 and F=10−1=9; t=10 is ineligible. At S(11), W=11 and F=11−1=10, releasing A10.

## Row 4
Output: **AMBIGUOUS**: `DATA A10@P0 → FRONTIER F11`, possibly preceded by `FRONTIER F9`.  
Exit: no  
Reasoning: A12 supplies P0’s strict boundary 12. With other cutoffs at 20, W=min(12,20,…)=12; F=12−1=11. If those cutoffs already equal 20 when A10 arrives, an earlier W=10 gives F=10−1=9.

## Row 5
Output: `hold`  
Exit: no  
Reasoning: W=min(100,0,0,0)=0. No frontier exists at W=0; A10 remains pending.

## Row 6
Output: `DATA A10@P0 → FRONTIER F99`  
Exit: no  
Reasoning: After the remaining boundaries arrive, W=min(100,…,100)=100; F=100−1=99.

## Row 7
Output: `DATA A11@E → DATA B12@E → FRONTIER F12`  
Exit: no  
Reasoning: The shared-ring inversion is sortable and does not itself establish a clock fault. W=13; F=13−1=12.

## Row 8
Output: `TEXT x → DATA A10@P0 → FRONTIER F10`  
Exit: no  
Reasoning: Release pending TEXT before eligible DATA; x satisfies A10’s dependency. W=11; F=11−1=10. TEXT receives no timestamp.

## Row 9
Output: **AMBIGUOUS**. If all four records have t=10 and the same generation: `DATA FORK → DATA INIT → DATA terminal TRANS → DATA EXIT → FRONTIER F10`.  
Exit: no  
Reasoning: W=11; F=11−1=10. The row supplies neither the actual timestamp nor equality of generations. At t=11, the output would instead be only `FRONTIER F10`, with all DATA held.

## Row 10
Output: N/Q: `DATA enter X@10 → DATA leave X@10 → FRONTIER F10`  
Exit: no  
Reasoning: N preserves same-source order; valid Q preserves causal sequence order. Both physical records survive, including the zero-duration transition. W=11; F=11−1=10.

## Row 11
Output: N: `VOID<d>`; **AMBIGUOUS detection time**. Q: **AMBIGUOUS** between `DATA MARKER10(q=3) → DATA TRANS10(q=4) → FRONTIER F10` and `VOID<d>`.  
Exit: no  
Reasoning: N refuses the same-generation cross-source tie. Q orders 3 before 4 if sequence history establishes no gap; otherwise a gap causes VOID. Successful release has W=11; F=11−1=10.

## Row 12
Output: N: `VOID<d>`; **AMBIGUOUS detection time**. Q: **AMBIGUOUS** between `DATA first10@P1(q=1) → DATA second10@P0(q=2) → FRONTIER F10` and `VOID<d>`.  
Exit: no  
Reasoning: N also refuses transition/transition migration ties. Q reorders by sequence if its starting expectation permits 1; an expected missing predecessor causes VOID. Successful release has W=11; F=11−1=10.

## Row 13
Output: `DATA A enters X@4 → FRONTIER F5 → VOID<9>`  
Exit: no  
Reasoning: W=6; F=6−1=5. Loss at detection time 9 discards the tentative suffix and freezes every existing attachment at its last valid frontier. It supplies no new coverage endpoint.

## Row 14
Output: `VOID<12>` — additional output after row 13.  
Exit: no  
Reasoning: A positive LOST notification independently causes immediate VOID. It supplies no frontier and must not be added to helper-failure counts as another distinct missing event.

## Row 15
Output: **AMBIGUOUS detection time**; eventual `VOID<d>`.  
Exit: no  
Reasoning: Reliable counter accounting must detect the failure despite permanent transport silence. The existing F5 corresponds to W=6, F=6−1=5. No new frontier is permitted; the row gives no counter-read or detection time.

## Row 16
Output: **AMBIGUOUS detection time**; `VOID<d>`.  
Exit: no  
Reasoning: Consuming A12 establishes P0’s strict boundary 12; subsequent A11 violates it. Other cutoffs keep W=min(12,0,…)=0, so no frontier or DATA has been released. This fault suspends the epoch.

## Row 17
Output: `FRONTIER F10 → VOID<d>`; **AMBIGUOUS detection time**. The epoch must also be marked invalid and reported.  
Exit: no  
Reasoning: S(11) gives W=11; F=11−1=10. Late t=9 contradicts both P1’s accepted boundary and the published certificate. VOID cannot restore that certificate’s validity.

## Row 18
Output: `FRONTIER F10 → DATA P1@11 → FRONTIER F11`  
Exit: no  
Reasoning: Initially W=11; F=11−1=10. DATA at 11 is permitted but ineligible. S(12) gives W=12; F=12−1=11, releasing it.

## Row 19
Output: **AMBIGUOUS detection time**; `VOID<d>`.  
Exit: no  
Reasoning: Rejected publication does not release A7 and prevents a new frontier. Existing F5 corresponds to W=6, F=6−1=5; current eligibility requires W>7, but its exact value is unstated. VOID publication succeeds under the default success assumption.

## Row 20
Output: `hold` before handling the breach; subsequent control output is **AMBIGUOUS**.  
Exit: **AMBIGUOUS: yes/no**  
Reasoning: Duplicate transport identity is explicitly an internal invariant breach. The contract permits nonzero exit for this condition, but does not expressly require it or specify the output preceding termination.

## Ambiguities

- **Row 4 — timing of the other boundaries.** If other sources reach 20 after A12, the output is `DATA A10 → FRONTIER F11`. If they already equal 20 before A10, processing A10 can first emit `FRONTIER F9`.

- **Row 9 — missing timestamp.** With t=10, all four records are eligible at W=11. With t=11, none is eligible, although `FRONTIER F10` can advance. Independence of the rows prevents importing t=10 from another example.

- **Row 9 — missing generation relationship.** If all records share a generation, rank gives `FORK → INIT → terminal TRANS → EXIT`. If terminal TRANS/EXIT belong to an older generation and FORK/INIT to a newer generation, the generation key can give `terminal TRANS → EXIT → FORK → INIT`.

- **Rows 11 and 12, Q — sequence initialization/history.** One reading permits the first supplied sequence to establish the expected sequence, making the displayed contiguous pairs releasable. Another requires an established predecessor or initial value; an absent expected sequence causes VOID. The contract specifies neither initialization nor the prior sequence baseline for these rows.

- **Rows 11 (N), 12 (N), 15, 16, 17 and 19 — missing detection time.** The required refusal/failure output is determined, but its exact `d` is not. For example, detection at 20 and detection at 21 are both compatible with the supplied event and boundary times. Event timestamps and boundary cutoffs cannot substitute for detection time.

- **Row 20 — mandatory termination and preceding output.** One reading treats the named internal breach as requiring immediate nonzero exit, with no released output. The literal “exit nonzero only for” reading restricts when exit is allowed without requiring exit; handling could instead stop processing without terminating. No breach-output protocol chooses between these readings.
