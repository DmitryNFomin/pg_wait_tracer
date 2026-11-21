# Oracle4 comparison: answers-sol (A) vs answers-opus (B) vs EXPECTED, rows N1-N16, J1-J22

Counts: ALL-AGREE 35, ANSWERS-AGREE-EXPECTED-DIFFERS 2, ANSWERS-DIFFER 1, ANSWERS-AGREE-EXPECTED-INCOMPLETE 0, UNDETERMINED 0 (38 rows). Neither answerer wrote UNDETERMINED. Precedence used: where A and B contradict, the row is ANSWERS-DIFFER even if the cause is B's flagged assumption (J17).

Outcomes compared: released records and marks, FRONTIERs, VOID/fault time/label/reason, gap state and persisted end, b_i and route, x_i, P=L+Q, census known/unknown, attempts, abandoned/uncovered, contributions. All 38 rows agree on every outcome except the three listed.

| ID | class | difference |
|---|---|---|
| N1 | ALL-AGREE | none |
| N2 | ALL-AGREE | none |
| N3 | ALL-AGREE | none |
| N4 | ALL-AGREE | none |
| N5 | ALL-AGREE | none |
| N6 | ALL-AGREE | none |
| N7 | ALL-AGREE | none |
| N8 | ALL-AGREE | none |
| N9 | ANSWERS-AGREE-EXPECTED-DIFFERS | Both: `VOID20[Cf,counter_change]`, G open, P=1+0, and census=known (B flags this as an assumption: fixture census at fence 5 stays known after a later counter change). EXPECTED: `census=unknown`. Outputs otherwise identical (no bound, no frontier, attempts 0). EXPECTED is internally inconsistent with N15 (Cr, census=known). |
| N10 | ALL-AGREE | none |
| N11 | ALL-AGREE | none |
| N12 | ALL-AGREE | none |
| N13 | ALL-AGREE | none |
| N14 | ALL-AGREE | none |
| N15 | ALL-AGREE | none |
| N16 | ALL-AGREE | none |
| J1 | ALL-AGREE | none |
| J2 | ALL-AGREE | none |
| J3 | ALL-AGREE | none |
| J4 | ALL-AGREE | none |
| J5 | ALL-AGREE | none |
| J6 | ALL-AGREE | none |
| J7 | ALL-AGREE | none |
| J8 | ALL-AGREE | none |
| J9 | ALL-AGREE | none |
| J10 | ALL-AGREE | none |
| J11 | ALL-AGREE | none |
| J12 | ALL-AGREE | A alone adds: R fork-route numerical lower bound 6 under 25c, dN_0=1, dU_0=0. B and EXPECTED silent beyond 'Decision 25c's separate restriction remains'. No contradiction. |
| J13 | ALL-AGREE | none |
| J14 | ALL-AGREE | none |
| J15 | ALL-AGREE | A alone adds R's numerical lower bound 8 under 25c, dN_0=1, dU_0=0, U contributes nothing to N_k/U_k; B and EXPECTED silent; no contradiction. |
| J16 | ALL-AGREE | none |
| J17 | ANSWERS-DIFFER | A: `VOID20[C0,unknown_evidence]` ... 'J establishes inventory lifetime U before the contradictory matched disposition; b_U=none; x_U=none; gap=open; P=1=1+0; census=known'. B: `VOID20[C0,unknown_evidence]` ... 'P=0+1, census=unknown' (B assumes the timestamp contradiction invalidates the J resolution, Q stays 1; B notes that if J were applied it would be P=1+0 census known). EXPECTED sides with A (`P=1+0, census=known`, 'The journal establishes U'). The VOID itself agrees across A, B, EXPECTED. Also an UNDETERMINED-type assumption by B, counted here as ANSWERS-DIFFER. |
| J18 | ALL-AGREE | none |
| J19 | ANSWERS-AGREE-EXPECTED-DIFFERS | Both: `EXIT_U4 -> VOID20[Cf,counter_change]`, no discharge, P=1+0, census=known (A: 'P=1=1+0; census=known'; B: 'U pending: P=1+0, census=known'). EXPECTED: `P=1+0, census=unknown`. EXPECTED J18 (Cw) says known; the unknown here seems to rest on Cf being a lifecycle loss. |
| J20 | ALL-AGREE | none |
| J21 | ALL-AGREE | none |
| J22 | ALL-AGREE | Same as J12: A alone adds R lower bound 6, dN_0=1, dU_0=0; no contradiction. |

## Assumptions flagged by an answerer
Set A flagged none inline (its only unflagged reading is J17 'J establishes U before the contradictory disposition', which matches line 151 below). Set B's list:

1. N8: EXIT_D9 carries D's installed cookie (if cookie-less, TERM_D9 serves as the matched disposition; answer unchanged). Changes no output. Contract: DECISION 40 'If a discovery cookie exists, snapshot it before tombstoning ... Cookie-less EXIT is not automatically matching evidence' and DECISION 36 'Matching terminal lifecycle evidence retains Decision 40's discharge route without ADMIT'; contract silent on whether N8's EXIT carries the cookie.
2. N9 and N15: census stays known after a counter change observed after fence 5. Changes output for N9 (EXPECTED: unknown), not N15. Contract: DECISION 31 'Missing enumeration, lifecycle or completion evidence leaves census completion unknown'; silent on whether a post-fence counter change (Cf) counts as missing lifecycle evidence. Contract silent.
3. N12: only the AD carries the false claim, so one VOID not two. Changes no output (a VOID suspends). Contract: DECISION 36 'a claimed admission proof containing a false required fact reports FAULT(unknown_evidence,d,C)'; settled for the AD, silent on the CONFIRM witness.
4. J11: 'supplies unknown evidence' is reported as FAULT(unknown_evidence) -> VOID20[C0,unknown_evidence]. Changes no output (A, EXPECTED agree). Contract: DECISION 31 'Unknown conversion, zero or unreadable task start time is unknown evidence' and DECISION 40 'An unknown start time cannot pass either time comparison and supplies unknown evidence'; whether that lower-case phrase means FAULT(unknown_evidence) is not stated. Contract silent.
5. J17: the timestamp contradiction (journal 4 vs EXIT 5) invalidates the J resolution (Q stays 1, P=0+1, census unknown). CHANGES output (see J17). Contract: DECISION 40 'A J witness from Decision 31 establishes the journal-only inventory lifetime before its matched disposition is applied ... Match the particular EXIT transport record and journal witness' and 'A contradictory match supplies no discharge'. The first sentence supports A/EXPECTED (U established, P=1+0); the contract does not say whether a mismatched named EXIT also withdraws establishment.
6. J16-J19: a J resolution, once applied, moves the observation into L even when the later discharge fails. Changes no output for J16, J18, J19. Contract: same DECISION 40 sentence 'establishes the journal-only inventory lifetime before its matched disposition is applied' and 'x_i only when ... the complete release attempt commits'. Settled for J18/J19, depends on item 5 for J17.

## Contract or row text that needs a clarifying sentence
- DECISION 31 / 42 (census completeness) and rows N9, J19, J5: which counter changes after the census fence make census 'unknown' (Cf, Ck lifecycle losses -> unknown as EXPECTED has it; or Cw/Cr/Ce -> known as EXPECTED N14/N15/J18 have it; or none, census fixed at the fence). Readings: (a) lifecycle-class counters invalidate the census, others do not; (b) a fence-established census stays known and only the VOID is reported. EXPECTED needs one stated rule; as written N9/J19 (unknown) and N15/J18 (known) look inconsistent.
- DECISION 40 J-witness paragraph vs 'A contradictory match supplies no discharge', row J17: does a journal exit_ns that disagrees with the named EXIT's timestamp leave U established (P=1+0, census known, EXPECTED/A) or leave the observation unresolved (P=0+1, census unknown, B)?
- DECISION 31/40 'supplies unknown evidence' (row J11): is that sentence itself a FAULT(unknown_evidence) trigger or only a non-pass condition? EXPECTED, A and B all take the fault; the text never says so.
- Row N8: say whether EXIT_D9@L carries D's discovery cookie, or state that either form reaches the same discharge.
- Rows J12, J15, J22: EXPECTED, ROWS ask only for the restriction 'remains'; A also states R's lower bound (6 or 8) and dN_0=1. Either state the 25c numbers expected or say the rows do not ask for them.
- Row N12: say whether the CONFIRM witness (which describes the actual disabled watch) adds a second fault or only the AD claim faults (no output changes).

## Settlement

The design author settled the six clarification items; the changes below were applied to CONTRACT.md, ROWS.md and EXPECTED.md after the answer sets were written, so the answer files reflect the rows as first drafted.

- ITEM 1 — N9, N15, J5, J18, J19: The lifecycle-loss reading is right under DECISION 31: Cf compromises fork provenance; Cr/Cw alone do not compromise census completeness. Retain EXPECTED; the missing counter-class rule needs clarification.
- ITEM 2 — J17: EXPECTED and answer A are right: DECISION 40 establishes U before checking the matched disposition; the timestamp contradiction prevents discharge without withdrawing that establishment.
- ITEM 3 — J11: The fault reading is right under DECISIONs 31 and 40: an unknown start time prevents terminal classification and must explicitly report unknown evidence.
- ITEM 4 — N8: EXIT_D9 carries D's installed discovery cookie, snapshotted before tombstoning under DECISIONs 35 and 40; DECISION 36 permits terminal discharge without ADMIT.
- ITEM 5 — J12, J15, J22: Require the numerical bounds and population increments under DECISIONs 25c, 30 and 43; EXPECTED was incomplete. Make cohort-0 membership explicit so `dN_0=1` follows from the inputs.
- ITEM 6 — N12: One fault is right: DECISION 36 faults the false AD claim; under DECISION 37(c), a truthful failed CONFIRM candidate supplies no bound and no second fault.

N9 and J19: EXPECTED stands (census unknown after a lifecycle-loss counter change); both answer sets had assumed the census stays known because the contract did not name the counter class; DECISION 31 now does.
