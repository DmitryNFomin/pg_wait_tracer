# Timeline format (`timeline-NNNN.pgwtl`), version 1

`pg_wait_tracer --mode timeline -T <dir>` writes the wait timeline of every
PostgreSQL process into this format (#1006 unit 1).

The timeline lists each process's waits in order: wait 1, wait 2, wait 3,
and so on. Each wait has its event, start and end. It is built from
system-call boundaries plus one uprobe on `WaitEventSetWait` entry.

This document is the contract in prose. `src/timeline_format.h` is the same
contract in C, with static asserts on every size and offset. The two must
agree. The BPF-to-userspace wire (`src/timeline_wire.h`) is described at the
end of this document. That wire is frozen for unit 1 only. The disk format
is the long-lived contract.

## Files

- A capture writes segments named `timeline-NNNN.pgwtl` into the trace
  directory. `NNNN` is the segment index, counting up from 0.
- A segment rotates at 256 MiB.
- `--retention-gb` deletes the oldest segments first.
- Files have mode 0640 and the trace directory's group, the same as trace
  files.
- At stop the tracer writes `timeline-summary.json` and prints a text
  table.
- The UI and the server do not read `.pgwtl` in unit 1.

## Units and encoding

- All integers are little-endian.
- Every time field is **CLOCK_MONOTONIC nanoseconds**. This is the clock of
  `bpf_ktime_get_ns`. The only exception is `wall_start_ns`, which is
  CLOCK_REALTIME.
- Monotonic times can be compared only between captures with the same
  `boot_id`.
- Durations are `end_ns - start_ns`.

## Header (64 bytes, at offset 0 of every segment)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | char[8] | `magic` | `PGWTTL01`, no terminator |
| 8 | u16 | `version` | 1 |
| 10 | u16 | `rec_size` | 64. Readers step by this value. |
| 12 | u32 | `pg_version` | `server_version_num` (e.g. 170004). It is `major*10000` when only the major version is known, and 0 when unknown. |
| 16 | u32 | `postmaster_pid` | |
| 20 | u32 | `caps` | Capability bits, listed below |
| 24 | u64 | `mono_start_ns` | CLOCK_MONOTONIC at capture start |
| 32 | u64 | `wall_start_ns` | CLOCK_REALTIME at the same instant |
| 40 | u8[16] | `boot_id` | `/proc/sys/kernel/random/boot_id` as binary |
| 56 | u32 | `segment_index` | The `NNNN` of the file name |
| 60 | u32 | `reserved` | 0 |

Capability bits (`caps`):

| Bits | Name | Meaning |
|---|---|---|
| 0x1 | `MARKER_PROBE_ATTACHED` | The `WaitEventSetWait` uprobe was attached when the segment was opened |
| 0x6 | `QID_SOURCE` (2-bit field, shift 1) | 0 `NONE`; 1 `ST_QUERY_ID` (`PgBackendStatus.st_query_id`); 2 `PG13_RESERVED` (PG13 synthetic ids, unit 3) |
| 0x8 | `CPU_SPLIT_PRESENT` | `on_cpu_ns` and `runq_ns` hold measured values. Always 0 in unit 1. |

## Record (64 bytes, after the header)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | u64 | `start_ns` | |
| 8 | u64 | `end_ns` | Equals `start_ns` for a `ZERO_MARKER` wait |
| 16 | u64 | `query_id` | 0 means none; the QID flags below say why |
| 24 | u64 | `on_cpu_ns` | The sentinel (all ones) unless `CPU_SPLIT_PRESENT` is set |
| 32 | u64 | `runq_ns` | The sentinel (all ones) unless `CPU_SPLIT_PRESENT` is set |
| 40 | u32 | `pid` | 0 for global kinds |
| 44 | u32 | `wait_event_info` | WAIT: the event. 0 for other kinds. |
| 48 | u32 | `n_syscalls` | WAIT: the number of system calls merged into it |
| 52 | u16 | `kind` | One of the kinds listed below |
| 54 | u16 | `flags` | Flag bits, listed below |
| 56 | u32[2] | `reserved` | 0 |

### Kinds

| Value | Kind | Meaning |
|---|---|---|
| 0 | `WAIT` | One wait of one process |
| 1 | `NO_WAIT` | A gap with no wait event. Reserved for unit 3, which splits it into on-CPU time and time waiting for CPU. |
| 2 | `PROBE_STATE` | A global span (`pid` 0) during which the marker probe was off (`MARKER_OFF`). Reserved for unit 2. |
| 3 | `LOSS` | A span of one `pid` during which raw records were lost. It starts at the end of the last record of that pid that arrived before the loss, and ends at the start of the first record after it. `n_syscalls` is 0. This kind replaces a sticky "after loss" bit. |
| 4 | `BACKEND` | Backend start and exit. Reserved; unit 1 does not write it. |
| 255 | `STATS` | The segment trailer, laid out as shown below |

### Flags

| Bit | Flag | Meaning |
|---|---|---|
| 0x01 | `MARKER_OFF` | The wait was assembled without the marker. In unit 1 this happens only when the uprobe failed to attach. On a `PROBE_STATE` record it means the probe was off. |
| 0x02 | `ZERO_MARKER` | A marker with no call of its word. `end_ns == start_ns`, `n_syscalls` is 0. |
| 0x04 | `TRUNC_START` | The wait was already in progress at attach. `start_ns` is the attach time. |
| 0x08 | `TRUNC_END` | The wait was still in progress at stop or at the process's exit. `end_ns` is that time. |
| 0x10 | `QID_UNAVAILABLE` | No query id could be read (PG13 until unit 3, or a failed read). `query_id` is 0 and carries no meaning. |
| 0x20 | `IN_CMD_NO_QID` | The process was inside a command whose query id was not set. `query_id` is 0. |
| 0x40 | `PG13_SYNTH_QID` | `query_id` is a PG13 synthetic id. Reserved for unit 3. |
| 0x80 | `LWLOCK_TRACEPOINT` | The wait came from the built-in LWLock tracepoints, not from a system call. Reserved for unit 4. |

### STATS trailer (kind 255)

The trailer is the last record of every segment that was closed cleanly.
`kind` and `flags` sit at the same offsets as in every other record.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | u64 | `records` | Records in this segment before the trailer |
| 8 | u64 | `lost_raw_total` | Raw wire records lost since capture start (ring reserve failures). Cumulative across segments. |
| 16 | u64 | `end_ns` | When the trailer was written |
| 24 | u32 ×7 | `n_wait`, `n_zero_marker`, `n_no_wait`, `n_probe_state`, `n_loss`, `n_backend`, `n_other` | Per-kind counts in this segment. `n_zero_marker` counts the WAIT records that have `ZERO_MARKER`. |
| 52 | u16 | `kind` | 255 |
| 54 | u16 | `flags` | 0 |
| 56 | u32 | `stats_flags` | 0x1 `FINAL` (the capture stopped cleanly); 0x2 `ROTATED` (the segment was closed by rotation) |
| 60 | u32 | `reserved` | 0 |

## Sentinel rules

- `on_cpu_ns` and `runq_ns` hold `0xFFFFFFFFFFFFFFFF` (the same value as
  `PGWT_CPU_NS_UNKNOWN`) whenever they were not measured. In unit 1 they are
  never measured. **The value 0 never means "not measured."**
- `query_id` 0 carries no meaning on its own. Read it together with
  `QID_UNAVAILABLE` and `IN_CMD_NO_QID`. With neither flag set, 0 means the
  process was outside a command.
- A wait with no system call takes the last query id of its pid.
- `pid` 0 appears only on global kinds (`PROBE_STATE`).

## Reader rules

1. Refuse a segment whose `magic` or `version` is unknown.
2. Step by the header's `rec_size`, never by `sizeof`. A later version may
   append fields to a record.
3. **Skip records of unknown kinds** by `rec_size`. Do not reject the file
   because of them.
4. **Ignore unknown flag bits.**
5. **A segment with no STATS trailer as its last record is TRUNCATED.** Its
   loss is unknown, never zero. A partial record at the end of a segment is
   also TRUNCATED. A comparison or acceptance gate refuses a TRUNCATED
   segment.
6. **`ZERO_MARKER` waits have no duration and are never summed.** They never
   count toward wait time, maximum duration or the number of waits with
   duration. They are counted separately (`n_zero`). A comparator may pad
   its own matching window for them. The product never invents a duration.

## Summary (`timeline-summary.json`)

The summary comes from the same records as the file, through one
aggregation that runs either live or from the file. The two must be equal,
and a test proves it.

- **Per event:** name (`pgwt_event_full_name`), `n`, `total_ns`, `max_ns`,
  `syscalls`, `n_zero`.
- **Per query:** rows keyed by (`query_id`, QID flags) with `n`,
  `total_ns` and `n_zero`. Each query also has per-(query, event) rows.
- **Totals:** `lost_raw_total`, and `truncated` when any segment lacks its
  trailer.

## Wire (BPF to userspace, `src/timeline_wire.h`, frozen for unit 1 only)

Each record starts with a 16-byte header:

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | `size` |
| 2 | u8 | `version` (1) |
| 3 | u8 | `kind` |
| 4 | u16 | `flags` |
| 6 | u16 | `reserved` |
| 8 | u32 | `pid` (tgid) |
| 12 | u32 | `tid` |

A consumer refuses and counts any record with an unknown version, or with a
size smaller than its kind requires. It steps by `size`.

The records:

| Kind | Name | Size | Body |
|---|---|---|---|
| 1 | `CALL` | 72 | `t0` (sys_enter), `t1` (sys_exit), `qid`, `task_cpu_ns`, `run_delay_ns`, `w` (nonzero wait word), `ret` (`(u32)(s32)`), `sc_id`, `reserved` |
| 2 | `MARK` | 48 | `t` (`WaitEventSetWait` entry), `task_cpu_ns`, `run_delay_ns`, `w` (argument 5), `reserved` |
| 3 | `SEED` | 48 | `t0` (attach time), `t1` (the first sys_exit seen for this pid), `qid`, `w`, `sc_id` |
| 4 | `EXIT` | 24 | `t`. Emitted only when pid == tgid. |

`CALL` and `MARK` reserve two u64 fields: cumulative task CPU ns and
cumulative run_delay ns. Unit 3 fills them; unit 1 writes the sentinel.

Wire flags:

| Bit | Flag | Meaning |
|---|---|---|
| 0x1 | `LOST_BEFORE` | A record of this pid was lost since its previous record. The assembler writes a `LOSS` span. |
| 0x2 | `IN_CMD` | The process was inside a command when the query id was read |
| 0x4 | `QID_UNAVAILABLE` | No query id could be read |

Ordering: records of different pids interleave in any order. The records of
one pid arrive in program order.
