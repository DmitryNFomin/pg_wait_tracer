# Contracts

Each displayed number has a contract here, stored as the bundle the oracle author worked in,
verbatim: `<metric>-v<N>/contracts/metrics/` (definition with worked examples),
`<metric>-v<N>/contracts/examples/` (inputs), `<metric>-v<N>/contracts/expected/` (expected
answers), `<metric>-v<N>/reference/` (the reference evaluator), `<metric>-v<N>/CHALLENGES.md`
(ambiguities raised before answering) and `<metric>-v<N>/TASK.md` (what the oracle author was told).

Current: `wait_time-v1/` — wait time and completed waits, 48 cases.

The expected answers were written by an oracle author that had only the contract and the inputs,
never the product code, and were checked by a deliberately naive reference evaluator
(`reference/time_grid.py`, which walks small windows one nanosecond at a time). The oracle author
also wrote `hand_answers.py` to transcribe its hand derivations into JSON; each expected file
carries the derivation in plain arithmetic. The challenges the
oracle author raised before answering are kept next to the contract.

Rules (docs/REWORK_PLAN_2026-11-11.md, section 3):
- An implementation change never edits anything under `contracts/`. A contract change is its own
  reviewed change, and it invalidates every result that depended on the previous version.
- The product's output is never used to produce an expected answer.

Reference check: `cd contracts/wait_time-v1 && python3 -I reference/run_all.py`.
