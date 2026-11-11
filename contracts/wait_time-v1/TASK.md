# Task: independent expected answers for contract wait_time version 1

You are the oracle author. Your working directory contains ONLY a contract and example inputs. You
have never seen the product that will implement this contract and you must not look for it: do not
read, list or search anything outside this directory. Your whole value is that your answers come
from the contract alone. Everything you run is logged and the log will be checked for accesses
outside this directory.

Inputs:
- contracts/metrics/wait_time.md — the definition, with worked examples.
- contracts/examples/wait_window_NN.json — 48 cases: `evidence` (life = [start, end-or-null],
  coverage = list of [a, b), waits = list of [event, start, end], open = [event, start, as_of] or
  null) and `query` (event, from, to). All spans are half-open; times are integer nanoseconds.

Produce, in this directory:

1. CHALLENGES.md — FIRST, before computing anything: every place where the contract is ambiguous,
   contradictory, silent, or where a reasonable reader could compute two different answers for one
   of the 48 cases; and anything you think the definition gets wrong for a tool that reports how
   long a database session waited. For each: the case ids affected, the alternative readings, the
   answer under each reading, and the reading you chose. Do not resolve an ambiguity silently.
2. contracts/expected/wait_window_NN.json for every case: an object
   `{"id": ..., "wait_time": <result object>, "completed_waits": <result object>}` exactly in the
   result-object shape of the contract, derived BY HAND REASONING from the contract. Add a field
   `"derivation"` with two or three lines of arithmetic a human can check.
3. reference/time_grid.py — a deliberately naive, slow, obviously-correct reference evaluator in
   Python 3 (standard library only), written after and independently of step 2: for the small
   cases it must walk the window one nanosecond at a time and classify each instant (in lifetime?
   covered? inside which wait?), with no interval arithmetic shortcuts; for cases whose numbers are
   too large to walk, it must use exact integer interval arithmetic in a separate, clearly marked
   function and say so in its output. It reads a case file and prints the result objects as JSON.
4. reference/run_all.py — runs time_grid.py over all cases and compares with contracts/expected/,
   printing one line per case and a final count. Where your hand answer and your reference
   disagree, do NOT quietly fix one: record the disagreement and which one was wrong and why, in
   CHALLENGES.md under "Self-disagreements".
5. Propose, in CHALLENGES.md under "Missing cases", up to ten further cases (as JSON in the same
   format) that would catch an implementation that is wrong in a way none of the 48 detects.

Finish by printing: the run_all.py final line, the number of challenges, and the number of
self-disagreements.
