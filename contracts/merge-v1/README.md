# merge-v1 — per-CPU transport merge stage (issue #871)

Frozen 2026-11-11. An implementation change never edits anything in this directory; a contract
change is its own reviewed change and invalidates every result that depended on it
(docs/REWORK_PLAN_2026-11-11.md, section 3).

- `CONTRACT.md` — the normative contract (v3, sections 2-5) with its change list and 20 worked
  examples with expected outputs. Written by Astra (gpt-6-astra) in a fresh session.
- `DESIGN-v2.md` — the v2 draft it came from: disposition of the challenge (section 0), evidence
  (section 1), the numbered prototype run list with gate-box hour estimates (section 6) and what is
  deferred to the full fix (section 7). Sections 2-5 there are superseded by `CONTRACT.md`.
- `CHALLENGE-fable.md` — Fable's challenge of v1. Two of its claims were corrected by an
  independent audit; the corrections are stated in DESIGN-v2.md section 0.
- `oracle-v2/`, `oracle-v3/` — what two independent answer authors (Claude Sonnet and gpt-6.1-sol)
  were given (`CONTRACT.md` with the expected column blank, `TASK.md`) and what they answered.
  They had no code access.

Oracle results:

| Round | Rows matching the expected output, both authors | Ambiguities raised |
|---|---|---|
| v2 | 10 of 20 | 10 rows (detection time, implicit boundaries, release granularity, sequence baseline, repeated VOID, writer rejection, breach exit) |
| v3 | 20 of 20 on order, frontier values, d, C and exit | VOID reason labels undefined (Sonnet, 5 rows); N-mode applicability of rows 11-12 (Sonnet); none (sol) |

After v3 the lead added the closed list of VOID reasons to section 4 (marked in the text). The
v3 labels the authors guessed differ only in wording from that list.
