# Independent answers for revision r3b: cases 17, 31 and 32-50 (2026-11-14)

Two answer authors worked in isolated directories:
- gpt-6.1-sol via `codex exec --sandbox read-only`;
- Claude Sonnet as an agent told to read only its directory.

Their inputs were CODEC.md, CONTRACT.md, REVISION-r3.md, REDELIVERY-blank.md (as REDELIVERY.md) and CASES-blank.md.
Neither saw an expected value, Astra's answer, Fable's challenge or any code.

**Result: all three sets agree on every value for all 21 cases**, in records, normalized codec stops, replay effect
and aggregate status. Cell-by-cell table: COMPARE.md.

Differences in label or detail only, counted as agreement:
- **Case 40:** Sonnet names the SC stop's file "V", where the reference and sol use A, i.e. (b,a,0). V keeps A's
  header.
- **Case 47:** Sonnet writes the stop's file as "A2t0" instead of A2, and omits "no replay for a".
- **Case 34:** Sonnet does not spell out LX.
- **Cases 36 and 50:** Sonnet does not mention the lifecycle uncertainty at 30.
- Sonnet also re-derived cases 1-30. It differs from the reference only in labels: a suppressed diagnostic's
  delivery name in case 5, and the stop file written Nb/Ni in cases 27/28.

Points the authors marked undetermined. None changes a value; the implementation's oracle fixes the notation:
- **sol:**
  - the exact reason string of the VOID consumed from w4 (cases 45-46);
  - the literal diagnostic text and serialization;
  - query metrics, since the cases supply no query.
- **Sonnet:**
  - one stop per file, as in case 29 (RF kept, BC a diagnostic);
  - whether PU after an earlier cutoff is a normalized stop or a diagnostic (cases 9, 19, 20, 23, 24, 27, 28, 33).
    The reference lists it as a stop;
  - the notation for a replay stop that is not a baseline mismatch (case 30);
  - the canonical witness order without hashes;
  - the field layout of PU results;
  - the root-missing wording in case 24;
  - whether lifecycle-uncertainty effects on pids 2..5 are observable (cases 36, 45, 50).

**Costs:**
- reference evaluator that found #913/#914: sol 101,293 tokens;
- advisers: Astra 45,929 and Fable 42,552 tokens;
- Astra: r3 76,563, r3b 79,170, and the write task 101,907 tokens;
- Fable's challenge: 83,577 tokens;
- answer authors: sol 58,897 and Sonnet 118,967 tokens;
- comparison: Sonnet 84,529 tokens.
