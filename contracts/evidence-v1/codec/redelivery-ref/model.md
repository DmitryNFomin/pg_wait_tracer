# Abstract redelivery model

`model.json` is one complete input; `cases.json` contains a delivery catalog and
50 multisets selecting from it. `ref_redelivery.py model.json` evaluates an input.
With no argument it reads JSON from stdin. No filenames, fixture aliases, arrival
indices, product code, real clock, or external state participate in evaluation.
All Python programs use only the standard library and local input files.

## JSON representation

An input is `{"format":"abstract-redelivery-v1","deliveries":[delivery,...]}`.
The evaluator also accepts the delivery array directly through its Python API.
Multiplicity is allowed. The catalog form in `cases.json` is test storage, not
the evaluator's input: the checker substitutes each selected catalog delivery.
`model.json` is a concrete JSON example of ordinary delivery fields. It supplies V, Q0 and X: a replay-baseline failure
with a retained predecessor prefix and a separately usable independent capture.

A delivery has `header`, `blocks`, and `tail`:

| Field | Meaning |
|---|---|
| `header.valid` | Independent complete-header validation result. False covers cuts, bad header CRC, unsupported schema, malformed descriptor/map and invalid ordinals. |
| `header.physical` | `[boot,capture,file_no]`. Trusted only if valid. Invalid-header copies may carry these untrusted fixture annotations, which the evaluator never uses for grouping or stops. |
| `header.H` | Declared absolute first-block offset. |
| `header.raw_length` | Actual header bytes available on an invalid-header cut; defaults to H. |
| `header.content_id` | Interned identity of the entire immutable header, including CRC, omitting exactly bytes `[6,8)`. |
| `header.raw_content_id` | Optional identity of the actual invalid immutable header bytes, omitting available flag bytes, e.g. after an unrepaired CRC flip. Otherwise content_id describes the raw immutable header. |
| `header.flags` | Actual mutable u16, included in delivery identity but not header compatibility/digests. |
| `header.qualified`, `baseline`, `mapping` | Qualification and mapping are static link fields. Mapping is the ordered array `[id,role,name]`. Baseline is replay state: null or fifteen u64s, initialized by file 0 and checked at each reachable successor (clauses 13–14,19). |
| `header.creation` | Fixture shorthand for immutable start_time_ns; all other unmentioned creation metadata are zero. Header identity, rather than this decoded convenience field, governs HC. |
| `header.previous` | Null for file 0; otherwise `{file_no,D,digest}`. A purported valid header with a non-immediate ordinal is a malformed model: encode it with valid=false instead (CODEC §2). |
| `header.failure` | Optional independent invalid-header explanation. |
| `blocks` | Raw physical units in sequential order. Normally whole blocks; rejected units may remain, along with later units, as raw bytes. Scanning always stops at the first rejected one. |
| `block.start`, `end` | Absolute physical byte span `[start,end)`. The next unit begins exactly at end; no artificial alignment. |
| `block.type`, `codec` | Actual block type 0/1/2 and codec 0/1; all types participate in prefix comparisons. Type 2 allows only raw. |
| `block.content_id` | Identity of all stored block bytes, including header, payload, CRC and preceding-CRC field. Repaired chaining normally changes all later identities. Equal decoded records do not imply equal stored block identities. |
| `block.checks` | Independent validation facts described below. Missing facts default to true. |
| `block.records` | Full ordered decoded records, each with `offset`, `content_id`, `value`, and `fault`. Values include every logical and retained metadata field. |
| `record.offset` | Type 2 kind offset, or type 0/1 timestamp-varint offset in decoded column 1. It is never an absolute file offset. |
| `record.content_id` | Exact record-content identity; equal contents at different physical positions are distinct observations. |
| `record.value` | Decoded logical fields, including kind, t and all applicable fields. JSON null, integer 0 and absent optionals are distinct. |
| `record.fault` | Null or `{kind,detail?}` for a **structural** local fault under trustworthy outer framing. Unknown kinds, bad fields and unmapped IDs fit here. Semantic run/counter/dependency failures must not be annotated as structural faults. `kind="null_counters"` explicitly describes the readable loss exception and creates no RF. |
| `tail.kind` | `eof`, `footer` (all remaining bytes exactly match the reconstructed footer), or `rejected`. A footer-shaped suffix with any mismatch is rejected, not footer. |
| `tail.length`, `content_id` | Length and exact identity of all remaining bytes. EOF has length zero and empty identity. Torn tails are part of full delivery identity. |
| `tail.checks` | Validation facts for a rejected unit beginning at the last validated end. |

A block may also carry sample metadata (`sample_period`, `max_weight`); omitted
hints/metadata follow the notation's defaults. Multiple records per block, all
block types, markers, samples, repeated contents, differing lengths, compressed
forms, empty files, invalid headers and rejected tails fit the same form.

`checks` describes facts, not a preselected winning reason. It can assert multiple
simultaneous defects. The evaluator applies clause 6 in order:

1. `header_complete=false` → T.
2. `header_fields_valid=false` or `safely_bounded=false` → IB.
3. `bytes_complete=false` → T.
4. `crc_valid=false` → CRC.
5. `decode_valid=false` → IB.

All earlier checks must pass before a later one matters. Header fields include
type, codec, count, sizes, reserved bytes, self offset and preceding CRC. Whole
block decoding includes schema, outer frames, common t, column layout, LEB128,
LZ4 output/end constraints, timestamps/weights and consumption of the payload.
A record fault in a block that fails any whole-block check is never observed.
The model deliberately supplies these independently determined byte facts; the
reference evaluator is a reconciliation oracle, not a second byte parser.

## Exact content and symbolic digest identities

Identity tokens are interned names for **exact bytes**, not similarity labels,
CRC values, decoded summaries, or per-delivery unique names. At a corresponding
header/block/record position, bytes agree exactly iff their content identities
agree. Different whole-block identities mean at least one stored byte differs;
they do not assert that every constituent byte differs. A cut carries the
identity of its actually present byte slice. In particular Ac2t1 and At1 are
identical deliveries. The same prefix slice must get the same identity even if
it was obtained through different mutations. Regions use the canonical physical
partition: header, complete/raw block units, then one final tail. The evaluator
flattens the final tail into the raw span sequence for delivery identity.

`digest` has one of these forms:

```
{"kind":"prefix", "header":"immutable-header-id", "D":957,
 "blocks":[[193,305,"stored-block-id"],...], "partial":null}
{"kind":"wrong", "content_id":"deliberately-distinct-digest"}
```

The first form denotes SHA-256 of predecessor `[0,6)||[8,D)`, **not** SHA-256 of
JSON. It describes predecessor content, independently of which predecessor
copies are delivered. For an interior D, `blocks` lists complete blocks and
`partial` is `[containing_block_content_id, bytes_used_from_its_start]`. Such D
cannot be a witness or seal boundary; digest_match is null, not false, in its
link evidence. Wrong digests are deliberately unequal to every applicable
correct digest, with collisions outside this abstract domain. Empty predecessor
prefixes have no blocks and D=H. Flags and footer never appear in this identity. On invalid header cuts only flag octets actually present are included in full delivery identity; nonexistent flag octets cannot distinguish deliveries.

The output's delivery name is `length:abstract-sha256:...`: the suffix hashes
the canonical **symbolic byte description** solely to keep identifiers compact.
It explicitly is **not** a claimed SHA-256 of v5 bytes. This implements the
fixture notation's permission to use content-derived names without guessing
real hashes. Fixture labels are translated only by the checker, never by the
oracle. An encoder-backed harness replaces these symbolic names throughout the
result with `(actual_byte_length,SHA256(all_actual_bytes))`. It must enforce that
interned equal byte tokens have equal byte materializations. No collision
simulation is used. Contradictory annotations for the same delivery identity
are model errors, not codec reason codes.

Canonical ordering inside this abstract representation uses `(boot,capture)`,
then file ordinal and byte/payload position, then reason rank and symbolic
content-derived names. HC/BC pairs are unordered and canonicalized; SC pairs
always preserve `[predecessor,successor]`. Representation order across independent
captures is only serialization, not an admission or replay order. A real-byte
harness can canonicalize witness names by their materialized byte strings;
relabeling does not affect witness membership or any codec decision.

## Mapping to a real v5 encoder

Maintain a content dictionary mapping these symbols to their concrete recipes.
The delivery catalog includes the base predecessors even in cases that omit
them, so a successor's digest can be encoded without delivering its predecessor.
A standalone generated model needs the same materialization dictionary in its
harness. Abstract prefix digest identities can be compared without that dictionary.

Encode little-endian headers, maps and zero/default metadata with CODEC §§1–3;
H must match the claimed size. Encode complete records/columns with §§5–6,
preserving original offsets. Type-2 hints are zero for these fixtures; use raw
codec unless a variant requests a valid raw LZ4 block. Encode the 64-byte block
header, self offset, preceding CRC, stored payload and block CRC under §4.
An all-literal LZ4 encoding is sufficient for compressed generated fixtures.
Repair CRC chaining after a valid content/header change. Header flags are outside
header CRC and predecessor SHA. Set successor D and hash the actual immutable
predecessor bytes through D under §8, or flip a digest bit for kind=wrong. Finish
with no footer, the exactly reconstructed footer, or the specified rejected bytes.
Verify resulting offsets, lengths and equal-token byte equality before testing.

For malformed inputs, first encode a valid template and then mutate/cut its raw
bytes. A production writer must refuse malformed records; a test harness must
inject them after encoding. An unknown kind retains its bounded original body
(e.g. Au retains the ATTACH_OK body, changing only its kind to 255). A kind-local
fault can specify a malformed boolean, method, string, ID, PID, etc.; missing
common t or outer frame overruns are whole-block failures. CRC-bad copies keep
all later raw bytes unchanged; valid changes repair later chaining. Truncation
copies only the requested bytes and never fills a declared body with invented
bytes. A clean boundary cut has EOF, not T. For invalid-header cuts raw_length
is the real available byte count, regardless of the declared H. The abstract
checks state the required independent validator outcomes; choosing a mutation
that actually realizes those outcomes remains the byte harness's responsibility.

## Results and failure levels (clauses 0, 2a, 13–20)

`records` is the **codec** array: physical
`[boot,capture,file_no,block_offset,payload_offset]`, content_id and full values.
`stops` contains one normalized eligible codec candidate per verified physical
file. `diagnostics` preserves every independently observed failure, including
suppressed tails, record faults, local PU evidence and all proving conflict
pairs. Every such diagnostic carries `failure_level="delivery"`. PU includes
its declared predecessor and all alternatives in exactly the successor's own
`(boot,capture,file_no-1)` group. Invalid headers supply no trusted group.

`admissions` reports codec file admission, all maximum-prefix sources, missing
ordinal ranges `[first,end)`, record count and every delivery's local link
facts. Admission means the descriptor is reached before the codec cutoff and
is not withheld under 2a. Records may still be cut within that file. A locally
witnessed link can remain excluded by an earlier stop or ambiguity. Baselines
never enter static witness or sealing-proof checks. This applies even when
the successor would fail a replay check if reached.

`ambiguities` has exactly one `CAPTURE_BOOT_AMBIGUITY` per capture ID observed
under multiple boots in header-valid deliveries. Each entry contains capture,
complete sorted boots and distinct witnesses with delivery, boot, file_no and
`position=[0,null]`. Non-root, unlinked, dirty and otherwise cut-off deliveries
still count. Invalid headers do not. Full byte identity deduplication applies
before witness enumeration. No codec reason or rank is invented. Independent
inspection, local linkage and normalized codec stops remain observable.

`capture_results` contains capture ID, boots, PASS/FAILED status, ambiguity,
codec stops and replay result. A capture fails for any normalized codec stop,
reachable replay stop or 2a ambiguity. An ambiguous capture emits no codec
records under any boot and has `replay=null`, exposing no LIFE, certificates,
coverage or other derived evidence. Other captures remain separately usable.
A capture whose root descriptor is not codec-admitted also has `replay=null`;
its codec failure remains explicit. A usable descriptor with zero records has
an actual empty replay state, rather than being confused with failure.

`aggregate_status` is FAILED iff a capture failed or an invalid-header delivery
has an unassigned header failure; otherwise PASS. Suppressed delivery failures
alone cannot fail it. `unassigned_header_failure` remains explicit.
`decode_failed` is the Boolean alias of `aggregate_status=="FAILED"`; unlike the
older version it includes reachable replay failures and capture ambiguity.
PASS means successful reconciliation/replay, not coverage or qualification.

Replay receives descriptors and codec records separately. `replay_stops` lists
reachable semantic stops; descriptor baseline mismatches carry file, position
`[0,null]`, reason `baseline_mismatch`, expected running vector and declared
vector. Equality checks all fifteen components, with unknown different from
known zero. Checks never reset baseline, synthesize VOID, alter codec arrays,
link evidence or normalization. Only the earlier replay prefix survives; no
later VOID or descriptor is evaluated after that stop. Codec inspection still
continues independently. A codec cutoff can make a successor check unreachable.

A replay result contains retained `records`, `baseline`, surviving `frontiers`,
latest `frontier` (F), `high_water` (H), revoked certificates, global run coverage,
verified `life_entries`, per-generation `projections`, lifecycle uncertainty,
semantic diagnostics and optional stop. In this bounded nonwaiting domain,
projections have waits=[] and open=null; LIFE without an attachment has no
coverage. The legacy `life` list is retained for the original probe callers.
Readable/null counter loss consumes a certificate solely as VOID; ignored
unqualified certificates never move baseline. Explicit VOID still applies in
unqualified captures. Later retained VOID revokes prior certificates even for
a query ending before its detection time.

`Reconciler.add` snapshots each delivery and recomputes from the accumulated
multiset. A newly established 2a ambiguity retracts that entire capture's prior
records and replay result. Storage monotonicity otherwise permits retraction
only for HC/BC/SC at or before each removed position in its own boot/capture.
Certificates and metrics are separately nonmonotone under VOID and semantic
cutoffs; the storage-prefix property never asserts their monotonicity.

## Explicit errors and scope

`ModelError` rejects invalid/contradictory abstract input, including contradictory
validation annotations for the same symbolic byte identity. `AmbiguityError`
is reserved for unresolved normative choices and names the controlling clause;
cross-boot reuse no longer raises it. The previous baseline ambiguity is also
resolved by the revised contract. Clause 11's invariant-conflicts wording still
needs clarification concerning witness membership; complete evidence under
clauses 2/16 determines witness sets without choosing a winner (see REPORT.md).

`replay_effects.py` implements the table's LIFE, nonwaiting reread attachment,
FRONTIER, VOID, counter loss, lifecycle eligibility and descriptor-baseline
semantics. It resolves delayed LIFE through the next FRONTIER/EOF for its
bounded attachment shape. Schema-valid transitions with no addressable run are
ignored as required by CONTRACT §5.6. It does not implement the full named-wait,
transition-anchor, END-dependency or certified-exit projection oracle. Such
unsupported semantics raise `ReplayScopeError` explicitly naming clause 19
and the relevant CONTRACT sections; `evaluate` translates this to `ModelError`
(and the CLI returns structured error JSON with exit code 2). An unsupported
shape is a scope limitation, not a newly discovered contract ambiguity, and is
never reported as PASS. Structural validation is supplied by the model facts;
this remains an abstract reconciliation reference, not a byte parser.
