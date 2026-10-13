Issue #339 evidence, run on Linux x86_64 (Oracle Linux 9, GCC 11.5.0).

Reproduce in an isolated checkout:

```sh
make pgwt-server
make -C tests test_transitions_loader gen_test_traces
python3 tests/transitions_loader_mutations.py
python3 tests/test_data_window_bound.py
```

`redgreen.txt` records both assertion failures and the restored green runs.
The `>=` mutation changes the multi-file predicate call in
`transitions_from_block_aggs`, leaving the shared helper unchanged. It loses
exactly one record: aggregate 8192 versus raw 8193; restoration gives 8193 on
both paths, with two merged blocks and one decoded boundary block. Removing
the production refusal separately fails the zero-provenance assertion.

The no-CPU-source fixture has 24576 records across three two-block files.
Eight warm windows each require positive merged provenance and compare all
response fields exactly, including double bits, links/nodes and ordering.
The cold decode is cache preparation, followed by a six-block merged answer.
`window-bound.txt` records the existing production-refusal test (43/43).
`nm` also confirmed the test entry point exists in the test binary and is
absent from the production server. `bagg_window_eligible` is unchanged.

This proves equality only for the no-CPU-source fixture. General aggregate
versus tagged-raw equality still requires command-state carry-over; the CPU
fixture continues to show two untagged CPU-to-ClientRead transitions versus
one tagged/served transition.
