Frozen v2/v3 response expectations from master eaef7462 (this branch's base).

`mixed_v3` was written by master d7f045fd (whose writer is unchanged
from eaef7462) from its
`tests/test_data_fidelity.py:build_mixed()` scenario. Other inputs reuse the
existing frozen golden rev2/rev3 and sample_weights_v3 traces.
`responses.json` retains every deterministic response field; only request id,
wall-clock now_ns and build-specific server_version are omitted. Expectations
must never be regenerated from the server under test. The host-dependent
`info.num_cpus` is asserted against local POSIX sysconf instead of the
generating host's CPU count.

The later master commit d7f045fd adds unrelated `offcpu_aas`/`cpu_ms`
response fields in #857. `master_857_additions` freezes those additions from
that independently built master server. Tests accept either complete master
response body, so the same expectations run before and after merging #857;
all original fields and values must match exactly in either case.

The additive `transitions.nodes[].entered` expectations are derived solely
from each frozen response's own links: sum `value` where `target` equals the
node's `name`, including zero for nodes without incoming links. Every frozen
graph has `truncated: false`, and `link_count == total_link_count == len(links)`;
the link values also sum to `total`. No capped graph needs reconstruction or
exclusion. Sampled-only refusals have no nodes. No server response was used to
derive these additions; all pre-existing fields remain unchanged.

Sweep for other frozen Transitions responses (`tests/*.py`, `tests/fixtures/**`,
the mock server, and golden replay inputs):

- `tests/test_data_fidelity.py` loads the full response bodies in
  `responses.json`, including the golden rev2/rev3 replay expectations updated
  here. Its uncapped response comparisons also assert the incoming-link identity.
- `tests/mock_server.py` already includes `entered`. Its capped graph documents
  the omitted input pairs and their counts; summing only the returned links
  would undercount entries, so those independently specified counts stay intact.
- `tests/test_protocol_drift.py`, `tests/replay_ui_compare.py`,
  `tests/test_replay_ui.py`, and `tests/test_web_ui_snapshots.py` already cover
  the additive field or its presentation after the preceding changes.
- `tests/test_data_transitions.py` pins selected link/count/dwell properties
  and restart equality, rather than frozen node response bodies.
  `tests/test_demo_workload_coverage.py` supplies link-only checker inputs.
- `tests/fixtures/transitions.mjs` and `tests/fixtures/matrix.mjs` re-export
  gallery builder inputs from `web/static/dev/fixtures/`. The former was
  updated previously; the latter supplies partial matrix inputs, not complete
  response expectations. The excluded `web/` files are unchanged here.
- `tests/fixtures/golden/{rev2,rev3,rev4,rev6}/scenario.json` and their `trace/`
  files freeze input records and encoded traces, not Transitions JSON responses.
  `tests/test_golden_fixture.c` checks their decoded columns and checksums.

`make check` compiles `tests/*.py` but does not run `test_data_fidelity.py`.
The synthetic server suite in `tests/run_all.sh` runs it through `box-check`.
It is box-tier registered, not inherently Linux-only: it also runs on macOS
with the BPF-free replay server, trace generator, and sample-weight helper built.
