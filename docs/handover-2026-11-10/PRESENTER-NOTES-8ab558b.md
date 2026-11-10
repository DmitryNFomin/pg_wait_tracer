# Presenter notes — demo candidate `8ab558b`

Written 2026-10-22 from the three-way media analysis (lead + both advisers) of
`demo-video-8ab558b/footage-v2/`, `demo-gifs-8ab558b/` and `demo-zoom-8ab558b/`.

These are the things that are **true but easy to say wrongly**, and the questions the material
invites. Everything here was established by measurement, with the source named.

---

## 1. Say "synthetic lock loop" BEFORE any Lock number

The workload is a deliberate contention generator, and the audience can see it:

- the tables on Queries and Waterfall are named **`_smoke_lock_wait`** and **`_smoke_row_lock_wait`**
- p50 = p99 = **1.0 s exactly** over 182 executions
- `Lock:relation`'s 716.8 s is **one session** (PID 2842220, 100% a single event)

Any of those invites "is this a test fixture?" mid-talk. Saying "this is a synthetic lock loop, so
the numbers are clean by construction" **first** turns that from a catch into a design choice.

## 2. "CPU (waiting for a core)" is a RESIDUAL, not a measured counter

The Overview row reads `CPU (waiting for a core) 107.2 s / 8.6% / AAS 0.12`.

**What it actually is:** `max(0, DB Time − measured CPU − other wait classes)` over the `we==0`
(no-wait-event) intervals, clamped at zero (`src/compute.c`). It therefore contains run-queue delay
**and** anything else uninstrumented — major faults, writeback, throttling.

**Safe wording:** "time inside CPU-labelled intervals that was not on a core — an **upper bound** on
run-queue delay there."

**Do NOT say:** "pgwt measures run-queue delay", or "this is what F1/#504 adds". That would be
false. #504 proposes a *kernel-measured* `sched_info.run_delay` **per interval under every label**
(Lock, IO, ClientRead) — a quantity this row cannot see at all.

**The evidence they are different** is in our own rehearsal: counted attempt 2's
`offcpu_decomposition.jsonl` shows kernel run-delay at **104 / 144 / 125 / 142 / 104 %** of this row
across five 60 s windows — above it in 5 of 5. One adviser's independent read: residual 8168.34 ms
vs kernel run-delay 9366.56 ms (114.67%).

If asked "so can you show me scheduler delay?" the honest answer is: "not per wait label yet — that
is the next feature; today this row bounds it inside CPU intervals only."

## 3. Explain the drill-down's target

The headline GIF's dominant wait is **`Lock:relation` (57.3%)**. The drill-down lands on
**`Lock:transactionid` (7.3%)**, and there is **no visible pivot** connecting them — no query
selection, no SQL on screen. Both advisers flagged this independently as the clips not being one
continuous diagnosis.

Either say why you are changing target, or (if the re-cut succeeds) drill into an execution of the
dominant wait so the two clips tell one story.

## 4. Do NOT present the Transitions graph as a DB-Time ranking

At `footage-v2/transitions/transitions.webm[250]` the graph **omits `Lock:relation`** — the
dominant wait — because it falls under the view's count threshold. The view says so on screen.

It is a transition-topology view, not a ranking. Selling it as "the biggest waits" contradicts the
Overview two tabs earlier, and someone will notice.

## 5. Do NOT sell burst-marker density as anomaly detection

`footage-v2/concurrency/concurrency.webm[250]` is crowded with burst markers. Density here reflects
the synthetic workload's shape, not discriminating detection (see #442).

## 6. The microsecond demonstration — what it shows and what it does not

`demo-zoom-8ab558b/` resolves a **33.2 µs `CPU*` bar across 342 px**, axis stepping in 30 µs, after
three plain drags (1001.8 ms → 138.9 µs). Two independent sides agree: 342 px measured from the
video's pixels, 344.2 px computed from `dur_ns` through the zoom geometry in the retained trace.

**It is a CPU segment, not a WAL wait.** The `IO:WalWrite` / `IO:WalSync` names in the drill-down
header belong to **containing backend activity**, not to the executor bars — measured: **0 of 100**
executions carry an `IO:Wal*` executor event. The on-screen sentence "Bars show executor phase only"
is what scopes it.

This misled three readers (both advisers and me) before a measurement settled it, so expect the
question. Filed as **#523**; the owner has approved re-wording the line.

## 7. Overhead: what we can and cannot claim

**Measured, n=3 per box, bare watchpoint, no BPF, no PostgreSQL:** 6,699 ns/fire on Intel Xeon
Skylake (gate-1), 5,235 ns on AMD EPYC-Rome (gate-2), 4,669 ns on AMD EPYC-Genoa (gate-3).

- **README's 200–300 ns does not hold on any VM we own** — 20–35x low (filed #505).
- **Intel is the most expensive of the three**, so an AMD-specific explanation is excluded.
- **Per-fire cost is not a constant**: 5.26 µs at ~190k fires/s rising to 14.23 µs at ~5k fires/s.
  **Any figure quoted needs its fire rate attached.**
- **Uprobes cost more than watchpoints**: `asm_exc_int3` → uprobe handling ~0.39 cores vs
  `asm_exc_debug` → watchpoint ~0.33 cores. The product lever is fewer uprobe hits per statement
  (~6 today), which is #108.
- All of these are **KVM guests**. Bare metal may differ and we cannot measure it from inside a
  guest. The host-side `perf kvm stat` that would identify whether DR accesses cause VM exits is
  unavailable to us.

**Safe wording:** "on these AMD/Intel KVM VMs one trap costs ~5–7 µs at this fire rate, so the exact
tier cost 0.65 cores at 1,400 TPC-B tx/s. Bare metal or another hypervisor may differ."

**Pair any "+85%" with:** "at fixed rate — closed-loop on the same box was **−27.5% TPS**, which
matches the README's 29%." Those are different quantities (CPU cost vs throughput loss), and quoting
one against the other's baseline is the error the handover document made.

**All B/A runs in the handover are n=1.** The README says so; the handover does not.

## 8. What the material does NOT establish

- That every idle classification is correct — only the shown case. The validator proves a boolean is
  *present*, not that it is *right*; with an empty pacing mask it approves and the old defect returns
  invisibly (#515).
- That `footage-v2`'s matching `"unknown"` versions prove matching **binaries**. They silence the
  skew warning; binary identity rests on the rebuild, which was verified separately
  (61/61 served assets byte-identical).
- Any host-side causal claim about KVM trap interception.
