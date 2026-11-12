# Answers (Sonnet), from CONTRACT.md only

Assumptions: sources are P0, P1, E, L (rows name only these). `S(w)` gives every source boundary w. "DATA x" lists in release order. VOID<d> = VOID(d,C,reason), d is detection time (not given by rows). A FRONTIER is emitted whenever F advances, even if the released batch is empty (see Ambiguities, note A).

## Row 1
Output: DATA A10, DATA B12, FRONTIER 12
Exit: no
Reasoning: Same in N and Q. W=13, F=12. Eligible t<13: A10, B12. Sort (t,pid): A10 then B12 (arrival order irrelevant).

## Row 2
Output: DATA A10, DATA B10, FRONTIER 10
Exit: no
Reasoning: Same in N and Q. W=11, F=10. Both t=10; tie broken by pid (10<20). Different pids, so no same-generation tie ambiguity.

## Row 3
Output: FRONTIER 9, DATA A10, FRONTIER 10
Exit: no
Reasoning: Same in N and Q. S(10): W=10, F=9, no DATA with t<10, frontier advances from none to 9. S(11): W=11, F=10, release A10.

## Row 4
Output: AMBIGUOUS. Reading 1: hold (P0 has no BOUNDARY, so W=0, no F). Reading 2: DATA A10, FRONTIER 11 (P0 w=12 from consumed A12, W=min(12,20)=12, F=11; A12 stays held).
Exit: no
Reasoning: Same in N and Q. Depends on whether a consumed P0 record at t implicitly supplies strict w=t (needs the "qualified" producer, which the row does not state).

## Row 5
Output: hold
Exit: no
Reasoning: Same in N and Q. P0 w=100 but P1/E/L are 0, so W=min=0. Nothing released; at W=0 no frontier exists.

## Row 6
Output: DATA A10, FRONTIER 99
Exit: no
Reasoning: Same in N and Q. All sources now 100, W=100, F=99. A10 has t<100, released.

## Row 7
Output: DATA A11, DATA B12, FRONTIER 12
Exit: no
Reasoning: Same in N and Q. E arrival order B12, A11 is a shared-ring inversion between unrelated producers: sortable, not a clock fault. W=13, F=12; sorted by (t,pid).

## Row 8
Output: TEXT x, DATA A10, FRONTIER 10
Exit: no
Reasoning: Same in N and Q. W=11, F=10. Pending TEXT is output first, then eligible DATA; the TEXT dependency is available so no hold. TEXT carries no timestamp.

## Row 9
Output: DATA FORK, DATA INIT, DATA TRANS(terminal), DATA EXIT, FRONTIER 10
Exit: no
Reasoning: Same in N and Q. t=10 assumed (S(11) implies it). Ranks FORK0, INIT1, TRANS2, EXIT3 override arrival order; terminal TRANS precedes its EXIT. W=11, F=10. Only one TRANS, so no sequence-free tie refusal.

## Row 10
Output: DATA A-enter-X@10, DATA A-leave-X@10, FRONTIER 10
Exit: no
Reasoning: Same in N and Q. Two distinct physical records (zero-duration transition kept). Same source: N keeps source order, Q keeps q order. W=11, F=10.

## Row 11
Output: N: VOID<d> (ambiguous_tie), no DATA. Q: DATA MARKER10@E(q=3), DATA TRANS10@P0(q=4), FRONTIER 10
Exit: no (both)
Reasoning: Same pid/t/generation, both rank 2, different sources. N (q=0): cross-source TRANS/MARKER tie gives counted ambiguous_tie VOID, not exit. Q: causal q order 3 then 4. W=11, F=10.

## Row 12
Output: N: VOID<d> (ambiguous_tie), no DATA. Q: DATA A10@P1(q=1), DATA A10@P0(q=2), FRONTIER 10
Exit: no (both)
Reasoning: Transition/transition migration tie across P0 and P1. N: ambiguous_tie VOID (contract names this case). Q: q order overrides arrival. W=11, F=10.

## Row 13
Output: DATA A4, FRONTIER 5, VOID9
Exit: no
Reasoning: Same in N and Q. S(6): W=6, F=5, A4 released. Loss detected at d=9: VOID(9,C,reason) immediately, tentative suffix discarded, all attachments (A and B) frozen at last valid frontier 5.

## Row 14
Output: AMBIGUOUS. Reading 1: no further output (same missing event, epoch already void and suspended). Reading 2: a second VOID12 is output but counted once.
Exit: no
Reasoning: Same in N and Q. Contract says a positive LOST independently causes VOID and must not be summed with helper-failure counts, but does not say whether a second VOID is emitted inside an already-void epoch.

## Row 15
Output: VOID<d>
Exit: no
Reasoning: Same in N and Q. F5 stays the last frontier. Counter change causes VOID; a final helper failure with permanent silence must be detected without a LOST callback. No new frontier. d (detection time) not given.

## Row 16
Output: AMBIGUOUS. Reading 1: hold (W=0 because other sources are 0). Reading 2: VOID<d> (A11 arrives below the strict boundary w=12 set by consumed A12; data below accepted boundary / clock regression, no exit).
Exit: no
Reasoning: Same in N and Q. Turns on whether a consumed P0 record implicitly supplies strict w=t.

## Row 17
Output: AMBIGUOUS. Reading 1: FRONTIER 10, VOID<d>, epoch marked invalid. Reading 2: FRONTIER 10, epoch marked invalid only (no ordinary VOID).
Exit: no
Reasoning: Same in N and Q. S(11): W=11, F=10 published. Late P1 data at 9 < 11 is below an accepted strict boundary (VOID, no exit) and disproves the published F=10, so the epoch is marked invalid. The contract says ordinary VOID cannot repair the certificate but does not say if VOID is still output.

## Row 18
Output: FRONTIER 10, DATA P1@11, FRONTIER 11
Exit: no
Reasoning: Same in N and Q. S(11): W=11, F=10. P1 DATA at t=11 is not below boundary 11 (strict), so it is valid and held. S(12): W=12, F=11, releases it.

## Row 19
Output: AMBIGUOUS. Reading 1: VOID<d> only (DATA A7 not released because publication failed, no FRONTIER). Reading 2: DATA A7 emitted to the callback, then VOID<d>, no FRONTIER.
Exit: no
Reasoning: Same in N and Q. Writer failure causes VOID/paused capture; no frontier is published (F5 stays). The contract does not say whether DATA rejected by the writer counts as a released output.

## Row 20
Output: none (no release)
Exit: yes
Reasoning: Same in N and Q. Duplicate delivery of the same source position is an internal invariant breach, which is the exit-nonzero case.

## Ambiguities

- A (rows 3, 17, 18, minor, answered under reading 1): the contract never says whether FRONTIER is output when the eligible batch is empty. Reading 1: yes, whenever F advances (row 3: FRONTIER 9 after S(10)); reading 2: only alongside a released DATA. I used reading 1 (F "exists" independent of data, per the Row 15 setup and "no F exists" initially).
- Row 4 and row 16: does a consumed P0 record at t supply strict boundary w=t implicitly (the "qualified user-mode watchpoint-only producer" is not stated for the examples)? Reading 1: only explicit BOUNDARY inputs count, so W=0 and hold. Reading 2: implicit w=t, giving DATA A10 and FRONTIER 11 in row 4 and VOID in row 16.
- Row 14: second VOID emitted inside an already void epoch (counted once) vs no output.
- Row 17: whether VOID is output in addition to marking the epoch invalid.
- Row 19: whether DATA rejected by the writer appears among released outputs.
- All VOID rows (11, 12, 13, 15, 17, 19): d and C are not given by the rows, and the VOID reason string for rows 17/19 is not specified. Also the number of CPUs (P0..Pn) is not stated; I assumed P0, P1.
