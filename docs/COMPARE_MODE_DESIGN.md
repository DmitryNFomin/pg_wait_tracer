# Compare mode — design (Track U Phase U4)

_Status: DESIGNED 2026-08-05, awaiting build approval. The capability every
ASH investigation ends with — "is this abnormal, and what changed?" — judged
against AWR Compare Periods / OEM compare, and deliberately deferred by
docs/INSTRUMENT_ARCHITECTURE.md §5 (trigger 3) and
docs/VISUALIZATION_REVIEW.md §5 until URL state existed. It does now (U2)._

## 1. The capability

Two time windows — **A** (the investigation window: whatever is brushed,
zoomed, or followed right now) and **B** (the baseline) — rendered and ranked
against each other:

- the AAS pane shows *how the shape changed*;
- the tables re-rank by *what changed* (delta-first, not biggest-first);
- the whole comparison is a URL.

## 2. Decisions (with the argument, alternatives recorded)

### D1. Baseline = an OFFSET, not a second free window
B is defined as `A shifted back by offset` (presets: −1 h, −24 h, −7 d,
custom offset; equal spans by construction). Consequences:

- **Compare survives live mode.** In follow, B slides with A — "now vs
  yesterday, continuously" — which no incumbent offers and which falls out
  of the representation for free.
- **One camera + one scalar**, not two cameras. B has no independent
  gestures in v1; every pan/zoom of A moves both. This deletes the entire
  camera-synchronization problem.
- *Rejected for v1:* free-form `B = [from2, to2]` (unequal spans force
  time-axis scaling, which lies about rate-shaped phenomena) and
  "pin current window as fixed baseline" (useful, but it reintroduces
  fixed-window bookkeeping; scheduled as v1.1 — `b_from/b_to` XOR `b_off`
  in the codec, pin button emits the fixed form).

### D2. Rendering = ghost outline + per-class diff strip (not mirrored, not
### double-stack)
- **Ghost**: B's stacked TOTAL as a dashed neutral-gray outline behind A's
  full stacked area (uPlot draw hook; one series worth of geometry).
  Sixteen ghost bands under sixteen live bands is mud — the ghost answers
  only "was the total shape different"; per-class answers live elsewhere.
- **Diff strip**: a slim signed bar lane under the AAS pane — per bucket,
  per class, `A − B` in AAS-seconds, class-colored above zero (grew) and
  dimmed below (shrank). This is the "what changed, when" read.
- *Rejected:* mirrored A-above/B-below (halves vertical resolution of both
  and breaks the shared-y reading); full double-stack overlay (unreadable).

### D3. Tables rank by DELTA
In compare, overview/events/queries tables gain columns `A | B | Δ | ×` and
sort by |Δ| of DB-time by default (toggles: by ratio, by A). Entities in one
window only are labeled `new` / `gone` (never a fabricated ∞ ratio). A noise
floor (|Δ| < max(1% of window DB-time, 50 ms)) collapses into an honest
"… N entities below the change floor" row — the U3 truncation-row pattern.

### D4. Fidelity policy — compare never silently mixes evidence classes
Each window carries its own fidelity badge (the U2 per-pane chip). If A and
B fidelities differ (e.g. exact vs sampled), deltas still render but the
compare header shows a persistent warning chip ("Δ compares exact against
sampled estimates") — flagged, never blocked, never silent. EXACT-required
views (histogram, waterfall, scatter, matrix) are OUT of compare v1: compare
applies to the AAS pane + the three delta tables only.

### D5. URL: `cmp=1&b_off=<ns-as-string>`
Rides the existing hash codec (P9 rules: strings for ns, allowlisted keys,
live=1 re-anchors A to NOW and B follows by offset). Exiting compare is one
✕ (and one history entry, per the U2 push policy).

### D6. Zero server changes in v1
B's AAS strip comes from the same `{from,to,buckets}` tile interface through
the SAME strip cache (keys already carry the window; the generation/
invalidation rules from U2 apply unchanged); B's table aggregates are the
same commands with B's window. Cost: one extra strip fetch per settle and
one extra table request per pane per refresh — acceptable; the parked wire-
compression item gets more attractive, noted, not required. A server-side
`compare` convenience command (one round trip, server-computed deltas) is
v2-if-measured-necessary, per the no-speculative-server-work rule.

### D7. Honest edges
- B before trace start → the ghost/diff render nothing and a quiet note
  says "baseline predates the trace" (expected-state, not an error card).
- B in a sampled region while A is exact → D4's warning chip.
- In-progress data at A's live edge compares against B's closed data — the
  diff strip's final bucket gets the same provisional treatment the live
  AAS edge already has.

## 3. UX summary

A `Compare ▾` control beside the time controls: preset offsets, custom
offset, ✕ to exit. Active compare = ghost + diff strip + delta tables +
per-window fidelity badges + the offset shown in the header ("vs 24 h ago").
Everything else — drills, chips, breadcrumbs, live pause — behaves exactly
as today, operating on A; B follows.

## 4. Build plan (U4, the established triangle)

Single stage (UI-only, no server): Codex builds against this doc — camera
offset plumbing, ghost/diff draw hooks (uPlot), delta-table computation in
the pure builders, URL codec extension, honesty chips, deterministic
fixtures + gallery cells (compare-on states), unit + Playwright coverage
(delta ranking, offset-follow in live, fidelity-mismatch chip, URL
round-trip, baseline-predates-trace note) — then Opus adversarial review
(attack surfaces: delta math honesty, noise-floor fabrication risk, ghost/
diff correctness at strip boundaries, codec hostile input, live-follow
drift), Codex fix round, supervisor gate, PR. Estimated ~600–900 owned LOC.

Out of scope v1 (recorded, not forgotten): pinned fixed baselines (v1.1),
compare on EXACT-required views, server-side compare command, more than one
baseline, cross-instance baselines.
