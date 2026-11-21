# Independent answer set (opus): rows N1-N16, J1-J22

Conventions: mode N; `P=L+Q` (unresolved inventoried lifetimes + unresolved numeric observations); "gap closed P_close=n" = startup gap committed closed with persisted GAP_CLOSE end n; [0,P_close) unavailable, windows starting at or after P_close eligible only under all other rules. "attempts" = wait-word attachment attempts (Decision 38). No VOID/FAULT/DATA beyond those listed.

N1: `F5 → F7`; at the S(8) evaluation b_D=6 route (c) commits and the gap closes: C_close=max(5,6)=6, P_close=6 (no uncertified discovery TRANS/MARKER at 6). P=0+0, census=known. attempts=0. D shown 'present, not measured', role logger 13; its waits, completed-wait counts, CPU and off-CPU time are unavailable (not zero); contributes nothing to N_k, U_k, any numerator or the measured population (exclusion identified). [0,6) unavailable.
why: D37(c) commit needs completeness strictly beyond e (S(6) gives W=6, not >6; S(8) gives W=8); D41/D42 (37(c) triggers no increment); D43; D38 (NULL read consumes no attempt).

N2: `F7`. No ADMIT, no bound; pointer watch retained; D pending. P=1+0, census=known, gap open. attempts=0. No numbers for any window from boot_start (gap open).
why: D37(c)/D39 (client backend, no qualified permanent role; NULL alone insufficient); D38 (NULL candidate without role: no ADMIT, retain watch and pending entry).

N3: `F7`. Witness fails (no pinned NULL read after e); no ADMIT, no bound, no FAULT (no false claim). D pending: P=1+0, census=known, gap open. attempts=0.
why: D37(c) (read must follow e); D36 (ADMIT only after the initial NULL witness passes).

N4: `F9`. Witness fails: one pointer-watch fire (at 7) between enable 6 and fence 9, but the route requires zero fires through the fence; no ADMIT, no bound, no FAULT. D pending: P=1+0, census=known, gap open. attempts=0 (a NULL observation makes no wait-word attempt eligible).
why: D37(c) "zero pointer-watch fires between enable and that fence"; D38 (only a non-NULL fire enables 37(a)/(b) attempts).

N5: `F5 → F7` (gap closed P_close=6, b_D=6 route (c), as N1) `→ VOID20[C0,unknown_evidence]`; S(14) hold (suspended). Revocation: b_D revoked, base coverage revoked from boot_start, cached answers invalidated, startup gap reopened (no gap end in effect), D back to pending: P=1+0, census=unknown. No automatic REARM, no handoff gap, no new attachment.
why: D37(c)/D43 (non-NULL fire after an accepted 37(c) bound → FAULT(unknown_evidence), revoke, reopen, re-establish census); §4 suspension.

N6: `F5 → F7` (gap closed P_close=6, b_D=6 route (c)) `→ EXIT_D12 → F12`. PTR_D12(NULL) produces no event, no loss (internal observation accounting only). No invented terminal TRANS. b_D=6 and persisted P_close=6 unchanged; P=0+0, census=known; D remains 'present, not measured' (logger 13), quantities unavailable, no population contribution.
why: D37(c) (NULL fire); D40 (no invented TRANS; cookie-bearing EXIT released normally); D41 ("later lifecycle EXIT ... does not change its committed bound or the already persisted startup endpoint").

N7: `F12`; at S(13) b_D=11 route (b) commits and the gap closes: C_close=max(5,11)=11, P_close=11. P=0+0, census=known. attempts=1 (the pointer-watch qualification is not an attempt). D is an ordinary measured process (client backend), not 'present, not measured'; contributes to the measured population from 11 (seed X at 11; no completed wait through 13); no N_k/U_k contribution.
why: D38 (non-NULL fire at 9 makes D eligible for 37(a)/(b)); D37(b) (ADMIT e=12 matches, completeness W=13>11); D41/D42 (no uncertified record at 11).

N8: `EXIT_D9 → F9`; discharge x_D=9 commits with that release; gap closes: C_close=max(5,9)=9, P_close=9 (lifecycle EXIT alone triggers no increment). P=0+0, census=known. attempts=0. No b_D; D supplies no numbers; [0,9) unavailable.
why: D36 ("matching terminal lifecycle evidence retains Decision 40's discharge route without ADMIT"); D40 (x_i only on committed release incl. FRONTIER); D41/D42.

N9: `VOID20[Cf,counter_change]`; S(8) hold (suspended). Witness fails (counters not unchanged); no ADMIT, no bound. D pending: P=1+0, census=known, gap open. attempts=0.
why: D37(c) (unchanged readable counters required); §4 counter_change on CHECK, single report for one change.

N10: `F5 → F7`; b_D=6 route (c) commits at S(8); gap closed C_close=6, P_close=6. P=0+0, census=known. attempts=0. D 'present, not measured', role stats collector 12; quantities unavailable, no population contribution.
why: as N1 with the independently qualified role 12 (D37(c), D39, D41-D43).

N11: `F7`. No ADMIT, no bound; pointer watch retained; D pending. P=1+0, census=known, gap open. attempts=0.
why: D39 (an unqualified numeric role value is insufficient); D38.

N12: `VOID20[C0,unknown_evidence]`; S(8) hold. ADMIT refused (explicit false continuity claim); the CONFIRM's witness describes the actual disabled watch and supplies no bound (no second fault: only the AD claim is identified as false). No b_D; D pending: P=1+0, census=known, gap open. attempts=0.
why: D36 (claimed admission proof containing a false required fact → FAULT(unknown_evidence)); D37(c) (failed witness supplies no bound); D39 (silent disabling forbidden).

N13: `H10[U] → F10`; at S(11): b_D=6 route (c) and b_H=10 route (a) commit; gap closes: C_close=max(5,6,10)=10, P_close=11 (H10 is a released discovery fresh-record anchor stamped exactly C_close). P=0+0, census=known. attempts: D=0, H=0. H's incoming Y interval (duration, completed-wait count, CPU) unavailable; completions at 10 unknown; D 'present, not measured' (logger 13), no contribution. Windows starting at or after 11 eligible.
why: D37(a) (first record after e=8 → [U], b_H=10); D37(c) (W=11>6); D41/D42 (increment from H10, not from D's 37(c) bound).

N14: `VOID20[Cw,writer_push_fail]`; no FRONTIER (rejection precedes any FRONTIER publication); capture paused. b_D not committed; gap open; D pending: P=1+0, census=known. attempts=0.
why: D37(c) (commit requires successful publication of supporting coverage evidence); D41 (publication failure commits neither bound nor closure); §4 writer failure.

N15: `VOID20[Cr,counter_change]`; S(8) hold. No NULL witness, no ADMIT, no bound; read failure keeps its ordinary accounting. D pending: P=1+0, census=known, gap open. attempts=0 (pointer-watch read is not a wait-word attempt).
why: D37(c) ("failed required pointer read retains its ordinary failure accounting"); D38; §4.

N16: `F15`. No ADMIT, no bound; deadline expiry supplies no initialization, discharge, census completeness or bound; D pending (P=1+0, census=known, gap open) and diagnostics report the waiting limit (14) exceeded. No FAULT. attempts=0.
why: D38 (authenticating-client waiting target and expiry rule); D37(c)/D39 (no permanent role).

J1: `F3 → EXIT_U4 → F7`. J resolves the PID-50 observation into journal-only lifetime U (Q 1→0, L=1); matched disposition delivered before the EXIT applies on consumption (EXIT not held); x_U=4 commits with the S(8) release; gap closes: C_close=max(5,4)=5, P_close=5. Final P=0+0, census=known.
why: D31(J); D40 (cookie-less EXIT passes leader/parent/start<F0; disposition before EXIT; x_i on committed release); D41/D42.

J2: `F7`; G drops the numeric observation as non-member (no x_i); gap closes at S(8): C_close=5, P_close=5. P=0+0, census=known.
why: D31(G) (ESRCH at 4 after failed open, completed fence 5, C0 over [F0,5]); D41.

J3: `F7`. kill returning 0 is not ESRCH and ENOENT open is not the gone-proof; no J witness, no R binding → observation pending. P=0+1, census=unknown, gap open.
why: D31(G) ("Open failure, including ENOENT, is not the gone-proof; success ... is not ESRCH"); D31 (unresolved observation prevents census completeness).

J4: `F7`. EPERM is not ESRCH; observation pending. P=0+1, census=unknown, gap open.
why: D31(G).

J5: `VOID20[Cf,counter_change]`; S(8) hold. G fails (counters changed over [F0,fence]); observation pending. P=0+1, census=unknown, gap open.
why: D31(G) (unchanged counters required); §4.

J6: hold (no L boundary: W=0, no FRONTIER). The journal witness is not eligible (relevant producer entered, not completed); no resolution, no FAULT (missing completion holds). P=0+1, census=unknown, gap open.
why: D31 (J requires completed relevant producers); D40; §3 (unbounded source blocks release).

J7: `VOID20[Ck,counter_change]`; S(8) hold. The exit filter emits no discovery EXIT (leader postmaster child with known start 0 ≥ F0, no fork cookie = missing-fork loss, accounted once in lifecycle_fork_drop). Not a J witness (start not < F0); observation pending. P=0+1, census=unknown, gap open.
why: D40 (missing-fork loss rule); D31(J); §4.

J8: `F7`; gap closes at S(8): C_close=5 (census fence, empty inventory), P_close=5. Exit filter emits no record and no counter change (non-leader; a live discovery-map entry is not a pass condition). P=0+0, census=known.
why: D40 ("Non-leaders and non-children emit no discovery lifecycle EXIT"; live identity/tombstone not a pass condition); D41.

J9: `F7`; gap closed C_close=5, P_close=5. No EXIT, no lifecycle_fork_drop (non-child; tombstone not a pass condition). P=0+0, census=known.
why: D40 (non-children emit nothing; missing-fork accounting applies only to postmaster children).

J10: `F7`; gap closed C_close=5, P_close=5. No EXIT, no counter change (stale live discovery identity is not bound to this lifetime and the three facts fail). P=0+0, census=known.
why: D40 ("A discovery cookie is usable only when bound to this exact kernel lifetime and the same three facts hold").

J11: `VOID20[C0,unknown_evidence]`; S(8) hold. Exit filter emits no discovery EXIT and fabricates no lifecycle_fork_drop (unknown start passes neither comparison); the unknown start is unknown evidence. Observation pending: P=0+1, census=unknown, gap open.
why: D31 ("zero or unreadable task start time is unknown evidence"); D40 ("An unknown start time cannot pass either time comparison and supplies unknown evidence"); §4.

J12: `FORK_R3 → R6[U] → F7`. R binds the holder as a fork-route lifetime (25b), resolving the numeric observation (Q 1→0; no discovery inventory entry); gap closes at S(8): C_close=5, P_close=5 (R6 is a 25b record, not a discovery record, and not at 5). P=0+0, census=known.
why: D31(R), D30 (valid fork provenance follows the fork route); §2/§3 25b/25c (rows 32/39); D41/D42.

J13: `U7[U] → F7`. R binds U as an ordinary discovery candidate (Q 1→0, L=1); fresh-record anchor U7 gives b_U=7 route (a); gap closes: C_close=max(5,7)=7, P_close=8 (anchor stamped exactly C_close). P=0+0, census=known. U's incoming Y interval unavailable; completions at 7 unknown; windows from 8 eligible.
why: D31(R); D37(a); D41/D42 increment.

J14: `F7`. R classifies the holder as a non-member; observation dropped; gap closes C_close=5, P_close=5. P=0+0, census=known.
why: D31(R) (non-member; no earlier journal lifetime to preserve); D41.

J15: `EXIT_U4 → FORK_R6 → R8[U] → F8`. J creates U and its matched disposition gives x_U=4 on the committed S(9) release; R (pid 50, g=1) follows 25b, U's EXIT ordered before R's FORK; gap closes: C_close=max(5,4)=5, P_close=5. P=0+0, census=known.
why: D31(J)/(R) ("rebinding a replacement never removes a separate earlier lifetime"); D40; §2 25b (previous generation's EXIT precedes FORK); D41/D42.

J16: `VOID20[C0,unknown_evidence]`; S(8) hold (EXIT_U4 not released). The terminal control's match is contradicted (J start −100 ≠ U's retained −200); contradictory match supplies no discharge. U unresolved: P=1+0, census=known, gap open.
why: D40 (exact agreement with retained kernel start identity; contradictory match supplies no discharge); D36/D37 (false claim → FAULT(unknown_evidence)).

J17: `VOID20[C0,unknown_evidence]`; S(8) hold. Journal exit_ns 4 ≠ named EXIT timestamp 5, and the control falsely claims equality; no matched disposition, no discharge, no J resolution. P=0+1, census=unknown, gap open.
why: D40 ("The corresponding lifecycle EXIT has that same semantic timestamp"; match the particular EXIT record); D31 (contradictory facts authorize no resolution).

J18: `VOID20[Cw,writer_push_fail]`; no FRONTIER; capture paused. Rejected EXIT_U4 is absent; no discharge; U (created by J) pending: P=1+0, census=known, gap open.
why: D40 (x_i only when the release commits); clarification (rejected EXIT leaves lifetime pending, gap open); §4.

J19: `EXIT_U4 → VOID20[Cf,counter_change]`; no F7. EXIT_U4 is observable only in the uncertified suffix; no discharge in this or any later epoch; U pending: P=1+0, census=known, gap open.
why: D40 ("an EXIT in a subsequently VOIDed release attempt supplies no discharge"); clarification after the delta round; §3/§4.

J20: `F7` (G drops the observation; gap closed C_close=5, P_close=5, as J2); then LATE_U(40): base coverage revoked from boot_start, cached answers invalidated, gap open again from boot_start, U pending: P=1+0, census=unknown. No merge VOID.
why: D43 late-unlisted revocation; clarification (row D10: pending 1, census no longer established, no VOID).

J21: `F7`. ESRCH observed before the failed open does not satisfy G; observation pending. P=0+1, census=unknown, gap open.
why: D31(G) ("After the failed open, kill(pid,0) returns ESRCH").

J22: `FORK_R1 → R6[U] → F7`. Observed fork provenance takes precedence over the pre-F0 start time: R follows the fork route (25b), not discovery; observation resolved (Q 1→0); gap closes C_close=5, P_close=5. P=0+0, census=known.
why: D30 ("a fork whose task start timestamp predates capture cannot use discovery"); D31 ("Observed fork provenance takes precedence"); D33; §2 25b.

## Assumptions where the packet was silent
- N8: EXIT_D9 is taken to carry D's installed discovery cookie (notation substitutes D=(30,2^63)), so no cookie-less disposition is needed; if cookie-less, TERM_D9 is taken as the matched disposition and the answer is unchanged.
- N9, N15: the fixture's complete census at fence 5 is taken to remain known after a counter change observed after that fence.
- N12: only the AD carries the identified false claim; the CONFIRM witness reports actual facts, so one VOID, not two.
- J11: "supplies unknown evidence" is taken to be reported as FAULT(unknown_evidence) → VOID with C0.
- J17: EXIT_U5@L is taken as delivered; the timestamp contradiction is taken to invalidate the J resolution (Q stays 1). If J resolution were still applied, P would be 1+0 with census=known; the VOID is unchanged.
- J16-J19: a J resolution, once applied, is taken to move the numeric observation into L even when the discharge later fails (J18, J19).
