# pg_wait_tracer on-disk trace format (v4)

Status: authoritative spec for trace format version 4 (what a current build
writes) and the sidecar files in a trace directory. v2/v3 files remain
READABLE (v2 TRANSITIONS records surface `cpu_ns = PGWT_CPU_NS_UNKNOWN`
and fall back to gap-inference; v3 retains its measured CPU column); v1 stays rejected.
Written for Phase T5 of the Trust Milestone (DUR-5/6: the format and its
durability guarantees were previously implicit in the code); the v3 cpu_ns
column was added in Phase T8 (measured CPU, docs/ROADMAP_AND_STATUS.md).

**Upgrade the daemon and server together.** An older server (reading only
v2/v3) refuses a v4 file on stderr with `ERROR: unsupported trace version 4
... regenerate with this build`. Its JSON response carries `fidelity: "none"`
and zero DB Time (and zero `from_ns`/`to_ns`), so a partial upgrade can show an
empty view. Check the server's stderr and upgrade it to read the new capture.

## Scope and general rules

A trace directory (`--trace-dir`) contains:

| File | Written by | Purpose |
|---|---|---|
| `current.trace` | event writer | live raw-event file (this hour) |
| `current.trace.meta` | event writer | committed-block high watermark |
| `YYYY-MM-DD_HH[.N].trace.lz4` | rotation/recovery | finished hourly archives |
| `current.summary` / `.meta` | summary writer | live per-second summary file |
| `YYYY-MM-DD_HH[.N].summary.lz4` | rotation/recovery | finished summary archives |
| `query_texts.jsonl` | query-text capture | context + query grouping key → sourced SQL text sidecar |
| `query_sources.jsonl` | PG13 sampler | context + grouping key → durable synthetic-attribution quality |
| `backends.jsonl` | backend-meta writer | pid → type/user/db sidecar |
| `wait_event_names.json` | daemon | event-id → name tables (PG17+); carries no Extension/InjectionPoint/BufferPin keys (`pg_wait_events` gives no ids for them, #801), and a reader ignores any it finds |
| `*.corrupt.<unixtime>` | recovery | preserved unreadable leftovers |
| `pgwt.sock` | daemon | control socket |

**Endianness and packing.** All integers are little-endian, two's
complement. All on-disk structs are `__attribute__((packed))` — there is no
implicit padding, and the byte layouts below are exact. The format is not
portable to big-endian writers/readers (none are supported).

**Timestamps.** Raw event timestamps are `CLOCK_MONOTONIC` nanoseconds.
Each file header records one (`CLOCK_REALTIME`, `CLOCK_MONOTONIC`) pair
captured at file creation; readers derive the mono→wall mapping per file
(and re-anchor across files per clock generation — see FID-7 in
`docs/ROADMAP_AND_STATUS.md`). Summary block timestamps are wall-clock.

## Trace file layout

```
[file header — 36 bytes in v4; 28 in v2/v3]
[block 0][block 1]...[block N-1]
[footer: block index — N × 16 bytes][block count — u32]
```

`current.trace` has no footer while being written; archives always do
(rotation and startup recovery both append one).

### File header (v4: 36 bytes; v2/v3: 28 bytes)

| off | size | field | value |
|---|---|---|---|
| 0 | 4 | magic | `0x54574750` ("PGWT" as LE bytes) |
| 4 | 2 | version | 4 (v2/v3 still read) |
| 6 | 2 | flags | bit 0: LZ4-compressed blocks |
| 8 | 4 | pg_version | PostgreSQL major version |
| 12 | 8 | start_time_ns | CLOCK_REALTIME at file creation |
| 20 | 8 | clock_offset_ns | CLOCK_MONOTONIC at file creation |
| 28 | 8 | nominal_sample_period_ns (**v4 only**) | configured `1e9 / sample_rate_hz`; 0 = unknown |

The first 28 bytes are `struct pgwt_trace_file_header`; the v4 extension is
one little-endian u64. The daemon sets the nominal period before the first
write. It is durable in **every raw file**, survives rotation/recovery, and
belongs to that file's capture generation (not a mutable directory sidecar).
Server `sample_period_ns` responses and fidelity edge slop use this nominal
cadence; no jittered effective weight is labelled nominal. A library caller
must set `writer.nominal_sample_period_ns` before its first push.
Unconfigured v4 callers have unknown nominal cadence (0); the server omits
that cadence field and uses a 1-second edge slop. For v2/v3 files the server
preserves the legacy responses: the last nonzero stored block period supplies
`sample_period_ns` and the fidelity edge slop, even though that period is a
measured mean rather than configured nominal cadence.

Summary files reuse the **28-byte prefix only**, without the v4 extension, with magic `0x53574750`
("PGWS") and their own version (currently 2; v1 files remain readable).

### Block header (v4: 48 bytes; v2/v3: 40 bytes)

| off | size | field |
|---|---|---|
| 0 | 8 | first_timestamp_ns (mono) |
| 8 | 8 | last_timestamp_ns (mono) |
| 16 | 4 | num_events (1 … 4096) |
| 20 | 4 | compressed_size (bytes following this header) |
| 24 | 4 | uncompressed_size |
| 28 | 2 | block_type (enum pgwt_block_type) |
| 30 | 2 | reserved (0) |
| 32 | 8 | sample_period_ns (v4 SAMPLES: **first tick weight**; v2/v3: stored per-record mean; else 0) |
| 40 | 8 | max_sample_weight_ns (**v4 only**; maximum effective weight of any sample in this block; TRANSITIONS: 0) |

The first 40 bytes are `struct pgwt_trace_block_header`. The v4 maximum is a
separate u64 extension on **all** raw block types, so readers can scan without
decoding payloads. Header-only sample lookahead uses this maximum, including
when the largest weight is not the first. The format cost is 8 bytes per raw
file plus 8 bytes per block; payload columns and the footer are unchanged.

The compressed payload (LZ4, `LZ4_compress_default`) follows immediately.

### Block types and columnar payloads

A block holds records of exactly one type (never mixed). After LZ4
decompression the payload is column-major:

**TRANSITIONS (type 0)** — exact wait-event transition intervals from the
watchpoint (full/escalated) tier:

1. timestamps: delta-encoded unsigned LEB128 varints (first delta is the
   absolute value; deltas may be 0)
2. pids: raw u32 × num_events
3. old_event: raw u32 × num_events (the event whose interval just ended)
4. new_event: raw u32 × num_events (`0xFFFFFFFF` = process exit)
5. duration_ns: unsigned LEB128 varint × num_events
6. query_id: raw u64 × num_events
7. cpu_ns: unsigned LEB128 varint × num_events **(v3/v4)** — measured
   on-CPU nanoseconds consumed during `old_event` (the task's
   `se.sum_exec_runtime` delta over the interval, read by the watchpoint
   BPF in the backend's own task context). For a wait-labeled `old_event`
   this is ≈0 (a sleeping task burns no CPU — the built-in self-check); for
   an on-CPU gap (`old_event == 0`) it is the measured CPU, and
   `duration_ns − cpu_ns` is the off-CPU / runqueue-unaccounted remainder
   (rendered as **Off-CPU\***). The sentinel `PGWT_CPU_NS_UNKNOWN`
   (`0xFFFFFFFFFFFFFFFF`) means "not measured" — a legacy-capability daemon
   (no `/proc/<pid>/schedstat`), or any v2 file (which has no column 7 at
   all); the reader surfaces the sentinel and the compute layer falls back
   to gap-inference for those records. v2 files are decoded through columns
   1–6 exactly as before.

**SAMPLES (type 1)** — point observations from the userspace sampler:

1. timestamps: delta varints (as above; one tick's samples share a value)
2. pids: raw u32 × num_events
3. event: raw u32 × num_events (the sampled wait event)
4. query_id: raw u64 × num_events

A sample means "at time T, pid P was in event E". Its estimated backward
interval is `(T − weight, T]`; this is an estimate, never an interval
measurement. On disk no duration or flags column is stored. The reader sets
`PGWT_EVENT_FLAG_SAMPLE`, `duration_ns` to the effective weight, and `cpu_ns`
to `PGWT_CPU_NS_UNKNOWN`. Both replay and server consumers preserve that
per-record weight. The sampled tier's CPU signal is the `event == 0` sample,
weighted exactly like every other sample.

**v4 weight decoding.** Start with the header's `sample_period_ns`. All
records of the first tick share that weight. At each **distinct** timestamp,
replace the weight with `timestamp − previous_distinct_timestamp`; all
records sharing that timestamp receive the same weight. Do not use the prior
record's delta when it is zero (several backends observed in one tick).
A block split in the middle of a tick repeats that tick's weight in the next
block's header, so it also decodes independently.

**v2/v3 weight decoding is unchanged:** every record retains the stored block
mean, including old inflation and integer truncation. Old traces therefore
keep their stored (inflated) weights. Re-deriving them would require original
tick boundaries, the nominal first-edge term, clamp knowledge and evidence of
zero-record/failed ticks. Those ticks leave no record, so a timestamp gap in
an old block cannot distinguish one slow tick from several ticks with empty
ones in between. Timestamp-only retroactive correction is not exact and is
not performed.

**Weights and batching (DUR-8, #863).** Consecutive ticks use their measured
inter-tick elapsed time, including intervals below nominal under ±10% jitter.
The first tick has a nominal-period edge estimate; a query clips the sample's
backward interval to its requested window. Stalls credit at most 60 seconds
to the state observed at the end; the excess is unobserved, uncredited time.
It is never carried into a later tick. A daemon restart starts a fresh edge
term. During escalation the sampler keeps ticking; the server subtracts
exact coverage from each sample's backward interval.

The v4 writer lets a tick join the pending block only when its supplied
weight equals `tick_timestamp − last_pushed_tick_timestamp` (with strictly
increasing tick timestamps). Otherwise it flushes and starts a block whose
header stores that tick's supplied weight. The rule detects an omitted empty
or failed tick, a clamped stall, and non-monotonic/nominal fallback ticks;
the first tick of any block always has its explicit supplied weight. The
initial nominal edge may share a block with subsequent ordinary ticks.
Jitter alone never cuts a block. Existing cuts remain: 4096 records, a batch
spanning at least one second at the end of a push, publication, rotation and
close. A production push is one tick; bulk callers can supply multiple ticks
with a common weight. The per-block maximum includes the first weight and
all subsequent effective tick weights.

### Markers

Query lifecycle and escalation markers are TRANSITIONS records with
sentinel event ids (`0xFFFFFFF0`–`0xFFFFFFF5`, see `pg_wait_tracer.h`),
duration 0. Escalation markers use pid 0 and pack reason/duration into
query_id (`PGWT_ESC_PACK`). They are structural — every consumer must
exclude them from wait aggregation (`pgwt_filter_matches` is the
chokepoint); only variants/lifecycle stats and coverage read them.
PG13 command markers synthesized from an at-escalation activity snapshot carry
the in-memory `QUERY_SYNTH` quality bit. Durable/offline consumers recover the
same `pg13-synth-v1` quality from the context/source-aware query-text sidecar.

**Flush records (v3, T8).** At de-escalation and at daemon shutdown the
open exact interval of each still-attached backend is closed with a final
TRANSITIONS record carrying `old_event == new_event` (the interval's own
event, NOT a `0xFFFFFFFF` process-exit) and its measured `cpu_ns`. For a
pure-CPU command straddling capture end (`old_event == 0`, no wait boundary
ever fired) this is the ONLY record of its on-CPU stretch — without it the
trace would be empty. A failed schedstat read at flush time leaves
`cpu_ns = PGWT_CPU_NS_UNKNOWN` so compute infers rather than records a
false 0. Flush records are ordinary intervals (not markers): they enter
wait/CPU aggregation normally.

### Footer

`N × struct pgwt_block_index_entry { u64 first_timestamp_ns; u64
file_offset; }` followed by `u32 N`. Offsets point at block headers,
strictly increasing; the first entry is always at offset 28 (right after
the file header). Readers verify this shape before trusting a footer, so a
footer-less crash leftover can never be misread through the footer path.

### Block index sortedness

The index (and the physical block sequence) is sorted by
`first_timestamp_ns`: when both a transitions buffer and a pending sample
batch exist, the writer flushes the buffer holding the older first
timestamp first. Readers binary-search the index for range queries.

## Reader strategies and sanity caps (DUR-7)

`pgwt_reader_open` determines the block index by, in order:

1. **Meta high watermark** — `<file>.meta` holds the committed block count
   (see below); scan exactly that many block headers sequentially.
2. **Footer** — archives; validated as described above.
3. **Sequential scan** — fallback; stops at the first implausible header.

All three paths enforce the same caps — a corrupt meta/footer/header must
never drive a huge allocation or a seek into garbage:

| quantity | cap |
|---|---|
| blocks per file (reader) | `PGWT_MAX_READ_BLOCKS` = 4 Mi |
| blocks per file (writer; forces early rotation) | `PGWT_MAX_FILE_BLOCKS` = 1 Mi |
| num_events per block | `PGWT_BLOCK_EVENTS` = 4096 |
| compressed/uncompressed block size | 10 MB |

## The meta high-watermark protocol

Readers may open `current.trace` while the daemon is writing it. After
every block flush the writer:

1. `fflush(fp)` — the block reaches the OS page cache, visible to readers;
2. writes the new committed block count into `current.trace.meta.tmp`;
3. `rename(meta.tmp, meta)` — atomic on POSIX.

A reader that sees `meta == N` can safely scan/decode exactly N blocks;
anything after that is an uncommitted tail that must be (and is) ignored.
The meta file is removed on rotation (the archive has a footer) and by
startup recovery.

## File lifecycle: open, rotation, recovery

**Open.** `current.trace` is created with `O_EXCL` semantics (`"wbx"`) —
the writer NEVER truncates an existing file (DUR-1; the pre-T5 code's
`fopen("wb")` destroyed up to an hour of data on every restart).

**Rotation.** On the first write after the local-time hour changes (or when
the writer block cap forces it): flush both pending buffers, append the
footer, `fsync` the file, close, rename to `YYYY-MM-DD_HH.trace.lz4` (the
hour the file STARTED), remove the meta file, `fsync` the directory.

**Archive naming** is local time (operators correlate archives with server
logs). Collision safety, not name uniqueness, is what protects data
(DUR-2): if the target name exists — a restart within the same hour, or a
DST fold replaying an hour — the writer picks `YYYY-MM-DD_HH.N.trace.lz4`
for the first free `N` instead of clobbering. Discovery and retention parse
the leading `YYYY-MM-DD_HH` and the suffix, so suffixed archives are
first-class.

**Startup recovery (DUR-1).** If a `current.trace`/`current.summary`
exists at writer init (crash, kill -9, OOM, or plain shutdown — a clean
close also leaves the current file in place):

1. read the header; if unreadable, preserve the file as
   `current.trace.corrupt.<unixtime>` (never delete data that might be
   someone's evidence; a header-only file is simply removed);
2. scan committed blocks with the reader's sanity caps, requiring each
   block's payload to lie fully inside the file — a torn tail (killed
   mid-`fwrite`) ends the scan;
3. cross-check against the meta watermark; fewer recovered blocks than
   committed is loudly reported (it means FS-level loss, not a writer bug);
4. truncate the torn tail, append a footer, `fsync`;
5. rename to a collision-safe archive name and `fsync` the directory.

The recovered archive is indistinguishable from a normally rotated one.

## Durability policy (DUR-5/6)

What survives what:

| failure | guaranteed loss bound |
|---|---|
| daemon crash / kill -9 | the unflushed tail only: < 1 buffered TRANSITIONS block (≤ 4096 events) + < ~1 s of batched samples + the summary second still accumulating. Since #277 a summary second is written only once it is complete, so that last bound is measured from the EVENT, not from the second: one tick interval + PGWT_SUMMARY_FLUSH_LAG_NS (100 ms) after the second ends, i.e. up to ~2.1 s at `-i 1` and ~6.1 s at `-i 5`. On a busy stream the next second's first event flushes the previous one, so the loss is the current partial second; once the stream quiesces the last second waits for a tick. Every flushed block is in the OS page cache and is recovered on restart. |
| OS crash / power loss | rotated/recovered archives are `fsync`ed and safe. The CURRENT hour's file has no per-block fsync — up to the entire current file can be lost (page cache never reached disk). The meta file may also be stale/absent, which readers tolerate. |

Chosen policy: **fsync on rotation, close, and recovery + directory fsync
after every rename; no per-block fsync.** Measured on the test host
(Hetzner, ext4 on SSD, 64 KB blocks ≈ one compressed block):
0.067 ms/block with the chosen policy vs 0.963 ms/block (~14×) with
per-block fsync. At the batched block rate the absolute cost would be
small on this SSD, but per-block fsync stalls the capture loop on slower
storage (10–20 ms per fsync on spinning disks, unbounded on saturated
volumes) exactly when the database is busiest — and the tool's promise is
"a daemon crash loses at most the unflushed tail, never a committed hour",
which page-cache commits already deliver. Power-loss durability of the
in-progress hour is explicitly out of scope; operators who need it can run
with a shorter rotation via external sync.

## Retention (DUR-3)

Runs every 60 daemon ticks:

- **Hours** (`--trace-retention`, default 24 h): archives (trace, summary,
  suffixed, and preserved `.corrupt.*` files) older than the cutoff are
  deleted. Age comes from the `YYYY-MM-DD_HH` name when parseable, else
  file mtime. `--trace-retention 0` disables the hours rule only.
- **Size cap** (`--retention-gb`, fractional GiB, default off): if the
  trace directory's total size exceeds the cap, eligible archives are
  deleted oldest-hour first, with each hour's summaries before its raw
  archives. This is a best-effort target: live `current.*` files and sidecar
  `.jsonl` files are never deleted, and a pending summary protects its hour's
  raw archives until publication (normally at hourly rotation). Usage can
  therefore remain over the cap for up to an hour during normal rotation,
  even with archives present. A failed unlink does not stop eviction from
  other hours; a failed summary unlink also protects all raw pieces with
  the same `ftime` (parsed hour, otherwise file mtime) for that pass. Removal failures
  or an incomplete scan can also prevent reaching the cap. Any remaining
  excess is recounted after the pass; a WARN reports its byte count and
  the limiting reason.
- **Orphans**: stale `*.meta.tmp` (torn meta writes) and `current.*.meta`
  files whose current file is gone are removed once provably stale (> 1 h
  old, so a live daemon's meta cycle is never raced).

## Summary files

Same header struct (magic "PGWS", version 2), then per-second blocks:

```
struct pgwt_summary_block_header {   /* 24 bytes, packed */
    u64 wall_ns;            /* the second this block covers */
    u32 num_events;         /* distinct event ids */
    u16 num_sessions;       /* distinct pids */
    u16 num_queries;        /* distinct query ids */
    u32 compressed_size;
    u32 uncompressed_size;
};
```

The LZ4 payload is the serialized per-second accumulator (time-model class
totals, per-event stats + histograms, per-session, per-query — see
`pgwt_summary_serialize`). Lifecycle, meta protocol, rotation, recovery,
naming, and retention are identical to trace files with the
`.summary`/`.summary.lz4` names.

## query_texts.jsonl (and PII — DUR-10)

One JSON object per line: `{"q":"<signed decimal grouping key>","d":<db oid>,
"u":<user oid>,"s":"pgss|synthetic|full","t":"<SQL text>","ts":<wall ns>}`.
PG13 synthetic records also carry `"v":"pg13-synth-v1"`. Legacy records
without context/source load as `(0,0)` full/raw entries. The key is
`(d,u,q)`, not qid alone. Source priority is `full > synthetic > pgss`: a later
exact/full raw first-seen record appends a replacement and always supersedes a
sampled normalized record for the same context. A qid-only reader returns text
only when every context has identical winning text.

`query_sources.jsonl` independently records each PG13 synthetic
`(d,u,q,"pg13-synth-v1")` key. Its dynamically grown dedup table is not subject
to the sampled text table's 4096-key admission cap, so offline readers retain
the attribution-quality flag even when normalized text is unavailable.

The file is append-only across daemon restarts. At startup the daemon restores
the context/source precedence table (bounded: 4096 keys; overflow is logged)
and compacts only above 32 MB (`PGWT_QT_COMPACT_BYTES` overrides for tests),
preserving source upgrades. Retained traces reference these keys, so the file
must not be truncated while matching traces exist.

**PII warning:** `full` text is RAW running SQL and may contain inline literal
values. `pgss` and `synthetic` text is normalized; the latter deliberately
replaces literal constants, but neither should be treated as an authorization
or redaction boundary. Files use trace permissions (0640, `--trace-group`).
There is no redaction option; restrict the trace directory when SQL text must
not be exposed. Trace keys and `backends.jsonl` metadata are also sensitive.

## Versioning

Readers accept versions 2, 3 and 4; v1 stays rejected. v3 added TRANSITIONS
column 7 (`cpu_ns`); v2 records lack it and surface `PGWT_CPU_NS_UNKNOWN`.
v4 changes SAMPLES weight semantics and extends raw file/block headers as
specified above; v2/v3 payload decoding and stored weights remain unchanged.
Recovery uses the file version to scan the appropriate header sizes and
archives the original bytes without conversion.

An old (v3) `pgwt-server` rejects each v4 file before block decoding and prints:

```
ERROR: unsupported trace version 4 in <path> (this build reads versions 2-3; regenerate with this build)
```

It skips that file; when it is the only file, `--dump` reports no events
(and currently exits 0). It does not apply the legacy mean rule to v4 data.
Appending a weight column under v3 would be unsafe: old decoders do not reject
trailing SAMPLES columns and would silently use the wrong weights. Any
semantic/layout change requires a file version bump and gated decoding.
