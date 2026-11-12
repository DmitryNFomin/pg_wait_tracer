# Per-CPU transport measurements (Refs #871)

Linux only: this standalone bench loads no daemon and needs no PostgreSQL. Build
with clang's BPF target, Linux headers, libbpf >= 1.0, libelf, zlib and pthreads.
Use the project's bundled libbpf on EL8. Do not run BPF on macOS.

```sh
make -C tests bench-nmi-transport
sudo tests/bench_nmi_transport/qualify.sh --repeats 3 --seconds 30 \
  --period-ns 10000000 --bytes 67108864 --out /var/tmp/transport-qualification
```

Supply `LIBBPF_INC`, `LIBBPF_LIB`, `CC` and `CLANG` through the environment when
using the bundled library; use absolute paths for recursive make. The output
must be a new directory outside the repository. The protocol saves exact build
commands/output, source/binary/object hashes, running kernel package, full kernel
config, clocksource, raw command/stdout/stderr/exit records, and one summary per
CPU-population/case/repeat cell. Missing required provenance makes the run VOID.
A summary PASS means only that measurement gates passed; see
[QUALIFICATION.md](QUALIFICATION.md) for the separate kernel argument.

Each cell interleaves shared ring, perf, perf + 10 ms boundary, and perf + 100 us
boundary. Repeat order rotates the arms. CPU populations are 1, 4 and all CPUs in
the caller's allowed online affinity; a missing 4-CPU population is VOID, never a
silent skip. The named 4 and all cells both run on a four-CPU box. Cases are idle,
one CPU active, all active, and one writer migrating through the participating
CPUs every 256 stores. Writers run at maximum rate in every active case, including
the 100 us adversarial arm. Idle cases have no DATA writer; migration on one CPU
necessarily degenerates to a pinned writer. Other CPUs remain without DATA
writers in the one-active case; they still get their own boundaries.

Before **each cell**, fresh two-second controls run on the same participating
CPUs, binary and BPF object: perf with one page per CPU and draining paused until
after production, and perf with a boundary deliberately stamped +1 ms. The first
must lose records by attempts minus delivery (a LOST callback is not required);
the second must reach the seal assertion. A non-firing or malformed control makes
that cell VOID. These are separate bounded phases in the same protocol invocation,
so corrupt control records cannot contaminate normal measurements. Controls use
all-active writers even for an idle measurement cell.

All measurement arms, **including the shared baseline**, must reconcile per CPU
and site, lose zero records, and have zero seal violations and timestamp
regressions. Thus the known lossy shared-ring baseline may fail the overall
protocol; the perf results and failures remain separately visible. Boundary arms
must advance every CPU's consumed boundary within 100 ms, including startup and
final silence, and have fires equal helper-successful outputs. Both consumed-time
gaps and boundary-stamp gaps are checked. Equality with a strict boundary is legal;
a repeated boundary stamp does not count as advancement.

The paired perf/shared CPU-ns-per-attempt ratio must be <=1.10 in active cases.
Idle cases have zero attempts and report a null ratio; they instead measure the
boundary-only incremental CPU fraction per participating CPU for each boundary
arm, which must be <=1%.
CPU cost uses participating CPUs' `/proc/stat` busy time (user, nice, system, irq,
softirq, steal; guest is already included in user), including boundary interrupts
of the idle task. Raw ticks, USER_HZ, elapsed time and process CPU time are retained.
The idle delta is boundary-idle busy-ns/elapsed minus perf-idle busy-ns/elapsed,
divided by 1e9 and the participating CPU count. Negative deltas are retained. Use
an otherwise idle box: background work and USER_HZ quantization affect the result;
the script applies the frozen limits without retries or tolerance changes.

An individual run:

```sh
sudo tests/bench_nmi_transport/nmi_transport --transport perf --cpus 4 \
  --case one --period-ns 10000000 --duration 30 --bytes 67108864 \
  --object tests/bench_nmi_transport/transport.bpf.o
```

`--boundary` enables the default 10 ms period; `--period-ns N` enables a specified
period. Without either, perf carries DATA only. Only user-mode, 8-byte write
watchpoints (`exclude_kernel=1`) generate DATA. The boundary is a system-wide,
same-CPU `SEC("perf_event")` program on CPU-clock software events (`pid=-1`,
`cpu=i`, `freq=0`, `exclude_idle=0`). It writes BOUNDARY to the same perf map with
`BPF_F_CURRENT_CPU`. No other records enter these buffers.

The bench ABI is a fixed 64-byte record with timestamp, CPU, TID, per-site attempt
sequence, DATA/BOUNDARY site, and zero padding. A boundary has TID zero. Perf RAW
framing is 80 bytes: header 8, raw length 4, payload 64, trailing kernel padding 4;
the advertised raw length is 68. Payload is unaligned and decoded with `memcpy`.
Kernel framing padding is unrestricted; bench record padding is checked.

BPF counters are per CPU and site: attempts, helper successes, helper failures and
error diagnostics. In JSON, **produced means attempts**, including failures. After
joining writers, disabling/closing boundary events, and draining to empty, the
reader includes initial, internal and trailing attempt-sequence gaps and compares
them with produced minus delivered. The check catches a last failed attempt even
without a subsequent LOST record. `lost_notifications` and `lost_callbacks` are
separate and are never added to helper failures. A helper success does not prove
publication on older kernels (see the kernel argument).

The original `percpu-ring` exploratory arm and `run.sh OUTPUT [REPEATS [SECONDS
[BYTES]]]` remain available; the latter is a measurement driver with its original
skip/load-rejection behavior, **not** the frozen qualification protocol. `--threads`
is retained as an alias for participating `--cpus` with all-active writers. The
per-CPU ring arm creates rings for every possible CPU; perf budgets only
participating CPUs. `--bytes` is total data capacity, normally a power of two;
power-of-two per-buffer capacities sum exactly to it. The `--one-page` control
instead allocates exactly one page per participating CPU. Metadata pages and
framing are outside that byte budget. Ring output uses NO_WAKEUP, with a 10 ms
poll timeout followed by unconditional consume; perf uses a watermark of
min(64 KiB, capacity/8) and the same timeout. No capacity-equivalence claim is made.

Portable checks (also on macOS):

```sh
CPATH=/opt/homebrew/include LIBRARY_PATH=/opt/homebrew/lib \
  make -C tests tools/merge_tie_count test_merge_tie_count test_nmi_transport_reader
./tests/test_merge_tie_count
./tests/test_nmi_transport_reader
python3 -B tests/test_nmi_transport_qualification.py
make -C tests/bench_nmi_transport test-reader
bash -n tests/bench_nmi_transport/run.sh tests/bench_nmi_transport/qualify.sh
```

The test registrations append to master's lists. Reader/accounting and protocol
gates have portable tests; Linux builds, verifier acceptance, control activation
and measurements still require the lead's boxes.
