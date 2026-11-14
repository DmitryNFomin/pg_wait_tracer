# Evidence-v1 binary codec for trace v5

Status: proposed normative byte contract, for `contracts/evidence-v1/CODEC.md`.
This is a design, not a claim that the daemon emits or qualifies v5. It supplements
CONTRACT.md and REVISION-r2.md without changing their replay semantics. Source
baseline: `origin/master` at `84779083f2fcf9a4a02e595bf041d14040927e7f`;
logical adapter types: `aa31b588` (master after #903; reviewed as local commit 33d9139e, whose
`src/core/evidence_replay.{c,h}` are byte-identical). Revision r2 follows the logical
contract where that replay implementation disagrees; see REVISION-codec-r2.md,
“CONTRACT vs REPLAY”. This is not a claim of compatibility with its error paths.

“MUST”, “MUST NOT” and “reject” are requirements. A **segment** is one complete
block, including its header and checksum; the file header is a separate atomic
validation unit. A framing/integrity failure rejects the segment, emits none of
its records, and stops that capture's chain at the preceding validated block.
Earlier intact certificates survive, subject to the contract's revocation rules.
A malformed logical record inside trustworthy framing instead acts as global
VOID at the preceding F, with continuation under §9. These are distinct outcomes:
no ordinary record is emitted for the malformed record, and the loss must not be
silently omitted. A standalone payload decoder returns a complete ordered batch
of records and explicit record-fault notifications, or a framing failure with no
batch. It cannot establish file identity or integrity on its own. Writers MUST
refuse to serialize malformed records or unsupported kinds; reader fault handling
is not an alternative encoding available to a writer.

> **Redelivery precedence:** The wording identified by original line number in
> [REDELIVERY.md, “Superseded CODEC.md wording”](REDELIVERY.md#superseded-codecmd-wording)
> is superseded in the respects listed there. REDELIVERY.md governs reconciliation
> and the codec/replay stop boundary; byte-format requirements remain unchanged.

## 1. Representation and limits

All fixed-width integers are unsigned, little-endian, with exactly the stated
number of octets: `u8`, `u16`, `u32`, `u64`. No native structs, alignment, padding,
C enum widths, terminators, or implicit fields occur. Tables concatenate fields
in listed order. Offsets and lengths are octet counts. Ranges `[a,b)` exclude b.
Timestamps and durations are monotonic nanoseconds in the boot clock domain;
zero is a value, never a null sentinel. File wall-clock metadata cannot establish
evidence time, life, or coverage.

| Primitive | Encoding and validation |
|---|---|
| `bool` | One u8: exactly 0 or 1. |
| `optional_u64` | `bool present`, then u64 only if present=1. Absent consumes one byte, present consumes nine; present zero differs from absent. |
| `bytes16` | u16 byte length followed by exactly that many bytes. Context supplies the permitted length range. No trailing NUL. |
| `counters` | `bool known`; if 1, exactly fifteen u64 values in the order below; if 0, no values. Sizes 121 or 1. Unknown is not fifteen zeros. |
| `event_ref` | u32 numeric ID, interpreted by §3. Zero means nonwaiting only in a wait-state field. |
| `uleb64` | Shortest unsigned LEB128: successive 7-bit low groups, continuation bit 0x80 except on the last octet. At most ten octets; tenth is exactly 0x01 when used. No redundant terminal zero groups. |

Strings are arbitrary length-delimited bytes, compared byte-for-byte, including
embedded zero bytes; no case folding, locale, or Unicode normalization. JSON
adapters encode strings as UTF-8 without normalization. A nonempty byte string
containing a zero byte is not null. Identity strings have length 1..255; event
and marker names 1..1024; VOID diagnostics 1..4096. Empty strings are invalid in
these fields. Null wait state is ID 0, not an empty name. Names need not include
a category prefix; the authoritative mapping fixes their exact bytes.

Hard limits (inclusive): 16,777,216 header bytes; 65,536 mapping entries;
4,194,303 blocks per file; 1..4096 records per block; 10,485,760 stored payload
bytes and 10,485,760 decoded payload bytes per block; 65,535 bytes per EVIDENCE
record body. No record crosses a block and no block crosses a file. A producer
splits before any bound would be exceeded. No silent truncation, clamping,
saturation, count narrowing, or wrapping size arithmetic is allowed. Validate
bounds and remaining input before allocating or adding/multiplying lengths.
The explicit modulo timestamp operation in §6 is the sole arithmetic exception.
An implementation with smaller resource limits reports failure, not completeness.

The counter slots are frozen, zero-based, with no sum or per-CPU substitution:

| Slot | Name |
|---:|---|
| 0 | lifecycle_fork_drop |
| 1 | lifecycle_init_drop |
| 2 | lifecycle_exit_drop |
| 3 | wait_read_fail |
| 4 | query_text_drop |
| 5 | pg13_query_text_drop |
| 6 | exact_query_update_fail |
| 7 | exact_cmd_update_fail |
| 8 | exact_phase_update_fail |
| 9 | accum_update_fail |
| 10 | fork_state_update_fail |
| 11 | event_ring_drop |
| 12 | state_map_update_fail |
| 13 | seen_query_ids_update_fail |
| 14 | writer_push_fail |

Unreadability, reset, wrap, or incomplete accounting uses known=0. Known zero
is `01` followed by 120 zero bytes. Lifecycle-sensitive comparisons use slots
0, 1, 2, 10, 12, 14, as specified by the logical contract.

## 2. File header

The v4 36-byte header prefix is retained, followed immediately by a mandatory
v5 extension. The total header length H includes the prefix and header CRC.
The first block starts at offset H, without alignment. Empty files may contain
only a valid header; they certify nothing.

| File offset | Type | Field |
|---:|---|---|
| 0 | u32 | magic = 0x54574750; bytes `50 47 57 54` (PGWT) |
| 4 | u16 | trace version = 5 |
| 6 | u16 | mutable legacy flags; see below |
| 8 | u32 | PostgreSQL version metadata, opaque to evidence replay |
| 12 | u64 | start_time_ns, realtime at file creation |
| 20 | u64 | clock_offset_ns, monotonic clock at file creation |
| 28 | u64 | nominal_sample_period_ns; 0 if unspecified |
| 36 | 4 bytes | extension magic `45 56 44 31` (EVD1) |
| 40 | u32 | H, total header bytes |
| 44 | u16 | descriptor schema version = 1 |
| 46 | u16 | reserved = 0 |
| 48 | bytes16 | boot, length 1..255 |
| next | bytes16 | capture, length 1..255 |
| next | u64 | file_no |
| next | bool | has_previous |
| if previous | u64 | previous_file_no |
| if previous | u64 | previous_data_end, absolute exclusive byte offset in predecessor |
| if previous | 32 bytes | previous_sha256, raw digest bytes in standard SHA-256 output order |
| next | bool | qualified |
| next | counters | loss_baseline |
| next | u32 | mapping_count |
| repeated mapping_count times | mapping entry | §3 |
| H-4 | u32 | header_crc32 |

Every field must end exactly at H; unknown extensions and trailing bytes are
not permitted. The smallest header is 73 bytes (one-byte identities, unknown
baseline, no predecessor, no mapping). H is 73..16,777,216, additionally subject
to the actual field sizes. First bound H, then parse all fields inside it.

The header CRC uses §4's CRC-32 over the concatenation of file bytes `[0,6)`
and `[8,H-4)`. The two flag bytes are **omitted**, not replaced by zeros; the
CRC field itself is excluded. All immutable descriptor and mapping bytes are
covered. An invalid header admits no blocks from that file.

Legacy flag bits are 0x0001 LZ4, 0x0002 closure-tracked, 0x0004 closed-clean,
0x0008 rotated, and 0x0010 recovered. In v5 these bytes are advisory only;
block compression comes exclusively from each block's codec byte. Writers may
set the LZ4 hint if they use LZ4, and preserve the legacy closure meanings.
Readers ignore unknown flag bits for exact replay. Changing any of these bits
cannot change parsing, qualification, integrity, identity, or certification.
This permits recovery's in-place write at file offset 6.

boot identifies one monotonic clock domain; capture identifies one collector
incarnation and must be fresh on restart, including restart in the same boot.
They are opaque nonempty identifiers, not required to be UUIDs. A producer must
ensure their uniqueness in the collection being read. Reuse or conflicting
descriptors is ambiguous input, not grounds for stitching. file_no begins at 0
and increments by one without wrap. For file 0 has_previous MUST be 0; for every
other file it MUST be 1 and previous_file_no MUST equal file_no-1.

Every successor repeats boot, capture, qualified, **the capture's original**
loss_baseline, and the identical mapping entries. A successor header does not
reset the current baseline, which is changed only by replayed VOID records.
Other creation metadata may differ. Unknown baseline is representable even in
a qualified capture; it enables no certificates until restored by the contract's
readable-VOID and fresh-attachment protocol. qualified=1 asserts the capture
proof obligations, not a property established by the codec. qualified=0 retains
verified LIFE only. Missing qualification is malformed v5, not implicit true.

## 3. Authoritative event mapping

A mapping entry is `u32 id; u8 role; bytes16 name`. Entries are in strictly
increasing numeric ID order. IDs 0 and 0xffffffff MUST NOT appear. role is 1
(wait event) or 2 (administrative marker); other values reject the header.
Names are nonempty, at most 1024 bytes, unique across all entries, and never
exactly the five bytes `$EXIT`. The complete mapping is immutable and repeated
in every file of a capture; there is no sidecar fallback or incremental update.
An unknown event requires a new capture with a complete mapping, or a reported
capture failure; it cannot silently become nonwaiting or a guessed name.

| Reference/context | Meaning |
|---|---|
| ID 0 in seed, state, TRANS.old or TRANS.new | null / nonwaiting (`{NULL,0}` in logical C input) |
| ID 0 in a SAMPLE | The retained v4 CPU sample meaning; never an exact wait edge |
| ID 0xffffffff in TRANS.new | Reserved exit sentinel, logical new=`$EXIT` |
| ID 0xffffffff anywhere else | Invalid |
| role=1 ID | Exact mapped name bytes |
| role=2 ID | Marker only under the type-0 marker shape below; never a wait |
| Unmapped nonzero ID | Record fault under §9; never interpret as ID 0 |

seed/state and ordinary TRANS.old must be ID 0 or role=1; TRANS.new may
additionally be the exit sentinel. Administrative type-0 rows are recognized
only when old=new is the same role=2 ID and duration=0. Any other use of a marker
ID is a record fault under §9. Marker names are diagnostic; spelling cannot turn a
marker into a wait. SAMPLES accept ID 0 or role=1 only.

Scenario adapters resolve names by exact lookup, using ID 0 for JSON null and
0xffffffff for TRANS.new=`$EXIT`. They must construct a complete mapping before
writing the header. Numeric assignment is producer-chosen; ascending ID order
and unique names make reverse lookup unambiguous. The mapping plus checksums,
not a PostgreSQL version's compiled-in table or `wait_event_names.json`, is the
authority for reading those bytes.

## 4. Block framing and integrity

All three block types use this 64-byte v5 header, retaining the 48-byte v4
prefix. A block at absolute offset B occupies exactly `64 + S + 4` bytes, where
S is compressed_size. The four final bytes are block_crc32, outside S and U.

| Relative offset | Type | Field |
|---:|---|---|
| 0 | u64 | first_timestamp_ns |
| 8 | u64 | last_timestamp_ns |
| 16 | u32 | num_events, 1..4096 |
| 20 | u32 | compressed_size = S, 1..10,485,760 |
| 24 | u32 | uncompressed_size = U, 1..10,485,760 |
| 28 | u16 | block_type: 0 TRANSITIONS, 1 SAMPLES, 2 EVIDENCE |
| 30 | u16 | reserved = 0 |
| 32 | u64 | sample_period_ns |
| 40 | u64 | max_sample_weight_ns |
| 48 | u64 | self_offset = B |
| 56 | u32 | preceding_crc32 |
| 60 | u8 | codec: 0 raw, 1 raw LZ4 block |
| 61 | 3 bytes | reserved = `00 00 00` |
| 64 | S bytes | stored payload |
| 64+S | u32 | block_crc32 |

preceding_crc32 equals header_crc32 for the first block and the immediately
preceding physical block's block_crc32 thereafter, **including across block
types**. self_offset must equal the actual offset reached by sequential scan.
There are no gaps or alignment bytes. These checks detect interior removal,
insertion, reordering, or substitution when a retained later block exposes the
broken offset/CRC chain, subject to CRC collisions. Removal of a final complete
suffix is not detectable without a successor or trusted external length (§8). They are storage integrity fields, not producer event sequence
numbers or lifecycle identity tokens. They reset at each file header; file
continuity is separately bound by the predecessor SHA-256.

The CRC is **CRC-32/ISO-HDLC** (not CRC-32C): width 32, polynomial 0x04c11db7,
reflected polynomial 0xedb88320, initial register 0xffffffff, reflected input
and output, final XOR 0xffffffff. This equivalent byte algorithm fixes all
conventions; operations on c are unsigned 32-bit:

```
c = 0xffffffff
for each byte b in the specified byte stream:
    c = c XOR b
    repeat 8 times:
        c = (c >> 1) XOR (0xedb88320 if (c AND 1) else 0)
return c XOR 0xffffffff
```

Check value for ASCII `123456789` is 0xcbf43926 (stored `26 39 f4 cb`).
block_crc32 covers **all 64 header bytes followed by exactly S stored payload
bytes**, excluding only its own four bytes. It covers the schema version inside
the payload, count, sizes, type, offsets, previous CRC, codec and reserved bytes.
Check CRC before decompression or exposing records. Header CRC has the distinct
coverage defined in §2. Neither checksum is authentication against a party
that can rewrite bytes and recompute checksums.

codec=0 requires S=U. codec=1 is allowed only for types 0 and 1: one raw LZ4 block,
without frame wrapper, dictionary or checksum supplied by LZ4. It must consume
all S bytes and produce exactly U bytes. Its token's high nibble is literal
length (15 extends by successive bytes, adding 255 until a byte <255); copy the
literals, and if input ends this is the final literal-only sequence. Otherwise
read a nonzero little-endian u16 match distance, no greater than output produced;
match length is low nibble +4 (nibble 15 uses the same extension rule). Copy from
that distance back, allowing overlap, then repeat. Reject input/output overruns,
zero or out-of-range distances, missing extensions and missing final literal
sequence. Encoders MUST satisfy, and decoders MUST reject violations of, these
LZ4 end constraints: the last sequence is literals only; the last five decoded
bytes are literals when decoded length >=5; and, if any match exists, the last
match starts at least twelve decoded bytes before the end. Small blocks may use
an all-literal sequence. v5 writers may always choose raw encoding instead.

EVIDENCE MUST use codec=0: no compression inside or around its payload. Types
0 and 2 have both sample-weight fields zero. For type 2 the two timestamp fields
are non-semantic hints; writers MUST write zero, readers MUST accept any u64
values there without using them for selection or replay. Even inverted hint
values do not reject a type-2 block. Footer timestamps for type 2 are equally
non-semantic. **Evidence replay never prunes by time**, whether by these fields,
footer index, file creation time, transition time, or the user's query window.
It scans and validates the entire available capture prefix; a later VOID can
revoke certificates relevant to an earlier query.

## 5. EVIDENCE payload, version 1

num_events means record count, not transition count. The raw payload starts:

| Relative payload offset | Type | Value |
|---:|---|---|
| 0 | u16 | payload_version = 1 |
| 2 | u16 | reserved = 0 |
| 4 | u32 | record_count, exactly equal to num_events |
| 8 | records | exactly record_count consecutive framed records |

A record is `u8 kind; u32 body_length; body[body_length]`. body_length excludes
the five-byte frame; it is 8..65,535. Every body starts with the common u64 t,
independently of kind; the remaining bytes must exactly match the kind's fields.
No padding or unrecognized tail fields. Last record ends exactly at U. Validate
all outer frames before exposing any record or record fault. An out-of-bounds
frame, missing common t, or unconsumed payload suffix rejects the segment; a
kind-specific body error within an intact frame is a record fault under §9.
record_offset is the byte offset of its kind octet from the start of the decoded
payload; for EVIDENCE this is also its stored payload offset.

Wire kind values deliberately have their own namespace; they are not a memory
dump of `pgwt_evidence_kind`:

| Wire kind | Name | C logical kind |
|---:|---|---|
| 1 | LIFE | PGWT_EVIDENCE_LIFE |
| 2 | ATTACH_OK | PGWT_EVIDENCE_ATTACH_OK |
| 3 | FRONTIER | PGWT_EVIDENCE_FRONTIER |
| 4 | END | PGWT_EVIDENCE_END |
| 5 | VOID | PGWT_EVIDENCE_VOID |

All other kinds, including 0 and 6..255, are unsupported record kinds and act
as global loss under §9 when their frame and common t are intact. There is
**no TRANS or OTHER kind in type 2**; TRANS lives in type 0. In particular wire
kind 3 is FRONTIER, not the C enum's numeric TRANS value.

Each body is the concatenation below; all stated fields are mandatory, including
presence bytes. `generation` abbreviates `u32 pid; optional_u64 fork`. pid is
1..0xffffffff. Absent fork identifies the capture-local provisional generation;
present fork=0 is a distinct observed generation. There is no generation field
in FRONTIER or VOID.

| Kind | Body in byte order |
|---|---|
| LIFE | `u64 t; generation; u64 start; bool start_known; optional_u64 exit` |
| ATTACH_OK | `u64 t; generation; u8 method; event_ref seed; event_ref state; counters` |
| FRONTIER | `u64 t; counters` |
| END | `u64 t; generation; u8 reason` |
| VOID | `u64 t; counters; bytes16 reason; optional_u64 invalid_from` |

ATTACH_OK method is 0 transition, 1 reread; other values are record faults.
Map 1 to reread=true in the logical type. END reason is 1 detach, 2 deescalate,
3 stop; other values are record faults. Map to exactly those ASCII strings.
VOID reason is an opaque nonempty diagnostic, not this END enum and not
merge-v1's closed reason enum. Its bytes must be preserved; replay must not branch on their spelling.

LIFE requires start<=t; if start_known=1, fork is present and start=fork; if
start_known=0, fork is absent. If exit is present, start<=exit<=t. An observed
fork on any per-backend record must be <=t. invalid_from is optional; present 0
revokes from the start, absent retains prior certificates. It is not inferred
from t, and the codec imposes no ordering between t and invalid_from beyond
both being u64 (the semantic producer must choose a conservative boundary).

ATTACH_OK and FRONTIER MUST have known=1 and fifteen counters. An encoder MUST
refuse a certificate with unknown counters; it must report the loss using VOID
with counters=null instead. A reader can delimit a known=0 certificate but MUST
treat it as a record fault/global loss in a qualified capture under §9, not
silently ignore it or certify it; unqualified certificate handling is specified
there separately. Unknown counters remain valid in the descriptor and VOID.
Equality to the readable current baseline, LIFE consistency, run assignments,
counter changes,
frontier advancement, and retrospective revocation are replay checks under
CONTRACT.md. An otherwise valid certificate with a mismatching vector acts as
global VOID with that vector, not a certificate. CRC success proves none of
these semantic conditions.

## 6. Retained type-0 and type-1 payloads

For independent implementation, these are the complete decoded column layouts.
N is the **full** num_events, regardless of caller output capacity. Each column
contains N entries in row order; concatenate columns, with no column header.
All fixed-width columns are little-endian in v5.

| Column | Type 0 TRANSITIONS | Type 1 SAMPLES |
|---:|---|---|
| 1 | N uleb64 timestamp deltas | N uleb64 timestamp deltas |
| 2 | N u32 pid | N u32 pid |
| 3 | N u32 old_event IDs | N u32 event IDs |
| 4 | N u32 new_event IDs | N u64 query_id |
| 5 | N uleb64 duration_ns | absent |
| 6 | N u64 query_id | absent |
| 7 | N uleb64 cpu_ns | absent |

For timestamps, prev starts at zero **in each block**. Encode delta as
`(timestamp-prev) mod 2^64`; decode timestamp as `(prev+delta) mod 2^64`, then
replace prev. This explicitly preserves v4's unsigned arithmetic for physically
late observations without sorting records. Each uleb64 itself must be in u64
range and minimal. Other arithmetic must not wrap. First and last header times
for types 0/1 equal the first and last decoded row times (not their min/max).
These type-0 endpoints are NOT range bounds: v5 readers MUST NOT prune type-0
blocks, mark them beyond the query, or stop scanning using these header fields
or their footer timestamps, including in the legacy transition/sample view.
They must decode and inspect every type-0 row for range selection. For example,
row times [10,100,20] must not lose the interior row at 100 in a query [90,110).
Evidence replay scans every block regardless of type or query (§4).
All columns must consume exactly U bytes, with no suffix. Ordinary TRANS and
SAMPLE rows require pid>0. Only a valid role=2 marker shape (§3) permits pid=0;
it also permits a positive pid. This exception never creates backend identity.
A decoder with a small caller buffer must validate/decode all N entries and
record faults before releasing any; it may then page the ordered batch, keeping
original positions.

An ordinary type-0 row produces logical TRANS with t=timestamp, pid, old/new
resolved by §3 and duration=duration_ns. duration<=t. Ordinary old=new is invalid;
administrative same-event flushes must be replaced with END before serialization.
Valid marker-shaped rows (§3) MUST be omitted from the exact adapter's record
array; never emit PGWT_EVIDENCE_OTHER. Their physical bytes and positions remain
validated for CRC, linkage, and diagnostic views. query_id and cpu_ns are
retained unsigned metadata, not inputs to exact replay; cpu_ns=0xffffffffffffffff
means unknown measured CPU, encoded as `ff ff ff ff ff ff ff ff ff 01`. No
reader-side annotation flags are stored. TRANS carries no fork on wire; leave
has_fork=false in the logical row and resolve its generation from LIFE and time
under the contract, not from whichever PID is currently live. This absence on
TRANS does not assert the provisional generation. TRANS.counters is unused.

SAMPLES retain v4 weighting: all rows at the first tick have weight
sample_period_ns; each later distinct timestamp has weight equal to its positive
difference from the preceding distinct timestamp, shared by all rows of that
tick. Timestamps in a sample block are nondecreasing. max_sample_weight_ns equals
the largest effective weight in the block. Zero first weight is representable;
it is not exact evidence. When splitting a tick across blocks, the next block's
first weight must repeat that tick's original weight, not become zero. Samples
remain point observations, never exact TRANS, LIFE or certificates. For the
exact adapter valid samples MUST be omitted, never represented as OTHER. Their
bytes, weights and positions are still validated. Invalid rows are handled by
§9; omission is not permission to hide a record fault.

## 7. Physical order, publication, and logical adapter

The logical append order is file_no, then physical block offset, then payload
record order. Type-0/1 rows use their column row order. For the exact stream
(types 0 and 2), a writer batches a contiguous run of one type and flushes it
before writing the other type. In particular it MUST flush pending TRANSITIONS
before **any** later EVIDENCE, not just before FRONTIER; pending EVIDENCE likewise
cannot be overtaken by later TRANSITIONS. Choosing an exact block by its first
timestamp is not permitted.

Type-1 samples may be buffered separately and inserted at complete block
boundaries without flushing a pending exact block on every sample arrival.
Their placement MUST NOT reorder the type-0/type-2 subsequence or reorder sample
rows within their stream. Sample tick/weight rules still apply. Once placed,
every sample block participates in offsets, CRC chaining and predecessor SHA.
All sample acquisition, queue, and writer failures still feed the global loss
protocol; batching cannot defer detection/accounting of failure past a
certificate. Metadata sidecar updates and fsync per type alternation are not
required by this format. The fixed block overhead is 68 bytes; the exemption
avoids paying that overhead for every exact/sample alternation.

Before a FRONTIER is published, every dependency through its cutoff, including
ties, delayed LIFE, anchor TRANS and transitions through a pending END, must
have been appended and successfully flushed. A single EVIDENCE block may carry
dependencies followed by FRONTIER; its atomic validation admits them in order.
It cannot carry a TRANS dependency; those must already be in earlier type-0
blocks. Persistence claims additionally require successful ordered persistence.
File flags, CRCs and zero counters cannot prove a producer boundary.

A failed block write, flush, or required persistence operation is fatal for
that capture. Stop publication and append no further certificate or successor
under that capture identity; report the failure. A later capture uses a fresh
identity, LIFE and attachments. Retain only the independently validated prefix;
never drop the failed block and certify past it. If failure prevents persistence
of a required revocation, report the affected capture unavailable out of band
as CONTRACT.md requires. This design chooses fatal failure rather than an
in-place null-counter VOID recovery protocol.

LIFE may follow a transition. ATTACH_OK and END may physically precede or follow
their associated transitions; resolve by contract time/generation rules before
applying FRONTIER. Do not globally time-sort the append stream. Per PID, order
TRANS by timestamp, preserving physical append order at ties. The collector
must supply actual causal transition order at ties. Do not break ties by names,
IDs, query IDs, kind, or duration.

Physical identity is `(boot,capture,file_no,block_offset,record_offset)`. boot
makes the contract's capture identity explicitly clock-scoped. In type 0/1,
record_offset is the decoded-payload byte offset of that row's timestamp varint
in column 1. In type 2 it is the kind offset (§5). This permits stable identities
without inventing an event sequence number. Equal contents at different
positions are always distinct, including zero-duration waits. Only repeated
delivery of the same validated physical position may be suppressed; conflicting
bytes at that position are an error. Copying records into a successor is
forbidden. Repacking creates a different physical artifact, not a continuation
or deduplication identity for an existing capture.

Reconciliation of several deliveries of one physical file is governed by [REDELIVERY.md](REDELIVERY.md), which takes precedence where they differ.

Deduplication MUST occur in the codec before building the logical record array:
pgwt_evidence_record has no physical-position field. Codec conformance tests
must cover repeated delivery of one position (emitted once), identical rows at
different positions (both retained, including zero waits), conflicting bytes at
one position (error), and replay of an identical file (no duplicate chain member).
Omitting markers/samples MUST NOT renumber surviving rows or their positions.
These are required future codec tests, not a claim that this design implements them.

The adapter supplies version=5, present=true, verified descriptor fields,
previous ordinal, and records in this order to `pgwt_evidence_file`. SHA, CRCs,
map roles, offsets and provenance must be validated and retained by the codec;
the logical C type's ordinal-only previous field is insufficient for doing so.
Unused C fields are initialized to zero/false. Known=false ignores value[];
absent optionals ignore their value storage. No `unsupported_shape` bit is
serialized; it reports an adapter failure. Do not pass partially decoded records
to replay. Record faults require the ordered loss path in §9; merely setting
unsupported_shape in the reviewed aa31b588 API would wrongly refuse the whole
input. That replay error path must change before this behavior is implemented.
Preserve input string storage for the logical API's borrowed lifetime.

## 8. File chain, footer, and predecessor digest

Let D be the offset immediately after the last complete block's CRC (or H for
an empty data region). A finalized file may append the retained v4 footer:
N entries of `u64 timestamp_ns; u64 file_offset`, in physical block order,
followed by u32 N. N is the actual block count, including all types; zero is
allowed. Footer size is exactly `16*N+4`. Each offset equals the corresponding
validated block offset; timestamps repeat that block's first_timestamp_ns.
Type-2 writers therefore put zero in their footer timestamp entries. Mixed
indexes can be nonmonotonic (for example, a positive type-0 timestamp followed
by type 2); they are not necessarily unsorted in every file. A reader must test
sortedness before any generic binary search and otherwise fall back to a scan.
Even sortedness does not make type-0 endpoint timestamps safe range bounds (§6).
Evidence replay always scans the validated prefix; the footer is only an index
hint, and fallback costs performance, never coverage or correctness.

Scan sequentially from H. At a block boundary, a footer is recognized only if
**all remaining bytes** exactly match this reconstructed footer. Otherwise try
the next complete block, or report a rejected/truncated tail. Never trust a
footer's count or offsets to skip validation, choose D, or hide later blocks.
EOF at a block boundary is valid without a footer and conveys no clean-close
claim. A torn/bad footer may be discarded as a rejected tail, preserving the
validated blocks; it cannot excuse an earlier malformed block. Recovery may
truncate rejected tail bytes and rebuild the footer, without changing any
validated header or block bytes except the header flags.

A successor's previous_data_end is the predecessor's D at rotation. The
predecessor must be sealed against further block appends before the successor
is published. Define its integrity digest as **SHA-256** (the standard 256-bit
SHA-2 function, FIPS 180-4), with these bytes as its entire message, concatenated
in this order:

```
predecessor bytes [0,6) || predecessor bytes [8,previous_data_end)
```

No domain string, implicit length prefix, zero placeholders or host structure is
included. SHA-256's standard message padding is internal to SHA-256; the stored
32 bytes are its conventional digest output, not eight little-endian u32 words.
This covers every immutable header byte, the header CRC, mapping, and **all**
block headers, stored payloads and block CRCs of all three types, in physical
order. It excludes exactly header flag bytes 6..7 and the entire footer starting
at D. It also excludes any rejected tail; such a tail is not permitted in a
predecessor admitted for continuation. Empty-file digest uses D=H.

To admit a successor, first require a complete validated predecessor data
region ending exactly at previous_data_end, and no further bytes except an
absent or exactly valid footer. Then verify digest, immediate ordinal, matching
boot/capture, qualification, original baseline and mapping. A shorter prefix
with a valid CRC is insufficient; extra data blocks after the referenced D are
also invalid for this link. If recovery removed any referenced block bytes,
the link fails; rebuilding only the footer or changing flags leaves it valid.
The same ordinal with conflicting physical files is ambiguous and must not be
chosen by timestamp. Re-delivery of the identical validated file is not a new
chain member.
Reconciliation of several deliveries of one physical file is governed by [REDELIVERY.md](REDELIVERY.md), which takes precedence where they differ.

Start each capture at file 0. A missing, corrupt, unsupported or mismatched file
stops that capture; no later same-capture file can repair it or be read as an
independent root. Earlier validated prefix certificates remain subject to all
VOIDs in that prefix. Independent captures can still be read separately; never
stitch them, even for the same boot/PID. A boot change changes clock domain.

Integrity is corruption detection, not authentication or proof of a complete
archive: CRCs have collisions and unkeyed hashes can be recomputed. With no
successor or trusted external length, removing a final suffix at a complete
block boundary is indistinguishable from an earlier crash/snapshot. It yields
only that earlier prefix; no missing END, exit, or frontier may be invented.
In particular a withheld later revocation cannot be recovered from old bytes;
CONTRACT.md requires out-of-band unavailability if it cannot be persisted.

Live readers must take a stable snapshot through a complete validated block
boundary, retain that boundary as read provenance, and incorporate all later
published blocks (including retroactive VOID) before claiming an updated result.
Mutable meta sidecars, footer indexes and closure flags are advisory; none may
substitute for the validation above or claim that no later revocation exists.

## 9. Refusal and compatibility

Validate a complete header or segment before exposing any ordered batch. Report
one of two distinct reader failures; neither is an empty successful decode.
The unqualified-certificate exception below applies to record-fault effects:

| Condition | Required result |
|---|---|
| Trace version other than 5, descriptor version !=1, bad extension magic or invalid descriptor/mapping | Reject file header; use a separate legacy decoder where applicable |
| Unsupported payload version, block type or codec; nonzero header/payload reserved field | Reject whole segment; framing/schema is not trusted |
| Count 0 or >4096, unequal counts, oversized outer lengths, unsafe arithmetic, missing common t | Reject whole segment before unsafe allocation or copying |
| Truncated outer record frame/body, payload or CRC; unconsumed payload suffix; undecodable column layout | Reject whole segment; no complete-looking records from inside it escape |
| Bad header/block CRC, bad predecessor digest, wrong self_offset or preceding CRC | Reject affected unit/link; stop chain there |
| Malformed LEB128/LZ4, wrong output size, inconsistent block metadata/weights | Reject whole segment |
| Unknown record kind, method or END reason inside a bounded frame | Record fault: global loss at preceding F; continue in physical order |
| Invalid record boolean/optional/string, kind-specific body size/tail, invalid PID or fork/time, invalid LIFE local fields | Record fault if outer frame and common t remain trustworthy; otherwise reject segment |
| Unresolved event ID, forbidden marker/same-event row, duration>t | Record fault if the entire column layout/row boundaries are trustworthy; otherwise reject segment |
| ATTACH_OK/FRONTIER with known=0 | Record fault: global loss with unknown counters; never a certificate |

A rejected segment stops the chain at the preceding validated block, as in
CONTRACT.md §6. The caller receives the stopped position and reason. Earlier
blocks may be replayed only as an explicitly bounded prefix; the failed full
decode is not successful. A failed first header gives no verified LIFE. A crash
mid-block cannot salvage its earlier complete-looking records.

For a record fault, trust only the validated capture identity, position, outer
frame/column boundaries, and common u64 t. Validate the entire enclosing block
before delivering anything. Deliver valid records and explicit fault notifications
in physical order, including valid records before and after the fault in that
same block. Do not deliver the malformed record as ordinary evidence. A fault
notification applies the contract's global VOID effect at the preceding accepted
F: freeze all active runs, drop uncertified suffixes and pending ENDs, retain
surviving certificates and publication high-water H, and continue scanning. For
a malformed/unsupported record use unknown counters and invalid_from=null;
its t is the detection instant, never a coverage endpoint. This conservatively
taints existing identities under §4 of CONTRACT.md. A later readable VOID,
fresh eligible observed fork/LIFE and fresh ATTACH_OK are required for recovery.
The notification is an adapter control result, not an extra wire kind, not a
serialized replacement record and not permission to resynchronize corrupt bytes.
It must carry a diagnostic and physical position to the caller. If capture
identity, framing or common t cannot be trusted, stop instead. In particular an
unknown payload schema cannot be interpreted as a framed unknown record kind.

Semantic inconsistencies requiring multiple intact records follow the contract:
contradictory LIFE invalidates that generation; ambiguous assignment withholds
affected certification; a run consistency failure drops the tentative suffix
and acts as global VOID. An otherwise valid certificate's unexpected counter
vector acts as VOID with that actual vector (invalid_from=null), so lifecycle
sensitivity still depends on its changed slots. Invalid frontiers do not advance
F or H. Explicit valid VOID records retain their encoded invalid_from and vector;
they are never replaced by the malformed-record default. In an unqualified
capture, the contract's explicit certificate exception takes precedence over
record-fault effects for recognizable ATTACH_OK/FRONTIER/END: report their
semantic errors diagnostically but ignore those certificate-shaped records,
so they cannot change retained LIFE or create coverage. Framing/integrity
validation still applies. LIFE and explicit VOID retain the contract's rules;
unknown kinds have no certificate exemption. Byte checks never authorize
certification. Writers MUST reject all malformed input before emitting its
block; failure cannot be hidden by silently dropping a record and continuing
with later certificates.

The reviewed replay at aa31b588 performs whole-input refusal for record faults,
including unknown certificate counters, and some run consistency errors. That
contradicts CONTRACT.md §5 and is not this codec's required behavior. The replay
must gain ordered fault/global-loss handling; passing OTHER or relying on its
current scope() rejection does not implement this specification. No replay or
contract source change is made by this design revision.

The unchanged v4 reader at the pinned source's `src/event_reader.c:37` rejects
header versions above 4 before reading any block. It cannot skip EVIDENCE and
retain exact continuity. The unchanged v4 writer's recovery at
`src/event_writer.c:603-616` treats a v5 `current.trace` as unreadable and attempts
to rename it to `current.trace.corrupt.<timestamp>` (and removes its meta file);
it does not upgrade or resume it. A failed rename is an I/O failure, not support
for v5. Do not put v5 blocks under a v4 header. A version-aware implementation
must replace the old `version>=4` header-size shortcuts with explicit v5 sizes.

Legacy v2-v4 decoding/sample meanings remain separate and unchanged; they have
no exact evidence-v1 LIFE or certificates. Never backfill LIFE from transition
extrema, PID metadata, checkpoints or clean flags. Supporting v5 reading does
not authorize switching the default daemon writer from v4.

## 10. What this draft does not freeze

This freezes the proposed bytes, validation boundaries, event mapping, checksum
coverage, predecessor linkage and storage order above. Changing them after
acceptance requires an explicit versioned contract change; reserved fields are
not extension escape hatches. It does not freeze an implementation/API, buffer
sizes within these limits, batching targets, choice of raw versus LZ4 for types
0/1, compression level, numeric event-ID assignments, identifier generation
mechanism, filenames, directories, retention policy, sidecar publication
protocol, UI, summary format, default writer version, or delivery schedule.

It does not implement or qualify FRONTIER barriers, coherent rereads, coherent
END instants, producer ordering, loss-counter completeness, capture overhead or
power-loss behavior. Those remain the logical contract's proof obligations.
No kernel birth token, per-event sequence number, periodic per-backend open
checkpoint, extra evidence kind, or new metric semantics are introduced.
