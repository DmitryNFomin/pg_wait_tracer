# Golden trace fixtures (TST-10)

These are **committed on-disk trace bytes**, frozen in git, that pin the trace
format's backward compatibility. `tests/test_golden_fixture.c` decodes them with
the current reader and checksums the recovered event stream: if any future
change alters how already-written files decode, the test fails.

This is the one format test that is NOT a same-code round-trip — every other
format test writes and reads with today's code, so a change that rewrites the
on-disk bytes while keeping the write/read pair self-consistent slips past them.
These frozen bytes cannot move with the code.

## Layout

```
tests/fixtures/golden/
  rev<N>/
    scenario.json     # the deterministic input (documents what the bytes are)
    trace/            # the generated, committed trace directory
      current.trace          current.trace.meta
      current.summary        current.summary.meta
      backends.jsonl         query_texts.jsonl
```

`<N>` is the trace format version — `PGWT_TRACE_VERSION` in
`src/event_writer.h` (currently 4; `rev2/`, `rev3/` and `rev4/` are retained).

## Determinism

`gen_test_traces` is byte-deterministic for a fixed scenario: timestamps are the
literal values in `scenario.json`, file headers are patched to a synthetic
clock domain (`mono == wall`, anchored to the first event), and the sidecar
`.jsonl` files use fixed timestamps. There is no wall-clock or random content.
Regenerating from the same `scenario.json` reproduces identical bytes.

## Regenerating rev2 (only if the input scenario itself must change)

```bash
make -C tests gen_test_traces
cd tests
rm -rf fixtures/golden/rev2/trace && mkdir -p fixtures/golden/rev2/trace
./gen_test_traces -o fixtures/golden/rev2/trace -s fixtures/golden/rev2/scenario.json
./test_golden_fixture          # prints the new blocks/events/checksum
```

Then update `GOLDEN_REV2_*` in `tests/test_golden_fixture.c` to the printed
values and commit both the regenerated `trace/` and the test. Do this ONLY for a
deliberate fixture change — a checksum mismatch on an UNCHANGED fixture means the
reader/writer changed the on-disk format and is the exact regression this guards.

## Intentional format bumps — ADD, never replace

When you deliberately change the on-disk format (new `PGWT_TRACE_VERSION`):

1. Keep `rev2/` and its pinned checksum exactly as-is — a current reader must
   keep decoding files written by every shipped version.
2. Add `rev<N+1>/scenario.json`, generate `rev<N+1>/trace/`, and add a
   `test_golden_rev<N+1>()` case with its own pinned checksum.

The point is cumulative coverage: every historical on-disk shape stays tested.

## rev4 (#863)

Uses the same input as rev3, with the v4 writer: 36-byte raw file header,
48-byte raw block headers, nominal cadence 100000000 ns, and per-block
maximum weights. The frozen checksum includes the reader-derived sample
weights. Legacy rev2/rev3 checksums continue to checksum on-disk columns
(sample durations remain zero for those checksums), with a separate frozen
`sample_weights_v3` fixture checking the unified weight API.
