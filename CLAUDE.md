# pg_wait_tracer — working agreement for agents

eBPF wait-event tracer for PostgreSQL (C + BPF daemon, `pgwt-server` analysis
backend, Go WebSocket bridge, ECharts/uPlot web UI). Plan and status:
`docs/ROADMAP_AND_STATUS.md`. Nothing eBPF-related runs on macOS.

## The three commands

| Command | Runs where | What | When |
|---|---|---|---|
| `make check` (`check-fast` = seconds) | this Mac | Node builder tests, Go bridge, py_compile, Playwright UI + chaos suites vs `tests/mock_server.py` | before every push — the push guard enforces it |
| `make box-check [OS=el8\|el9\|ubuntu] [PG=13\|16\|17\|18]` | x86 Linux box over ssh (`$PGWT_BOX`, `$PGWT_BOX_EL8`…) | Linux build, C units, synthetic-data tests, protocol drift, real-PG capture (`tests/run_all.sh --require-live`, incl. the live-UI-smoke walk — `tests/ui_live_smoke.sh` / `tests/results/ui_live/summary.json`) | before every PR; `OS=el8` when touching kernel/libbpf/layout code |
| `make box-check EPHEMERAL=1 [PG=…] [KEEP=1]` | throwaway Hetzner VM created from the `pgwt=gate-snapshot` image, always deleted at the end (`KEEP=1` leaves it up and prints the delete command) | the same live tier as above, on a private one-run VM — no `$PGWT_BOX`, no flock contention with other agents | agents' inner iteration loop (issue #141) — ephemeral for iteration, the persistent box for the final pre-PR run |
| `make ui-gallery [BASE=ref]` | this Mac | before/after screenshots of every UI snapshot cell → `tests/results/ui_gallery/index.html` + `summary.json` | before every PR that touches `web/` |

Logs: `tests/results/box-check-*.log` (`box-check-ephemeral-*.log` for `EPHEMERAL=1`), `tests/results/ui_gallery/`, `tests/results/ui_live/summary.json`.

`make hetzner-sweep` deletes any `pgwt=ephemeral`-labelled Hetzner VM older than
6h (also runs automatically at the start of every `box-check`); the
persistent gate box (`pgwt-gate`) is never touched by it.

## Definition of done

A PR is ready when ALL of these are true and the evidence is in the PR body:
1. `make check` passed (full, not `--fast`) on the final tree.
2. `make box-check` passed — paste the run_all summary (last ~20 lines), which
   includes the live-UI-smoke one-line verdict (`tests/results/ui_live/summary.json`).
   Carve-out: a branch touching only CI infrastructure (no `src/` change, no
   test-assertion change) may substitute real `gh workflow run` evidence on
   the gate box for `make box-check` — state that substitution explicitly in
   the PR body.
3. If `web/` changed: `make ui-gallery` ran; the `summary.json` counts and every
   `changed`/`added`/`removed` cell are listed with a one-line justification each.
4. A fresh **reviewer** agent (`.claude/agents/reviewer.md`; `ui-reviewer.md`
   for UI) approved, or its open questions are listed under "Design questions
   for the owner". Those questions are the ONLY thing the owner should have to
   think about.
Use `/pr-ready` to run this sequence.

## Who does what (main session → subagents)

The interactive session is the **main agent**: it plans, splits, spawns, and
judges. It writes no feature code itself for anything bigger than a one-liner.

- **Models** are pinned in `.claude/agents/*.md` (owner rule 2026-09-20:
  cheapest model that does good work — tokens are the budget):
  `implementer` = Sonnet for everything including UI (spawn with
  `model: fable` only for `src/` BPF, capture, discovery, backend layout or
  accounting logic; Opus only for design-heavy UI after a Sonnet attempt
  failed), `reviewer` = Sonnet (Opus only for `src/` logic; never Fable),
  `ui-reviewer` = Sonnet. `Explore`/triage: Sonnet or Haiku. ONE review round:
  READY-with-nits ships with the nits listed; a second round only for a code
  blocker; no confirmation re-reviews. Agents wait for box runs with one long
  sleep inside a single command, never minute-by-minute polling. Reports are
  at most ~15 lines.
- **Splitting**: one roadmap item = one issue = one branch = one implementer.
  Split only along an independent seam (disjoint files AND disjoint tests);
  never split a shared file; at most **2 tasks in flight**. Anything over ~a
  day of work goes to a `Plan` agent first; its steps run sequentially unless
  the plan shows them independent.
- **Spawning**: always `isolation: "worktree"`. The prompt is a contract:
  issue text, acceptance criteria, required `make` targets, "stop and report,
  do not open the PR".
- **Review chain**: (1) scripts — `make check`/`box-check`/`ui-gallery`
  produce files the implementer cannot argue with; (2) a fresh `reviewer`
  (+ `ui-reviewer` when `web/` changed) reads code + evidence and writes the
  PR body; blockers go back to the implementer via SendMessage; (3) the main
  agent reads the reviewer's report, not the diff, and decides ready / back /
  ask the owner. Only "Design questions for the owner" reach the human.

## Rules

- Branch `agent/<slug>`; one task per branch; work in an isolated git worktree
  (`EnterWorktree`). Two agents in one checkout collided once — never again.
- Never harden a test against runner noise (retries, wider tolerances,
  confirmation loops). If it is red only on shared runners, say so in the PR;
  it belongs on the dedicated gate box.
- `tests/run_all.sh`'s `KNOWN_FAILING` table (test name → tracking issue
  number) is only for a test that reproduces a real, filed product bug being
  fixed in `src/`; it never covers timing or runner noise, which gets moved
  or investigated, not listed. A listed test still runs every time; neither
  outcome fails the gate — an expected failure and an unexpected pass
  ("UNEXPECTED PASS ... intermittent or fixed; check the issue") both count
  as `known-failing`, the pass also as `xpass`. The reviewer checks any
  `xpass` line against its issue rather than assuming the list is stale. A
  listed test that could not even run (exit 126: not executable, 127:
  command/file not found) still fails the gate regardless of
  `KNOWN_FAILING` membership — that is not the known product bug, it is a
  broken test invocation.
- Never commit locally generated snapshot PNGs to `tests/web_snapshots/`
  (font rendering differs from CI). Baselines are regenerated by the CI
  `snapshots` job only.
- Unit tests: add C tests to `tests/unit_tests.list` (single source of truth);
  UI logic goes in pure builders with a `tests/web_unit/*.test.mjs` case.
- `[skip ci]` only for docs-only commits.
- Do not push without a passing `make check` stamp (the hook will block you;
  do not bypass it — `PGWT_SKIP_PUSH_GUARD` is for humans).

## Local setup (one-time)

```
brew install node go
python3 -m pip install --user playwright==1.60.0 websockets pillow numpy
python3 -m playwright install chromium
export PGWT_BOX=root@<gate-box-ip>        # optional: PGWT_BOX_EL8, PGWT_BOX_EL9
```

`.python-version` (the pyenv pin with Playwright) is untracked (`.git/info/exclude`), so a new worktree only gets it via pyenv's global default or an explicit `PYENV_VERSION`, never by checking it out.

## Layout

- `src/` daemon + BPF (`src/bpf/`), `src/server.c` = `pgwt-server`
- `web/` Go bridge; `web/static/` UI — `views/` per-tab, `dev/gallery.html` fixture gallery
- `tests/` everything: `run_all.sh` (Linux master runner), `ci_smoke.sh` (real-PG
  capture), `test_web_ui*.py` (Playwright), `web_unit/` (Node), `web_snapshots/` (baselines)
- `docs/VISUAL_CHECKLIST.md` — the seven words a UI reviewer grades against
- `.github/workflows/ci.yml` gating; `nightly.yml` OS matrix
