---
name: pr-ready
description: Run the mechanical definition of done, obtain fresh reviewer approval, and open the PR for the current agent/ branch. Use when implementation on a branch is finished.
---
Run the definition of done from CLAUDE.md for the current branch and open the
PR (or print the body if `gh` is unavailable). Do not skip steps; report
honestly which ones could not run and why.

From inside the worktree, run
`scripts/pr-ready.sh [OS=...] [PG=...] [EPHEMERAL=1] [KEEP=1]` for the
mechanical gates. The script owns their ordering and verification. Choose
the required OS/PG coverage from CLAUDE.md, including additional runs where
required. Fix failures and re-run the script; a refusal or failed gate means
the branch is not ready. Preserve its output and the log paths. `OS=`, `PG=`,
`EPHEMERAL=`, and `KEEP=` arguments go only to `box-check`. With `EPHEMERAL=1`,
`PGWT_BOX` is not required. The gallery runs when tracked or untracked changes
affect `web/static/` or `tests/web_snapshots/` relative to the merge base.

The caller handles the judgement steps:

1. Spawn the `reviewer` agent (`.claude/agents/reviewer.md`) with a fresh
   context via the Agent tool, plus `ui-reviewer` when `web/static/` or
   `tests/web_snapshots/` changed (as reported by the script). Supply the code
   and gate evidence. Have them
   check the items the script marks UNCHECKED against CLAUDE.md, including
   box log commit/tree provenance, the run_all summary and live-UI-smoke
   verdict, and gallery counts and a justification for every changed,
   added or removed cell. The reviewer writes the PR body with this evidence
   and any "Design questions for the owner".
2. Address every blocker, re-run the script on the resulting tree, and obtain
   the required fresh review. Follow CLAUDE.md's review-round rules.
3. Commit, push (the push guard needs a fresh stamp), and open the PR with the
   reviewer's body. Attach the gallery `index.html` path and the box-check log
   name. If `gh` is missing, print the body and the `gh pr create` command.
