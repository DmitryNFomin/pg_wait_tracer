# pg_wait_tracer on-disk trace format (v3)

Status: authoritative spec for trace format version 3 (what a current build
writes) and the sidecar files in a trace directory. v2 files remain
READABLE (their TRANSITIONS records surface `cpu_ns = PGWT_CPU_NS_UNKNOWN`
and the compute layer falls back to gap-inference); v1 stays rejected.
Written for Phase T5 of the Trust Milestone (DUR-5/6: the format and its
durability guarantees were previously implicit in the code); the v3 cpu_ns
column was added in Phase T8 (measured CPU, docs/ROADMAP_AND_STATUS.md).

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
| `wait_event_names.json` | daemon | event-id → name tables (PG17+) |
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
[file header — 28 bytes]
[block 0][block 1]...[block N-1]
[footer: block index — N × 16 bytes][block count — u32]
```

`current.trace` has no footer while being written; archives always do
(rotation and startup recovery both append one).

### File header (28 bytes, struct pgwt_trace_file_header)

| off | size | field | value |
|---|---|---|---|
| 0 | 4 | magic | `0x54574750` ("PGWT" as LE bytes) |
| 4 | 2 | version | 3 (v2 still read; see the cpu_ns column below) |
| 6 | 2 | flags | bit 0: LZ4-compressed blocks |
| 8 | 4 | pg_version | PostgreSQL major version |
| 12 | 8 | start_time_ns | CLOCK_REALTIME at file creation |
| 20 | 8 | clock_offset_ns | CLOCK_MONOTONIC at file creation |

Summary files reuse the same header struct with magic `0x53574750`
("PGWS") and their own version (currently 2; v1 files remain readable).

### Block header (40 bytes, struct pgwt_trace_block_header)

| off | size | field |
|---|---|---|
| 0 | 8 | first_timestamp_ns (mono) |
| 8 | 8 | last_timestamp_ns (mono) |
| 16 | 4 | num_events (1 … 4096) |
| 20 | 4 | compressed_size (bytes following this header) |
| 24 | 4 | uncompressed_size |
| 28 | 2 | block_type (enum pgwt_block_type) |
| 30 | 2 | reserved (0) |
| 32 | 8 | sample_period_ns (SAMPLES: nominal per-sample weight; else 0) |

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
7. cpu_ns: unsigned LEB128 varint × num_events **(v3 only)** — measured
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

A sample means "at time T, pid P was in event E" and is worth ONE
`sample_period_ns` of estimated time — never an interval measurement. The
reader surfaces samples with `PGWT_EVENT_FLAG_SAMPLE` set; on-disk records
carry no flags. SAMPLES blocks are UNCHANGED in v3 — they have no cpu_ns
column (the sampled tier's CPU signal is the `we==0` sample itself, weighted
by `sample_period_ns`); the reader surfaces `cpu_ns = PGWT_CPU_NS_UNKNOWN`
for every sample.

**Batching (DUR-8).** The writer buffers sample-type pushes and cuts a
block roughly once per second (or at 4096 records), instead of one tiny
block per sampler tick — ~10× fewer blocks at 10 Hz. The block's
`sample_period_ns` is the count-weighted mean of the batched ticks'
periods, so the block's total estimated time equals the exact per-tick sum;
a push whose period deviates more than ~1.6 % (period/64) from the pending
batch — an SMP-3 stall tick with its measured, compensating weight — is cut
into its own block so its weight stays exact. The buffering layer is
type-agnostic: a future sample-like block type flows through the same
batching with its own type/period gate. Readers make no assumption about
records-per-block, so files written by older per-tick writers stay
readable.

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
| daemon crash / kill -9 | the unflushed tail only: < 1 buffered TRANSITIONS block (≤ 4096 events) + < ~1 s of batched samples + the current partial summary second. Every flushed block is in the OS page cache and is recovered on restart. |
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
  trace directory's total size exceeds the cap, the OLDEST archives are
  deleted first until it fits. The live `current.*` files and the sidecar
  `.jsonl` files are never deleted; if they alone exceed the cap, a WARN
  says so.
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

Readers accept versions 2 and 3 (a file newer than the build knows is
skipped loudly, never mis-decoded); v1 stays rejected (no deployed v1
installs existed). v3 added column 7 (`cpu_ns`) to TRANSITIONS blocks as a
STRICTLY APPENDED column — a v2 file simply lacks it and the reader stamps
`PGWT_CPU_NS_UNKNOWN`, so appending a trailing varint column is itself a
sanctioned additive-evolution move alongside: new block types (the reader
skips blocks with a type it does not know rather than mis-decoding them),
the block header's `reserved` field, and header `flags` bits. When adding
another such column, bump the version and gate the extra `for` loop on
`header.version >= N` exactly as the reader does for cpu_ns.
