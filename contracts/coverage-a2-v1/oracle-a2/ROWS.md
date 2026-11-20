INFERRED input fixtures. These are inputs only; they do not assert implementation or empirical qualification.

MERGE FIXTURE NOTATION AND INHERITED RULES

Each M row and lettered variant starts independently.
All times are exact integer nanoseconds.
The complete source set is {P0,P1,E,L}; P0/P1 are qualified per-CPU perf sources, E is the marker source, and L is the lifecycle source.
boot_start=0. The epoch is active, queues are empty, every source cutoff is 0, and no frontier has been published.
Initially attached identities are (pid=10,g=1) and (pid=20,g=1). The new identity D=(pid=30,g=1) has no prior history or attachment proof.
Every ordinary FORK has fork_seed_proved=false.
Source positions start at 1 and increase separately on each source for every consumed record or boundary.

Rows apply in both sequence-free mode N and sequence-bearing mode Q.
In Q, a generation with valid ordinary creation proof starts at next_q=1; markers shown below carry q=1. In N, omit q.
A2 lifecycle-only admission requires no invented TRANS/MARKER sequence evidence.

C0 is the all-zero counter vector with components:
lifecycle_fork_drop, lifecycle_init_drop, lifecycle_exit_drop, wait_read_fail,
query_text_drop, pg13_query_text_drop, exact_query_update_fail,
exact_cmd_update_fail, exact_phase_update_fail, accum_update_fail,
fork_state_update_fail, event_ring_drop, state_map_update_fail,
seen_query_ids_update_fail, writer_push_fail.
Ci, Cx and Cw are C0 except, respectively, lifecycle_init_drop=1,
lifecycle_exit_drop=1 and writer_push_fail=1.
The epoch baseline and every unstated observation are C0.
Every observation has detection time d=20.

Record notation KIND_Dt@source means a record of kind KIND, identity D and semantic timestamp t from that source.
B abbreviates FORK_D10@L.
X abbreviates EXIT_D12@L.
EXIT flags are ordered (DISPOSITION,BOOTSTRAP_FIRED,INIT_STORED,WP_EVER_LIVE); the default is (1,0,0,0).
i is EXIT's stored init_ts, default 0.
The original FORK receive_ns is 10.
Identity binding, retained producer history, loss accounting and supported-kernel mappings are qualified and complete unless overridden.
Attachment history contains no attempts unless overridden.
No unstated records, errors, callbacks or counter changes occur.

S(w) expands, in order, to one qualified strict BOUNDARY at w for P0, P1, E and L, each consuming its next source position.
Process every input and expanded boundary separately.
The release cutoff W is the minimum source boundary. Only records with t<W are eligible.
At W=0 there is no release. At W>0 an advancing frontier is W-1.
Validate the complete eligible batch before publishing any of it; missing pending dependencies hold the batch and frontier, while lost or contradictory dependencies fault.
Successful DATA publication follows semantic timestamp, identity and kind rank: FORK before INIT before TRANS/MARKER before EXIT at equal timestamps.
The A2 declaration is inserted immediately before its EXIT.
An empty eligible batch can advance the frontier.

CHECK observes counters. A changed readable vector faults with counter_change; an unreadable vector faults with counter_unknown and vector unknown.
LOSS(source,n) with n>0 faults with perf_lost.
FAULT(reason) faults with the stated reason.
A fault publishes VOID at detection time 20 with the observed vector, discards the tentative suffix and suspends the epoch. Later DATA and boundaries cannot resume it.
A rejected publication is not output. Earlier successful publications remain in the uncertified suffix; no frontier follows a partially rejected release.
All publications, including VOID, succeed unless explicitly rejected.

Output notation requested from answer authors:
F<n> = FRONTIER(n,C0).
VOID20[C,reason] = VOID(20,C,reason).
U(f,e,r) = UNTRACED(30,1,f,e,r).
An arrow denotes successful publication order.
“hold” means no successful output.
Give the entire output sequence for each variant, including any prefix before a fault.

Append S(13) to every M variant unless that variant explicitly supplies its complete ending.
An override replaces the relevant default rather than adding a second contradictory record.

M44: B,X.

M45: B,X(flags=(1,1,1,0),i=11). The producer protocol stores INIT only in EXIT and requires no separate INIT publication.

M46: B,INIT_D11@L,X(flags=(1,1,1,0),i=11).

M48: B,EXIT_D10@L; complete ending S(11).

M49: B,X(flags=(1,1,0,0),i=0).

M50a: B,X(flags=(1,0,1,0),i=11).
M50b: B,X(flags=(0,0,0,0),i=0).
M50c: B,INIT_D11@L,X(flags=(1,0,0,0),i=0).

M51: B,X(flags=(1,0,0,1)); history records successful ENABLE at 11 and a subsequent attachment interruption; then FAULT(unknown_evidence), before any boundaries.

M52a: X without B.
M52b: B,CHECK(Cx), with no EXIT.
M52c: B,EXIT at 12 with pid=30,g=2; no EXIT for D.

M53a: B,INIT_D11@L,X(flags=(1,1,1,0),i=9).
M53b: B,INIT_D11@L,X(flags=(1,1,1,0),i=12).
M53c: B,INIT_D11@L,X(flags=(1,1,1,0),i=13).

M54a: CHECK(Ci),B,X; every subsequent observation retains Ci.
M54b: unreadable CHECK,B,X.
M54c: LOSS(P0,1),B,X; counters remain C0.

M55: B,X; FORK publication succeeds; UNTRACED publication rejects and writer_push_fail becomes 1.

M56: B,S(11),X,S(13); this is the complete input sequence.

M57a: B,X; history records lifetime-bound perf_event_open(disabled) failure ESRCH at 11.
M57b: B,X; history records lifetime-bound perf_event_open(disabled) failure EMFILE at 11.
M57c: B,X; history records ENABLE failure EINVAL at 11, with no successful enable.

M58: B,MARKER_D11@E(q=1),X; the marker carries a snapshot taken at 10 before any enable; no independent valid observation proof exists.

M59: B; terminal reconciliation records state-map lookup ENOENT, task death and a qualified lifecycle source boundary through 13, but no kernel EXIT was produced or retained. This is terminal reconciliation, not an unfinished attempt.

M60: B,X; complete history records successful ENABLE at 11 while EXIT's WP_EVER_LIVE remains clear.

M61a: B,X; an attachment attempt occurred, but its operation and original errno were not retained.
M61b: B,X; target /proc open failed ENOENT, with only PID identification and no independently retained lifetime/task binding.

M62a: B,X(flags=(1,1,1,0),i=10); the producer protocol requires no separate INIT publication.
M62b: B,X(flags=(1,1,1,0),i=12); the producer protocol requires no separate INIT publication.

M63a: B,X; target /proc open failed ENOENT at 11, with independently retained lifetime/task binding.
M63b: B,X; state-map lookup failed ENOENT and the retained matching EXIT supplies all missing terminal state.
M63c: B,X; state-map update failed ENOENT; the adapter reports FAULT(unknown_evidence) before boundaries, with counters stipulated C0 for this adapter-rule fixture.
M63d: B,X; attachment was cancelled only after consuming X.
M63e: B,X; lifetime-bound perf_event_open(disabled) failed ESRCH, but this kernel's operation/error mapping is unqualified.


COVERAGE FIXTURE NOTATION

Each C row and lettered variant starts independently.
Times in this section are milliseconds; decimals are exact, and 0.000001 ms is one ns.
Unless overridden: boot_start=0, H_A2=1000 ms, K_A2=2, L_A2=1000 ms, N_qual=200.
For these design fixtures, stipulate an applicable qualified policy with those parameters. This stipulation asserts no real measurement result.
J_k=[1000k,1000(k+1)) unless boot_start is overridden.
W is the requested nonempty half-open selection.

Q(f,e,r) supplies a distinct identity's retained GAP_OPEN/GAP_CLOSE pair with fork=f, exit=e and FORK receive=r.
When r is omitted, r=f+1.
Each pair has a distinct gap_id, valid merge-admission evidence and detection time max(e,r)+0.000001, except where an explicit override contradicts these defaults.
Q does not assert that the coverage limits pass.
PIDs and nonzero cookies are distinct and never reused.

A stated N applies to the cohort containing the listed births, or to the expressly named cohort.
Unless overridden, U equals the number of Q births in that cohort; the certificate and complete retained birth ledger agree.
Other births counted in N have proved attachment.
Every required cohort has qualified source completeness, completed reconciliation, a durable certificate and all required pairs.
Unspecified cohorts have N=U=0 and corresponding zero certificates.
Base coverage permits measured numbers unless explicitly overridden.
N_qual is a qualification bound, not another birth count.

B_LOSS names the unchanged base reason supplied for an ordinary capture-loss gap in C15; it is a fixture symbol, not a new product reason.
For answers, report status/reason and omission note as affected_lifetimes/backend-ms; distinguish an omitted zero-count note from note values withheld because inventory is unknown.

C1: J_0:N=200; Q(100,106),Q(103,109); W=[0,1000).

C2: J_0:N=200; Q(100,106),Q(103,109),Q(200,206); W=[0,1000).

C3a: J_0:N=1; Q(100,106); W=[0,1000).
C3b: J_0:N=2; Q(100,106); W=[0,1000).

C4a: J_0 certificate N=0,U=0; W=[0,1000).
C4b: J_0 certificate N=0,U=1, with Q(100,106); W=[0,1000).

C5a: J_0:N=100; Q(100,106); W=[106,200).
C5b: J_0:N=100; Q(100,106); W=[105,106).
C5c: J_0:N=100; Q(100,106); W=[99,100).

C6: J_0:N=3; Q(998,1004),Q(999,1005),Q(999,1006); J_1:N=0; W=[1000,1005).

C7a: J_0:N=1; Q(100,1100); W=[100,1100).
C7b: J_0:N=1; Q(100,1100.000001); W=[100,1100).

C8a: J_0:N=1; Q(100,106,200); W=[0,1000).
C8b: J_0:N=1; Q(100,106,200.000001); W=[0,1000).
C8c: J_0:N=1; Q(100,115.318503,291.490332); W=[0,1000).

C9: For every integer k=0 through 3599, J_k:N=200 and two omissions Q(J_k.start+100,J_k.start+106),Q(J_k.start+103,J_k.start+109); W=[0,3600000).

C10: C9's complete inputs, plus Q(J_1800.start+200,J_1800.start+206); J_1800 still has N=200.

C11: J_0:N=200; Q(5i,5i+2,1001) for every integer i=0 through 199; W=[0,3600000).

C12: J_0:N=200 with Q(500+5i,502+5i,1501), and J_1:N=200 with Q(1000+5i,1002+5i,1501), for every integer i=0 through 99 in each cohort; W=[0,2000).

C13: No births and no omissions; a 1000 ms daemon pause occurs inside W=[0,2000); base coverage is complete and both zero certificates are retained.

C14a: J_0 has complete birth accounting N=200 and retained pairs Q(100,106),Q(103,109). A different birth at 300 has neither attachment proof nor terminal disposition in the live ledger. The other 197 births have attachment proof. No final J_0 certificate exists. W=[0,1000).
C14b: C1's inputs, except retention removed Q(100,106)'s GAP_CLOSE; its GAP_OPEN and J_0 certificate remain.
C14c: C1's inputs, except retention removed the required J_0 certificate; no live unfinished-reconciliation record exists.

C15: C1's inputs plus an ordinary capture-loss gap [500,510), for which base coverage returns Unavailable with reason B_LOSS; the A2 omission inventory itself remains complete.

C16: J_0:N=200,U=0; J_1:N=1 with Q(1000,1006); W=[1000,2000).

C17: J_0:N=1; Q(100,100,101); W=[0,1000).

C18a: C2's exact evidence and W, requested through an enabled raw route.
C18b: C2's exact evidence and W, requested through an enabled summary route.
C18c: C2's exact evidence and W, requested through an enabled server route.
C18d: C2's exact evidence and W, requested through an enabled CLI route.
C18e: C2's exact evidence and W, requested through an enabled UI route.
C18f: C2's exact evidence and W, requested as one chart bucket.

C19a: J_0:N=1; Q(100,1100.000001); W=[500,1000).
C19b: J_0:N=1; Q(100,1100.000001); W=[0,50).
C19c: J_0:N=1; Q(100,106,200.000001); W=[500,1000).

C20a: J_0:N=1; Q(100,238.028412,291.721244); this is the first observed birth and its receive time is the daemon's first ring receive; W=[0,1000).
C20b: J_0:N=1; Q(100,115.318503,291.490332); this is the first observed birth and its receive time is the daemon's first ring receive; W=[0,1000).

C21: boot_start=100; a decoded A2 pair claims fork=99,exit=106,receive=107, overriding the valid-birth default. The certificate for [100,1100) says N=U=0. W=[100,200).

C22a: J_0:N=3; Q(100,100),Q(101,101),Q(102,102); W=[500,600).
C22b: J_0:N=3; Q(100,100),Q(101,101),Q(102,102); J_1:N=0; W=[1000,1100).

C23: J_0:N=201; Q(100,106); N_qual remains 200; W=[0,1000).

C24: J_0 certificate says N=2,U=1; Q(100,106); complete retained FORK evidence establishes three distinct births, with the other two attached; W=[0,1000).

Fixture clarifications (lead, after the oracle round): after a stated CHECK, every later observation retains that vector (as in M54a). The stipulated qualification identity is the symbol QSTIP.
