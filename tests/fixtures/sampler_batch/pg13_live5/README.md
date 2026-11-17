PG13 live5 regression data (Refs #866)

`ubuntu.json` and `el8.json` retain the recorded cohort statements, raw persisted
samples, independent clock probes, churn metric snapshots and all perf tick
marker timestamps from the live5 candidate arms. `source` identifies the
original directory; `source_sha256` hashes each original input. JSON values are
unchanged; tick timestamps are converted from the original decimal seconds to
integer nanoseconds. These fixtures are evidence, not generated expected answers.

The original captures predate per-failure diagnostics, fork/EXIT recording,
incarnation witnesses and statement-return witnesses. Those facts cannot be
reconstructed from aggregate counters, and the fixture deliberately invents
none. The new gate must therefore report all 135 Ubuntu / 66 EL8 failures as
unclassified and RED (87/48 and 43/23 first-pass/gate aggregate counts). It must
retain the Ubuntu PID 13275 attribution at T+1 as RED under (c). Both captures
have 200/200 attribution, no first-to-last sample holes, and 44 / 27 pre-T
sampled points. Those successes cannot offset unexplained failures.

`pg13_live5_recorded_cases` in `python3 -B tests/test_sampler_batch_summary.py`
replays both. Fresh box captures are required to classify the old unknown
population and record the late backend's fork time; these fixtures do not
qualify that evidence retrospectively. Exact rerun commands are in
`../pg13_vm/README.md`.
