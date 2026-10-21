# #410 — bucket-equalization measurement (fix/answer-contracts)

Not a test, not a gate — a saved, re-runnable measurement. Produced while
implementing branch A of the answer-contracts plan, in response to: "run
both paths on one trace with buckets = range-in-seconds; if they still
diverge, report it and stop" rather than ship a basis qualifier over an
unexplained divergence.

Run it with: `python3 tests/results/issue410-measurement/probe.py`
(needs `pgwt-server` + `tests/gen_test_traces` built — see
`demos/build-server-local.sh` on macOS). It regenerates `results.json`.
`sample-run.txt` is the captured stdout of one run, for a reviewer who does
not want to rebuild the binaries.

## Result

`tests/summary_raw_parity_expected.json` pins `aas/max_aas` divergences at
`buckets=10` (the value `tests/test_summary_raw_parity.py`'s `ask()` uses).
Equalizing to `buckets = range-in-seconds` (one bucket per second, matching
the summary path's native granularity) does **not** make the two paths
agree — it makes the divergence far worse on four of the five windows:

| window | range | buckets=10 raw / summary / diff% | buckets=range-s raw / summary / diff% |
|---|---|---|---|
| fractional_bounds | 160s | 2.970 / 4.501 / 51.6% | 3.501 / 42.501 / **1114.0%** |
| interior_160s | 160s | 3.001 / 4.314 / 43.7% | 3.501 / 42.501 / **1114.0%** |
| shift_1s | 160s | 2.939 / 4.501 / 53.2% | 3.501 / 42.501 / **1114.0%** |
| to_capture_close | 171s | 2.814 / 4.124 / 46.6% | 3.501 / 42.501 / **1114.0%** |
| straddle_130s | 130s | 2.886 / 2.886 / 0.0% | 3.501 / 4.501 / **28.6%** |

Conclusion: this is **accounting, not basis**. A pure basis mismatch
(coarse-bucket average vs per-second peak) should shrink or vanish once
the two paths use the same bucket width; instead it grows by more than
20x on four windows and appears from nothing on the fifth.

## Mechanism (confirmed by review against source)

`aas_summary_visitor` (`src/compute.c:2424`) deposits a record's **whole**
`class_ns` into the single bucket containing its *closing* second. The raw
path (`src/compute.c:935-937`) clips each event's contribution across
**every** bucket it overlaps. The fixture's pid 1001 carries a 40 s
`IO:DataFileRead` wait (`tests/test_summary_raw_parity.py`'s
`build_capture()`, `ev.append({"pid": 1001, ..., "dur": 40 * S, ...})`
at `BASE + 240s`): at 1 s buckets, the summary path credits all 40 s of it
to one second (AAS 40.0 there), while the raw path spreads it across 40
buckets (~1.0 AAS each). 42.501 − 3.501 = 39.000 = 40 − 1 — exactly that
signature. Coarse buckets (the pinned `buckets=10` case) let the lump and
the spread partially cancel inside one 16 s bucket; equalizing to 1 s
separates them and exposes the defect instead of hiding it.

This is the same closing-second attribution issue #379 already names for
other fields; #410's `aas/max_aas` divergence looks like the same
mechanism rather than a second, independent "two meanings for one field"
case. No fix is made here — per the plan, this is a report, not a patch.
