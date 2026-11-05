# Retired summary v9 fixture (#763)

`current.summary` is a finalized, unmodified file emitted by the real
pre-#766 writer at `fccf5c96f91baa457c44559793fee32dd0afb05e`
(`9b424ec8^`), including that revision's `summary_writer.h`. Its SHA-256 is
`23cb7e097d4a0fe2aafd74de05b44549e742718b070bfa212f7561d662443418`.
It is not a current payload with a patched version number.

To reproduce, compile that revision's `src/summary_writer.c` with its header
and `-DPGWT_SERVER -Isrc`, then link `tests/gen_test_traces.c` with that
object in place of `build/server_summary_writer.o` (the other objects and
`-llz4` are the same as the `tests/Makefile` generator target). Generate with
`-s scenario.json -o output`, using the JSON returned by
`v9_wait_spin_scenario()` in `tests/test_data_summary_honesty.py`. Keep only
`output/current.summary`: its footer supplies its index, so no meta file is
needed. The generator pins wall and monotonic clocks to the scenario.

The input spans 130 seconds starting at 10,000,000,000,000 ns, with a client
backend (PID 41001). Every second has CMD_START at 100 ms, an UNKNOWN CPU gap
ending at 150 ms (50 ms wall), a Lock wait ending at 160 ms (10 ms wall,
2 ms measured CPU), and CMD_END at 161 ms. All intervals are in command.
The v9 writer therefore records 130 CPU observations from wait spin, with
zero measured CPU-gap ns, 6,500 ms UNKNOWN gap wall and 260 ms wait spin.
The raw events are regenerated separately by the current generator.

Section 7 of `test_cpu_split_parity.c` requires both reader and preflight
to refuse these bytes. `test_data_summary_honesty.py` checks v9-only and
mixed v9/v10 windows fall back for the entire request, and a v10-only
window still uses summaries. The AAS response's raw-only `cat` field proves
the route; Time Model supplies the `CPU*` row (AAS has class keys).
