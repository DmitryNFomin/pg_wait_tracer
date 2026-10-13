Evidence for #387 on base ac515f6, macOS arm64, 2026-10-13.

`pgwt_compute_time_model` now sets `has_measured_cpu` only when the measured
record's in-window fraction is positive, in both the CPU-gap and wait branches.
The numeric accumulation expressions and their order are unchanged. Measured
zero remains a signal; no overlap does not. Empty results remain successful
results with `has_measured_cpu=0`; their `offcpu_ms=0` scalar is unavailable,
not a measured zero. No Off-CPU row is emitted.

New C functions: `test_empty_window`, `test_zero_fraction_records`,
`test_nonempty_windows`, `main`, and the unused-summary-streaming link stub
`pgwt_visit_summaries`. The existing Python test function
`test_offcpu_cap_not_applicable_without_measured_cpu` gained the empty-window
and checked-zero comparison. No consumer implementation or threshold changed.

The new binary is registered in tests/Makefile and tests/unit_tests.list.
`build.txt` records the final C build. The four dependency objects were compiled
from src/wait_event.c, idle_rule.c, spawn.c and cJSON.c with
`cc -g -O2 -Wall -DPGWT_SERVER -Isrc -Iinclude -c` into
`/tmp/pgwt-387-build`. For spawn.c only, a temporary `-include` header supplies
macOS's missing pipe2 as pipe + fcntl(FD_CLOEXEC); no production source was
altered, and these tests do not invoke it. This is local C evidence, not a full
Linux/BPF suite run. An attempted broader build stopped at test_variants_p95's
GNU-only `--wrap=realloc` linker flag, so that binary was excluded.

Red/green demonstration: first built and ran the fix green, then removed only
its two guards. `git diff --exit-code -- src/compute.c` confirmed the reverted
source was exactly the base version. Rebuilt and ran
`tests/test_time_model_absence /tmp/pgwt-387-build/before.bin`: `red.txt` records
exit 1 with nine flag failures (eight individual CPU/wait zero-fraction shapes
and their combined array). Restored both guards, rebuilt, and ran
`tests/test_time_model_absence /tmp/pgwt-387-build/restored.bin`: `green.txt`
records exit 0. The truly record-free case passes on both versions.

Both invocations write snapshots of the nine NONEMPTY cases, each asserting
positive DB Time. Cases include full overlap, clipped overlap, measured zero,
wait-only measured CPU, UNKNOWN-only, mixed UNKNOWN and measured records,
clamped CPU, an unrelated zero-fraction measurement, and one-nanosecond overlap.
Snapshots contain the entire zero-initialized result struct (pointer normalized
to NULL) and every allocated result row. `cmp before.bin restored.bin` exited
0: all 5,016 bytes match. `byte-identity.txt` records both identical SHA-256s.
The optional snapshot argument allows this differential to be reproduced.

`regressions.txt` records nine existing C binaries plus the new binary's
separate green evidence, the Python consumer suite (189/189 assertions), and
both registration checks. Python ran in a temporary venv with numpy and Pillow.
`consumer.txt` shows the existing Off-CPU cap returns ok with an explicit
"no Off-CPU* signal to check" reason for absence, distinct from checked zero.
The workload gate's independent minimum-DB-Time floor is unchanged.

No box-check, pull request, push, tolerance, retry, or sleep was used.
