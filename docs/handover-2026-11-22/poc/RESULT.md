# PoC result, 2026-11-22: stopped before any code, on both independent attacks

No PoC code was written and no machine time was spent on it. Two independent reviewers attacked the idea and the plan first (attack-fable.md, attack-opus.md in this folder). Both say stop, for the same reasons, from the code and from measurements already in the repository:

1. The idea is not new. Kernel-side accounting without a stream ships today as `--mode full --lightweight`; per-(pid,event) and per-(query,event) kernel maps were removed in February (commit fe405a01) in favour of the stream.
2. The trap is the cost, not the stream. A bare watchpoint hit costs 4.7 to 6.7 microseconds with no program attached (docs/handover-2026-11-10/HANDOVER-REVIEW-LEAD.md). Measured on 17 November (tests/results/session-20261117/overhead-levers/table.txt, n=3 per cell): removing the stream changes throughput at saturation by about -1 to +3 points; removing the per-query probes changes it by 22 to 29 points.
3. "Nothing can be lost" was false as stated (torn reads on 4.18, map capacity under churn, a watchpoint that stops firing, waits that never touch the watched word).
4. Per-query attribution needs the extra probes the product already uses; reading the query id in the trap was tried and removed in March for giving stale ids.
5. About half of the interface (percentiles, timelines, executions, lock chains) needs individual events, so the stream stays the source of truth there.
6. Four of the plan's eight kill criteria could not fail (H1, H3, H4, H6) and two would fail for the wrong reason (H2, H5).

The plan's own H6 throttled criterion (at most +10% CPU per transaction) is already failed by the 17 November numbers for the stream-free mode, by the reviewer's division of the medians: about +20% on Ubuntu and +17% on EL8.

What stays open and is the owner's to judge: whether the cost of trapping every transition is acceptable (see the table in the lead's message of this date), and one hypothesis nobody has measured: every number above was taken on cloud virtual machines, where a debug trap also costs a trip through the hypervisor; the cost on bare metal is not known.
