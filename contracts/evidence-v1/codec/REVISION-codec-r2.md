# Codec revision r2 — independent challenge dispositions

Reviewed: `fable-challenge.md`, CODEC.md, WORKED.md, the supplied replay source
and `src/core/evidence_replay.h` at `aa31b588` (master after #903; reviewed as local commit
33d9139e, byte-identical replay files), and
`contracts/evidence-v1/{CONTRACT.md,REVISION-r2.md}` on `origin/master` at
`84779083f2fcf9a4a02e595bf041d14040927e7f`. This revision changes only the codec
and worked-example design documents plus this disposition record. It does not
change CONTRACT.md, replay code, daemon behavior, or capture qualification.

## CONTRACT vs REPLAY

CONTRACT.md §5 says:

> An unexpected counter vector, malformed record, or unsupported evidence kind acts as global VOID
> at the preceding F; if framing/identity cannot be trusted, stop the file chain there instead.

That requires ordered global-loss handling and continued replay when framing
and capture identity are trustworthy. Prior surviving certificates are retained;
recovery still requires the §4 baseline/identity/attachment protocol. Section 6
separately stops at a corrupt/unsupported file or block and retains the validated
prefix. It does not turn a framed logical error into whole-input refusal.

The replay at `aa31b588` contradicts this text: `scope()` rejects malformed
records, unknown kinds, and unknown-counter certificates before replay, returning
PGWT_REPLAY_UNSUPPORTED without any retained result. Several consistency errors
in `replay_file()` also take a whole-input unsupported path where §5 specifies
suffix discard/global VOID. The header's promise that malformed records refuse
the input describes that implementation; it does not override CONTRACT.md.

**The replay must change to match the logical contract.** The codec must not
silently redefine the contract to match `scope()`. No contract behavior change
is needed. For the lead's separately routed clarification, the exact proposed
replacement sentence for the quoted contract sentence is:

> An unexpected counter vector, malformed record, or unsupported evidence kind MUST act as a global VOID at the preceding accepted frontier and permit continued replay when framing and capture identity remain trustworthy; otherwise replay MUST stop that capture's file chain at its last validated prefix, and neither case discards earlier surviving certificates by refusing the whole input.

CODEC.md §§5,9 now distinguish segment framing/integrity failure from a record
fault inside an intact segment. All outer frames/columns and the full block CRC
must validate before any records or faults are released. A common u64 t is
required even for an unknown kind; an unreadable common envelope stops the
chain. A framed fault yields an explicit ordered adapter loss notification,
not an ordinary malformed record, an extra wire kind, or a silent omission.
For malformed/unsupported records its conservative loss vector is unknown and
invalid_from is null; for an otherwise valid certificate's vector mismatch,
use the actual vector as the contract requires. Valid explicit VOID retains its
encoded revocation. Recognizable certificate-shaped records in unqualified
captures remain subject to the contract's explicit ignore rule. This design
requires a replay/adapter error-path change and does not claim that passing
OTHER or unsupported_shape to the current API already provides it.

## The twelve points

1. **ACCEPT — retain and reverify the fixture.** WORKED.md now records r2's
   independent field-by-field reconstruction and checksum audit. All 646
   serialized bytes match; final CRCs and SHA-256 remain unchanged. Recomputing
   the small zero-byte demonstration did expose five incorrect intermediate
   register values, now corrected and listed below.

2. **ACCEPT — omit markers and samples.** CODEC.md §6 requires valid marker
   rows and samples to be omitted from the exact logical record array. Neither
   becomes PGWT_EVIDENCE_OTHER. Their storage bytes, original positions and
   integrity checks remain; diagnostic/sample views can still consume them.
   WORKED.md adds the corresponding conformance case.

3. **ACCEPT — unknown counters are an explicit error, not mere nonpromotion.**
   CODEC.md §5 requires writers to refuse ATTACH_OK/FRONTIER with known=0 and
   report unknown loss using VOID. Readers treat such an intact framed
   certificate as a record fault/global loss in qualified captures (§9), with
   the unqualified certificate exception stated explicitly. Descriptor and
   VOID counters may still be unknown. The current scope() whole-input refusal
   needs the replay correction described above; this is not fixed by wording
   that implies the current replay accepts the record.

4. **ACCEPT — resolve the contract/replay disagreement.** The logical contract
   wins for trustworthy framing/identity; replay must change. CODEC.md's opening
   and §9 replace unconditional segment rejection for logical record faults
   with ordered global-loss handling. Framing/integrity/schema failures still
   stop the chain with no records from the failed segment. Writers MUST refuse
   malformed serialization and cannot silently drop a bad record then publish
   dependent certificates. WORKED.md distinguishes the unknown framed kind
   (valid LIFE then loss) from an unsupported payload version (no records from
   that block). See “CONTRACT vs REPLAY” for the exact proposed clarification.

5. **ACCEPT — preserve PID-zero administrative markers.** CODEC.md §6 permits
   pid=0 only for the exact role=2 marker shape (old=new, duration=0). Positive
   marker PIDs remain permitted. Ordinary TRANS/SAMPLE and per-backend evidence
   still require positive PID. The exception establishes no lifecycle identity.

6. **ACCEPT — forbid unsafe type-0 pruning.** Keep first/last row timestamps
   and modulo deltas, but CODEC.md §6 forbids using type-0 header/footer times
   for pruning, beyond-query classification or early scan termination in any
   v5 view, including the legacy transition/sample view. Every type-0 row must
   be inspected. The [10,100,20] / [90,110) example makes the late interior-row
   case explicit. Exact evidence replay already scans all types in full.

7. **ACCEPT — make LZ4 end constraints mandatory for readers.** CODEC.md §4
   now says encoders MUST satisfy and decoders MUST reject violations of the
   final literal-only, last-five-literals, and last-match-distance-from-end
   constraints. The no-match/all-literal case is explicit. WORKED.md adds a
   refusal case independent of successful decompression and CRC validation.

8. **ACCEPT — failed flush is fatal for the capture.** CODEC.md §7 chooses the
   fatal branch: a failed block write, flush or required persistence operation
   prevents further certificates or successors under that capture identity.
   Report the failure; a new capture requires fresh identity/LIFE/attachments.
   Required revocation that cannot be persisted also requires out-of-band
   unavailability. No silent failed-block omission or in-place recovery is
   authorized by this design.

9. **ACCEPT — exempt samples from exact-stream batching.** CODEC.md §7 allows
   separate type-1 buffering and placement at complete block boundaries while
   preserving the type-0/type-2 subsequence and sample row/tick/weight order.
   Once placed, sample blocks participate in all CRC/offset/SHA checks. Failure
   accounting cannot be deferred past a certificate. The text records 68-byte
   block overhead and clarifies that neither sidecar updates nor fsync on every
   alternation is a format requirement. Serialization of the worked exact
   blocks is unaffected.

10. **ACCEPT — qualify removal detection.** CODEC.md §4 now limits the block
    chaining claim to interior changes exposed by a retained later block,
    subject to CRC collisions. Section 8 and WORKED.md explicitly retain the
    final-suffix limitation: without a successor or trusted length, truncation
    at a complete block boundary is indistinguishable from an earlier snapshot.
    The digest excludes exactly [6,8) and the footer; coverage is unchanged.

11. **ACCEPT — explain footer fallback, with a precision correction.**
    CODEC.md §8 explains that zero type-2 timestamps can make mixed indexes
    nonmonotonic and require scanning rather than generic binary search.
    “Never ascending / always falls back” is too broad: all-zero or some mixed
    indexes can be sorted. Sortedness must be tested, and even a sorted footer
    does not authorize type-0 range pruning or skipping exact evidence.
    WORKED.md's actual optional footer would have timestamps [0,5,0].

12. **ACCEPT — specify codec-owned deduplication tests.** CODEC.md §7 assigns
    deduplication to the codec before logical replay, because the logical record
    lacks positions. Required cases cover repeated delivery of one position,
    identical bytes at distinct positions (including zero waits), conflicting
    bytes at one position, repeated identical files, and preservation of offsets
    after marker/sample omission. WORKED.md gives the concrete TRANS identity.
    These are design conformance requirements, not claims of implemented tests.

## Recomputed values and validation

The complete header and three blocks were independently reconstructed from the
field layouts using fixed little-endian packing; they match all four literal
hex sections in WORKED.md byte for byte. Each CRC was recomputed both with the
specified bit recurrence and Python's standard-library CRC-32. SHA-256 was
recomputed over the reconstructed 644-byte message `[0,6) || [8,646)` and
cross-checked with OpenSSL.

| Value | Recomputed r2 | Changed? |
|---|---|---|
| Header CRC | 0x1e3a7196 | No |
| Example 1 CRC | 0x3b714f39 | No |
| Example 2 CRC | 0x77d5698f | No |
| Example 3 CRC | 0x7d06bde8 | No |
| ASCII 123456789 CRC | 0xcbf43926 | No |
| Single zero-byte CRC | 0xd202ef8d | No |
| Predecessor SHA-256 | 4c24de25c90c09fe8f9f33b25899eeaa0223eb4d348176c38ae3c2c28edd5463 | No |

The four pre-final-XOR registers also remain e1c58e69, c48eb0c6, 882a9670,
82f94217. The one-zero-byte demonstration had these five corrected intermediate
registers: step 3 bff51a87 → bff51d87; step 4 b2420e63 → b2420de3;
step 5 b4998411 → b49985d1; step 6 b7f44128 → b7f441c8;
step 7 5bfa2094 → 5bfa20e4. Steps 1, 2 and 8 are unchanged. These are the only
arithmetic values changed. No stored checksum, digest, length or offset changed.

Additional arithmetic checks verify flag-byte mutation and addition of the
exact optional footer leave the predecessor digest unchanged. No product codec
or replay was implemented or executed; the new fault behavior and conformance
cases remain requirements for the implementation work routed by the lead.
