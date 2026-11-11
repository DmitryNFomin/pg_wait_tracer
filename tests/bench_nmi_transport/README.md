# NMI transport measurement (Refs #871)

This Linux-only standalone tool attaches an 8-byte user-write hardware breakpoint
to each pinned writer TID. It does not load the daemon or access PostgreSQL.
It needs clang with the BPF target, Linux development headers, libbpf >= 1.0,
libelf, zlib, pthreads and permission to open hardware breakpoints/load BPF.
Use the daemon's bundled libbpf on EL8, rather than the distribution's old 0.5.

From the repository root, with system libbpf:

```sh
make -C tests bench-nmi-transport
sudo tests/bench_nmi_transport/run.sh /var/tmp/nmi-transport.jsonl 5 30 67108864
```

With an existing bundled libbpf build (absolute paths survive recursive make):

```sh
sudo env LIBBPF_INC="-I$PWD/build/libbpf/root/usr/include -I$PWD/build/libbpf/include/uapi" \
  LIBBPF_LIB="$PWD/build/libbpf/root/usr/lib64/libbpf.a" \
  tests/bench_nmi_transport/run.sh /var/tmp/nmi-transport.jsonl 5 30 67108864
```

An individual run:

```sh
sudo tests/bench_nmi_transport/nmi_transport \
  --object tests/bench_nmi_transport/transport.bpf.o \
  --transport percpu-ring --threads 4 --duration 30 --bytes 67108864
```

`--transport` accepts `shared`, `percpu-ring`, `perf`. Defaults are shared,
30 seconds, all allowed online CPUs, and **64 MiB total data capacity** for each
transport. `--bytes` is a total, not a per-CPU size, and must be a power of two
with at least one page per possible CPU. Capacities are distributed as
power-of-two page multiples whose sum is exactly the requested total. The `buffer_cpus`
array (CPU -1 denotes the shared ring) and per-buffer capacities are printed. Per-CPU rings exist for every
possible CPU; perf buffers exist for each CPU in the process's online affinity
mask. Unused possible CPU rings still count against the ring arm's budget.
Perf metadata pages, ring metadata pages and double virtual mappings are not
part of data capacity; equal bytes do not imply equal framing/stall tolerance.
No sizing equivalence beyond data capacity is claimed.

All rings use NO_WAKEUP, matching the daemon's shared ring. Poll timeout is
10 ms, followed by unconditional consume. Perf uses a byte watermark of
min(64 KiB, buffer capacity / 8) and the same maximum poll timeout. Writers
begin only after every attachment and consumer exists. Each writer also checks
the common deadline every 256 stores, so a saturated drain cannot prolong
production indefinitely. Writers are stopped and
joined, then breakpoints disabled, then consumers drained and counters read.
`hits` counts actual volatile stores; compare it with `attempts` as well as
comparing successful attempts with delivery. No startup stores are counted.

Each process emits one JSON line. `cpus` means allowed online CPUs; `possible_cpus`
also includes CPUs without a writer. `duration` is requested seconds; `elapsed`
includes stopping/joining. `seq_gaps` counts forward-skipped sequence values, including
initial and trailing loss after quiescence; sequence starts at one for each
thread/CPU. `seq_reorders` and `bad_records` are separate diagnostics.
`lost_notifications` sums the lost-record counts reported by PERF_RECORD_LOST;
`lost_callbacks` counts the notifications themselves. They are not added to
helper failures as if independent missing records. `max_drain_gap_ms` is the
largest interval between completed full consumer drain passes, including the
final drain after stopping/joining writers.
Per-CPU delivery and attempts are printed as well as aggregate counters.

Perf RAW framing is an 8-byte event header, a 4-byte raw length, the 64-byte
record, and 4 bytes of trailing kernel padding: the advertised raw length is
68, and the whole event is 80 bytes. The old reader required raw length 64
and rejected every sample before checking its fields. Payload starts at byte
12, not byte 16; `memcpy` avoids alignment assumptions and reads `cpu` at
payload offset 8. The reader requires the exact framing and length, a matching
callback CPU, an in-range participating CPU, the expected TID, nonzero timestamp
and sequence, and zero record padding. Kernel framing padding is unspecified.
`make -C tests/bench_nmi_transport test-reader` tests this decoding on macOS too.

`failures.EAGAIN` and `.ENOSPC` are exact helper return values. A NULL return from
ringbuf reserve has **no errno**; it increments both `other` and the diagnostic
`reserve_null`. The loader disables unused transport maps and sets a read-only
load-time selector, so unsupported ring maps/helpers do not poison the perf arm.
`accepted=true` means map creation, program verification and inner-map installation
succeeded; it is not a claim that a hardware breakpoint could be attached.
Any load rejection emits `accepted=false`, stage, signed libbpf/operation error
and errno, preserves libbpf diagnostics on stderr and exits **3**. Setup/runtime
errors after acceptance exit 1. Invalid command arguments exit 2.

`run.sh OUTPUT [REPEATS [SECONDS [BYTES]]]` builds and interleaves all three arms
for each repeat at 1, 4 and all allowed CPUs; unavailable 4-thread cells are
explicitly skipped on smaller hosts. It refuses existing output files, appends
JSON lines to OUTPUT, and saves verifier diagnostics/run labels to OUTPUT.stderr.
Exit 3 is a measurement result and the matrix continues. Other errors stop it.
Choose output paths outside the repository. Do not run the experiment on macOS.
