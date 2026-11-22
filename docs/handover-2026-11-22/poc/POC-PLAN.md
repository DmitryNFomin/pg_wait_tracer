# PoC: kernel-side accounting of PostgreSQL wait events (plan and kill criteria, written before any run)

Written 2026-11-22 by the lead on the owner's order to run a proof of concept and all needed tests on the gate boxes, and to verify the ideas before anything is built on them.

This is a kill test, not a build. Its only job is to find out, cheaply and with raw numbers, whether the idea below fails. Nothing here goes into the product. No certainty is promised: section 5 says what the PoC cannot prove.

## 1. The idea under test

Today (as the lead understands the product; to be confirmed against the code by the reviewers): a hardware watchpoint on each backend's wait-event word fires on every change; the handler writes an event record into a buffer; user space rebuilds every backend's timeline from that stream and computes all numbers from it. One lost event makes the affected window untrustworthy, so it is withheld.

The idea: keep the trap, change what happens after it.
- The handler updates a per-backend record in a kernel map: current event, since when, and exact accumulated time and count per event. Nothing is sent per event.
- On-CPU, runnable and sleeping time per backend are accounted the same way from the context-switch tracepoint.
- User space reads the records once a second and writes per-second totals, adding the still-open interval.
- An already-running backend is armed, its word is read once, and it is counted from that moment; only the interval in progress at arming is marked as started before tracing.
- The per-event history becomes a second, best-effort channel (long waits; every event for chosen backends), with drops counted in the kernel record. It is out of scope for this PoC.

## 2. How the previous route failed, and the test that must pass for each

| # | Failure of the previous route (measured) | Hypothesis | Test | KILL if |
|---|---|---|---|---|
| H1 | Not a failure, a precondition: EL8 4.18 is a required target | The handler can keep per-backend accounting in a kernel map on 4.18 and on 6.8 | Load a minimal program on both gate boxes and trap one backend | The kernel refuses it on 4.18 and no variant within the PoC loads |
| H2 | Numbers were never checked against a known truth | Totals are exact on a quiet machine | One backend runs a script of sleeps and lock waits whose durations and counts are known from the server's own clock; compare the kernel totals | Any count differs, or a total differs by more than 1% of its true value |
| H3 | Disagreeing views of one quantity | A backend's time adds up | For every traced backend, every second: the sum over all states equals the wall time since arming | The difference exceeds 1 ms for any backend at any second of a 10-minute run |
| H4 | 0.5% of events lost under load on EL8 meant 100% of the window withheld | Nothing is lost under saturation or when the reader stalls | Unthrottled pgbench with more clients than CPUs, 10 minutes, both kernels; the reader is stopped for 30 s in the middle. Compare each backend's transition count in the kernel record with the kernel's own hit counter of the same watchpoint | Any backend's two counts differ, or H3 fails during or after the stall |
| H5 | Sampled CPU was 74% of the kernel's own figure on EL8 (90% on Ubuntu); the exact tier 84% on EL8 | CPU time from the context-switch tracepoint matches the kernel | Same runs: per-backend on-CPU time against the scheduler's own per-task run time | The ratio is outside 0.99 to 1.01 for the total over backends on either kernel |
| H6 | Exact tracing cost about half the throughput at saturation | Accounting without a stream costs less than today's exact tracing, and stays inside the budget used before when throttled | pgbench throughput and CPU per transaction, three arms alternated, three runs each, both kernels: no tracer; this PoC tracing every backend; the product's current exact mode. Throttled and saturated | Saturated: the PoC arm is not better than the product's exact mode. Throttled: CPU per transaction rises by more than 10% over no tracer. Report the saturated loss against no tracer whatever it is: that number is the owner's to judge |
| H7 | Accounting for already-running processes grew to a 190-row proof | Each already-running process is counted from arming with no global condition | Start the PoC against a server with idle connections, busy connections, one connection inside a long lock wait, and the auxiliary processes; then under connection churn | A process alive at start has no record 10 s later; the long wait's time from arming differs from an independent measurement by more than 1%; under churn a process seen by the fork and exit tracepoints has no record |
| H8 | Per-query numbers are a required feature | The query id can be taken when an interval opens | PostgreSQL 17 or 18: read the backend's query id in the handler; fixed script; compare per-query counts with pg_stat_statements; measure the added cost per trap | Counts differ, or the read cannot be done in the handler. PostgreSQL 13 has no such field: state what would be needed, do not build it |

Order: H1, then H2 and H3, then H4 and H5, then H6, then H7, then H8. A KILL on H1 to H4 stops the PoC at once. H6 is reported to the owner whatever it shows.

## 3. Rules that keep this from ending the same way

- The criteria above are fixed before the first run. A criterion is not edited after a run; if one turns out to be wrong, the run is reported as it is and the change is proposed to the owner.
- Every run is reported, including the failed ones, with the command and the raw output. No run is repeated because of its result.
- One implementer writes the PoC. Someone else checks the results against the raw output and tries to break the claims. The lead does not write the code and does not judge it alone.
- Two independent reviewers attack the idea and this plan before any code is written (section 4).
- Machines: only the two gate boxes (Ubuntu 6.8, EL8 4.18), under the non-blocking box lock, in a scratch directory that is removed afterwards. No new servers.
- The PoC lives outside the product tree (a scratch directory and, if it must be kept, a branch that is never merged).
- No product design, contract or implementation work starts until H1 to H7 have a result and the owner has seen it.

## 4. Before any code: the attack on the idea

Two independent reviewers answer, from the code and from what they know of the kernel and PostgreSQL:
1. Is the description of today's design in section 1 right? What does the handler do today, and what state already exists in the kernel?
2. Why will this idea fail? Look for: the per-trap cost (is the stream or the trap the expensive part); what a watchpoint can and cannot see (writes that do not change the value, a backend's word address, process exit, more traps than debug registers); races between the handler, arming and the reader; kernel 4.18 limits; per-query attribution; memory per backend and per query; what the UI needs that totals cannot give.
3. Is each kill criterion able to fail, and is anything missing that would let the PoC pass while the product still ends the same way?

## 5. What this PoC cannot prove

- That the per-event history (timelines, per-query traces) works under load: it stays a best-effort channel with declared gaps.
- Long-run stability, every PostgreSQL version, every workload, the UI.
- That the build afterwards will be on time. It only removes or confirms the technical reasons the last one was not.
