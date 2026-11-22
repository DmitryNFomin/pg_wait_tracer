**Coverage spec r2, Amendment A4 (2026-11-20): the evidence-v1 pilot route is outside the coverage label**

The U2b implementer stopped at spec §3 "pilot_wait_metrics ANDs its evidence verdict". Both design advisers answered (Astra, Fable): the rule is wrong as written and is amended, not implemented.

- The pilot route reads only v5 evidence-v1 files; v2/v3/v4/v6 containers are filtered out before its codec. v5 files carry no COVERAGE records, and evidence-v1 carries its own life/coverage/loss verdict per (capture, pid).
- The two certify different things on different clocks: pilot windows are monotonic ns, the coverage verdict is wall-mapped. ANDing would withhold every answer from a v5-only directory, and in a mixed directory would judge a monotonic window against wall boots.
- **Rule:** the pilot response keeps its own shape (`pilot:true`, `contract:"evidence-v1"`, per-metric completeness and reasons) and carries no `fidelity` and no `coverage` key, on success and on `pilot_unsupported`. It bypasses the coverage emitter entirely.
- **Tests:** a dispatch-level case with a real v5 fixture inside a restart directory (the response must equal the v5-alone response and carry neither key), plus two mutants: pilot added to the withhold list, and the coverage emitter re-enabled for the pilot.
- **Stated limit:** with the pilot enabled, the Events page can show a pilot value beside coverage-withheld panels for the same window. It is a different quantity (one backend, one capture, its own certificate), not a second view of one number.
- A stale web comment and two web tests that assume the server ANDs are left for a later web change.

Refs #947
