---
name: pr-ready
description: Take the current agent/ branch through the definition of done — make check, make box-check, UI gallery if web/ changed, fresh reviewer agents — and produce the PR body. Use when implementation on a branch is finished.
---
Run the definition of done from CLAUDE.md for the current branch and open the
PR (or print the body if `gh` is unavailable). Do not skip steps; report
honestly which ones could not run and why.

1. `make check` (full). Fix failures; re-run.
2. `make box-check` (add `OS=el8` if kernel/libbpf/backend-layout code changed;
   `PG=13` additionally if PG13 attribution changed). Fix; re-run. If no box is
   configured (`$PGWT_BOX` unset), stop and say so — a PR without a live run
   is not ready.
3. If `git diff master...HEAD --name-only | grep -q '^web/'`: `make ui-gallery`.
4. Spawn the `reviewer` agent (and `ui-reviewer` when step 3 ran) with a fresh
   context via the Agent tool. Address every blocker they raise, then re-run
   the affected steps and re-review.
5. Commit, push (the push guard needs a fresh stamp), and open the PR with the
   reviewer's body. Attach the gallery `index.html` path and the box-check log
   name. If `gh` is missing, print the body and the `gh pr create` command.
