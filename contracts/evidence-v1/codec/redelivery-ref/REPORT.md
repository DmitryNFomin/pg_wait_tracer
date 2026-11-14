# Independent reference evaluator — r3b

Cases **50/50**, zero cell disagreements. The evaluator now implements capture-local
CAPTURE_BOOT_AMBIGUITY (2a), running successor-baseline checks (19), failure levels
(0), and predecessor candidates confined to the successor's own immediate physical
group (13). The two earlier reported ambiguities are resolved by the revised text.
Only files in this directory were read; all authored artifacts and results are here.
No product implementation, parser, output, repository outside this directory, or
remote source was consulted. Normative input documents were left unchanged.

## Implementation and reproduction

`build_cases.py` transcribes fixtures and expectations from REDELIVERY.md without
importing or running the evaluator. It adds all cases 32–50 and aggregate status
for every case, including amended 17 and 31. The new expectations were transcribed
before modifying the old evaluator. `cases.json` holds full logical contents and
physical identities, codec stops, diagnostic witnesses, provenance, admission,
ambiguity witnesses, capture/aggregate status and specified replay effects.
`check_cases.py` also contains separately handwritten local-link facts.

`ref_redelivery.py` deduplicates complete symbolic byte identities, independently
validates every delivery, and inspects conflicts and links even after cutoffs.
Under 2a it collects boots from every validated header, including non-root or
unlinked deliveries, and emits one outcome with all distinct witnesses per
ambiguous capture. That capture exposes no codec records or replay-derived LIFE,
certificates, coverage or projections. Independent captures and all intrinsic
codec diagnostics/stops remain returned. No codec reason is assigned to ambiguity.

Static linkage and sealing proofs exclude baseline equality. Replay separately
checks each reachable successor against the running baseline after retained VOID
and applicable readable/null counter handling. Unknown differs from known zero;
all fifteen components matter. Unqualified certificates do not update baseline,
while explicit VOID and successor checks still apply. Replay mismatch preserves
the preceding prefix and revocations without altering codec records or admission.
An earlier codec or semantic stop prevents unreachable baseline checks.

Delivery diagnostics carry their failure level. Each capture has its own status,
codec stops, ambiguity and usable replay prefix. The aggregate fails for a failed
capture or unassigned header failure. Suppressed delivery failures alone pass.
`decode_failed` now aliases aggregate failure, including semantic failures and 2a.
The evaluator comments cite the controlling clauses; `model.md` documents the API.
`model.json` demonstrates V/Q0's replay failure alongside independently usable X;
`model-result.json` is its evaluated output.

Run from this directory, using Python 3 and only its standard library:

```
python3 build_cases.py
python3 ref_redelivery.py model.json > model-result.json
python3 check_cases.py
python3 check_props.py
python3 check_mutants.py
```

## Case checks

All **50/50** cases match the frozen REDELIVERY r3b columns, cell by cell.
Every disagreement is printed and retained in `cases-result.json`; its final
failure list is empty. Checks include full physical record identities and
contents; normalized codec stops and complete witnesses; expanded S diagnostics;
suppressed failures; local link facts and PU candidate-group evidence; source
provenance; admission; capture outcomes; aggregate status; and specified replay
baselines, F/H, revocations, LIFE, per-generation coverage and replay stops.

Every one of **160** unique table permutations is compared directly against
these independent frozen cells. **606** accumulated-prefix checks cover **128**
distinct accumulated submultisets, comparing incremental results to fresh
whole-multiset evaluation. Those intermediate fresh evaluations are consistency
oracles, not additional independently frozen answer sets.

Cases 17/42 preserve passing A with RA despite aggregate failure from an invalid
unassigned header; case 31 has no trusted capture. Cases 32–35/47 withhold capture
a across all boots while retaining unrelated X and intrinsic PU/T diagnostics.
Cases 36/37 use the same clean predecessor witness V but differ only in reachable
replay outcome. Case 40's authenticated extension proves SC even though baseline
would differ; neither v4 nor a successor check is reached. Cases 43/44 distinguish
unknown from zero. Cases 45/46 consume w4 solely as counter-loss VOID. Cases 48–50
exercise the unqualified-certificate exception and explicit VOID separately.

## Property checks

The final deterministic run reports:

| Property or bound | Count |
|---|---:|
| Generated distinct multisets, sizes 0–4 | 3,959 |
| Unique permutations | 15,972 |
| Every-permutation accumulated-prefix checks | 69,643 |
| Incremental addition edges | 7,521 |
| Storage monotonicity edges | 7,521 |
| Duplicate idempotence edges | 1,519 |
| Edges newly establishing 2a ambiguity | 131 |
| Header/block/payload/CRC/footer byte cuts | 1,026 |
| Original replay scenario assertions | 4 |
| Added r3b replay scenario groups | 9 |
| Individual baseline-component assertions | 15 |
| Total assertions | 105,621 |
| Failures | 0 |

Size distribution is 1 empty multiset, 1,102 singleton, 1,253 size-two, 533
size-three and 1,070 size-four multisets. The generator exhausts sizes 0–4 within
fourteen thematic alphabets and four additional alphabets, deduplicating overlap;
it does not claim a global Cartesian-product enumeration. The extra alphabets
cover multiple seal claims (5), empty/dirty predecessors (4), malformed footers
(3), and distinct 2a witnesses (6). Bounds and actual counts are machine-readable
in `props-result.json`. Targeted direct assertions additionally use up to seven
input deliveries to test complete ambiguity witnesses and independent captures.

All accumulated prefixes of every generated permutation are checked, including
successor-before-predecessor, reversed chains and interleaved captures. Added
alphabets cover second boots, non-root reuse, invalid purported reuse, baseline-
changing VOID before a successor, unknown baselines, FRONTIER changing baseline,
and both ignored and explicit baseline effects in unqualified captures. Targeted
assertions check all fifteen baseline slots, unknown file-0 initialization,
readable restoration, and successor checks made unreachable by prior replay/codec
stops. Distinct flag/footer/tail variants remain distinct ambiguity witnesses;
identical deliveries contribute once.

The exact storage-monotonicity assertion is: every removed record must either
belong to a capture ID **newly proved to name multiple boots by independently
validated headers**, or have a newly observed HC/BC/SC at or before its physical
position in that same boot/capture. Only the first condition permits whole-capture
retraction under 2a. For those edges, no record under that capture ID may survive
and its entire replay result must be null. This exception cannot excuse loss in
another capture. Conflicts retain the existing allowed suffix retractions.
Certificates and metrics have separate VOID/semantic-cutoff tests and are never
asserted monotone. The query ending at 15 still incorporates VOID detected at 30.

Cuts cover every absolute offset 0..1025 of A plus its exact footer, both alone
and alongside clean A. The inherited validation generators also cover compound
terminal defects, malformed/exact footers, bounds and offset/chaining faults,
unsupported schemas/types/codecs, CRC versus decode precedence, raw/LZ4 symbolic
forms and different lengths, sample/marker omission, distinct physical copies of
equal rows, zero-duration transitions, structural faults before later conflicts,
missing files, wrong/nonboundary digests, empty predecessor witnesses, and
conflicts beyond earlier cutoffs. These are abstract validation facts, not tests
of a concrete compression or CRC parser.

Each invocation performs one deterministic exhaustive round, with zero random
rounds. Final timing is recorded in `props-result.json`. Exploratory authoring
runs are not independent seeds or independent answer sets. Gate-box/product calls
and gate-box cost are zero. Token usage and monetary model cost are not exposed
as measured values by this environment and are not fabricated.

## Mutation checks

**51/51 mutants built, reached their designated probes, and were killed by their
intended assertions; zero survivors.** `mutants-result.json` records each result.
Compilation errors or runtime exceptions do not count as kills. Mutants are
compiled in memory and never replace the evaluator files.

Coverage includes every clause 1–20, plus 0 and 2a: **22/22 numbered clauses**.
New mutants cover root-only boot observation, trusting invalid-header identities,
leaking ambiguous prefixes, withholding independent captures, incomplete non-root
witnesses, dropping codec stops under ambiguity, cross-boot predecessor candidates,
restoring static baseline equality, baseline-dependent seal proofs, omitted or
premature replay checks, conflating unknown with zero, skipped unqualified checks,
ignored certificates moving baseline, explicit VOID ignored without qualification,
FRONTIER loss failing to move baseline, losing revocations at a replay stop, and
misclassifying delivery, capture or aggregate failures. This is targeted mutation
coverage of the named obligations, not an exhaustive claim about all possible bugs.

## Ambiguities and specification issues

The former cross-boot ambiguity is fully specified by clause 2a and CONTRACT §1's
explicit precedence. The former original-versus-running-baseline conflict is
resolved by clauses 13–14/19 and the revised CODEC §§2/8. Neither raises the old
whole-multiset AmbiguityError. No revised table cell was found wrong, and no
unresolved executable choice was found in the exercised r3b domain.

One wording issue remains: **clause 11 versus clauses 2/16**, concerning the claim
that flag/footer variants cannot change “conflicts or admission.” Such variants
have distinct complete delivery identities. Adding one beside a conflicting copy
necessarily adds proving pairs; adding one beside a successor can add actual
predecessor witnesses. Full conflict/link evidence therefore cannot stay literally
identical. The evaluator preserves record arrays, stop positions/reasons and
Boolean admission while returning all newly required evidence under clauses 2/16.
Clause 11 should explicitly limit its invariance statement to decisions rather
than witness/provenance membership. Complete-witness clauses determine membership;
this does not require a guessed boot winner or another codec reason. There are
no other identified unresolved normative ambiguities in this bounded model.

## Limits and independent-author status

This is one independent author updating the supplied earlier reference. It is
**one answer set**, with inherited cases 1–31 revalidated against the revised
clauses and freshly transcribed cases 32–50. It is not two isolated authors or two
fresh pre-implementation answer sets with withheld expected columns. That broader
implementation-unit obligation in REDELIVERY.md remains unmet by this single-
author exercise; generated self-consistency checks cannot substitute for it.

The model uses interned identities for exact byte content and abstract prefix
digests. Its compact hashes describe symbolic bytes, not invented SHA-256 values
for unmaterialized wire files. A concrete encoder/parser harness must materialize
those bytes and independently verify validation facts. No collision branch exists.

Replay covers the complete table's specified effects and the generated nonwaiting
reread/LIFE/VOID/counter domain. It does not claim a general named-wait,
transition-anchor, END-dependency or certified-exit oracle. Unsupported semantics
raise explicit clause-19/CONTRACT-section scope errors, not PASS or guessed output.
Those are implementation scope limits, not additional normative ambiguities.
The CLI's structured scope error and the separation of codec arrays from a replay
baseline failure were also checked directly.
