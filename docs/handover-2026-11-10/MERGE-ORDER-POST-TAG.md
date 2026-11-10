# Post-tag merge order — parked branches at candidate `8ab558b`

Nothing here merges until the owner tags `8ab558b`. Any merge before that voids both counted
rehearsals (`docs/DEMO_REHEARSAL_CRITERIA.md:1175-1180`) and forces re-qualification **plus**
re-recording the video and GIFs.

Order below is Astra's plan as amended by what the reviews actually found. **One PR at a time.**
At every position: update onto current master, re-run `make check` + `box-check` on the final tree
(`OS=el8` where noted — but see #532, no el8 box exists), and wait for strict branch-protection CI
on the combined tree. **Each preceding merge invalidates the previous branch's mergeability and CI
evidence.** Never arm the whole queue.

---

## CRITICAL — one constraint that must not be got wrong

**`agent/508-protocol2` physically contains `agent/515-idle-provenance`.** Its base is
`ec39f06` = `769a692` (#515) + `886fb53` (#501) merged `--no-ff`. Verified by a reviewer:
`git rev-list --parents -n1 ec39f06` shows both parents, and `git merge-base` confirms both fork
directly off `8ab558b`.

**Do NOT cherry-pick `a8a080a` alone onto master.** That ships a header declaring protocol rev 2
while the server emits no `idle_classification_complete` — precisely the class of false claim #508
exists to remove.

Merging #508 merges #515. So either merge #515 first and rebase #508, or accept that #508 brings it.

---

## The queue

### 1. `agent/regate-405` @ `1822d8f` — #405, with #492 and #493 riding it
Needs a **fresh gate** on tagged master; its existing evidence predates everything. Goes first
because `agent/495-replay-cpu` is based on it.

### 2. `agent/cli-holder-offcpu` @ `b4b9ee3` — #497, #498
READY-with-nits. Overlaps #496 on `src/compute.c`, so it precedes it.

### 3. `agent/trace-sidecar-perms` @ `cedcf4c` — #499, #500
READY-with-nits. Shares `README` and `tests/run_all.sh` with #505/#506, and
`tests/Makefile`/`unit_tests.list` with #497/#498 — resolve those before any consumer.
**Do not reimplement these three; preserve their reviewed nits.**

### 4. `agent/512-root-werror` @ `b69ecc3` — #512
READY-with-nits. Changes compilation for every C branch after it, so it goes before them.
**Gated Ubuntu-only** — el8 unreachable (#532). `src/` has zero implicit declarations today.

### 5. `agent/495-replay-cpu` @ `7b1e723` — #495
READY-with-nits. **Based on `agent/regate-405`**, so it follows position 1.

### 6. `agent/496-detail-clip` @ `13ade34` — #496
Reviewed. **Based on `agent/cli-holder-offcpu`**, so it follows position 2. Includes an approved
two-line `src/compute.h` comment fix.

### 7. `agent/501-catalog-gate` @ `886fb53` — #501
READY-with-nits. Touches only `tests/test_protocol_drift.py`. No textual conflict with #515.

### 8. `agent/515-idle-provenance` @ `769a692` — #515 backend
READY after three rounds. Touches only `tests/mock_server.py` of the shared files.
**See the CRITICAL note — #508 contains this.**

### 9. `agent/508-protocol2` @ `a8a080a` — #508
READY-with-nits. **Rebase, do not merge.** Contains #515 and #501.

### 10. `agent/518-run-isolation` @ `5739de9` — #518 + disk preflight
READY after three rounds. Isolate launcher changes from the harness ones that follow.

### 11. `agent/520-refusal-retention` @ `143bae9` — #520
READY. Independent.

### 12. `agent/514-rehearsal-coverage` — #514
In flight at time of writing. Shares rehearsal harness files with #517.

---

## Still unbuilt, in planned order

#517 (gate controls, stacks #514) · #519 (frame probe) · #505 (README measured claim) · #503
(re-capture report) · #506 (per-source hits, needs `OS=el8`) · #502 (source analysis, needs #506).

Then the `web/` serial lane: **#510 + #507 + #515's UI consumer**, then **#523**.
#510 must follow #508, because its readiness metadata depends on rev 2.

---

## Nits to carry into PR bodies, not re-litigate

- **#508**: `src/pg_wait_tracer.h:33`'s comment says rev 2 "requires" `idle_classification_complete`
  readiness. **No client reads that field** — it is enforced only by the drift test's schema
  comparison. The contract is real; the wording implies a client requirement that does not exist
  yet. Risk named by the reviewer: a future reader deletes the field believing the UI validates it.
  **This is the third misleading comment found today** (`compute.h:749-750` and `compute.c:837-840`
  were the others), and each cost someone real time. Fix the wording when the PR opens.
- **#508**: `web/bridge_test.go:537`'s message assertion re-derives the match count from the regex
  under test, so it is vacuous. The reviewer verified the real `err == nil` gate catches both
  mutants. Simplest resolution is to **delete the conditional**, not strengthen it.
- **#520**: `.refused.N` accumulates full frame sets, not small diagnostics — see #527.
- **#512**: Ubuntu-gated; el8 unproven for the narrow built-in-recognition case only.
