INFERRED qualification protocol for evidence under:
tests/results/session-20261119/a2-loss50/

This protocol defines what results are required. It does not declare that those results exist or pass.

1. Freeze the population and evidence before fitting.

Use four cells: Ubuntu/PG13, Ubuntu/PG18, EL8/PG13 and EL8/PG18, identified by exact binary/tree, kernel, hardware, driver and capture configuration.

Reconstruct every observed birth and every never-attached candidate, including candidates previously labelled not lost. Establish merge admission from retained lifecycle, disposition, attachment-operation and counter evidence, not from an outcome label alone.

Report separately:
- admitted omissions;
- candidates rejected by the admission conditions;
- candidates with insufficient evidence;
- successful-enable/clear-EVER_LIVE races;
- attached and late-attached lifetimes;
- actual transport or publication failures.

Do not discard failed runs, startup omissions, slow receive delays or inconvenient child types. A rejected or unresolved candidate cannot be silently removed to obtain a smaller maximum. Insufficient admission evidence leaves the affected qualification unknown.

Obtain boot_start from matching capture metadata. Never substitute the first birth, first receive or display edge. If boot_start or source completeness cannot be established, actual boot-anchored verdicts are unqualified even if sliding-window counts can be computed.

2. Identify the 1 Hz source before treating its distribution as qualification evidence.

Record birth, omission, daemon-loop/timer and driver-action phases on the same clock, with the scripts and event definitions needed to reproduce the comparison.

Use a predeclared matched perturbation that changes driver cadence or phase independently of daemon cadence, and a corresponding daemon-timer perturbation where applicable. Identify whether the loss phase follows the driver, the daemon or another measured source. A phase plot alone does not establish causation.

If the 1 Hz pattern is caused by the driver, its per-second distribution is a rig artefact and qualifies no K or L for production behavior. Preserve those results as diagnostics; qualification needs replacement evidence from a representative workload independent of that artefact.
If the source remains unidentified, mark distribution qualification unqualified.
Do not classify the two old capture-start delays as steady-state stalls merely to preserve an R threshold.

3. Keep fitting and validation separate.

For arm A, fit using original runs 1–25 in each cell; validate using original runs 26–50.
For the existing ten-run parallel-query arm B, fit using runs 1–5 and validate using runs 6–10 in each cell. Do not describe this arm as a fifty-run parallel-query measurement.
Retain original run numbering, including failures. No retries or replacements may enter the split silently.

Use the ordinary and parallel-query fitting populations together when choosing common H, K and L. Report each cell and arm separately as well.

Freeze the selected parameters before inspecting validation outcomes. If those outcomes were already inspected, label this a retrospective split and require fresh held-out runs before claiming prospective validation.

4. Fit K across every anchor phase, with a fixed ceiling.

The only candidate widths are h=1 second and h=5 seconds, considered in that order.
The predeclared ceiling is K_h <= 4.

For each candidate h, define:
A_h = maximum number of admitted omitted births in any half-open interval
      [a,a+h), over every anchor a and every ordinary/parallel-query fitting run.

Compute this sliding-window maximum exactly from birth timestamps, including simultaneous births and startup births. Births exactly h apart do not occupy the same half-open h-window. Do not combine separate captures into one fitting interval.

Set K_h=max(2,A_h).
If K_h>4, reject that candidate width; never raise the ceiling to fit the data.

For each independent one-second churn-stall control and each cohort anchor phase φ in [0,h), form the h-wide partition at that phase over the fully observed control interval.
Let its value be the largest omitted-birth count in any partition cohort.
Define C_h as the minimum of those values over all tested controls and all anchor phases.
Require K_h<C_h. This ensures that even the least favorable partition phase of each measured control contains a count violation.
A control with missing admission evidence, incomplete observation or no qualifying omissions cannot establish this separation.

5. Fit L without receive-delay selection.

For each candidate h surviving the K ceiling, let l_max be the greatest complete omitted-lifetime duration in the ordinary and parallel-query fitting populations.
Include capture-start omissions and every receive delay. Do not condition this population on receive latency, a preselected L, or whether a window would pass.

Define ceil_ms(x) as rounding x upward to the next whole millisecond.
Set:
L_h = min(h, ceil_ms(2*l_max)).

Require a nonempty lifetime-calibration population and l_max<=L_h.
If the population is empty, evidence is incomplete, or l_max>L_h, that candidate's L is unqualified.

Select the smallest h satisfying the K ceiling, the lifetime condition and the independent churn-control separation.
Freeze H_A2=h, K_A2=K_h and L_A2=L_h.
If neither h qualifies, report qualification failure; do not introduce a percentage denominator, a larger K ceiling or an R threshold.

The old 138.028412 ms lifetime would contribute to this maximum rather than being excluded by delay. If it were the maximum of an otherwise eligible fitting population with h=1 second, the formula would yield L_h=277 ms. This arithmetic is not qualification from the new fifty-run data.

6. Freeze the measured birth-count envelope.

For the selected H, compute each cell's N_qual as its largest completely accounted boot-anchored N_k in the fitting population, recording the arm and run supplying it.
Use that frozen cell-specific value during validation. Do not enlarge it after seeing a held-out over-envelope run.

A held-out cohort with N_k>N_qual is untraced_unqualified, even if U_k<=K.
Report the actual largest N_k observed across all runs as a separate descriptive statistic; it does not retroactively expand the frozen envelope.

Do not extrapolate qualification to a different platform, process mix or workload rate. All applicable cells must satisfy the evidence and source-identification requirements for a common supported claim.

7. Validate the frozen policy and report its availability.

On held-out ordinary and parallel-query runs, report:
- every count exceedance, lifetime exceedance and qualification-envelope exceedance;
- actual boot-anchored cohort verdicts;
- sliding h-window exceedances as a separate phase-sensitivity result;
- durations and receive delays of all omissions, without delay-based exclusions;
- every resulting whole-window and chart-bucket verdict;
- complete omission counts and backend-time bounds;
- all base failures, pending cases and missing evidence.

Held-out exceedances are reported as actual Unavailable results; they never cause parameter retuning or sample removal.
A qualified bounded-omission policy is not a claim that every ordinary run remains numerical. State the measured availability explicitly, including zero exceedances with their finite observation time.

Evaluate overlapping selections and concatenations of passing cohorts, together with selections containing a single failing cohort or a lifetime crossing a cohort boundary.
Do not use a capture-wide omission percentage as the expected answer.

Check K versus K+1 omissions, L versus L+1 ns, half-open edges, empty lifetimes, startup reception after exit, and N_qual versus N_qual+1.
Require matching-window equality across every enabled consumer route.

8. Report the projected share of red one-hour windows.

Define red as Unavailable and report separate contributions from:
- count or duration limits;
- qualification-envelope failures;
- missing or contradictory A2 evidence;
- base coverage failures.

For each cell and held-out arm, publish the measured cohort failure frequency p, the number of observed cohorts and runs, and the formula:
P_iid(red hour) = 1-(1-p)^(3600 seconds/H_A2).

Label this projection INFERRED and conditional on independent, identically distributed cohort outcomes. The observed phase locking does not establish that assumption.

Also produce an INFERRED projection using whole-run or contiguous-run-block resampling that preserves within-run dependence and 1 Hz phase. Describe block length, resampling method, startup handling and uncertainty.
A projected continuous hour has one capture startup, not a new startup every ten seconds. If the evidence cannot separate startup from subsequent behavior, mark that continuous-hour projection unqualified and report the repeated-short-capture projection separately.

Do not equate zero observed failures with zero one-hour risk. Publish uncertainty and the finite exposure supporting the estimate.
No one-hour availability target is supplied by this protocol; never claim one was met without a separately stated target.

9. Keep the two stall controls distinct.

For churn-stall controls, demonstrate an untraced_limit result from the frozen A2 evidence independently of any base VOID. A transport VOID alone does not show that K or L detected omissions.

C13 has a design answer but remains empirically unqualified until a separate no-birth control is measured.
That control must retain observed birth accounting, show no qualifying omissions during a one-second daemon pause, and distinguish the A2 result from any base coverage failure.
If base coverage passes, A2 must retain numbers with no omission note.
If the available measurement arms omit this control, report it as missing; do not infer it from the churn control.

10. Qualification report.

Publish the frozen parameter manifest, fitting/validation membership, per-cell N_qual, source-identification evidence, raw-data paths, every failing interval, control results and one-hour projections.
Label directly recomputed measurements VERIFIED and causal interpretations or extrapolations INFERRED.
Missing evidence leaves the corresponding qualification unqualified.

The result may establish bounded omission handling within a measured envelope. It cannot establish general stall detection, absence of future omissions or guaranteed numerical availability for arbitrary hour-long selections.
