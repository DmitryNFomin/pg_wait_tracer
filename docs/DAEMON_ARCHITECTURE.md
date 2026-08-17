# Daemon Architecture: Raw Event Storage & Compute-on-Read

## Context

Phases 1-8 built the CLI with 6 views, multi-window support, and active sessions.
The current architecture uses in-kernel aggregation (PERCPU_HASH maps) and an
in-memory ring buffer of snapshots. All data is lost when the process exits.

The daemon stores tracing data persistently and supports arbitrary time-range queries
(AWR-like). The key insight: pg_wait_tracer captures **every wait event transition**
via hardware watchpoints (tracing, not sampling). Storing only periodic snapshots
throws away this precision. The architecture preserves raw events as the source of
truth.

**Design principles:**
1. Store raw events, aggregate at query time (industry standard for tracing)
2. No external dependencies (no SQLite, no ClickHouse) — binary files only
3. PG extension provides SQL interface, CLI provides live/replay display
4. Columnar-in-block encoding + LZ4 for compression (10-12x target)

---

## Architecture Overview

```
BPF on_watchpoint handler
    │
    ├── state_map (per-PID current state, kept for duration computation)
    │
    └── event_ringbuf (streams every transition to userspace)
            │
    ┌───────┴────────┐
    │  Daemon process │
    │                 │
    │  ┌─────────────────────────────────┐
    │  │ Event stream reader (epoll)     │
    │  │   ├── In-memory aggregator      │──→ Live CLI display (6 views)
    │  │   ├── Raw event writer          │──→ Event files (disk)
    │  │   └── Periodic snapshot writer  │──→ Snapshot files (disk)
    │  └─────────────────────────────────┘
    │                                          │
    │                                     PG extension reads
    │                                     binary files → SQL
    └───────────────────────────────────────────┘
```

---

## Data Flow Changes

### Current (Phases 1-8)
```
BPF on_watchpoint → accumulate in PERCPU_HASH (wait_stats, query_wait_stats)
Userspace timer   → read PERCPU_HASH → rebuild accumulator → push snapshot → display
```

### New (Daemon)
```
BPF on_watchpoint → emit event to ringbuf + update state_map
Userspace epoll   → read ringbuf events →
                      ├── aggregate in memory (replaces PERCPU_HASH read)
                      ├── write to event files (disk)
                      └── every N seconds: write snapshot to snapshot file (disk)
```

**Removed:** `wait_stats` and `query_wait_stats` PERCPU_HASH maps.
**Kept:** `state_map` (needed in-kernel for duration computation + active view).
**Added:** `event_ringbuf` (BPF_MAP_TYPE_RINGBUF for streaming events to userspace).

---

## Raw Event Format

### Event Structure (36 bytes wire format)
```c
struct pgwt_trace_event {
    uint64_t timestamp_ns;   // absolute ktime_get_ns()
    uint32_t pid;            // backend PID
    uint32_t old_event;      // previous wait_event_info
    uint32_t new_event;      // new wait_event_info
    uint32_t _pad;           // alignment
    uint64_t duration_ns;    // time spent in old_event
    uint64_t query_id;       // active query during old_event
};
```

### BPF Program Changes
```c
// Event output ringbuf
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 64 * 1024 * 1024);  // 64 MiB buffer
} event_ringbuf SEC(".maps");

SEC("perf_event")
int on_watchpoint(struct pt_regs *ctx) {
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    u32 new_event = /* read from watchpoint address */;

    struct pgwt_pid_state *st = bpf_map_lookup_elem(&state_map, &pid);
    if (!st) { /* first event for this PID — init state_map */ return 0; }

    u64 now = bpf_ktime_get_ns();
    u64 duration = now - st->last_ts;

    // Emit raw event to ringbuf
    struct pgwt_trace_event *evt =
        bpf_ringbuf_reserve(&event_ringbuf, sizeof(*evt), 0);
    if (evt) {
        evt->timestamp_ns = now;
        evt->pid = pid;
        evt->old_event = st->last_event;
        evt->new_event = new_event;
        evt->duration_ns = duration;
        evt->query_id = st->last_query_id;
        bpf_ringbuf_submit(evt, 0);
    }

    // Update state_map (keep — needed for duration computation)
    st->last_event = new_event;
    st->last_ts = now;
    // query_id updated separately by query_id watchpoint

    return 0;
}
```

**Full-tier NMI limitation:** hardware-watchpoint perf-event programs can run
in NMI context. The kernel's shared BPF ring uses a spinlock for reservation,
so a reservation can fail in NMI context even when the ring has free space;
increasing `max_entries` does not remove that contention failure. Every such
loss is exposed as `ringbuf_drops_total`. Consumers that require a provably
lossless event-ring interval must check that the counter did not advance;
other capture health counters cover failures outside this ring. High-rate
multi-CPU capture can report a non-zero counter even with ample ring capacity.
See the kernel's [BPF ring-buffer design](https://docs.kernel.org/bpf/ringbuf.html#design-and-implementation).

**Removed from BPF:** All `wait_stats` and `query_wait_stats` map updates (count++,
total_ns+=, min/max, histogram). This simplifies the BPF program and reduces
per-event overhead in kernel.

---

## On-Disk Storage

### Event Files (raw trace data)

Rotating hourly files with columnar block encoding:

```
traces/
  current.trace              # actively written
  2026-02-25_14.trace.lz4   # completed, compressed
  2026-02-25_13.trace.lz4   # older
  ...
```

#### File Format
```
[File header]
  magic:           4 bytes  "PGWT"
  version:         2 bytes
  flags:           2 bytes  (compression type, etc.)
  pg_version:      4 bytes
  start_time_ns:   8 bytes  (wall clock at file creation)
  clock_offset_ns: 8 bytes  (ktime_get_ns() at start — for wall clock conversion)

[Block 0]
  block_header:
    first_timestamp_ns:  8 bytes
    last_timestamp_ns:   8 bytes
    num_events:          4 bytes
    compressed_size:     4 bytes
    uncompressed_size:   4 bytes
  compressed_data:  [columnar-encoded events, LZ4 compressed]

[Block 1]
  ...

[File footer / index]
  num_blocks:      4 bytes
  block_index[]:   array of { timestamp_ns, file_offset } per block
```

#### Columnar Block Encoding

Within each block (~4096 events), fields are stored column-wise for compression:

```
[all 4096 timestamps, delta-encoded as varint]  // ~8-16KB → ~2-4KB compressed
[all 4096 PIDs, as uint32]                      // ~16KB → ~1KB compressed (few distinct)
[all 4096 old_events, as uint32]                // ~16KB → ~1KB compressed
[all 4096 new_events, as uint32]                // ~16KB → ~1KB compressed
[all 4096 durations, as varint]                 // ~16-32KB → ~4-8KB compressed
[all 4096 query_ids, as uint64]                 // ~32KB → ~2-4KB compressed
```

Total uncompressed: 4096 x 36 = 147KB per block.
Target compressed: ~12-20KB per block (7-12x compression).

#### Volume Estimates

| Workload | Transitions/sec | Raw/sec | Compressed/sec | Per hour | Per 24h |
|----------|----------------|---------|----------------|----------|---------|
| Idle (10 backends) | ~100 | 3.6KB | ~360B | 1.3MB | 31MB |
| Light OLTP (50 backends) | ~5K | 180KB | ~18KB | 65MB | 1.6GB |
| Moderate (100 backends) | ~30K | 1.1MB | ~110KB | 400MB | 9.4GB |
| Heavy OLTP (500 backends) | ~100K | 3.6MB | ~360KB | 1.3GB | 31GB |
| Extreme (1000 backends) | ~500K | 18MB | ~1.8MB | 6.5GB | 155GB |

#### Time-Range Query on Event Files

To read events between T1 and T2:
1. Find relevant files by filename (hourly boundaries)
2. Read file footer → binary search block_index for T1
3. Read blocks from T1 to T2, decompress, decode columns
4. Aggregate in userspace (compute time_model, histograms, per-PID stats, etc.)

---

### Snapshot Files (aggregated, for long-term history)

Periodic snapshots (every 1 minute) for efficient coarse queries.
Written by daemon from in-memory aggregation state.

```
snapshots/
  2026-02.snap       # one file per month
  2026-01.snap       # older month
  ...
```

#### Snapshot Structure (extended with per-PID data)
```c
struct pgwt_snapshot_v2 {
    uint64_t timestamp_ns;
    uint32_t num_backends;

    // System-wide (same as current pgwt_snapshot)
    struct pgwt_time_model tm;
    int num_events;
    struct pgwt_snap_event events[MAX_SNAP_EVENTS];       // 512 max
    int num_query_events;
    struct pgwt_snap_query_event query_events[MAX_SNAP_QUERIES]; // 1024 max

    // Per-PID (for session_event and active views)
    struct pgwt_snap_pid {
        uint32_t pid;
        uint32_t backend_type;     // from cmdline parsing
        uint64_t db_time_ns;       // cumulative
        uint64_t cpu_time_ns;      // cumulative
        uint32_t current_event;    // state at snapshot time
        uint64_t current_wait_ns;  // duration in current state
        int num_events;
        struct pgwt_snap_pid_event {
            uint32_t wait_event;
            uint64_t count;
            uint64_t total_ns;
        } events[];                // variable length, max ~30 per PID
    } pids[];                      // variable length
};
```

Written as variable-length records:

```
[File header: magic, version, interval]
[Snapshot 0: timestamp(8) + length(4) + binary data]
[Snapshot 1: timestamp(8) + length(4) + binary data]
...
[Footer index: array of {timestamp, offset}]
```

#### Snapshot Size Estimate (variable-length, realistic)
- Time model: 88 bytes
- System events (50 active): 50 x 20 = 1,000 bytes
- Query events (200 active): 200 x 20 = 4,000 bytes
- Per-PID (100 backends x 15 events): 100 x (28 + 15 x 20) = 32,800 bytes
- System histograms: 50 x 128 = 6,400 bytes
- **Total: ~44KB per snapshot**

| Interval | Per day | Per month | Per year |
|----------|---------|-----------|----------|
| 1 min | 63MB | 1.9GB | 23GB |
| 5 min | 12.7MB | 380MB | 4.6GB |

**Default: 1-minute snapshots, 1-year retention.** 23GB/year is acceptable.

---

## Tiered Storage Summary

| Tier | Data | Granularity | Default retention | Size estimate |
|------|------|-------------|-------------------|---------------|
| Raw events | every transition | nanosecond | 24 hours | 2-20GB/day |
| Snapshots | aggregated | 1 minute | 1 year | ~23GB/year |

**Retention is configurable:**
```
--trace-retention 24h      # keep raw events for 24 hours
--snapshot-retention 365d   # keep snapshots for 1 year
--snapshot-interval 60      # snapshot every 60 seconds
```

---

## In-Memory Aggregator

The daemon maintains an in-memory accumulator updated from the event stream
(replaces PERCPU_HASH map reading):

```c
void handle_trace_event(struct pgwt_trace_event *evt, struct pgwt_accumulator *acc) {
    // Update per-PID stats
    struct pgwt_pid_accum *pa = get_or_create_pid(acc, evt->pid);
    struct pgwt_event_stats *es = get_or_create_event(pa, evt->old_event);
    es->count++;
    es->total_ns += evt->duration_ns;
    update_histogram(es, evt->duration_ns);
    if (evt->duration_ns < es->min_ns) es->min_ns = evt->duration_ns;
    if (evt->duration_ns > es->max_ns) es->max_ns = evt->duration_ns;

    // Update system-wide stats
    struct pgwt_event_stats *se = get_or_create_system_event(acc, evt->old_event);
    se->count++;
    se->total_ns += evt->duration_ns;
    // ... same pattern

    // Update time model by class
    update_time_model(&acc->tm, evt->old_event, evt->duration_ns);

    // Update query events
    if (evt->query_id)
        update_query_event(acc, evt->query_id, evt->old_event, evt->duration_ns);
}
```

The timer handler (every --interval seconds) reads from the accumulator for display.
The ring buffer of snapshots for multi-window stays in memory for live display.

---

## PG Extension Interface (future)

The extension links against a shared library that reads binary files:

```sql
-- Time model for any time range
SELECT * FROM pgwt_time_model(
    start_time := '2026-02-25 14:00',
    end_time := '2026-02-25 14:05'
);

-- System events for last hour
SELECT * FROM pgwt_system_events(interval '1 hour');

-- What was PID 34521 doing at 14:02?
SELECT * FROM pgwt_active_sessions('2026-02-25 14:02:00');

-- Raw events for a specific backend
SELECT * FROM pgwt_trace_events(
    pid := 34521,
    start_time := '2026-02-25 14:00',
    end_time := '2026-02-25 14:05'
);

-- ASH-like: top queries for a wait event in a time range
SELECT * FROM pgwt_query_events(
    event := 'IO:DataFileRead',
    start_time := '2026-02-25 14:00',
    end_time := '2026-02-25 14:05'
);
```

---

## Implementation Phases

### Phase A: Event Ringbuf + Userspace Aggregation
- Add `event_ringbuf` to BPF program, emit events from `on_watchpoint`
- Add userspace ringbuf reader + in-memory aggregator
- Keep PERCPU_HASH maps initially (dual path for validation)
- Verify: live display produces identical output from both paths
- Remove PERCPU_HASH maps once validated

### Phase B: Raw Event File Writer
- Implement columnar block encoder + LZ4 compression
- Hourly file rotation
- Block index in file footer
- Measure actual compression ratio and write throughput

### Phase C: Event File Reader + Replay
- Implement block decoder + time-range seek
- Aggregate from stored events → produce views
- CLI: `pg_wait_tracer --replay --from T1 --to T2 --view time_model`

### Phase D: Daemon Mode
- Long-running process with event streaming + file writing
- Periodic snapshot writer (1-minute aggregates to snapshot files)
- File retention manager (delete old hourly files, rotate snapshots)
- CLI connects to daemon for live display OR reads files for historical

### Phase E: PG Extension
- Shared library for binary file reading + aggregation
- PG extension with SQL functions
- Reads daemon's data files directly (no socket needed)

### Phase F: Snapshot File Reader for Long-Term History
- Implement snapshot file reader with time-range queries
- Delta computation between any two snapshots (reuse ring_delta math)
- CLI and PG extension use snapshots for queries beyond raw event retention

---

## Files to Create/Modify

| File | Change |
|------|--------|
| `src/bpf/pg_wait_tracer.bpf.c` | Add event_ringbuf, emit events, remove PERCPU_HASH updates |
| `src/pg_wait_tracer.h` | Add pgwt_trace_event struct |
| `src/event_stream.c/h` | NEW — ringbuf reader, in-memory aggregator from events |
| `src/event_writer.c/h` | NEW — columnar block encoder, LZ4, file rotation |
| `src/event_reader.c/h` | NEW — block decoder, time-range seek, file reader |
| `src/snapshot_v2.c/h` | NEW — extended snapshots with per-PID data, file writer/reader |
| `src/daemon.c` | Event loop: epoll on event_ringbuf + timer for display/snapshots |
| `src/map_reader.c` | Simplify: remove PERCPU_HASH reading, keep state_map read for active view |
| `src/retention.c/h` | NEW — file retention manager (delete old files) |
| `ext/pg_wait_tracer_ext.c` | NEW (future) — PG extension SQL functions |
| `lib/pgwt_reader.c/h` | NEW (future) — shared library for file reading (CLI + extension) |
