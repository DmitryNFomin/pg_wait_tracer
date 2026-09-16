# Dev loop plan — fast, sustainable development with minimal owner involvement

_Decided 2026-08-28. Status as of 2026-09-16: steps 2 and 3 merged (#89, #90);
step 1's box + provisioning + `make box-check` are done (this branch) —
runner registration and the `ci.yml` job split are a follow-up (tracked as
the remainder of step 1, see below); steps 4, 5 open. This document is
self-contained: any agent or person can pick up an open step from it._

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
| **Gate box** — Hetzner cx33 (4 **shared** vCPU, 8 GB, x86), Ubuntu 24.04, PG 13/16/17/18, server name `pgwt-gate` — **created, provisioned, `make box-check` green** (this branch) | self-hosted GitHub runner, label `ubuntu` (registration is a follow-up, see below); runs the timing-sensitive PR jobs (capture-smoke ×4, sampled-overhead, snapshots); default target of `make box-check` | €10.27/mo gross (current Hetzner price list; the plan's original "CCX13 ≈ €13/mo" was stale — CCX13 is actually €52.02/mo. cx33 was chosen over dedicated-vCPU ccx13 for cost; the sampled-overhead noise table below characterizes the shared-vCPU tradeoff) |
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

## Step 1 — Gate box + CI split  `[box + provisioning + box-check DONE; CI split + runner registration OPEN — follow-up task]`

**Goal:** the timing-sensitive jobs run on dedicated hardware; hosted runners
keep only deterministic jobs. Stops the hardening tax immediately.

**Done (agent/gate-box branch, 2026-09-16):**
- Box created: Hetzner **cx33** (4 shared vCPU, 8 GB), image **ubuntu-24.04**,
  location **fsn1** (EU), name **pgwt-gate** — never delete it. `cx33` was
  used instead of the originally-planned CCX13 (dedicated vCPU): the current
  Hetzner price list has CCX13 at €52.02/mo, not the ≈€13/mo this doc
  originally assumed; cx33 is €10.27/mo. This is a **shared**-vCPU box, not
  dedicated — see the noise table below for what that costs in timing
  precision.
- `tests/hetzner-vm.sh`: added `--image`, `--name`, `--location`; the
  location list is now EU-only (`fsn1 nbg1 hel1` — US/Singapore cost ~3x and
  are never tried automatically).
- `tests/provision-runner.sh ubuntu`: idempotent (verified via two
  back-to-back runs — the second is a clean no-op modulo apt/PGDG metadata
  refresh). Installs the daemon + `pgwt-server` build deps and bpftool
  fallback (mirrors `ci.yml`/`nightly.yml`), the PGDG repo, and PostgreSQL
  **13/16/17/18**, one cluster each, on ports **5413/5416/5417/5418**
  (`pg_stat_statements` preloaded, `compute_query_id` on for 14+, pgbench
  pre-initialized at scale 10 — several live tests document "Requires ...
  pgbench initialized" but never initialize it themselves, matching the
  convention the old `tests/cloud-init-rocky9-pg18.yaml` used). `el8`/`el9`
  are clearly-marked stubs that exit non-zero (step 4's job).
- **Found and fixed a real bug** in `scripts/box-check.sh`: its rsync
  excludes `pgwt-server*` / `pg_wait_tracer*` were unanchored, so besides the
  built binaries at the repo root they also matched `src/pg_wait_tracer.c`,
  `src/pg_wait_tracer.h` and `src/bpf/pg_wait_tracer.bpf.c` (rsync excludes
  without a leading `/` match at any depth) — every remote build failed with
  "No rule to make target 'build/pg_wait_tracer.o'". This had presumably
  never been caught because nothing had run `make box-check` successfully
  since PR #89 introduced it. Fixed by anchoring the excludes to the repo
  root (`/pgwt-server`, `/pg_wait_tracer`, etc).
- **PGPORT selects the cluster transparently**: Debian's `pg_wrapper` (the
  real binary behind `/usr/bin/psql` and `/usr/bin/pgbench`) already reads
  `PGPORT` and picks the matching local cluster/version — confirmed it
  survives `sudo` on this box (PAM's `pam_env` re-applies `/etc/environment`
  for the `sudo` target session, independent of `env_reset`). So
  `tests/run_all.sh`'s and the live tests' plain `psql`/`pgbench` calls (no
  explicit `-p`) already target the right cluster once `/etc/environment`
  sets `PGPORT=54<major>` on the box — no code changes needed for this part
  of the step-1 plan's "select the PG cluster by port" idea. The box's
  default is `PGPORT=5418` (matches `run_all.sh`'s own highest-version
  auto-detect when no `--pg-version` is given); switch it for other versions
  until step 4's ephemeral-VM `--pg-version`-aware plumbing lands.
- `make box-check` (default, PG=all) and `make box-check PG=13` both ran to
  completion (not clean — see "Known gaps" below) against the real box;
  8/8 PostgreSQL versions×ports verified with `psql -c 'select version()'`.

**Known gaps found while verifying box-check (real bugs, not provisioning —
left unfixed, in scope for a follow-up):**
1. `tests/run_all.sh`'s C/Python unit-test loop (`unit_tests.list`) runs
   binaries directly from the repo root. `test_effective_cores` resolves its
   fixtures via a path relative to CWD (`fixtures/effective_cores/...`), so
   it needs `cwd=tests/`; run directly from the repo root (as `run_all.sh`
   does) it fails all 21 checks with `cores -1, expected N`. Passes cleanly
   under `make -C tests check` (which does `cd tests`) — that's the path
   `ci.yml` uses, so this was never caught before.
2. Same loop: `test_sampled_overhead_gate.py` and
   `test_data_query_text_context.py` are tracked as mode `100644` (no `+x`)
   and `run_all.sh` execs list entries directly (no `python3` prefix) — both
   fail with `Permission denied`. `make -C tests check` special-cases `*.py`
   with an explicit `python3` prefix, so this was never caught either.
3. `tests/test_cli.sh`'s "no args (auto-discover)" check invokes the tracer
   with no `--pid`, relying on there being exactly one running PostgreSQL
   instance. With all 4 clusters up (this step's own design), the daemon
   correctly refuses ("Multiple PostgreSQL instances found ... Use --pid").
   Correct product behavior, but incompatible with concurrently running all
   four clusters — this test (and several others; see next point) assume a
   single active instance.
4. Several live tests are explicitly documented as "Requires: ... running
   PostgreSQL 18" (`test_session_accuracy.py`, `test_query_accuracy.py`,
   `test_daemon_server.py`, `test_multi_window.py`) and do not behave
   correctly against other versions — e.g. `test_accuracy.py`'s IO
   cross-check reads `pg_stat_io`, a PG16+ view, so it reads 0 on PG13. This
   is why `make box-check PG=13` (10 failures) is redder than the default
   run (5 failures against PG18): most of the difference is this
   documented, pre-existing version-scoping, not a new problem.
5. One additional failure (`test_multi_window`'s "Non-idle top-level %DB
   sums to X%", tolerance 15–125%) reproduced only intermittently across
   repeated runs (92.6% pass, then 141.8% fail with no code change) — timing
   noise consistent with cx33's shared vCPU, not hardened per CLAUDE.md.

None of the above were modified (per CLAUDE.md: never harden a test against
noise, and real tree bugs get reported, not routed around, by an agent whose
task is the box itself). `run_all.sh`'s two structural bugs (1 and 2) look
like quick, well-scoped fixes for a future task; 3–5 need a design decision
(single global test PG version vs the multi-cluster-by-port model this step
introduces) that belongs to the owner or a dedicated `Plan` pass.

**Noise characterization (2026-09-16, cx33 shared vCPU, PG 17, port 5417,
9 pairs, `--characterize`, same methodology as
`docs/SAMPLED_OVERHEAD_GATE.md`'s manual profile):**

| Workload | Pairs | Median | IQR | Observed range |
|---|---:|---:|---:|---:|
| RO (pgbench `-S`) | 9 | +1.16% | 3.44pp | +0.49% … +5.31% |
| RW (pgbench standard) | 9 | +0.61% | 2.82pp | -1.76% … +9.61% |
| 256 high-cardinality shapes | 9 | +2.54% | 3.25pp | +0.80% … +10.82% |

All three IQRs are under the ~5pp acceptance bar. `sampled-overhead: PASS`,
`verdict: pass` in the JSON, zero structural failures, zero timing failures —
not tuned to get there, this is the box's first and only run of this
command. Compare to `docs/SAMPLED_OVERHEAD_GATE.md`'s own numbers: this
shared-vCPU cx33 box's IQRs (2.8–3.4pp) sit between the dedicated Rocky-8 box
(1.4–2.0pp) and the GitHub `ubuntu-latest` hosted runner (5.8–11.0pp) —
meaningfully quieter than a hosted runner, but not as quiet as a dedicated
vCPU. Individual-pair outliers exist (RW pair 1 +9.61%, high-cardinality
pair 2 +10.82%) even though the median/IQR are tight, consistent with
"shared vCPU, occasional noisy-neighbour spike" rather than a systematic
bias.

**Remaining work (follow-up task — do NOT register a runner or edit
`.github/workflows/*` from this branch):**
1. Provision + register the GitHub Actions self-hosted runner (labels
   `self-hosted,linux,x64,ubuntu`, passwordless sudo) — `tests/provision-runner.sh`
   does not do this yet (no `--runner-token` support).
2. Take a Hetzner snapshot named `pgwt-gate-ubuntu-<date>` once the runner is
   registered and green.
3. `.github/workflows/ci.yml`: change `runs-on: ubuntu-latest` to
   `runs-on: [self-hosted, ubuntu]` for `sampled-overhead`, `capture-smoke`,
   `snapshots`. Delete their apt/PGDG install steps (preinstalled); select
   the PG cluster by port (`PGPORT=54${{ matrix.pg }}` — confirmed to work,
   see above) instead of dropping and reinstalling clusters. Add
   `concurrency: { group: gate-box, cancel-in-progress: false }` to those
   jobs so timing runs never overlap.
4. Shrink `sampled-overhead`: with this box's noise profile (see table
   above), decide whether the 21+21 confirmation pairs and 60-minute budget
   are still warranted, or can start smaller.
5. Branch protection: required checks = deterministic jobs + the three gate
   jobs; enable merge queue.
6. Fix the `run_all.sh` "Known gaps" above (at least items 1–2, which are
   pure bugs) before relying on `make box-check` as a clean pass/fail signal
   in CI.

**Acceptance (original, still open for the CI-split part):** three
consecutive green master runs with the gate jobs on the box; `sampled-overhead`
wall time under 15 min.

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

Gate box (cx33) €10.27/mo · ephemeral compute ≈ €0.50 · snapshots ≈ €0.60 ·
optional arm64 cell ≈ €0.10 → **≈ €11–12/mo**.

Current Hetzner price list used above (gross, EU, as of 2026-09-16): cx23
€6.64, cx33 €10.27, cx43 €19.35, ccx13 €52.02, ccx23 €104.05 per month.
