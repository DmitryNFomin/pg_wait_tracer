# Lead's position on HANDOVER-tracer-work.md

Written 2026-10-22 by the orchestrator session, after both advisers reviewed
`tests/results/cpu-limit-and-locktail-20261021/HANDOVER-tracer-work.md` independently.
Their verbatim reviews are the primary sources; this document is **my own judgement**, which is
the one artifact in this process that no fresh reviewer checks. So it states explicitly what I
verified myself and what I am relaying.

- Fable's review: `/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/258d73f0-4867-4c95-9aca-9a7966648e2b/scratchpad/a-handover-fable.md`
- codex (astra)'s review: `/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/258d73f0-4867-4c95-9aca-9a7966648e2b/scratchpad/a-handover-codex.md`

## 0. Verification status of THIS document

**Verified by me, in-session, against `origin/master` = `af93c35`:**

- Fable's per-transaction normalisation of bug 1. `0.65 cores / 1,400 tx/s` = **464 us/tx**;
  `3.07/3551 - 2.63/4897` = **327 us/tx**. Spread **1.42x** — same order.
- Closed-loop TPS loss `4897 -> 3551` = **27.5%**, against the README's 29%. Matches.
- `lock_chains` appears only in `src/compute.{c,h}` and `src/server.c`; **no `web/` file**
  references it.
- The `Off-CPU` rows appear only in `src/` (`output.c`, `compute.c`, `control.c`, `daemon.c`,
  `server.c`, `summary_writer.h`); **no `web/` file**.
- The merge-window rule at `docs/DEMO_REHEARSAL_CRITERIA.md:1175-1180`: the clean-attempt counter
  resets only on a merge **after attempt 1 starts**.
- The demo reads traces on the box that wrote them: `docs/DEMO_MAC_WALK_CHECKLIST.md:24` puts only
  Chrome and the Go bridge on the Mac, with the daemon over ssh.

**Relayed, NOT independently verified by me:** every per-bug source citation in both reviews, and
all of F1's kernel semantics. Two advisers reading independently agreed on most of it, which is
evidence but not proof. Where only one adviser verified something, I say so below.

## 1. Where I agree with the advisers

1. **F1 cannot ship before the tag.** I agree, and for a reason that is mechanical rather than a
   judgement call: F1 adds a time-model row, which changes the JSON, which touches
   `test_protocol_drift`, `mock_server`, `web/` and the snapshot baselines. That makes it a `web/`
   branch, and this repo allows **one** `web/` branch at a time; it would also need a CI baseline
   regeneration. Independently, both advisers put it over the 12-hour cap. Two independent reasons
   to defer, either sufficient.

2. **Bug 2's proposed remedy is wrong.** Excluding the `Extension` class would hide real client
   work (postgres_fdw, dblink). I agree, and the repo's own record supports it: `MORNING-STATUS.md`
   records #439 as having "turned out to be *labelling*, not exclusion", and
   `docs/AAS_SEMANTICS_DECISION.md` already decides that background work legitimately belongs in
   total DB Time. So bug 2 is the CLI half of a question this project already answered — the axis
   is `backend_type`, not wait class.

3. **The acceptance criteria that cannot fail are defects, not nitpicks.** This project found six
   tests that could not fail in the last week. A criterion that stays green against a broken
   implementation is worse than no criterion, because it is cited as evidence. The "<=3% of cgroup
   CPU" one is the clearest: the daemon runs **outside** the cgroup being measured.

4. **The exclusivity claims should go.** "pg_wait_sampling cannot ever" and "the only
   PostgreSQL-aware observer outside the cgroup" are both stronger than the evidence.
   `runqlat` plus a pid map already supplies the raw signal. The defensible claim — and it is still
   a strong one — is that F1 is the only thing that attributes runnable time **per wait label**.

5. **ClientRead's runnable part must not become DB Time directly.** It would break the
   idle-event => not-DB-Time invariant that `idle_rule.c` and #487's new idle flag encode. I agree
   it needs its own pseudo-event. I would add: we merged that idle flag *today*, so this is a live
   invariant, not a historical one.

## 2. Where I disagree, or go further

1. **On bug 1's proportionality I side with Fable, and more firmly than Fable put it.** I checked
   the arithmetic. The handover's claim rests on comparing cores at two different throughputs;
   normalised, the spread is 1.42x, not the inverse relationship the document implies. codex kept
   "nonlinearity plausible", but codex did not normalise — so on this specific point I treat
   Fable's position as the verified one and codex's as unexamined. **The handover's sentence should
   be deleted, not softened.**

2. **On the Intel repeat I disagree with both, mildly.** Fable says it cannot discriminate (KVM
   re-intercepts DR6/DR7 on both vendors) and proposes a microbench; codex says run it anyway as
   narrowing. My call: **run the microbench first** because it is ~10 minutes and answers the
   question the 12 us figure was trying to answer (per-fire cost), then run the Intel pair,
   because the box already exists and the run is nearly free. Not either/or — ordered. Neither
   adviser is wrong; they are weighing cost against information differently, and the cost here is
   small enough that the ordering dissolves the disagreement.

3. **On bug 4 I side with codex against Fable.** codex's explanation is specific and falsifiable:
   closed #464 fixed duplicate executions for new captures in `b7228ea`, and the handover's trace
   comes from the **2026-10-14 build**, which predates it. That is testable in one re-capture.
   Fable treated it as new and unexplained. I would not spend an implementer on bug 4 until the
   re-capture says it still reproduces. Fable's separate finding stands either way: the "2x" is a
   denominator mismatch (whole-trace rows vs a 60 s pgss window), real ratio ~1.3x.

4. **I weight bug 3 above everything else in the list**, higher than either adviser's ordering
   implies. It is a *silently wrong number on a path a user would reasonably trust* — CLI replay
   reports CPU* 22.4% where live reports 4.3%. Both advisers verified it in source and both
   confirmed `pgwt-server` is correct, so the browser path is unaffected. That bounds the damage
   but does not reduce the severity of the CLI being wrong.

5. **I am less worried about bug 1 for the talk than the advisers are, and more worried about the
   sentence describing it.** The overhead is real and measured; the risk is not the number but
   stating a *cause* we have not established. The talk can say "we measured +0.65 cores at fixed
   rate on this VM; cause under investigation" and lose nothing.

## 2a. UPDATE 2026-10-22: bug 1 is largely settled, and it corrects me

A presentation agent's triage at `tests/results/overhead-triage-20261022/` answers most of #502.
This supersedes my position 5 below and one thing I told the owner.

**Measured** (`raw/bpbench.c`, no BPF, no PostgreSQL, 3 runs per box):

| Box | CPU | Cost per watchpoint fire |
|---|---|---|
| gate-3 (cpx32) | AMD EPYC-Genoa, KVM | 4,722 / 4,669 / 4,657 ns |
| gate-2 (cx33) | AMD EPYC-Rome, KVM | 5,214 / 5,246 / 5,232 ns |

So **~5 us per fire, not 200-300 ns** — roughly 20x the documented figure, on these VMs.

**I was wrong in one specific thing I said to the owner.** I said that if per-fire cost came out
near 200-300 ns the watchpoint would be exonerated. It did not, so the watchpoint is implicated —
but the more useful finding is that it is **not the bigger half**. `perf record -a -g` attributes
the whole +0.66 cores to two trap paths:

- `asm_exc_int3` -> uprobe handling: **9.8%, ~0.39 cores** (~6 uprobe hits per statement)
- `asm_exc_debug` -> `hw_breakpoint_exceptions_notify`, `pv_native_set/get_debugreg`: **8.3%, ~0.33 cores**

**Uprobes cost more than watchpoints.** That redirects the product lever: reducing uprobe hits per
statement matters more than anything about watchpoints, which makes #108 (attach per-execution
USDT probes only during escalation windows) the relevant issue rather than a watchpoint change.

Also settled, against the handover's suspect list: `--skip-query-id` and
`PGWT_TEST_NO_SCHED_ONCPU=1` changed nothing, so query-id reads and sched_switch tracking are
irrelevant; io workers and auxiliary processes carry none of the cost (confirming Fable); the
sampled tier costs nothing measurable; `--lightweight` removes about a third.

**Still open, and now running:** the Intel comparison. I verified gate-1 is an **Intel Xeon
Skylake** while gate-2 is AMD EPYC-Rome, so this is free on an idle box. It settles the one
adviser disagreement that was argued in the abstract — Fable said an Intel repeat cannot
discriminate because KVM re-intercepts DR6/DR7 on both vendors; codex said it narrows the problem.
A comparable ~5 us on Intel supports Fable; materially lower supports a vendor-specific effect.

**Caveat on the triage itself:** its microbench is n=3, but its PostgreSQL table (section 2) is
**single runs**, and it used the 2026-10-14 build. The per-fire number is the solid part.

**The talk sentence this licenses:** "on these AMD KVM VMs one trap costs ~5 us, so the exact tier
cost 0.65 cores at 1,400 TPC-B tx/s; bare metal or another hypervisor may differ." Do not claim a
cause beyond trap cost — whether KVM intercepts the DR accesses is still unverified from the host
side.

### 2a-bis. UPDATE 2026-10-22, later: the Intel comparison, and two corrections to ME

Measured on all three boxes we own, n=3 each, bare watchpoint, no BPF, no PostgreSQL
(`tests/results/issue502-intel-comparison/`):

| Box | CPU | ns/fire (median) |
|---|---|---|
| gate-1 | **Intel** Xeon Skylake, KVM | **6,699** |
| gate-2 | AMD EPYC-Rome, KVM | 5,235 |
| gate-3 | AMD EPYC-Genoa, KVM | 4,669 |

**Intel is the most expensive, not the cheapest** (1.28x gate-2, 1.43x gate-3). This supports
Fable's position that the repeat cannot discriminate the KVM DR6/DR7 hypothesis, while delivering
codex's narrowing: an **AMD-specific cause is excluded**. Both advisers were partly right, and the
data settles it in a way neither predicted.

**The production arithmetic now closes with no residue.** At n=3 the cgroup delta is **+0.700
cores** (spread 0.045; the owner's n=1 +0.65 sits 6% below, inside the spread). Client backends
carry **99%** of it, auxiliary processes 2%, context switches equal. Against 52,631 uprobe fires/s
and 79,339 ring events/s that implies **5.30-8.82 us per trap**, versus 5.17-5.40 us measured bare
on the same box. Trap count x platform trap cost explains the whole delta.

**Correction 1 to what I wrote above and told the owner.** I said the 12 us figure "is not
comparable" to 200-300 ns. More precisely: it was a **denominator short** — it divided excess CPU
by watchpoint transitions alone, omitting ~52,631 uprobe traps/s. Divided by *all* traps it is
5.30 us, which is the measured bare cost. So the figure was recoverable, not wrong-by-construction,
and **the gap to defend is ~20x (5.3 us vs 200-300 ns), not 40-60x**.

**Correction 2, and this one matters more.** I told the owner suspect (a) — debug-register
save/restore on context switches — was ruled out. **It is not.** An armed-but-never-firing
watchpoint costs ~0 per write but **3.4-4.7 us per schedule-in** on gate-2, i.e. 0.049-0.068 cores
= **7-10% of the delta**. The handover's "~44 us/switch would be needed" is correct as stated, and
my inference from it — that the component is zero — was wrong. It is a real minority contributor.

**Third correction, to the triage and to my own talk sentence.** Per-fire cost is **not a
constant**: 5.26 us at ~190k fires/s rising to 14.23 us at ~5k fires/s on AMD. Any per-fire figure
must state its fire rate.

**Honesty items carried from the measurement, which I am not going to smooth over:** the #390
throttling conclusion is **conditional, not clean** — the agent could not make its own throttle
detector go red (sysctl at 1,000, 30x under the fire rate, produced nothing), so the "not
throttled" claim rests on `fires == writes` in 20/20 arms per box plus exact fire counts, and on an
unverified reading that a throttled event stops counting. Two of its negative controls also failed
to go red (a forced BPF-load failure was accepted; the sample-rate clamp produced no throttle
records). One contaminated bpbench set was discarded and disclosed. The triage's headline numbers
inherit the same throttling caveat.

**Filed from this:** #505 (README's 200-300 ns is 20-35x low on every VM we own) and #506 (no
per-source split of `events_total`, so the watchpoint/uprobe mix can only be inferred; ties to
`docs/ROADMAP_AND_STATUS.md` D6 and to #108).

**Not answerable from here:** the rate dependence beyond the per-schedule DR component needs
host-side `perf kvm stat`, and we are the guest on every box we own.

## 3. What can be done before the tag, and what cannot

The constraint is not effort. It is: **does it touch `web/`?** A `web/` change means the
one-`web/`-branch rule, a snapshot baseline regeneration, and another serialized CI cycle. A
`src/`-only change does not.

### CAN be done before the tag (all `src/`-only, each plausibly under 12 h)

| Item | Why it qualifies |
|---|---|
| **Bug 3 (#495)** — CLI replay ignores `cpu_ns` | `src/event_reader.c` only; I verified the Off-CPU rows live only in `src/` |
| **Bug 8 (#499)** — sidecar written before the directory exists | One-line ordering fix, `src/discovery.c` / `src/event_writer.c` |
| **Bug 9 (#500)** — trace files root-only | `src/event_writer.c` plus docs/default |
| **Bug 7 (#498)** — `system_event` has no Off-CPU* row | Verified `src/output.c` only, no `web/` file |
| **Bug 6 (#497)** — "holder" wording | Verified `lock_chains` exists only in `src/`; no `web/` reference |
| **Bug 1 (#502)** — the microbench | A measurement, no product change at all |

### CANNOT be done before the tag

| Item | The blocker |
|---|---|
| **F1 (#504)** | Adds a time-model row => JSON change => `web/` + snapshots + protocol-drift; and >12 h. Two independent blockers |
| **Bug 5 (#496)** — clip `execution_detail` to boundaries | `src/compute.c`, but it **changes displayed numbers**, so it moves snapshot cells and needs a regeneration. Also the headline (22/97, not 63/97) needs settling first |
| **Bug 2 (#439)** — CLI DB Time with pgws loaded | Belongs on #439's semantics, which is a decided contract; changing attribution mid-window risks the conservation checks the rehearsals assert |
| **Bug 4 (#503)** | Not a fix at all until a re-capture says it reproduces |

**My recommendation:** do **none** of the "CAN" list before the tag either, except the bug 1
microbench, which changes no code. Not because they are risky individually, but because the window
is one E-branch away from closing, and each merge adds a CI cycle on boxes that are also the CI
runners. Land them immediately **after** the tag, as one `src/`-only batch. The microbench runs now
because it decides a sentence in the talk and touches nothing.

## 4. The fix list, ordered

All filed 2026-10-22 as **#495-#504** plus a comment on #439. Issue numbers inline below.

Priority is **data correctness first, then UX**, per the owner's two axes.

| Rank | Item | Axis | Window |
|---|---|---|---|
| 1 | **Bug 3 — #495** — replay CPU* silently wrong | data | after tag |
| 2 | **Bug 5 — #496** — `execution_detail` unclipped; summed details wrong | data | after tag (snapshot impact) |
| 3 | **Bug 2 — comment on #439** — collector idle time as client DB Time (CLI half) | data | after tag |
| 4 | **Bug 1 — #502** — cause largely SETTLED (see 2a); uprobes are the bigger half | cost/claim | Intel comparison running |
| 5 | **Bug 4 — #503** — re-capture, then decide if a bug remains | data | re-capture after tag |
| 6 | **Bug 8 — #499** — dynamic PG17+ name provenance lost | data (narrow) | after tag |
| 7 | **Bug 6 — #497** — "holder" asserted as fact | UX / data-if-asserted | after tag |
| 8 | **Bug 7 — #498** — `system_event` cannot sum to 100% | UX | after tag |
| 9 | **Bug 9 — #500** — trace readable by root only | UX / deployment | after tag |
| 10 | **F1 — #504** — run-delay attribution, split three ways | feature | after tag, >12 h |
| 11 | **#501** — `test_protocol_drift` compares shapes not values | gate integrity | after tag; cross-ref #206 |

## 5. F1: what can be built, and what cannot

**Can be built, as specified:** the exact tier. Read `current->sched_info.run_delay` with guarded
CO-RE; both boundaries are in task context, so the delta is the interval's runnable time. No
`sched_waking` hook needed for the totals — the handover is right about that.

**Can be built, with corrections:** the sampled tier, as a **per-backend and global** measurement.
Exact **per-query** allocation cannot be done from tick sampling, because several transitions may
lie between ticks — the handover already concedes this, and it should stay conceded.

**Cannot be built as specified:**

1. **"CPU wait* (request queued)" counting toward DB Time.** Breaks the idle invariant we merged
   today. Needs its own pseudo-event, and "request queued" overclaims — latch wakeups land in
   ClientRead too.
2. **Three of the four acceptance criteria.** Not "hard" — they cannot go red, so they cannot be
   acceptance criteria. Replace with: a synthetic per-label check (a known Lock wait under a 100 ms
   throttle; its run-delay must equal the throttled time inside it), the free invariant
   `runq_ns <= Off-CPU*` on `we==0` intervals, and daemon rusage plus incremental target CPU with a
   CPU-burning mutation required to fail.
3. **A single 12-hour task.** Splits three ways: BPF + format / sampled tier / UI.

**Unresolved and worth settling before anyone writes code:** whether the kernel in play uses the
newer task-based CFS throttle model, which **dequeues** throttled tasks and would break the
"includes throttle" property. Fable raised it; codex's reading of 6.8 says the delay is booked
regardless. They do not actually conflict — they describe different kernels. Verify against
`cpu.stat throttled_usec` on the target kernel rather than assuming either.

**Worth knowing before planning it:** #408 already provides a "CPU (waiting for a core)" row for
`we==0` gaps, and #194 is the same ClientRead-interval family. Part of this surface exists; F1
should extend it rather than introduce a parallel concept.

## 6. What the talk must not claim

Union of both advisers, and I agree with all of it:

- CLI `--replay` time-model CPU* (bug 3)
- CLI DB Time with pg_wait_sampling loaded (bug 2)
- `lock_chains` "holder" as fact (bug 6)
- Summed `execution_detail` numbers (bug 5)
- The B2 tracer-induced collapse as *diagnosis* rather than a measured limitation
- "+85%" without "at fixed rate; closed-loop on the same box was -27.5% TPS, matching the
  README's 29%" — I verified both numbers

And two omissions to fix in the document itself: every B/A run is **n=1** (the README says so, the
handover does not), and the README's 200-300 ns was itself measured on a KVM VM, so "documented"
is not "bare metal".
