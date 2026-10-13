Issue #355 — cached per-event allocation must count against the byte budget
Branch: agent/block-agg-event-class-totals; gh issue view 355 read first.

FIX AND ALLOCATION AUDIT
bagg_entry_bytes() now adds (size_t)a->ev_cap * sizeof(*a->events).
CAPACITY, not n_events: events_grow() calloc's every slot, including unused
slots, just as pairs_grow()/nodes_grow() do. key_add() likewise allocates
key_cap slots. Neither the readability flag nor live counts free storage.

Audited every allocation in src/block_agg.c against struct pgwt_block_agg
and pgwt_block_agg_free(): pairs, nodes, keys, events are the four owned
allocations. No other uncharged aggregate-owned allocation was found.
pairs_sorted()/nodes_sorted()/events_sorted()/classes_sorted() return arrays
owned by their callers; classes are derived, never stored in the aggregate.
window_from_reader()'s decode buffer is scratch, freed before returning.
The server's outer cache-slot array is separate from these owned tables;
this existing byte counter measures aggregate tables, not total process RSS.

THE TEST AND ITS TEETH
test_bagg_cache.c includes the shipping server.c (renaming only main), links
the normal server core, and directly calls bagg_entry_bytes(), put/get/drop.
Real pgwt_block_agg_build() fixtures have 4096 records and either 8 distinct
events (64 slots/table) or 45 (grown to 128). Both have unused capacity.
It pins the exact charge, one-byte-over-budget refusal with caller ownership,
exact-budget acceptance with ownership transfer, stale removal, and drop.
An additional capacity-only case pins all four terms with live counts zero.

redgreen.sh temporarily drops ONLY the event-capacity term, rebuilds the C
test, requires exit 1, restores server.c, rebuilds, and requires exit 0.
The source is restored even on failure/interruption. redgreen.txt is the
complete output: 9 assertion failures when dropped; 0 when restored.
No epsilon, tolerance, retry, or sleep was added to any test.

BUILD AND UNIT EVIDENCE (2026-10-13)
Linux x86_64, Ubuntu gate-2, kernel 6.8.0-138-generic, isolated source snapshot
at /tmp/pgwt-issue355-258d73f0; no other checkout/worktree was modified.
Both binaries built with make -j2 PGWT_BUILD_VERSION=issue355-uncommitted;
all test targets built with make -C tests -j2. Executed every non-Python
entry in unit_tests.list from tests/: 41 C binaries and 2 shell checks passed,
0 failed. linux-units.txt is the full runner stdout; its final "43" includes
the two shell checks. block-agg-linux.txt is the full aggregate-suite stdout.
The restored source also rebuilt successfully after the mutation experiment.
block-agg-green.txt is the earlier macOS aggregate-suite stdout, built using
the existing tests/results/block-agg-event-class/build.sh. The server cache
test and full C suite evidence use Linux and the real spawn implementation.
The unit-list registration guard also passed locally.
No make check/box-check, PR, merge, or push was performed.

REPRESENTATIVE CHARGE (real allocated capacities of synthetic built blocks)
4096 records, 8 events: pairs 64*24=1536, nodes 64*24=1536, keys 0,
events 64*176=11264: total 14336 bytes (14 KiB), previously 3072 bytes.
At 4096 such blocks, events alone are 44 MiB; all tables total 56 MiB.
4096 records, 45 events: 128 slots/table, events 22528 bytes, total 28672
bytes (28 KiB). The 64 MiB byte cap permits at most 2340 such blocks;
4096 would consume 112 MiB in aggregate tables and is now refused.

REPRODUCE ON LINUX, AFTER THE NORMAL PROJECT/TEST BUILD
  sh tests/results/issue355/redgreen.sh
  tests/test_bagg_cache
  (cd tests && ./test_block_agg)
