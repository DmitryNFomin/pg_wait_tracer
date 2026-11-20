`M` refers to the master merge contract, `A` to the merge amendment, and `C` to the coverage contract. Output record names preserve their input fields; all FORK, INIT and EXIT records below belong to D.

All merge rows leave the daemon alive and invalidate no previously published frontier. The epoch baseline remains C0. Suspended epochs discard the tentative suffix and freeze existing attachments without a valid frontier. Successful rows finish D’s lifecycle.

Every coverage answer includes policy revision 3, H_A2=1000 ms, K_A2=2, L_A2=1000 ms, N_qual=200, and the fixture-stipulated applicable qualification; the fixture supplies no literal qualification identifier. `note=a/b` means affected_lifetimes=a and untraced_backend_ns_upper=b×1,000,000 ns. `delay` is the labelled diagnostic receive_ns−fork_ns, in milliseconds, listed in pair order.

```text
M44[N] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A A2(a–e), declaration ordering and release; M §3
M44[Q] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A A2(a–e), lifecycle-only admission and release; M §3
M45[N] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A A2(c), stored INIT sufficiency, no synthesized INIT, release; M §3
M45[Q] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A A2(c), stored INIT sufficiency, lifecycle-only admission, release; M §3
M46[N] | FORK_D10 → INIT_D11 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A A2(c–d), delivered INIT agreement, genuine INIT release and declaration ordering; M §3
M46[Q] | FORK_D10 → INIT_D11 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A A2(c–d), delivered INIT agreement, genuine INIT release and declaration ordering; M §3
M48[N] | FORK_D10 → U(10,10,10) → EXIT_D10 → F10; epoch=active, frontier=10 | A A2(a), inclusive fork≤exit, declaration ordering; M §§2–3 kind ranks and strict release
M48[Q] | FORK_D10 → U(10,10,10) → EXIT_D10 → F10; epoch=active, frontier=10 | A A2(a), lifecycle-only admission, declaration ordering; M §§2–3
M49[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(c), fired-but-not-stored refusal; M §4
M49[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(c), fired-but-not-stored refusal; M §4
M50a[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(c): (BOOTSTRAP_FIRED,INIT_STORED)=(0,1) is inadmissible; M §4
M50a[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(c): (BOOTSTRAP_FIRED,INIT_STORED)=(0,1) is inadmissible; M §4
M50b[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(b), absent-disposition refusal; M §4
M50b[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(b), absent-disposition refusal; M §4
M50c[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(d), delivered INIT must agree exactly with stored INIT; contradictory-evidence refusal; M §4
M50c[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(d), delivered INIT must agree exactly with stored INIT; contradictory-evidence refusal; M §4
M51[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A attachment-interruption rule and A2(b); M §4 suspension
M51[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A attachment-interruption rule and A2(b); M §4 suspension
M52a[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A missing-FORK rule and A2(a); M §4
M52a[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A missing-FORK rule and A2(a); M §4
M52b[N] | VOID20[Cx,counter_change]; epoch=suspended, frontier=none | A actual loss remains independently fatal; M §4 CHECK and suspension
M52b[Q] | VOID20[Cx,counter_change]; epoch=suspended, frontier=none | A actual loss remains independently fatal; M §4 CHECK and suspension
M52c[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(a): FORK and EXIT must bind the same identity; missing-FORK and contradictory-evidence rules; M §§2,4
M52c[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(a): FORK and EXIT must bind the same identity; missing-FORK and contradictory-evidence rules; M §§2,4
M53a[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(c): stored init_ts precedes fork; delivered INIT also disagrees; M §4
M53a[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(c): stored init_ts precedes fork; delivered INIT also disagrees; M §4
M53b[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A delivered INIT must agree exactly with stored init_ts; contradictory-evidence refusal; M §4
M53b[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A delivered INIT must agree exactly with stored init_ts; contradictory-evidence refusal; M §4
M53c[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(c): stored init_ts exceeds exit; delivered INIT also disagrees; M §4
M53c[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(c): stored init_ts exceeds exit; delivered INIT also disagrees; M §4
M54a[N] | VOID20[Ci,counter_change]; epoch=suspended, frontier=none | A actual INIT loss is not forgiven; M §4 changed-counter reporting, unchanged rereads and suspension
M54a[Q] | VOID20[Ci,counter_change]; epoch=suspended, frontier=none | A actual INIT loss is not forgiven; M §4 changed-counter reporting, unchanged rereads and suspension
M54b[N] | VOID20[unknown,counter_unknown]; epoch=suspended, frontier=none | M §4 unreadable counters, observed-vector rule and suspension
M54b[Q] | VOID20[unknown,counter_unknown]; epoch=suspended, frontier=none | M §4 unreadable counters, observed-vector rule and suspension
M54c[N] | VOID20[C0,perf_lost]; epoch=suspended, frontier=none | M §4 positive perf LOST independently faults without fabricating counter increments
M54c[Q] | VOID20[C0,perf_lost]; epoch=suspended, frontier=none | M §4 positive perf LOST independently faults without fabricating counter increments
M55[N] | FORK_D10 → VOID20[Cw,writer_push_fail]; epoch=suspended, capture=paused, frontier=none; FORK remains an uncertified output | A declaration publication failure; M §§3–4 successful-publication observation point and partial-release failure
M55[Q] | FORK_D10 → VOID20[Cw,writer_push_fail]; epoch=suspended, capture=paused, frontier=none; FORK remains an uncertified output | A declaration publication failure; M §§3–4 successful-publication observation point and partial-release failure
M56[N] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A hold FORK, batch and frontier until EXIT is eligible; M §3 strict t<W and sequential release
M56[Q] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A hold FORK, batch and frontier until EXIT is eligible; M §3 strict t<W and sequential release
M57a[N] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A closed table permits lifetime-bound perf_event_open(disabled)/ESRCH; failure need not follow exit; release
M57a[Q] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A closed table permits lifetime-bound perf_event_open(disabled)/ESRCH; failure need not follow exit; release
M57b[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(e), closed table excludes perf_event_open/EMFILE; non-admissible failure refusal; M §4
M57b[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(e), closed table excludes perf_event_open/EMFILE; non-admissible failure refusal; M §4
M57c[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A closed table permits no ENABLE failure errno; M §4
M57c[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A closed table permits no ENABLE failure errno; M §4
M58[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2 admits no TRANS/MARKER without independent valid observation proof; M §§2–3 pre-enable marker restrictions and §4
M58[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2 admits no TRANS/MARKER without independent valid observation proof; M §§2–3 pre-enable marker restrictions and §4
M59[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A terminal reconciliation without matching kernel EXIT faults; lookup ENOENT exception requires retained matching EXIT; M §4
M59[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A terminal reconciliation without matching kernel EXIT faults; lookup ENOENT exception requires retained matching EXIT; M §4
M60[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(b,d), successful-enable/clear-EVER_LIVE contradiction receives no exception; M §4
M60[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(b,d), successful-enable/clear-EVER_LIVE contradiction receives no exception; M §4
M61a[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(d–e), operation/result/original errno retention requirement; unknown unsuccessful operation is inadmissible; M §4
M61a[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A A2(d–e), operation/result/original errno retention requirement; unknown unsuccessful operation is inadmissible; M §4
M61b[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A /proc ENOENT requires independently retained lifetime/task binding; PID-only evidence is insufficient; M §4
M61b[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A /proc ENOENT requires independently retained lifetime/task binding; PID-only evidence is insufficient; M §4
M62a[N] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A A2(c): init_ts=fork is permitted; stored-only INIT suffices and is not synthesized; release
M62a[Q] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A A2(c): init_ts=fork is permitted; stored-only INIT suffices and is not synthesized; lifecycle-only admission
M62b[N] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A A2(c): init_ts=exit is permitted; stored-only INIT suffices and is not synthesized; release
M62b[Q] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A A2(c): init_ts=exit is permitted; stored-only INIT suffices and is not synthesized; lifecycle-only admission
M63a[N] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A closed table permits /proc open ENOENT with independent lifetime/task binding; failure need not follow exit; release
M63a[Q] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A closed table permits /proc open ENOENT with independent lifetime/task binding; failure need not follow exit; release
M63b[N] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A closed table and terminal-state exception permit lookup ENOENT when matching EXIT supplies terminal state; release
M63b[Q] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A closed table and terminal-state exception permit lookup ENOENT when matching EXIT supplies terminal state; release
M63c[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A closed table permits no state-map update failure errno; explicit adapter FAULT; ROWS stipulates C0; M §4
M63c[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A closed table permits no state-map update failure errno; explicit adapter FAULT; ROWS stipulates C0; M §4
M63d[N] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A cancellation after consuming matching kernel EXIT is admissible; A2(a–e) and release
M63d[Q] | FORK_D10 → U(10,12,10) → EXIT_D12 → F12; epoch=active, frontier=12 | A cancellation after consuming matching kernel EXIT is admissible; A2(a–e) and lifecycle-only release
M63e[N] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A permitted operation/error mappings require kernel qualification; unqualified mapping is unknown evidence; M §4
M63e[Q] | VOID20[C0,unknown_evidence]; epoch=suspended, frontier=none | A permitted operation/error mappings require kernel qualification; unqualified mapping is unknown evidence; M §4

C1[default] | numbers; note=2/12; delay=(1,1); J_0:N=200,U=2 passes | C Scope and policy; Population, cohorts and finality; Limits and their affected windows; Status, reasons and omission notes
C2[default] | Unavailable(untraced_limit); note=3/18; delay=(1,1,1); J_0:U=3>2 | C Limits and their affected windows; Status, reasons and omission notes
C3a[default] | numbers; note=1/6; delay=1; J_0:N=1,U=1 passes | C absolute count cap in Scope and policy; Limits and their affected windows; Status, reasons and omission notes
C3b[default] | numbers; note=1/6; delay=1; J_0:N=2,U=1 passes | C absolute count cap in Scope and policy; Limits and their affected windows; Status, reasons and omission notes
C4a[default] | numbers; no note (zero affected); delay=none; J_0:N=U=0 | C Population, cohorts and finality; zero certificates in Carrier and retention; Status, reasons and omission notes
C4b[default] | Unavailable(untraced_invalid); note=1/6; delay=1; invalid certificate U=1>N=0 | C certificate bounds in Population, cohorts and finality; independent complete-inventory note computation in Status, reasons and omission notes
C5a[default] | numbers; no note (zero affected); delay=1 | C half-open overlap in Limits and their affected windows; Status, reasons and omission notes
C5b[default] | numbers; note=1/1; delay=1 | C half-open overlap in Limits and their affected windows; clipped backend-time sum in Status, reasons and omission notes
C5c[default] | numbers; no note (zero affected); delay=1 | C half-open overlap in Limits and their affected windows; Status, reasons and omission notes
C6[default] | Unavailable(untraced_limit); note=3/14; delay=(1,1,1); required cohorts={J_0,J_1}, J_0:U=3>2 | C required birth cohorts and failure propagation in Limits and their affected windows; additive clipped backend-time in Status, reasons and omission notes
C7a[default] | numbers; note=1/1000; delay=1; duration=1000 equals cap | C equality passes in Limits and their affected windows; Status, reasons and omission notes
C7b[default] | Unavailable(untraced_limit); note=1/1000; delay=1; duration=1000.000001 exceeds cap | C duration limit and required cohorts in Limits and their affected windows; clipped note in Status, reasons and omission notes
C8a[default] | numbers; note=1/6; delay=100 | C no receive-delay threshold in Scope and policy; Carrier and retention permits exit before receive; Status, reasons and omission notes
C8b[default] | numbers; note=1/6; delay=100.000001 | C no receive-delay threshold in Scope and policy; Carrier and retention permits exit before receive; Status, reasons and omission notes
C8c[default] | numbers; note=1/15.318503; delay=191.490332 | C no receive-delay threshold in Scope and policy; original interval endpoints in Carrier and retention; Status, reasons and omission notes
C9[default] | numbers; note=7200/43200; delay=1 for every omission; each of 3600 cohorts has U=2 and passes | C passing cohorts may combine without accumulated-omission failure in Limits and their affected windows; distinct-identity additive note in Status, reasons and omission notes
C10[default] | Unavailable(untraced_limit); note=7201/43206; delay=1 for every omission; J_1800:U=3>2 | C failing cohorts cannot be diluted in Limits and their affected windows; Status, reasons and omission notes
C11[default] | Unavailable(untraced_limit); note=200/400; delay_i=1001−5i for i=0..199; J_0:U=200>2 | C birth-anchored cohorts in Population, cohorts and finality; no receive-delay threshold; Limits and their affected windows; Status, reasons and omission notes
C12[default] | Unavailable(untraced_limit); note=200/400; delay_J0,i=1001−5i and delay_J1,i=501−5i for i=0..99; J_0:U=100>2, J_1:U=100>2 | C birth-anchored cohorts in Population, cohorts and finality; Limits and their affected windows; Status, reasons and omission notes
C13[default] | numbers; no note (zero affected); delay=none | C daemon pause without births creates no A2 omission in Status, reasons and omission notes; zero certificates in Carrier and retention
C14a[default] | Unavailable(untraced_pending); note values withheld (unfinished inventory); retained-pair delay=(1,1); J_0 has explicit unfinished reconciliation | C finality in Population, cohorts and finality; explicit-live-ledger pending exception and inventory withholding in Status, reasons and omission notes
C14b[default] | Unavailable(untraced_incomplete); note values withheld (missing pair member); delay=(unavailable,1) | C complete pair requirement and retention in Carrier and retention; incomplete precedence and inventory withholding in Status, reasons and omission notes
C14c[default] | Unavailable(untraced_incomplete); note=2/12; delay=(1,1); required J_0 certificate missing without live unfinished reconciliation | C certificate retention in Carrier and retention; missing-certificate reason and independent complete-inventory note computation in Status, reasons and omission notes
C15[default] | Unavailable(B_LOSS); note=2/12; delay=(1,1); omission bound covers declared omissions only | C base-failure precedence in Scope and policy and Status, reasons and omission notes; independent note and bound scope
C16[default] | numbers; note=1/6; delay=1; required cohorts={J_1}, J_1:N=1,U=1 | C fixed half-open cohorts in Population, cohorts and finality; required cohorts in Limits and their affected windows; Status, reasons and omission notes
C17[default] | numbers; no note (zero affected); delay=1; J_0:U=1 despite zero-duration lifetime | C zero-duration birth counting in Population, cohorts and finality; zero-duration non-overlap in Limits and their affected windows; Status, reasons and omission notes
C18a[raw] | Unavailable(untraced_limit); note=3/18; delay=(1,1,1); J_0:U=3>2 | C Limits and their affected windows; one core result across routes in Status, reasons and omission notes
C18b[summary] | Unavailable(untraced_limit); note=3/18; delay=(1,1,1); J_0:U=3>2 | C Limits and their affected windows; one core result across routes in Status, reasons and omission notes
C18c[server] | Unavailable(untraced_limit); note=3/18; delay=(1,1,1); J_0:U=3>2 | C Limits and their affected windows; one core result across routes in Status, reasons and omission notes
C18d[CLI] | Unavailable(untraced_limit); note=3/18; delay=(1,1,1); J_0:U=3>2 | C Limits and their affected windows; one core result across routes in Status, reasons and omission notes
C18e[UI] | Unavailable(untraced_limit); note=3/18; delay=(1,1,1); UI="3 backends not traced; at most 18 backend-ms omitted."; J_0:U=3>2 | C Limits and their affected windows; UI note and one core result across routes in Status, reasons and omission notes
C18f[chart bucket] | Unavailable(untraced_limit); note=3/18; delay=(1,1,1); J_0:U=3>2 | C identical-window bucket rule and one core result in Status, reasons and omission notes
C19a[default] | Unavailable(untraced_limit); note=1/500; delay=1; J_0 fails duration limit | C whole-birth-cohort duration failure in Limits and their affected windows; clipped note in Status, reasons and omission notes
C19b[default] | Unavailable(untraced_limit); no note (zero affected); delay=1; J_0 fails duration limit although omission does not overlap W | C evaluate every omission born in a required cohort in Limits and their affected windows; zero-overlap note omission in Status, reasons and omission notes
C19c[default] | numbers; no note (zero affected); delay=100.000001 | C no receive-delay threshold in Scope and policy; half-open overlap in Limits and their affected windows; Status, reasons and omission notes
C20a[default] | numbers; note=1/138.028412; delay=191.721244 | A boot_start and first-drain birth rule; C birth cohorts in Population, cohorts and finality, no receive-delay threshold in Scope and policy, Status, reasons and omission notes
C20b[default] | numbers; note=1/15.318503; delay=191.490332 | A boot_start and first-drain birth rule; C birth cohorts in Population, cohorts and finality, no receive-delay threshold in Scope and policy, Status, reasons and omission notes
C21[default] | Unavailable(untraced_invalid); note values withheld (invalid omission inventory); delay=8 for the invalid decoded declaration | C fork before boot_start is invalid in Population, cohorts and finality; decoded-field invalidity and inventory withholding in Status, reasons and omission notes
C22a[default] | Unavailable(untraced_limit); no note (zero affected); delay=(1,1,1); required J_0 has U=3>2 | C zero-duration birth counting in Population, cohorts and finality; count limit and non-overlap in Limits and their affected windows; Status, reasons and omission notes
C22b[default] | numbers; no note (zero affected); delay=(1,1,1) for declarations outside required cohorts; required cohorts={J_1} | C empty omitted lifetime propagates no failure beyond its birth cohort in Limits and their affected windows; Status, reasons and omission notes
C23[default] | Unavailable(untraced_unqualified); note=1/6; delay=1; J_0:N=201>N_qual=200 | C qualification bound in Scope and policy and Limits and their affected windows; independent note in Status, reasons and omission notes
C24[default] | Unavailable(untraced_invalid); note=1/6; delay=1; certificate N=2 contradicts complete retained birth count 3 | C retained FORK agreement in Population, cohorts and finality; certificate invalidity and independent complete-inventory note computation in Status, reasons and omission notes
```

AMBIGUITIES

- **C4b and C24:** I distinguish an invalid cohort certificate from an invalid omission inventory. The fixtures retain complete, valid omission pairs and matching omitted counts, so the independent note remains computable despite the invalid birth-count certificate.
- **C14c:** I retain the fixture’s stipulated complete pairs, birth accounting and completed reconciliation when only the certificate is removed. Thus missing certification makes status incomplete, while the omission inventory still supports the note.
- **M52c:** I treat the EXIT for g=2 without its own FORK as unknown evidence during complete-batch validation. The pending EXIT for g=1 does not turn that invalid record into an ordinary dependency hold.
- **M50c and M53a–c:** A genuine INIT does not bypass contradictory terminal evidence. The amendment’s exact-agreement and contradictory-evidence rules apply.
- **Qualification identity:** An applicable qualification is stipulated, but its concrete identifier is absent. The answers identify that stipulated qualification symbolically rather than inventing an identifier.