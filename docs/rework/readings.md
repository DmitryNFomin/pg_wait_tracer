# Readings behind the process rules (2026-11-11)

Published practice from Anthropic and OpenAI on building software with AI coding agents, read on
2026-11-11. Each rule added to `docs/REWORK_PLAN_2026-11-11.md` section 3 after that date names the
reading it comes from.

| Reading | Publisher | Date | URL |
|---|---|---|---|
| Harness design for long-running application development | Anthropic Engineering | 2026-03-24 | https://www.anthropic.com/engineering/harness-design-long-running-apps |
| Demystifying evals for AI agents | Anthropic Engineering | 2026-01-09 | https://www.anthropic.com/engineering/demystifying-evals-for-ai-agents |
| Building a C compiler with a team of parallel Claudes | Anthropic Engineering | 2026-02-05 | https://www.anthropic.com/engineering/building-c-compiler |
| Effective harnesses for long-running agents | Anthropic Engineering | 2025-11-26 | https://www.anthropic.com/engineering/effective-harnesses-for-long-running-agents |
| Best practices for Claude Code | Anthropic documentation | current | https://code.claude.com/docs/en/best-practices |
| Redesigning source control at Anthropic | Anthropic Engineering | 2026-10-13 | https://www.anthropic.com/engineering/redesigning-source-control |
| Best practices (Codex) | OpenAI documentation | undated | https://learn.chatgpt.com/guides/best-practices |

OpenAI's "Harness engineering" (https://openai.com/index/harness-engineering/) returned HTTP 403 and
was not read.

Rules adopted from these readings, with the owner's approval:

| Rule (plan section 3) | Reading |
|---|---|
| 8. Reviewers report defects, not opinions | Best practices for Claude Code; Redesigning source control |
| 9. Two strikes, then rewrite the brief | Best practices for Claude Code |
| 10. Status only a test can change | Effective harnesses for long-running agents |
| 11. Every check earns its place | Harness design for long-running application development |

Also adopted 2026-11-11 (owner approval), changing `CLAUDE.md`:

| Change | Reading |
|---|---|
| Advisers only for design or contract decisions, new failure modes and owner questions; one question per fresh session, never resumed | Best practices for Claude Code ("a fresh session still costs less"); Codex best practices ("one chat per coherent outcome") |
| The lead starts only authorised work; no "fill idle capacity every turn" | Best practices for Claude Code (instructions to keep going "are more likely to cause over-work than fix a stall") |
