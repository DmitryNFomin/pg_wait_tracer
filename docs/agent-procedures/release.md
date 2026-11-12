# Release checks and evidence

## RULE 2 — never ship a known defect. "Good enough" is not a state.

Do not ship known wrong numbers, divergent paths, mismatched percentage bases or
disagreeing views, even on a green branch. Fix correctly; disclosure or caveats do
not substitute. Only an explicit owner exclusion, in his words on the issue,
permits shipping; the lead cannot grant or infer it, including from "better than master".
Treat inability to report matching numbers as a product defect. Two views of one
quantity must agree; fix the disagreement instead of asking which wrong number to display.
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#matching-numbers`)

### Release tag criteria

**`tag-blocker`** — an open defect that makes the release tag wrong. Any one of:

- (a) the product reports a number a user would act on that is wrong, or two views of one
  quantity disagree;
- (b) a **UX defect** — the interface is wrong, misleading, or unusable on a path a user
  takes: a control that does nothing, a value rendered as a raw internal token or
  `null`, a view that never recovers from an error, a label that contradicts the data;
- (c) a documented guarantee is false;
- (d) a gate we rely on for (a)–(c) cannot detect its own failure case.

**Not** a tag-blocker: our own process tooling and dashboards; test coverage for a defect
that has not shipped; anything whose fix is a documentation change.

## The three commands

| Command | Runs where / evidence | When |
|---|---|---|
| `make check` (`check-fast` = seconds) | Mac: Node builders, Go bridge, py_compile, Playwright UI/chaos vs `tests/mock_server.py` | Before every push; push guard enforces it |
| `make box-check [OS=el8\|el9\|ubuntu] [PG=13\|16\|17\|18]` | x86 Linux over ssh (`$PGWT_BOX`, `$PGWT_BOX_EL8`…): build, C units, synthetic data, protocol drift, real-PG capture (`tests/run_all.sh --require-live`, `tests/ui_live_smoke.sh`, `tests/results/ui_live/summary.json`) | Before every PR; use `OS=el8` with `$PGWT_BOX_EL8` for kernel/libbpf/layout changes |
| `make box-check EPHEMERAL=1 [PG=…] [KEEP=1] [PGWT_SNAPSHOT_IMAGE=<id>]` | Private Hetzner VM from a `pgwt=gate-snapshot` image, same live tier; automatic deletion unless `KEEP=1` prints the delete command | Same Hetzner machine class as the gate box, so the same per-OS evidence; `EPHEMERAL=1` resolves `pgwt=gate-snapshot,os=$OS` (ubuntu alone also accepts the legacy, pre-os-label snapshot) — availability is a property of the snapshot inventory, not a hardcoded OS allow-list, so kernel/libbpf/layout work can run `OS=el8` this way too once an el8 image carries that label. `PGWT_SNAPSHOT_IMAGE=<id>` pins to an exact image id instead, bypassing the label lookup — for reproducing a past run, or proving a *candidate* image works end to end before it is promoted to the live label (promotion is a separate, deliberate step: labelling a new OS's image `pgwt=gate-snapshot` before every caller's `--snapshot-os` filter has merged makes it `latest` for every OTHER agent's unscoped `EPHEMERAL=1`, OS=ubuntu by default); see the **Persistent boxes FIRST** paragraph below for when to use this mode |
| `make ui-gallery [BASE=ref]` | Mac: all UI snapshot cells → `tests/results/ui_gallery/index.html` + `summary.json` | Before every PR touching `web/` |

(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#ubuntu-vm-evidence`)

**Persistent boxes FIRST; throwaway VMs only as exceptions.** Before creating any
VM, check both `pgwt-gate` and `pgwt-gate-2`; use a free box. Use `EPHEMERAL=1`
only when both are busy with CI or another agent holding the flock; report which
box was busy and why.
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#persistent-boxes-first`)

Logs: `tests/results/box-check-*.log` (`box-check-ephemeral-*.log` for `EPHEMERAL=1`),
`tests/results/ui_gallery/`, `tests/results/ui_live/summary.json`.

`make hetzner-sweep` deletes `pgwt=ephemeral` VMs older than 6h (automatic at
every `box-check` start). The script never touches `pgwt-gate` or `pgwt-gate-2`
by name, regardless of label. It refuses cutoffs below 1h unless `FORCE_ALL=1`;
regardless of cutoff, it never sweeps VMs younger than a few minutes or with
unknown age (missing/unparseable `created=`), which it never treats as infinitely
old (issue #162). Remove your own VM by id:
`tests/hetzner-vm.sh delete <id>`, never by sweep cutoff.
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#vm-cleanup`)

## Definition of done

Declare a PR ready only with ALL evidence in its body:

0. **Start `box-check` FIRST; run `make check` while it runs** (`make check`: median ~7 min, n=3; well under the 40-min box run, so serial execution wastes the check's wall time).
   (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#parallel-checks`)
1. Full `make check` (not `--fast`) passed on the final tree.
2. `make box-check` passed; paste the run_all summary (last ~20 lines), including
   the live-UI-smoke verdict (`tests/results/ui_live/summary.json`).
   CI-infrastructure-only branches (no `src/` or test-assertion changes) may
   substitute real `gh workflow run` gate-box evidence; state this in the PR body.
   After the first passing `box-check`, docs-only review rounds or `tests/`-only
   rounds touching no live-tier test may skip re-runs; name each round and why in
   the PR body. Never exempt `src/`, `web/`, `tests/run_all.sh`,
   `tests/unit_tests.list`, or any live-tier test; if unsure, run `box-check`.
   `.github/workflows/ci.yml` narrows tiers by changed files; CI still runs in
   full on every PR, and merge gates are unchanged.
3. If `web/static/` or `tests/web_snapshots/` changed, run `make ui-gallery`;
   list `summary.json` counts and every `changed`/`added`/`removed` cell with
   a one-line justification each.
   (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#ui-branch-scope`)
4. Get fresh reviewer approval (`.claude/agents/reviewer.md`; `ui-reviewer.md`
   for UI), or list open questions under "Design questions for the owner".
   Give the owner only those questions to resolve.

Use `/pr-ready` to run this sequence.
