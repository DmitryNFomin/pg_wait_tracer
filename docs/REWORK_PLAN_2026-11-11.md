# Rework plan, 2026-11-11

Status: **accepted by the owner on 2026-11-11; the first slice (section 4) is approved to start.**
Agreed between the lead and Astra (design adviser) after two review rounds on 2026-11-11. Steps
after the first slice start only when the owner says so, after seeing the slice's measured cost.

Astra's two full reviews, with file and line references, are in `docs/rework/`
(`astra-review-1-architecture.md`, `astra-review-2-method.md`).

## 1. Why

On 2026-11-11 there were 25 open tag-blocking defects, up from 20 the day before, and every
measurement round finds more. Classifying the 25 open and 51 recently closed blockers by origin
(Astra's classification):

| Origin | Open | Closed |
|---|---:|---:|
| Sampling timing, signal, process lifecycle | 7 | 2 |
| Several places computing the same number | 1 | 14 |
| Placing time across window edges; storage lifecycle | 5 | 6 |
| Missing or uncertain information shown as a value | 5 | 6 |
| Tests and gates that certify the wrong thing | 6 | 9 |
| Ordinary interface bugs | 0 | 2 |
| Our own process tooling | 1 | 12 |

Counted in the source at d7f045fd: DB Time is computed in 11 separate places, the CPU / off-CPU
split in about 10, per-event totals in 7, idle, per-session, per-query and event counts in 5 each.
The test suite is about three times the size of the source (185,606 lines against 65,073) and wrong
numbers still passed, because most expected values came from the product itself.

Fixing issues one at a time has no end. A rebuild done the same way would end in the same place.

## 2. What changes

Not a rewrite of the product and not a rewrite of "the middle" by file. One thing changes:
**who decides what a number means.**

- Capture records evidence.
- One core library turns evidence into numbers. Each number is computed in exactly one place.
- Server, CLI and interface are consumers. They receive finished values and cannot recompute them.
- Every value carries its state — observed, estimated, or unavailable — and a reason. Unknown is
  never shown as zero.

Language stays C (portable C11 for the core). The Go bridge, the views and charts, the BPF capture
and the trace container are kept, each qualified for a specific claim rather than declared solid.

Kept and lifted as they are, because they already have independent tests: integer window clipping
(`event_window_ns`, `pgwt_cpu_clip_ns`, `pgwt_offcpu_ns`), stable event sorting, the numeric
comparators, the query-id marker rule, deterministic tie-breaks.

Not trusted without a named check: the capture path (35 of 36 EL8 full-mode known-answer cells were
inside their truth bounds, one outside); the golden trace fixtures (their checksums came from the
product); the block decoder (suspected fault when the output buffer is smaller than a block — read
from source, not reproduced).

## 3. The method

These are rules of construction, enforced by files and by the build, not by review.

1. **A contract per number.** A small file with the definition (population, unit, window rule,
   what evidence it needs, what happens when evidence is missing) and worked examples calculated by
   hand, including the awkward cases: a wait ending exactly on a window edge, a legitimate zero, a
   wait that closes after the window, missing coverage, corrupt input.
2. **Independent expected answers.** A separate agent writes them from the contract only, in a
   workspace that does not contain the product code or product output. They are frozen before the
   implementer starts. An implementation change may not edit a contract, an expected answer or a
   tolerance; changing a contract is its own reviewed change and invalidates what depended on it.
3. **One numerical authority, enforced by the build.** The core does not export raw evidence to
   consumers; consumers get metric objects. A small check fails the build when a consumer sums
   durations itself.
4. **Deliberately broken versions must fail.** A mutant counts as caught only if it builds, reaches
   the intended case and fails the intended assertion.
5. **One unit of work, one review round.** A unit is one contract increment reaching one consumer,
   with the previous code path for that number deleted or explicitly left outside the route. A
   second review round is recorded as a sizing miss.
6. **Old behaviour is a migration check, never truth.** It may confirm trace compatibility, request
   shape, ordering and formatting. It may not decide DB Time membership, sample weights, open
   waits, category defaults, window placement or percentiles.
7. **Every unit records its real cost:** rounds, tokens, gate-box hours.

Added 2026-11-11, owner approval, from Anthropic's and OpenAI's published practice
(`docs/rework/readings.md`):

8. **Reviewers report defects, not opinions.** Only what breaks a contract, a documented guarantee
   or a real path, each with a concrete failure; no style, no hypothetical concerns
   (`.claude/agents/reviewer.md`).
9. **Two strikes.** If a unit fails review twice, there is no third correction: the lead rewrites
   the brief and a fresh implementer round starts from it.
10. **Status only a test can change.** Each contract has a status file listing its claims; a row is
    marked passing only by the test run that proves it, never by hand or by the implementer.
11. **Every check earns its place.** After each slice, compare each review stage's cost with what it
    actually caught, and drop or keep it per type of work on that evidence.

## 4. The first slice (pilot)

Purpose: prove the method on something small before anything else is committed.

- **Number:** wait time and completed-wait count for one event, one backend, any window, exact
  (non-sampled) evidence only.
- **Path:** contract → independent answers → new core → server → bridge → one small panel in the
  Events view, running beside the old code on the same trace files.
- **Budget:** 1 round for the contract, 1 for the independent answers, 2 implementation rounds
  (codex), 2 review rounds — one per unit, two units: the core, then trace-to-screen — 1–2 gate-box
  hours.
- **Must show:** hand calculation, independent reference and new core agree; the browser shows the
  values and the "unavailable" reasons; a mutant that moves time across a window edge fails even
  though the whole-trace total is unchanged; the same evidence split into different blocks gives
  the same result; no consumer computes either number itself.
- **Stop conditions — the method has failed and nothing further starts:** a third implementation or
  review cycle is needed because the contract keeps changing; the answer-writer needs product code;
  a required mutant survives; plain arithmetic work still needs box experiments.
- A missing field in the stored evidence is a different outcome: it means a storage step comes
  before further migration.

How it is run: lead on Opus, Sonnet reviewers (Opus only for `src/` logic), no long-lived adviser
session, stop and report to the owner with the measured cost when the slice ends.

## 5. After the pilot

Order, each step switching numbers over and deleting the code they came from. Sizes are Astra's
planning ranges, to be re-measured after the pilot; they are not promises.

| Step | What switches | Implementation rounds | Gate-box hours |
|---|---|---:|---:|
| Evidence and context | process identity, category coverage, sample timing, open waits; writer fields where needed | 3–4 | 4–8 |
| DB Time, wait / CPU, AAS | Overview and AAS | 3–4 | 1–2 |
| Events, sessions, queries, filters | main tables | 3–4 | 1–2 |
| Executions and derived views | timeline, concurrency, lock chains, variants | 3–5 | 2–4 |
| CLI | replay and dump call the core | 2–3 | 0–1 |
| Rollups (summaries) | off until this step rebuilds them (section 8) | 2–4 | 1–3 |
| Retirement | delete old compute, dead flags, old adapters | 2–3 | 2–4 |

Total planning range: 46–66 agent rounds (implementation, review and contract authoring) and 12–26
gate-box hours. Excluded: the sampler track and PostgreSQL 14–16.

## 6. Where the work lives

On `master`, in small merges, with the new core in its own directory beside the old code. No
long-lived rework branch: it would drift from master, and the rule "the old path is deleted as each
number switches" only means something on the branch that ships. Until a number has switched, the
new route for it is opt-in and visibly marked as a pilot. No release tag is set until the ledger in
section 9 is green for the release scope.

## 7. The sampler track (separate, small, measurement only)

The default mode samples; today its numbers are not trustworthy: the wait state is read about
0.23 ms after the stamped time, time was over-counted by about 2.5% (#863), the result is biased
when the sampler shares cores with sessions, and the daemon costs 6.9% of a core with 500 idle
connections (#866).

Before the storage format for the new core is frozen, five measurements decide what a sample record
must contain and what the sampled mode may claim. Each is run on Ubuntu and on EL8 4.18:

| Question | Measurement | What it decides |
|---|---|---|
| When was the value really read? | read time against independently recorded state changes, at 4 to 500 connections | whether a sample stores its observation time |
| How much time does a sample stand for? | jitter, stalls, missed reads, changing populations | whether each sample stores its weight |
| Is the estimate biased? | short and long waits, idle and saturated machine, repeated | the accuracy the product may state, and under what load |
| Is the query / category consistent with the wait? | controlled changes around a read | what goes to an explicit "unattributed" bucket |
| What does it cost? | paired runs with and without the daemon at 1, 50, 200, 500 connections | the supported rate and load, or a change of mechanism |

The exact-evidence pilot does not wait for this. Until this track reports, sampled numbers stay red
in the ledger.

## 8. Scope decisions recorded

- **PostgreSQL 14, 15, 16:** will be added, later, at lower priority (owner, 2026-11-11). Today
  only 13, 17 and 18 have layout tables; 16 is tested to refuse. Until the work is done the
  product and the README must say 14–16 are not supported, and "refuses cleanly" and "is supported"
  are separate ledger rows.

- **Summaries:** switched off until they are rebuilt from the core (owner, 2026-11-11). The raw
  route must refuse oversized windows with a clear message, never hang or silently shrink the window.
- **The four unmerged fix branches** (owner, 2026-11-11): finish #861 (box-check provenance) and
  #863 (sample weights); park #855 (interface marking) and #839 (process category) and turn their
  findings into contracts.
- **The stop hook** (`scripts/lead/sweep-hook.sh`) is removed from `.claude/settings.json` (owner,
  2026-11-11). The board on issue #850 and the ledger in section 9 replace it.
- **First slice:** go (owner, 2026-11-11).

## 9. What the owner looks at

One ledger. Each row is a claim, for example "exact wait time is correct for any window", "PG16
refuses cleanly", "PG16 is supported", "sampled DB Time is within the stated bound at 500
connections". Columns:

| Contract accepted | Independent answer exists | Correct in the real interface | Broken versions caught | Binary identified | Load and platform exercised | Old code deleted | Cost so far / next action |
|---|---|---|---|---|---|---|---|

A cell is green only on linked evidence. A row is green only when every cell is. Agreement between
two implementations turns nothing green. A change to a contract, an expected answer or the code it
covers turns the dependent cells back to red.

## 10. What makes this fail the same way again

Reviewers being the main check on meaning; expected values that move with the implementation;
"estimated" used as an excuse for any error; new scope arriving faster than old paths are deleted;
tests that only check that consumers agree with each other; leaving platform and load qualification
to an unpriced final phase.
