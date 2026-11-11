Frozen trace produced by the unmodified **eaef7462** (master) writer and
`pgwt_sampler_effective_period`, not the v4 writer. File version 3, PG18;
file clock anchors patched to 10000000000000 ns for reproducibility.

Nominal cadence: 100000000 ns. Tick offsets and record counts:

| offset (ns) | count | supplied weight (ns) |
|---|---|---|
| 0 | 1 | 100000000 (first tick) |
| 90000000 | 1 | 100000000 (legacy floor) |
| 180000000 | 0 | 100000000 (legacy floor; no record) |
| 290000000 | 1 | 110000000 |

The old writer batches the first two samples at 100000000 ns. The reader
must retain those stored weights, including inflation. The third record's
110000000 ns must **not** become the 200000000 ns timestamp difference:
the empty tick is invisible in the file. Never regenerate this fixture with
the current writer. `test_sample_weight` pins its version, timestamps,
weights and block count.
