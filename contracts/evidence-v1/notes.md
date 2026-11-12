# Source audit and unresolved capture questions

This is a proposed logical storage contract, not an implementation claim. Every normative design
choice not already in the frozen core is **INFERRED** as catalogued below. The user's adopted shape
supersedes the earlier three-block/birth-token/sequence/checkpoint design in astra-design.md.
No repository files were written; no expected-answer files were read or generated.

Reference convention: `M:` is repository HEAD `6a89be1584295a431acffeae99e643281e622197`;
`W:` is agent/863-sample-weight-sum `48d2a2026fe7b5f7f03a318090642b435f6a0136`;
`C:` is agent/868-core-wait-time `cca90301853baaf4ca67dd6f03af77b38bc7e6ed`;
`D:` is agent/868-diag-loss-counters `1464365d31febf7c6311148d3e208bbdde39150a`.
Paths and line numbers below refer to those exact revisions, not a checkout of their branches.
`S:` is the parent scratchpad/storage directory supplied by the user.

## N1 — INFERRED: lifecycle identity, bounds, and restart (contract §§2,4,6)

VERIFIED: kernel fork and exit events carry timestamps, with exit using the same `now` as its final
transition (D:src/bpf/pg_wait_tracer.bpf.c:739-748,799-813,853-863). Userspace handlers discard those
timestamps (D:src/daemon.c:223-233; M:src/daemon.c:165-175). Metadata is keyed only by pid
(M:src/backend_meta.c:30-44). Discovery reads ppid, not starttime (M:src/backend.c:370-385).

INFERRED: persist LIFE, use observed fork/exit timestamps to assign generations, and use 0 as the
safe pre-capture lower bound in this monotonic domain. What tighter bound can capture actually prove?
Is the fork tracepoint time the agreed operational start, or must kernel birth earlier than that
tracepoint be represented? This draft uses the adopted fork-observation convention; precision relative
to actual creation still needs qualification. No /proc-derived tighter bound is assumed.

INFERRED: a provisional `(capture,pid,null)` handle is not a proof of cross-capture process identity.
After lost lifecycle evidence, how can capture prove continuity without a birth token? Until answered,
require a fresh observed fork; do not silently recover identity from ATTACH_OK. Timestamp collisions
at PID reuse are ambiguous and withhold certification. The source has no persisted mechanism for this.

## N2 — INFERRED: global FRONTIER proof and PREEMPT_RT restriction (§§1,3,4)

VERIFIED: event-ring drain can be budget-limited and is not a producer barrier
(D:src/daemon.c:155-177). Wait transitions take time before output/state update
(M:src/bpf/pg_wait_tracer.bpf.c:555-598). Lifecycle uses a separate ring
(D:src/bpf/pg_wait_tracer.bpf.c:739-748,853-863). The existing committed-through watermark is only
the last written transition timestamp (W:src/event_writer.c:972-982).

INFERRED: what proves that every producer stamped <=t has completed before FRONTIER t is published?
An empty ring plus unchanged counters alone does not prove this: an in-flight producer may not yet
have reserved, submitted, or reported loss. The non-PREEMPT_RT assumption is required by the adopted
simplification but is not a demonstrated sufficient ordering/barrier protocol in the inspected code.
The draft therefore defines a certificate obligation, not a claimed working algorithm: without that
proof emit no frontier. No arbitrary sleep/grace interval or CPU-time bound is assumed.

VERIFIED (measurement report only): S:m0-ubuntu.md:1-6 reports all 15 counters zero in six Ubuntu
6.8 / PG18.6 runs, 120 s each, with idle connections, churn, and pgbench; EL8 was still in progress.
INFERRED: this supports choosing global conservative loss invalidation, but neither establishes the
barrier nor proves loss counters exhaustive or certifies PREEMPT_RT. No new performance claim is made.

## N3 — INFERRED: complete failure accounting and loss epochs (§§2–4)

VERIFIED: the diagnostic reader lists fourteen kernel counters plus writer_push_fail, reports
unreadable maps, and sums per-CPU values (D:src/daemon.c:33-76); event writer push failures are counted
(D:src/event_stream.c:85-89). These are diagnostics, not persisted evidence certificates.

VERIFIED: fallback wait reads remain unchecked (D:src/bpf/pg_wait_tracer.bpf.c:462-466); direct read
counts failure only with a nonzero address (D:src/bpf/pg_wait_tracer.bpf.c:446-451). Callback length
is ignored (D:src/event_stream.c:64; D:src/daemon.c:212). Buffered write/fflush failures occur after
push (W:src/event_writer.c:960-965,985-1008). Attach ignores preseed return (M:src/backend.c:329).

INFERRED: the fifteen slots are a starting inventory, NOT proof that every silent path is instrumented.
Which slot covers malformed callbacks, pointer-read failure, null addresses, queues, state setup,
allocation, asynchronous flush, and publication? Each must feed a checked counter or stop certification.
Are per-CPU increments safe under nested contexts and is reading a vector coherent enough for the
barrier? Can resets/overflow/hotplug be distinguished? Until proved, unknown counter state means VOID.
Counter vectors, global rollback to last F, and per-backend fresh attach after loss are proposed
semantics. A known loss in an irrelevant query-text field still invalidates all backends as requested.

## N4 — INFERRED: attach handshake and unknown wait starts (§§2–5)

VERIFIED: attach opens disabled, preseeds, enables, then returns with no post-enable wait reread
(M:src/backend.c:302-345). The seed's last_ts is attach time, not observed wait entry
(M:src/backend.c:249-255). Redundant writes are suppressed against the stored seed
(M:src/bpf/pg_wait_tracer.bpf.c:548-558), and the unseeded path initializes without emitting an edge
(M:src/bpf/pg_wait_tracer.bpf.c:615-643). A test hook deliberately creates a stale seed
(M:src/backend.c:194-202).

INFERRED: how is a coherent reread bracketed against BPF state and intervening transitions? Equal
values alone do not exclude ABA. How does capture distinguish the first REAL post-enable transition
from a same-word store that merely differs from a stale seed? The draft requires a verified genuine
edge; the present first emitted record is not automatically such proof. ATTACH_OK is forbidden if
these conditions cannot be established. The anchor is put before its matching TRANS in logical order.

VERIFIED: the frozen core demands a real entry for a wait/open and complete closes at a coverage
span's left endpoint (C:contracts/wait_time-v1/contracts/metrics/wait_time.md:20-34,51-56).
INFERRED: nonzero reread seeds therefore cannot immediately produce core time; wait until a known
entry. Discard the untrusted old close at the transition anchor. The projection's +1 ns trim excludes
that close instant while retaining exact known starts. This is a conservative projection of the
adopted tracking start, not another change to wait_time v1. It is a deliberate availability cost.

## N5 — INFERRED: END coherence, terminal exit, and synthetic flush removal (§§2–5)

VERIFIED: de-escalation disables all watchpoints before obtaining one later time and fabricating
open-stretch transitions (M:src/escalation.c:578-597). Open-stretch builder writes old==new
(M:src/open_interval.h:127-137). Cleanup flushes before provider stop
(M:src/daemon.c:1949-1965). Neither is proof that each backend's state remained unchanged until
that later observation. The core forbids inventing a close (C:contracts/wait_time-v1/contracts/metrics/wait_time.md:24-27).

INFERRED: how will capture establish each END instant while tracking still certifies state? END is
pending until FRONTIER; stop must perform a final certified drain or retain the prior frontier.
Replace administrative TRANS flushes with END; do not reinterpret arbitrary same-event records as
real completions. Known exit needs its matching terminal transition before exit coverage is granted.
If missing, retain the earlier certified open with as_of strictly before exit. These are new rules.

## N6 — INFERRED: one-run projection and deterministic selection (§5)

VERIFIED: the core permits at most one terminal open and forbids later closed waits
(C:contracts/wait_time-v1/contracts/metrics/wait_time.md:24-27,51-56). It never deduplicates waits
(ibid.:175-176). The C adapter can mark missing lifetime invalid (C:src/core/pgwt_core.h:37-50).

INFERRED: the source defines no storage-to-core run selection. This draft chooses by whole-query
coverage first, then largest R overlap, then earliest boundary/append order, without considering event
names. It intentionally does not aggregate detached censored waits with later windows, merge touching
attachments, or export uncommitted observations. Diagnostics describe only the selected projection.
This policy, +1 ns trim, strict exit fallback, and global suffix invalidation are draft decisions for
independent oracle review. None is claimed to have been validated by M0 or existing reader behavior.

## N7 — INFERRED: file identity, commit, rotation, codec, and legacy policy (§§1,6,7)

VERIFIED: W has v4 and two block types, common headers, and sample-weight extensions
(W:src/event_writer.h:19-22,72-117). It checks writes/fflush and publishes a sidecar block count
(W:src/event_writer.c:960-1008). Rotation uses wall-time names and existing checkpoint carryover
(W:src/event_writer.c:1395-1438). Older readers reject future versions/unknown blocks
(W:src/event_reader.c:37-47,296-300). Legacy metadata lacks generations (M:src/backend_meta.c:30-44).

INFERRED: type 2, payload version 1, descriptor identities, predecessor integrity references,
qualification flag, stable physical record identity, certificate dependency ordering, and conservative
stop-at-gap recovery are new storage requirements. Which bounded binary codec/integrity algorithm and
atomic persistence protocol will implement them? This draft intentionally freezes logical evidence,
not binary offsets; binary fixtures require a subsequent codec specification. No new BPF event
sequence or per-backend frontier snapshot is implied by file/record positions.

INFERRED: v2–v4 retain legacy viewing but map to LIFE_INVALID for this exact route. Existing readers
can silently skip future files; the adapter must instead surface unavailable and avoid bridging them.
No prior synthetic flush/checkpoint or clean bit supplies the missing generation/coverage authority.
Scenario descriptor abbreviations are logical input conventions, not measured capture output.

## N8 — INFERRED: lifetime clarification and scope

VERIFIED: the core computes R from numeric life_start and validates coverage against that lifetime
(C:contracts/wait_time-v1/contracts/metrics/wait_time.md:18-19,45-55,66-76). The 48 example inputs
contain no independent true birth time or start-known flag; all were inspected, without expected files.
INFERRED: admitting a proven lower bound changes the interpretation of supplied life_start, not any
existing example endpoint or formula. Thus none of the 48 existing answers should change. The proposed
wording is isolated in wait_time-v1-clarification.md; it does not allow invented wait starts.
