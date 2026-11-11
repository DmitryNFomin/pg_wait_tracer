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
