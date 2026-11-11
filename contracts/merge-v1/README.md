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
- `examples.json` — the 20 examples of `CONTRACT.md` section 5 as a machine-readable fixture
  (inputs expanded to primitives, expected outputs copied from the Expected column). Transcribed by
  a separate Sonnet agent that read only `CONTRACT.md`; per-row `transcription_notes` record every
  judgement. Tests read expected outputs from this file, never from code.
- `oracle-v2/`, `oracle-v3/` — what two independent answer authors (Claude Sonnet and gpt-6.1-sol)
  were given (`CONTRACT.md` with the expected column blank, `TASK.md`) and what they answered.
  They had no code access.

Oracle results:

| Round | Rows matching the expected output, both authors | Ambiguities raised |
|---|---|---|
| v2 | 10 of 20 | 10 rows (detection time, implicit boundaries, release granularity, sequence baseline, repeated VOID, writer rejection, breach exit) |
| v3 | 20 of 20 on order, frontier values, d, C and exit | VOID reason labels undefined (Sonnet, 5 rows); N-mode applicability of rows 11-12 (Sonnet); none (sol) |

Rows 21-23 (added 2026-11-11): the first implementation passed rows 1-20 but three required
broken versions (M6 boundary retraction, M11 sequence gap accepted, M12 frontier while suspended)
were not caught because no row exercised them. Astra wrote the three rows from the contract only
(`oracle-rows21-23/QUESTION-astra.md`, `ANSWER-astra.md`); Sonnet and gpt-6.1-sol answered them
independently and matched on every output. No rule text changed.

Rows 24-31 and DECISIONS 25/27/28 with amendments 25a/27a/28a (added 2026-11-11): the P1 code
review (Opus) reproduced behaviours the text governs but no row exercised: late DATA while
suspended, a backend created after the epoch starts, a dependency arriving after the record that
needs it, and REARM (restart after loss) on a suspended and on an active epoch. Astra wrote rows
24-28 from the contract alone and proposed DECISIONS 25, 27, 28 where the text did not decide
(`oracle-rows24-28/`). Fable challenged the decisions; the lead wrote the amendments 25a, 27a,
28a from that challenge and rows 29-31 that exercise them (`oracle-rows29-31/`). Sonnet and
gpt-6.1-sol matched every expected output of rows 24-31 independently, and rows 24-28 also match
the reviewer's own expected outputs. New fixture fields: `REARM` inputs, identity `D`, and
per-mode `epoch_invalid` and `statuses` expectations.

After v3 the lead added the closed list of VOID reasons to section 4 (marked in the text). The
v3 labels the authors guessed differ only in wording from that list.
