# Dev loop plan — fast, sustainable development with minimal owner involvement

_Decided 2026-08-28. Status as of 2026-08-29: steps 2 and 3 merged (#89, #90);
steps 1, 4, 5 open. This document is self-contained: any agent or person can
pick up an open step from it._

## Goal and principles

The owner makes design decisions and merges. Everything else — building,
testing, screenshotting UI, reviewing, chasing CI — is a script or an agent.

1. **Every gate is a script.** If a check needs a human to judge it, it is not
   a gate yet. Agents self-verify with the same commands CI runs.
2. **x86 Linux is the truth.** Nothing eBPF-related runs on the developer Mac
   (Apple Silicon, arm64). Every live check runs on real x86 Linux with real
   PostgreSQL, real kernels, real hardware watchpoints. No local Linux VMs.
3. **Noise gets moved, not hardened.** A test that is red only on shared
   GitHub runners moves to dedicated hardware or to nightly. It never gets
   another retry loop or wider tolerance. (June–August 2026: ~20% of commits
   were CI hardening; that is the tax this plan removes.)
4. **The owner sees evidence, not diffs.** A PR arrives with the live-suite
   summary, the before/after UI gallery, and a reviewer's verdict.

## Target infrastructure

| Machine | Role | Cost |
|---|---|---|
| Developer Mac | editing, agents, worktrees; natively runs Node builder tests, Go bridge tests, Playwright vs `tests/mock_server.py` | — |
| **Gate box** — Hetzner CCX13 (dedicated vCPU, x86), Ubuntu 24.04, PG 13/16/17/18 | self-hosted GitHub runner, label `ubuntu`; runs the timing-sensitive PR jobs (capture-smoke ×4, sampled-overhead, snapshots); default target of `make box-check` | ≈ €13/mo |
| **Ephemeral VMs** — Hetzner CX22 from snapshots: Rocky 8 (kernel 4.18), Rocky 9 (5.14), Ubuntu 24.04 (6.8) | nightly OS×PG matrix on real kernels; on-demand `make box-check OS=el8`; one per agent when two would collide | ≈ €1/mo |
| GitHub `ubuntu-latest` | deterministic jobs only: build+unit, web unit, Playwright vs mock, protocol-drift, Go bridge | — |

Why not one box: the OS axis *is* the kernel; containers share the host
kernel (the current nightly never runs 4.18). Why not local VMs: arm64 ≠ x86
(struct layouts, watchpoints, benchmarks), and x86 emulation is 10–20× slower.
Why a persistent gate box: the PR gate runs many times a day and needs the
same hardware for a timing baseline; ephemeral shared-vCPU VMs would
reintroduce noisy-neighbour variance in the one job most sensitive to it.

## Test tiers

| Tier | What | Where | Trigger | Blocks merge |
|---|---|---|---|---|
| Deterministic | C units, synthetic Python, Node builders, Go bridge, Playwright vs mock, protocol-drift | Mac (`make check`) and `ubuntu-latest` | every push | yes |
| Live gate | capture-smoke PG 13/16/17/18, sampled-overhead, visual snapshots | gate box | every PR | yes |
| UI gallery | before/after screenshots of all snapshot cells + checklist grading | Mac | PRs touching `web/` | yes, via reviewer |
| OS matrix | full live suite on Rocky 8 / Rocky 9 / Ubuntu × all PG | ephemeral VMs | nightly, `needs-el8` label, `make box-check OS=…` | nightly |
| Benchmarks | TPS / latency for README | gate box, manual | release | — |

## Life of a task (after all steps)

1. **Owner** writes the issue: design decision + acceptance criteria.
2. Main agent splits (see CLAUDE.md "Who does what"), spawns an `implementer`
   in an isolated worktree on `agent/<slug>`.
3. `make check` — deterministic tier on the Mac; the push guard refuses
   `git push` without a fresh stamp.
4. `make box-check` — live tier on x86; `OS=el8` for kernel/libbpf/layout code.
5. `make ui-gallery` if `web/` changed.
6. Fresh `reviewer` (+ `ui-reviewer`) agents verify evidence, hunt bugs,
   write the PR body; blockers loop back to the implementer.
7. CI: deterministic on hosted runners, timing jobs on the gate box; merge queue.
8. **Owner** glances at the PR body / gallery, answers design questions, merges.
9. Nightly matrix on ephemeral VMs → triage agent → morning digest.

---

## Step 1 — Gate box + CI split  `[OPEN — needs the owner's Hetzner account]`

**Goal:** the timing-sensitive jobs run on dedicated hardware; hosted runners
keep only deterministic jobs. Stops the hardening tax immediately.

**Prerequisites (owner):**
- Hetzner Cloud project + API token (`HCLOUD_TOKEN`) with read/write.
- SSH key uploaded to the Hetzner project (`tests/hetzner-vm.sh` matches it by fingerprint).
- GitHub: repo Settings → Actions → Runners → "New self-hosted runner" gives a
  registration token (valid 1 h), or a PAT with `repo` scope so scripts can
  mint tokens via `POST /repos/{owner}/{repo}/actions/runners/registration-token`.

**Work:**
1. Create the box: `HCLOUD_TOKEN=… tests/hetzner-vm.sh create --type ccx13`
   with an Ubuntu 24.04 image (add `--image ubuntu-24.04`; the script's
   default is `rocky-9`). Note the IP.
2. Provision with `tests/provision-runner.sh ubuntu --runner-token <token>`
   (written in step 4; until then do it by hand following `.github/workflows/ci.yml`
   "Install build dependencies" + "Install PostgreSQL" steps, once per PG
   version, each cluster on port `54<major>`: 5413, 5416, 5417, 5418).
   Install the GitHub runner as a systemd service with labels
   `self-hosted,linux,x64,ubuntu`. Runner user needs passwordless sudo
   (`ci_smoke.sh`, `run_all.sh` need root).
3. Take a Hetzner snapshot named `pgwt-gate-ubuntu-<date>` once green.
4. `.github/workflows/ci.yml`: change `runs-on: ubuntu-latest` to
   `runs-on: [self-hosted, ubuntu]` for `sampled-overhead`, `capture-smoke`,
   `snapshots`. Delete their apt/PGDG install steps (preinstalled); select
   the PG cluster by port (`PGPORT=54${{ matrix.pg }}`) instead of dropping
   and reinstalling clusters. Add `concurrency: { group: gate-box, cancel-in-progress: false }`
   to those jobs so timing runs never overlap.
5. Shrink `sampled-overhead`: with dedicated vCPU the 21+21 confirmation
   pairs and the 60-minute budget are unnecessary; start with 5 pairs and
   the existing threshold, watch a week of runs, tighten.
6. Branch protection: required checks = deterministic jobs + the three gate
   jobs; enable merge queue.
7. Set `PGWT_BOX=root@<ip>` locally (and in the owner's shell profile) so
   `make box-check` works.

**Acceptance:** three consecutive green master runs with the gate jobs on the
box; `make box-check` from the Mac prints a passing `run_all.sh` summary;
`sampled-overhead` wall time under 15 min.

---

## Step 2 — Agents self-verify  `[DONE — PR #89]`

`CLAUDE.md`; `make check` / `make check-fast` (`scripts/check.sh`, stamp =
`scripts/tree-hash.sh` → `.pgwt-check.stamp`); `make box-check [OS=…] [PG=…]`
(`scripts/box-check.sh`, `flock`-serialised, logs to `tests/results/box-check-*.log`);
push guard `.claude/settings.json` → `scripts/hooks/push-guard.sh`; `.claude/`
tracked in git.

Local one-time setup on a Mac:
```
brew install node go
python3 -m pip install --user playwright==1.60.0 websockets pillow numpy
python3 -m playwright install chromium
git remote set-url origin git@github.com:DmitryNFomin/pg_wait_tracer.git   # HTTPS+keychain cannot prompt from an agent shell
```

---

## Step 3 — UI review without the owner  `[DONE — PRs #89, #90]`

`make ui-gallery [BASE=ref]` → `tests/ui_gallery.sh` renders all 42 snapshot
cells at the merge-base (temp worktree) and on the working tree;
`tests/ui_gallery_report.py` writes `tests/results/ui_gallery/index.html`
(changed cells first, magenta diff overlay) + `summary.json`. Validated:
identical tree → 42/42 unchanged; a body-colour tweak → exactly the 6 table
cells flagged. Agents: `.claude/agents/implementer.md` (opus; spawn with
fable for `src/`), `reviewer.md` (fable/high), `ui-reviewer.md` (opus);
skill `/pr-ready`. `tests/test_web_ui_snapshots.py` honours `PGWT_SNAP_DIR`.

Still worth doing later: regenerate CI snapshot baselines on the gate box
(same chromium/fonts every run) so the `workflow_dispatch → download →
commit PNGs` dance disappears.

---

## Step 4 — Ephemeral VMs + real OS matrix  `[OPEN]`

**Goal:** EL8 (kernel 4.18) and EL9 (5.14) are tested on their real kernels
nightly and on demand, on throwaway VMs; agents can verify kernel/libbpf/
layout changes without the owner.

**Prerequisites:** `HCLOUD_TOKEN` as a GitHub Actions secret and in the
local environment; a PAT (`repo` scope) as secret `RUNNER_PAT` for JIT
runner tokens.

**Work:**
1. `tests/provision-runner.sh <el8|el9|ubuntu> [--runner-token T] [--labels …]`
   — idempotent, runs on the VM as root. Installs build deps (EL8 path:
   static libbpf/bpftool bundling is handled by the Makefile already), PGDG
   repo, PG 13/16/17/18 with one cluster each on ports 5413–5418
   (`pg_stat_statements` in `shared_preload_libraries`, `track_activity_query_size`
   as CI sets it), `perf`, `bpftool`, and — if a token is given — the GitHub
   runner as a systemd service in `--ephemeral` mode with the given labels.
   Reuse the install steps in `.github/workflows/nightly.yml` and
   `tests/cloud-init-rocky9-pg18.yaml`; they are the known-good recipes.
2. Snapshot builder `tests/build-snapshots.sh`: for each OS, create a CX22
   via `tests/hetzner-vm.sh`, run `provision-runner.sh` without a token,
   `hcloud` snapshot it as `pgwt-<os>-<date>`, delete the VM. Rerun
   monthly / when PG minors move.
3. `nightly.yml` rewrite:
   ```
   provision (ubuntu-latest, matrix os): create CX22 from snapshot pgwt-<os>-latest,
       mint JIT runner token (RUNNER_PAT), cloud-init registers an --ephemeral
       runner with label run-${{ github.run_id }}-<os>
   test (runs-on: [self-hosted, run-…-<os>], matrix pg: [13,16,17,18]):
       make && make -C tests check && sudo tests/run_all.sh --require-live --pg-version $pg
   teardown (always()): delete the VM by name
   ```
   Keep `fail-fast: false`; keep the loud-skip semantics of `ci_smoke.sh`
   for watchpoints. Delete the container-based matrix (it never tested the
   kernels it claimed to). Optional cell: `cax11` arm64 Rocky 9 to make the
   README's aarch64 claim true.
4. `scripts/box-check.sh`: when `OS=<x>` is set and `PGWT_BOX_<X>` is unset
   but `HCLOUD_TOKEN` is, create a VM from the snapshot, run, and delete it
   (`--keep` to leave it up for debugging). Same path serves "one VM per
   agent" when the gate box is busy.
5. On-demand label: a `needs-el8` / `needs-el9` PR label triggers the same
   matrix for that PR (`pull_request: types: [labeled]`).

**Acceptance:** nightly green on 3 OS × 4 PG with `uname -r` in each job's
summary showing 4.18 / 5.14 / 6.8; `make box-check OS=el8` from a Mac with
only `HCLOUD_TOKEN` set completes and deletes its VM; monthly cost under €2.

---

## Step 5 — CI triage without the owner  `[OPEN]`

**Goal:** mornings start with a digest, not red checks; agents open PRs
themselves.

**Prerequisites:** `brew install gh`, then `gh auth login` (interactive — the
owner runs it once); for scheduled runs, a `GH_TOKEN` in the routine's
environment.

**Work:**
1. `/pr-ready`: with `gh` present, actually run `gh pr create` with the
   reviewer's body and attach the gallery/box-check paths (today it prints
   the command).
2. `.claude/skills/ci-triage/SKILL.md`: given a run id / PR / "latest red":
   `gh run view --log-failed`; classify each failed job as
   **regression** (deterministic job, or reproducible twice), **environment**
   (apt/PGDG/bpftool/runner offline), or **timing** (only the known
   timing-sensitive jobs, passes on rerun). Actions: timing → `gh run rerun
   --failed` once and note it; environment → open an issue with the log
   excerpt; regression → spawn an `implementer` on `agent/fix-<slug>` with
   the failing log as the contract, then the normal review chain. Output: a
   one-paragraph verdict per run.
3. Scheduled routine (`/schedule`, daily after the nightly finishes): run
   `/ci-triage` over every red run in the last 24 h and every open PR with
   failing checks; post one digest (issue comment on a pinned "CI digest"
   issue, or the PR itself).
4. Rule enforcement: the triage skill never edits tests to add retries or
   widen tolerances; a timing flake on a hosted runner is a request to move
   the job to the gate box (step 1), reported as such.

**Acceptance:** for one week, every red run gets a triage comment within a
day without the owner reading a log; at least one flake auto-rerun and one
regression PR produced by the pipeline.

---

## Cost summary

Gate box ≈ €13/mo · ephemeral compute ≈ €0.50 · snapshots ≈ €0.60 ·
optional arm64 cell ≈ €0.10 → **≈ €14–15/mo**.
