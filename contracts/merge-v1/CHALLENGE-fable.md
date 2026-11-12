Fable's answer to fable-q1.md (verbatim, 2026-11-11)

PROCEED WITH CHANGES. The contract's direction is right; it is roughly twice the size it needs to be, two of its premises are unverified, and the acceptance thresholds sit at the noise floor.

## What I read
- R1. src/bpf/pg_wait_tracer.bpf.c:245-255: emit_event uses bpf_ringbuf_output with an already-stamped struct, so the timestamp precedes reservation. One counter covers all sites.
- R2. :652-672: on_bootstrap reserves on lifecycle_rb first and stamps ev->timestamp after. Its failure path has no counter (if (ev) {...} with no else).
- R3. No init_drop counter exists anywhere in src/.
- R4. :642: bpf_map_update_elem(&state_map…) sits before :652, so inside the watchpoint program. :501: accum_add updates accum_map on the lightweight path.
- R5. EL8 arch/x86/kernel/traps.c:896 nmi_enter(), :933 notify_die(DIE_DEBUG…), :944 cond_local_irq_enable. The breakpoint notifier, and so the BPF program, runs before interrupts are re-enabled.
- R6. EL8 kernel/bpf/ringbuf.c:322-327 is the trylock under in_nmi(). The same function carries pending_pos and stops at a BUSY header (:329-340).
- R7. src/event_sort.h:57-65: consumers depend on captured order at equal timestamps between a marker and an event.
- R8. Storage draft evidence-v1.md:13 ("no event sequence number"), :44 (FRONTIER about 1 Hz), :79 and :141 (global VOID, no per-backend exemption).
- R9. Issue #871 M0b: c16 runs of 14,413,750 and 14,053,788 events over 126 s on 4 vCPU; tps 3369.0 vs 3287.3 for the same configuration.

## 1. SEAL
Agree a boundary is required. Disagree that it is one unsolved mechanism; it splits by source.
- Ring sources: if every producer stamps after reserve, an in-band seal record ("read w, reserve, submit(w)") certifies everything with t < w once consumed. INIT already stamps after reserve; emit_event does not and must be reordered.
- Perf sources: the producer runs with interrupts off, arriving only from user mode, so a busy CPU's own FIFO seals itself below its newest record; an idle CPU needs one in-band seal record from a non-NMI BPF program on that CPU.
- UNVERIFIED for 6.8: that notify_debug runs before local_irq_enable in exc_debug_user.
- Clock: take w with bpf_ktime_get_ns inside BPF. Count seal violations and per-CPU regressions; treat one as VOID, not daemon exit.

## 2. bseq
Under-supported. One backend is one thread, so per-backend timestamps are already non-decreasing; bseq matters only at an exact-nanosecond same-pid tie across two sources. Measure first: count same-pid equal-timestamp marker/transition pairs in existing traces. If zero, make such a tie a counted FAULT; if nonzero, bseq is a st->seq++ in state_map plus 8 bytes per record.

## 3. INIT
Exposed, and its failure is silent (no counter), so INIT loss on EL8 is unmeasured, not zero; the M0b runs had no connection churn. Add the counter and run a churn cell. Hash-map updates from the watchpoint program in NMI context are also exposed and unread for EL8.

## 4. Global VOID
Agree for v1, but only tolerable if residual loss is about zero: any normal-load loss fails acceptance. Keep per-site perf failure diagnostics outside the 15-slot vector.

## 5. Acceptance and scope
- Zero loss at EL8 c16, five repeats: right.
- 1 MiB per CPU: about 2 MB/s per CPU means 1 MiB absorbs about half a second of daemon stall vs about 9 s for today's 64 MiB ring. Size for equal stall tolerance; report max daemon loop gap.
- Overhead thresholds 0.97/1.03: at the noise floor; two identical c16 runs were 2.5% apart. Run master vs master first and derive thresholds from that spread.
- Report EL8 proposed vs no-tracer too.
- Specify perf wakeup/watermark policy.
- 50 ms frontier age / 10 ms boundaries: unsourced new requirement; measure at 10 ms, 100 ms, 1 s; do not gate.
- Scope is the fix, not a prototype. The prototype needs: watchpoint move, per-site failure counters, merge with real writer/live callbacks, seal-violation counters. Stop, rotation, de-escalation, CPU offline, INIT move, evidence-aware consumers and MAX_CPUS belong to step 3.

## 6. Other
- Section 6 too harsh: kernel-permitted clock behaviour should VOID and re-attach, not exit.
- Per-CPU ring buffers (array-of-maps) never priced: a user-mode #DB cannot find its own CPU's ring lock held, so the trylock never fails; keeps reserve/submit and the cheap seal; removes lost-record machinery. Unverified whether EL8 accepts a ring buffer as an inner map. The owner approved perf buffers, so carry it to him with the prototype result, as a fourth arm if cheap.
- Not stated whether Ubuntu also moves transport.
- Overclaims to avoid: "Bootstrap must move too" (exposed by source, loss unmeasured), and restating M0b as showing only the transition site drops.
