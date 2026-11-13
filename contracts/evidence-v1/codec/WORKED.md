# Hand-worked v5 codec examples

These are three serialization examples derived from CODEC.md, not replay oracle
answers. Field bytes, sizes and offsets were laid out by hand from the document;
CRC arithmetic was evaluated using its bit recurrence and cross-checked with a
standard-library CRC-32 calculation. No product encoder, decoder or replay
output supplied the expected bytes. This distinction is intentional: the CRC
values are mechanically checked arithmetic, not a claim of manual long division.
Revision r2 independently rebuilt all 646 bytes from the field layouts, compared
them with the literal hex below, and recomputed every CRC and the predecessor
digest. No serialized byte, offset, final CRC or digest changed. Five intermediate
register values in the one-zero-byte demonstration were corrected (see below).

All hex blocks below contain literal bytes only. Whitespace is not part of the
encoding. Concatenate the shared header and examples 1, 2, 3, in that order, to
obtain one complete 646-byte footerless file. The examples are successive blocks,
not three standalone files; an isolated block retains the stated header/earlier
block assumptions. No byte is represented by ellipsis, repetition syntax or a
placeholder.

## Shared assumptions and exact header

Every choice not dictated by CODEC.md is an assumption for these examples:

- boot is the one-byte string `b` (62); capture is `c` (63). These toy IDs are
  assumed unique in this fixture collection; no UUID conversion is implied.
- This is file_no=0, has_previous=false. Thus no predecessor number, end offset
  or SHA-256 bytes occur in the header.
- qualified=true is an input assumption, not a tested capture claim. The initial
  baseline is known, with all fifteen counters zero, in CODEC.md's slot order.
- PostgreSQL-version metadata, realtime, monotonic-creation metadata, nominal
  sample cadence and mutable flags are all zero. Zero flags are permitted for
  raw v5 blocks and say nothing about capture qualification.
- There is exactly one mapping: ID 1, role 1 (wait), name `A` (41). No category
  prefix, marker, implicit name table or external sidecar is assumed.
- All blocks use raw codec 0. EVIDENCE timestamp hints and sample-weight fields
  are zero. Type-0 first/last timestamps equal its only row's timestamp.
- PID 7 names one provisional generation: fork is absent, never present zero.
  Times and durations below are small, chosen monotonic nanosecond values.
- There is no footer, later file, hidden record or supplied FRONTIER. The file
  exercises serialization only and by itself certifies no exact wait result.

Header size calculation:

```
36 common bytes + 12 fixed extension bytes
+ (2+1) boot + (2+1) capture + 8 ordinal + 1 previous flag
+ 1 qualified + (1+15*8) baseline + 4 mapping count
+ (4+1+2+1) mapping entry + 4 CRC = 201 = 0xc9
```

Offsets: boot=48, capture=51, file_no=54, has_previous=62, qualified=63,
baseline.known=64, counter values=65..184, mapping_count=185,
entry.id=189, entry.role=193, entry.name length=194, name byte=196,
header CRC=197..200. CRC message length is 195: bytes `[0,6)` plus `[8,197)`.
The CRC value is 0x1e3a7196, stored little-endian as `96 71 3a 1e`.
Each of the fifteen eight-zero lines following the three one-byte flags is one
counter value, slots 0 through 14.

```hex
50 47 57 54 05 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
45 56 44 31 c9 00 00 00 01 00 00 00
01 00 62 01 00 63
00 00 00 00 00 00 00 00 00 01 01
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
01 00 00 00 01 00 00 00 01 01 00 41
96 71 3a 1e
```

## 1. LIFE followed by ATTACH_OK: two records in one EVIDENCE block

Additional assumed inputs, in append order:

```
LIFE(t=1, pid=7, fork=null, start=0, start_known=false, exit=null)
ATTACH_OK(t=2, pid=7, fork=null, method=reread,
          seed="A", state="A", counters=[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0])
```

The lower bound start=0 is chosen, not inferred from discovery t=1. The reread
is assumed coherent for purposes of the logical record; it does not establish
A's entry time. The counter vector is known, so its first byte is 01.

LIFE body is `8 t + 4 pid + 1 absent fork + 8 start + 1 start_known + 1 absent
exit = 23 = 0x17`. Its frame is `01 17 00 00 00`, followed by those 23 bytes.
ATTACH_OK body is `8 t + 4 pid + 1 absent fork + 1 method + 4 seed + 4 state
+ 121 counters = 143 = 0x8f`. Its frame is `02 8f 00 00 00`.
Both A references are the four bytes `01 00 00 00`, not inline strings.

Payload size is `8 + (5+23) + (5+143) = 184 = 0xb8`; both counts are 2.
Record offsets within the payload are 8 (LIFE) and 36 (ATTACH_OK). Block starts
at file offset 201 (0xc9). Its preceding CRC is the shared header's CRC.
CRC input is 64+184=248 bytes; final CRC is 0x3b714f39. Including its four
CRC bytes, block length is 252 and the next block starts at 453 (0x1c5).

The first four lines below are the 64-byte block header. The following lines
are the payload prefix, framed LIFE, framed ATTACH_OK, fifteen counters, and CRC.

```hex
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
02 00 00 00 b8 00 00 00 b8 00 00 00 02 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
c9 00 00 00 00 00 00 00 96 71 3a 1e 00 00 00 00
01 00 00 00 02 00 00 00
01 17 00 00 00
01 00 00 00 00 00 00 00 07 00 00 00 00
00 00 00 00 00 00 00 00 00 00
02 8f 00 00 00
02 00 00 00 00 00 00 00 07 00 00 00 00 01
01 00 00 00 01 00 00 00 01
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
39 4f 71 3b
```

Physical record identities (including boot) are `(b,c,0,201,8)` and
`(b,c,0,201,36)`. No semantic deduplication is performed.

## 2. A TRANS uses type 0 and the header's name mapping

Additional assumed input after example 1:

```
TRANS(t=5, pid=7, old="A", new=null, duration=3)
```

The retained metadata is assumed query_id=0 and cpu_ns=0 (a supplied measured
zero, not UNKNOWN). Neither is an exact-replay counter. The row carries no fork.
The example is a genuine A-to-nonwaiting change, not a same-event flush. Its
stored old start is 5-3=2, but an earlier reread alone does not prove that entry;
these bytes do not override the logical contract's unknown-entry rule.

There is one row, so the seven column byte lengths are 1+4+4+4+1+8+1=23.
Timestamp delta from the block-local zero is 5 (`05`). The other columns are
PID `07 00 00 00`, old ID `01 00 00 00`, new ID `00 00 00 00`, duration `03`,
eight zero query-ID bytes, and one zero CPU varint byte. No EVIDENCE payload
prefix or per-record kind/length occurs in a type-0 block.

Block offset is 453 (0x1c5); both timestamp hints are 5; num_events=1;
S=U=23 (0x17); type=0; preceding CRC=0x3b714f39. CRC input is 87 bytes;
CRC is 0x77d5698f. Total block length is 91, next offset 544 (0x220).
The physical row identity is `(b,c,0,453,0)`, its timestamp varint being at
payload offset 0. The earlier pending EVIDENCE block has already been flushed.

```hex
05 00 00 00 00 00 00 00 05 00 00 00 00 00 00 00
01 00 00 00 17 00 00 00 17 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
c5 01 00 00 00 00 00 00 39 4f 71 3b 00 00 00 00
05 07 00 00 00 01 00 00 00 00 00 00 00 03
00 00 00 00 00 00 00 00 00
8f 69 d5 77
```

## 3. VOID: unknown counters and present invalid_from=0

Additional assumed input after example 2:

```
VOID(t=9, counters=null, reason="x", invalid_from=0)
```

The diagnostic is deliberately opaque one-byte `x` (78). It is not a closed
enum. Unknown counters consume one zero byte and **no** value array. The
revocation boundary is assumed conservatively zero, and is present: `01`
followed by eight zero bytes. This differs from absent invalid_from, which
would consume just `00` and retain prior certificates. Detection t=9 does not
replace the encoded rollback boundary. With no earlier FRONTIER in this small
file there happens to be nothing certified to revoke; serialization is unchanged.

Body size is `8 t + 1 unknown counters + (2+1) reason + (1+8) invalid_from
=21=0x15`. Frame is `05 15 00 00 00` and payload size is `8+5+21=34=0x22`.
Both counts are 1. Block offset is 544 (0x220), preceding CRC=0x77d5698f;
this demonstrates that the intervening type-0 block participates in the chain.
The writer must flush that TRANS before this VOID. CRC input is 98 bytes;
CRC is 0x7d06bde8. Block length is 102; data_end is 646 (0x286).
The record's physical identity is `(b,c,0,544,8)`.

```hex
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
01 00 00 00 22 00 00 00 22 00 00 00 02 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
20 02 00 00 00 00 00 00 8f 69 d5 77 00 00 00 00
01 00 00 00 01 00 00 00
05 15 00 00 00
09 00 00 00 00 00 00 00 00 01 00 78 01
00 00 00 00 00 00 00 00
e8 bd 06 7d
```

## Arithmetic checks and refusal variations

The checksum recurrence in CODEC.md starts with register 0xffffffff. For an
individual zero input byte from that initial register, its eight successive
bit steps are `92477cdf, a49b3d4f, bff51d87, b2420de3, b49985d1, b7f441c8,
5bfa20e4, 2dfd1072`; final XOR gives 0xd202ef8d. This supplies a small
hand-checkable bit-order example before applying the same recurrence to the
literal fixture bytes.

| Unit | CRC input bytes | Register before final XOR | CRC numeric value | Stored bytes |
|---|---:|---|---|---|
| Header (flags omitted) | 195 | e1c58e69 | 1e3a7196 | 96 71 3a 1e |
| Example 1 | 248 | c48eb0c6 | 3b714f39 | 39 4f 71 3b |
| Example 2 | 87 | 882a9670 | 77d5698f | 8f 69 d5 77 |
| Example 3 | 98 | 82f94217 | 7d06bde8 | e8 bd 06 7d |

For a hypothetical successor, previous_file_no would be 0 and
previous_data_end would be 646 (`86 02 00 00 00 00 00 00`). Applying SHA-256
to the concatenated file bytes `[0,6) || [8,646)` gives the following raw digest
(hex text here, not ASCII hex on wire):
`4c24de25c90c09fe8f9f33b25899eeaa0223eb4d348176c38ae3c2c28edd5463`.
This digest is a separately calculated standard-library arithmetic check,
not a fourth hand-derived record example. No successor is assumed present.

The CRC-32 check vector ASCII `123456789` was also recomputed as 0xcbf43926
(stored `26 39 f4 cb`); the single-zero-byte CRC remains 0xd202ef8d. All four
fixture CRCs and the SHA-256 above are unchanged. These five intermediate
register values were the only arithmetic values changed in r2:

| Zero-byte bit step | Previous text | Recomputed register |
|---:|---|---|
| 3 | bff51a87 | bff51d87 |
| 4 | b2420e63 | b2420de3 |
| 5 | b4998411 | b49985d1 |
| 6 | b7f44128 | b7f441c8 |
| 7 | 5bfa2094 | 5bfa20e4 |

These variations are requirements derived from the spec, not extra hex fixtures
or executed replay tests. For any modified fixture intended to continue after a
changed block, also repair every downstream preceding_crc32/block CRC and any
successor digest; repairing the changed block alone does not repair its chain:

- If the first EVIDENCE payload version byte is changed from 01 to 02 and its
  block CRC recomputed, the **whole** first block is unsupported; neither LIFE
  nor ATTACH_OK may escape. Without CRC repair it also fails the checksum.
- If that block's second record kind is changed to 06 with repaired integrity,
  its frame and common t=2 remain readable. Validate the entire block, then
  deliver LIFE followed by a record-fault notification: global loss at the
  preceding F, with unknown counters and invalid_from=null. There is no F in
  this fixture; LIFE remains retained, its generation becomes tainted, and
  replay continues. The unknown kind is not ATTACH_OK and emits no certificate.
- A framed ATTACH_OK or FRONTIER with known=0 is likewise a record fault/global
  loss in this qualified capture. Encoders must refuse to serialize it. A valid
  VOID with counters=null (example 3) remains allowed. These semantic errors
  differ from an unsupported payload version or untrustworthy framing.
- Truncating example 1 after its complete LIFE but before the block's final CRC
  releases no records. A count of 0 or 4097, count mismatch, body overrun, or
  bad CRC likewise refuses the entire block.
- Removing example 2 from the file leaves example 3 with a wrong physical
  offset and predecessor CRC. It cannot be replayed merely because its own CRC
  still matches. No time-based seek may skip the intervening segment.
- Rewriting only the two header flag bytes leaves the header CRC, all block
  CRCs and a successor's predecessor SHA-256 unchanged. Adding/removing a valid
  footer leaves that predecessor SHA-256 unchanged as well. Removing referenced
  data blocks changes its covered bytes/length and invalidates the link.

Additional r2 conformance cases (requirements, not new serialized examples):

- A role=2 marker with old=new, duration=0 and pid=0 is valid and omitted from
  the exact adapter. Valid samples are also omitted. Neither becomes OTHER;
  their bytes still participate in integrity and original offsets stay intact.
- Type-0 row times [10,100,20] have header endpoints 10 and 20. A range view of
  [90,110) must inspect the rows and find 100; endpoints/footer cannot prune it.
- An LZ4 stream violating the final-literal or last-match end constraints is
  rejected even if it produces U bytes and its block CRC is correct.
- A failed block flush terminates the capture: no later certificate or linked
  successor can hide the failure. Separately buffered samples may be placed
  between complete blocks but cannot reorder the type-0/type-2 subsequence.
- Removing a final complete suffix from this footerless file, with no successor
  or trusted length, is indistinguishable from an earlier snapshot. Interior
  removal with a retained later block is detected by offset/CRC checks.
- A footer for these blocks has timestamps [0,5,0] and offsets [201,453,544].
  It is unsorted, so a generic index search must fall back to scanning. Even a
  sorted index cannot authorize exact replay to skip blocks.
- Re-delivering `(b,c,0,453,0)` emits its TRANS only once. Identical records at
  distinct physical positions remain distinct; conflicting bytes at one
  position are an error. Test this in the codec, which owns positions, before
  calling replay; the logical record structure cannot perform that deduplication.
