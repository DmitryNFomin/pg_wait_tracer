# Oracle comparison (sol vs opus vs EXPECTED)

Counts: ALL-AGREE 38, ANSWERS-AGREE-EXPECTED-DIFFERS 0, ANSWERS-DIFFER 1 (D10). One ALL-AGREE row (D02c) carries a wording nuance.

| ID | class | difference |
|---|---|---|
| D01 | ALL-AGREE | none |
| D02a | ALL-AGREE | none |
| D02b | ALL-AGREE | none |
| D02c | ALL-AGREE | nuance: sol says X duration unavailable; opus says completed duration 0 ns, count 0, open wait not quantified; EXPECTED says no closed X interval supplied |
| D02d | ALL-AGREE | none |
| D02e | ALL-AGREE | none |
| D03 | ALL-AGREE | none |
| D04a-install | ALL-AGREE | none |
| D04a-state | ALL-AGREE | none |
| D04a-binding | ALL-AGREE | none |
| D04a-address | ALL-AGREE | none |
| D04a-cookie | ALL-AGREE | none |
| D04b | ALL-AGREE | none |
| D04c | ALL-AGREE | none |
| D04d | ALL-AGREE | none |
| D04e | ALL-AGREE | none |
| D04f | ALL-AGREE | none |
| D04g | ALL-AGREE | none |
| D07a | ALL-AGREE | none |
| D07b | ALL-AGREE | none |
| D08a | ALL-AGREE | none |
| D08b | ALL-AGREE | none |
| D09 | ALL-AGREE | none |
| D10 | ANSWERS-DIFFER | opus does not state that the gap reopens (open, pending=1 for U, census completeness no longer established); sol and EXPECTED do |
| D13a | ALL-AGREE | none |
| D13b | ALL-AGREE | none |
| D13c | ALL-AGREE | none |
| D13d | ALL-AGREE | none |
| D13e | ALL-AGREE | none |
| Q01 | ALL-AGREE | none |
| Q02 | ALL-AGREE | none |
| Q03 | ALL-AGREE | none |
| Q04 | ALL-AGREE | none |
| Q04b | ALL-AGREE | none |
| Q05 | ALL-AGREE | none |
| Q06 | ALL-AGREE | none |
| Q07 | ALL-AGREE | none |
| Q08 | ALL-AGREE | none |
| Q09 | ALL-AGREE | none |

## D10 (ANSWERS-DIFFER)

### sol
> D10: D12[U] → D15[T] → F15 → F30; initially b_D=12 and gap=closed@12. At detection time 40, revoke startup base coverage from boot_start, including affected cached answers; gap=open, pending=1 for U, census completeness no longer established. b_U=none; no additional merge VOID is specified by LATE_U alone.
> why: DECISION 43 explicitly requires late-unlisted revocation; it supplies no automatic REARM or discovery lower bound.

### opus
> D10: `D12[U] → D15[T] → F15` (as D01; gap closed@12), then S(31): `F30`. LATE_U(40): base coverage revoked from boot_start (the closure at 12 no longer certifies startup coverage), affected cached answers invalidated. No VOID (no FAULT/LOSS input), no REARM, no recovery-time admission of U.
> why: D43 (late-unlisted startup revocation retained; no recovery-time admission/automatic REARM), merge §4 (VOID only on fault report).

### EXPECTED
> | D10 | D01’s output followed by `F30`. Detection at 40 revokes base coverage from 0, invalidates affected cached answers and leaves `G(open), P=1` for U. The uncovered-record observation alone emits no new DATA or invented VOID; census completeness must be re-established. |

Cited sentences: sol cites DECISION 43 (late-unlisted revocation, no automatic REARM). opus cites D43 (late-unlisted startup revocation retained; no recovery-time admission/automatic REARM). Both cite the same decision; the difference is only that opus omits the resulting gap/pending/completeness state.

## D02c (nuance, classed ALL-AGREE)

### sol
> D02c: D9[U] → D10[T] → F12; b_D=9, fresh-record route; gap=closed@9, pending=0, census complete@5. Completed-X count in [10,13)=0; X duration is unavailable without a supplied endpoint or open-wait snapshot. D10 certifies 1 ns of Y.
> why: DECISIONS 37(a), 41–42. READ_D(12,X) neither confirms the earlier seed nor replaces the record-established baseline.

### opus
> D02c: `D9[U] → D10[T] → F12`. b_D=9 by route (a). gap closed@9 (pending 0). X in [10,13): no completed X wait — duration 0 ns completed, count 0; the X wait entered at 10 is still open and is not quantified (no open-wait snapshot supplied; READ_D(12,X) supplies no observation). D9's incoming X interval (from the stale seed) is uncertified; D10's Y interval (1 ns) is certified.
> why: D37(a), D42; ROWS note "no unstated open-wait snapshot"; READ_D is ordinary read, not seed/record.

### EXPECTED
> | D02c | `D9[U] → D10[T] → F12`; `G(9), P=0`. The ordinary reread establishes nothing. X is established open at 10; the row supplies no closed X interval or independent open-wait snapshot establishing its duration through 13. |
