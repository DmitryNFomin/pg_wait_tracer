# Adviser and failure procedure

## RULE 1 — advisers first, the owner last. Never ask the owner what an adviser can answer.

Send these questions to **both advisers first**, Fable (`.claude/agents/adviser.md`)
and Astra, in parallel: a design or contract decision; a failure mode not seen
before; anything that would otherwise become a question to the owner. Routine
readiness, ordering and numbers read straight from a test log need no adviser:
the contracts, the differential and the mutants decide those (owner, 2026-11-11).
Each adviser gets one written question in a fresh session, answers once and
stops; never resume an adviser conversation. Ask the owner only if
the advisers disagree and the lead cannot adjudicate on evidence; the lead disagrees with both;
or the decision is the owner's (money, machines, release timing, RULE 2 exclusions).
Tell the owner what both advisers said and where the lead disagrees.
A sol 6.1 coder may consult an Astra adviser directly, but the adviser MUST be
a separate agent session: a session never advises itself, and no session
reviews or advises on code it wrote.
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#advisers-before-owner`)

Try first; on failure, tell both advisers what was attempted and what happened.
Report failures to the owner only afterwards, with attempts, outcomes, options and
a recommendation; never a bare "this does not work, FYI". Solve it first otherwise.
If both advisers find no way, report what X, Y and Z they considered and each cost.
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#failure-reports`)
