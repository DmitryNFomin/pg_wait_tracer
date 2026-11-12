# Hooks and local setup

## Claude hooks

Registered in `.claude/settings.json` (`*` means no matcher):

| Event | Matcher | Command |
|---|---|---|
| `PreToolUse` | `Bash` | `scripts/hooks/push-guard.sh` |
| `SubagentStart` | `*` | `scripts/lead/agent-start-hook.sh` |
| `SubagentStop` | `*` | `scripts/lead/agent-stop-hook.sh` |

- `PreToolUse`: screens Bash command text; can refuse a push or other command
  matching the push-guard bypass/tampering denylist.
- `SubagentStart`: records the agent ID in the shared active-agent ledger;
  always exits 0 and cannot refuse agent creation.
- `SubagentStop` ledger hook: removes the agent ID from that ledger;
  always exits 0 and never refuses a stop.


## Local setup (one-time)

Activate the pre-push stamp guard per clone (covers linked worktrees;
`make check` activates it if unset). Before setting `core.hooksPath`, back up
or move personal `.git/hooks/` hooks to `scripts/git-hooks/`.
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#hook-setup`)

```
git config core.hooksPath scripts/git-hooks
```

```
brew install node go
python3 -m pip install --user playwright==1.60.0 websockets pillow numpy
python3 -m playwright install chromium
export PGWT_BOX=root@<gate-box-ip>        # optional: PGWT_BOX_EL8, PGWT_BOX_EL9
```

`.python-version` is untracked via `.git/info/exclude`; new worktrees get the
pyenv pin only from the global default or explicit `PYENV_VERSION`, never checkout.
