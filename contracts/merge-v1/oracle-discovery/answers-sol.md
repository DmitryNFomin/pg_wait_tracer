D01: D12[U] → D15[T] → F15; b_D=12, fresh-record route; gap=closed@12, pending=0, census complete@5. X duration=3 ns and completed count=1 in [12,16). The completion at 12 is excluded.
why: DECISIONS 37(a), 41–42; merge §3 strict release and successful publication.

D02a: F11 → F12 → D15[T] → F15; b_D=12, quiet-baseline route; gap=closed@12, pending=0, census complete@5. X duration=3 ns, completed count=1 in [12,16).
why: DECISION 37(b) requires completeness strictly beyond 12: S(12) cannot commit the baseline; S(13) can. DECISIONS 41–42.

D02b: F15; b_D=none; gap=open, pending=1, census complete@5. No numerical baseline is established.
why: DECISIONS 36–37 and 44: admission, an ordinary reread and idle boundaries do not establish a backend lower bound.

D02c: D9[U] → D10[T] → F12; b_D=9, fresh-record route; gap=closed@9, pending=0, census complete@5. Completed-X count in [10,13)=0; X duration is unavailable without a supplied endpoint or open-wait snapshot. D10 certifies 1 ns of Y.
why: DECISIONS 37(a), 41–42. READ_D(12,X) neither confirms the earlier seed nor replaces the record-established baseline.

D02d: VOID20[C0,unknown_evidence]; no DATA or FRONTIER; b_D=none; gap=open, pending=1, census complete@5.
why: DECISION 37(b): the explicitly claimed coherent baseline contradicts the intervening CPU accumulation. DECISION 43 preserves fault handling.

D02e: F15; both quiet attempts rejected, attempts=2/2; b_D=none; gap=open, pending=1, census complete@5. READ_D(15,Y) supplies no numerical baseline.
why: DECISIONS 37–38: exhaustion permits valid fresh-record admission but does not guarantee a qualifying record. No post-ADMIT reseed is permitted.

D03: D12[U] → D15[T] → F15, all after AD_D8; b_D=12, fresh-record route; gap=closed@12, pending=0, census complete@5.
why: DECISION 36 holds dependent DATA and its frontier until admission. DECISIONS 37(a), 41 establish the lower bound in validated release order.

D04a-install: VOID20[C0,unknown_evidence]; no DATA or FRONTIER; b_D=none; gap=open, pending=1, census complete@5.
why: DECISIONS 35–36 require initialization and installation visibility before enable; the admission claim is false.

D04a-state: VOID20[C0,unknown_evidence]; no DATA or FRONTIER; b_D=none; gap=open, pending=1, census complete@5.
why: DECISIONS 35–36 prohibit overwriting concurrent producer updates and reject the false preservation claim.

D04a-binding: VOID20[C0,unknown_evidence]; no DATA or FRONTIER; b_D=none; gap=open, pending=1, census complete@5.
why: DECISIONS 33 and 36 require complete lifetime binding; unknown required facts are false.

D04a-address: VOID20[C0,unknown_evidence]; no DATA or FRONTIER; b_D=none; gap=open, pending=1, census complete@5.
why: DECISIONS 36 and 39 require qualified address continuity; unobserved relocation contradicts admission.

D04a-cookie: VOID20[C0,unknown_evidence]; no DATA or FRONTIER; b_D=none; gap=open, pending=1, census complete@5.
why: DECISIONS 32 and 36 require the cookie to come from its allocator partition.

D04b: F15; no admission; b_D=none; gap=open, pending=1, census complete@5.
why: DECISION 33 requires the conservative start-time upper bound to precede boot_start strictly. Equality remains pending.

D04c: VOID20[C0,unknown_evidence]; no DATA or FRONTIER; no discovery lower bound; gap=open, pending=1, census complete@5.
why: DECISIONS 30, 33 and 36: the FD binds O, the numeric-PID read observes R, and an old start timestamp cannot establish common lifetime identity.

D04d: hold; D’s reserved cookie remains burned; the tombstone rejects discovery insertion. b_D=none; gap=open, pending=1, census complete@5.
why: DECISIONS 32, 34 and 40: exit tombstones even without mutable state; no delivered matching terminal witness means no discharge.

D04e: VOID20[Cf,counter_change]; no DATA or FRONTIER; b_D=none; gap=open, pending=1, census complete@5. Both late D insertions are refused; R has no valid emission cookie.
why: DECISIONS 34–35 preserve the tombstone through exit, fork-allocation failure and retries. DECISION 43 and merge §4 preserve the observed counter fault.

D04f: VOID20[C0,allocation_fail]; no DATA or FRONTIER; b_D=none; gap=open, pending=1, census complete@5. Successful allocations are fork=9223372036854775807 and discovery=18446744073709551615; both subsequent requests fail without wrapping or crossing partitions.
why: DECISION 32. The row supplies one fault report, so there is one VOID.

D04g: VOID20[Cs,counter_change]; no DATA or FRONTIER; b_D=none; gap=open, pending=1, census complete@5. The advanced CPU/query/command fields remain unchanged by discovery identity installation.
why: DECISIONS 35 and 43: identity insertion preserves producer-owned state; the separate map failure remains reliably counted.

D07a: F20; b_D=12 and b_H=20, both quiet-baseline route; gap=closed@20, pending=0, census complete@5.
why: DECISIONS 37(b), 41: S(21) permits both commitments; C_close=max(5,12,20)=20.

D07b: F20 → F25; b_D=12 and b_H=20, both quiet-baseline route. After S(21), gap=open with pending=0; after S(26), gap=closed@25, pending=0, census complete@25.
why: DECISION 41: C_close=max(25,12,20)=25, requiring completeness strictly beyond 25.

D08a: F17 → EXIT_H18 → F18; b_D=12, quiet-baseline route; b_H=none, x_H=18 by terminal discharge; gap=closed@18, pending=0, census complete@5. No terminal TRANS or UNTRACED is invented.
why: DECISIONS 37(b), 40–41 and 43. EXIT at 18 is ineligible at S(18) and publishes at S(19).

D08b: F17 → F18; b_D=12, quiet-baseline route; b_H=none, no x_H; gap=open, pending=1, census complete@5.
why: DECISIONS 40–41: PID-only ESRCH supplies no matching terminal discharge.

D09: D12[U] → D15[T] → terminal_D18[T] → EXIT_D18 → FORK_R20 → R22[U] → F22; b_D=12, fresh-record route; b_R=22, fork-seed route under 25c; startup gap=closed@12, pending=0, census complete@5.
why: DECISIONS 34–35 preserve queued D identities and refuse tombstone removal/reinsertion. DECISION 41 retains b_D after exit; merge 25a–25c admit R after D’s ordered EXIT.

D10: D12[U] → D15[T] → F15 → F30; initially b_D=12 and gap=closed@12. At detection time 40, revoke startup base coverage from boot_start, including affected cached answers; gap=open, pending=1 for U, census completeness no longer established. b_U=none; no additional merge VOID is specified by LATE_U alone.
why: DECISION 43 explicitly requires late-unlisted revocation; it supplies no automatic REARM or discovery lower bound.

D13a: VOID20[Ce,counter_change]; no DATA or FRONTIER; b_D=none; gap=open, pending=1, census complete@5.
why: DECISION 43 and merge §4: the real event_ring_drop change suspends the epoch before D12 releases.

D13b: A2=Unavailable(untraced_invalid); omit the omission note. Discovery D contributes neither N_k nor U_k. The evaluator input establishes no discovery lower bound or startup-gap closure; its otherwise-valid base coverage remains as supplied.
why: DECISION 43; coverage-a2 population rules reject UNTRACED with fork_ns<boot_start, and invalid omission inventory withholds note values.

D13c: VOID20[C0,unknown_evidence]; no FORK release or FRONTIER; b_R=none; startup gap=open, pending=1 for the default census candidate D, census complete@5. No qualifying A2 omission is established for R.
why: DECISIONS 30 and 36 prohibit discovery admission for an observed post-start birth; missing fork attachment/INIT evidence is not repaired.

D13d: VOID20[Cw,writer_push_fail]; rejected D12 is absent; no FRONTIER; b_D=none; gap=open, pending=1, census complete@5.
why: DECISIONS 37(a), 41 and 43: failed publication commits neither the fresh-record lower bound nor closure.

D13e: VOID20[C0,unknown_evidence]; no DATA or FRONTIER; b_D=none; gap=open, pending=1, census complete@5.
why: DECISIONS 30 and 36: Q-mode discovery is unqualified, so the claimed qualified admission is false.

Q01: D15[T] → F15; b_D=12, quiet-baseline route; gap=closed@12, pending=0, census complete@5. Supplied X duration=3 ns, completed count=1, CPU=1 ns in [12,16).
why: DECISIONS 37(b), 41–42: the valid coherent seed certifies the subsequent interval without first-record exclusion.

Q02: D15[U] → F15; quiet witness rejected; b_D=15, fresh-record route; gap=closed@15, pending=0, census complete@5. The advanced producer CPU/query/command fields are preserved. X duration/count and CPU for [12,16) are unavailable; D15’s incoming quantities remain uncertified.
why: DECISIONS 35, 37–38 and 42: unequal voluntary counts reject confirmation without rollback or an invented loss.

Q03: D15[U] → F15; quiet witness rejected; b_D=15, fresh-record route; gap=closed@15, pending=0, census complete@5. X duration/count and CPU for [12,16) are unavailable.
why: DECISIONS 37–38: S1=running fails inactivity confirmation; the otherwise-valid final attachment can still use its first fresh record.

Q04: D15[U] → F15; quiet witness rejected; b_D=15, fresh-record route; gap=closed@15, pending=0, census complete@5. X duration/count and CPU for [12,16) are unavailable.
why: DECISION 37(b): equal pcount observations and state letters do not supply the required kernel inactivity proofs.

Q04b: D15[U] → F15; quiet witness rejected; b_D=15, fresh-record route; gap=closed@15, pending=0, census complete@5. X duration/count and CPU for [12,16) are unavailable.
why: DECISION 37(b): equal schedstat values, including all-zero values, are insufficient inactivity evidence.

Q05: D15[U] → F15; quiet witness rejected; no VOID or counter increment from the optional EACCES observation; b_D=15, fresh-record route; gap=closed@15, pending=0, census complete@5. X duration/count and CPU for [12,16) are unavailable.
why: DECISIONS 37–38 and the reconciliation notes treat denied optional quiet proofs as failed candidates; separately required operations remain successful.

Q06: D15[U] → F15; quiet witness rejected; b_D=15, fresh-record route; gap=closed@15, pending=0, census complete@5. X duration/count and CPU for [12,16) are unavailable.
why: DECISIONS 37(b), 39 require single-threaded history throughout the bracket. The independently qualified final fresh-record attachment remains valid.

Q07: D15[T] → F15; b_D=12, quiet-baseline route; gap=closed@12, pending=0, census complete@5. Seed: wait=0, time=12, accumulator=0, CPU base=0, base_valid=true, on_cpu_ts=0. CPU for [12,15)=1 ns.
why: DECISION 37(b): zero is a valid CPU base; execution starts at 14, so measured CPU is 1 ns rather than 3 ns of elapsed time.

Q08: F15; quiet witness rejected; no ADMIT, DATA or VOID; b_D=none; gap=open, pending=1, census complete@5.
why: DECISIONS 37(b), 39 reject the address mismatch. A rejected candidate neither asserts false completed admission nor establishes a baseline.

Q09: D9[U] → F11 → D15[T] → F15; first two attempts rejected, third succeeds. At the first release, b_D=11 by quiet baseline, gap=closed@11, pending=0, census complete@5; baseline remains X@11 with valid CPU base 0 despite D9. At the second release, b_D remains 11 and the current wait baseline advances to 0@15; D15 certifies 4 ns of X.
why: DECISIONS 35, 37(b), 38 and 42 preserve earlier-attempt evidence without allowing it to rebase the admitted generation. S(12) supplies completeness strictly beyond 11.