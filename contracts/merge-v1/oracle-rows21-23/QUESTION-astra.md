You are Astra, design adviser for pg_wait_tracer. One written question; answer once and stop. Read-only.

The frozen merge contract is contracts/merge-v1/CONTRACT.md in the current directory (read §2-§5
fully; §4 includes a closed VOID reason list added after the last oracle round). Its 20 examples
are in §5. An implementation passes all 20, but three required "broken version" checks are not
caught because no example exercises them:
- M6: a weaker later boundary retracts an earlier one (w[s] taken as latest instead of max).
  Needs e.g. a source boundary 20, then a weaker boundary 10 for the same source, then DATA at 15.
- M11: Q mode accepts a sequence gap. Needs e.g. baseline next_q=1 with only q=2 eligible.
- M12: a FRONTIER is emitted while the epoch is suspended. Needs e.g. a suspended start state
  (cutoffs 6, last frontier absent or F5), then inputs that would otherwise advance W, and/or an
  unchanged-counter CHECK.

Write exactly three new rows, 21-23, in the same notation and with the same §5 conventions, each
fully specified (start state, inputs with d and C where faults are observed, modes N/Q where they
differ), each with its expected output, such that the correct contract behaviour differs from the
broken behaviour in the produced output. For each row state in one line which broken behaviour it
separates and what the broken output would be. Do not change any rule text. If the contract text
does not determine a row's answer, say so instead of choosing.

Output: the three table rows (markdown, same columns as §5), then the three one-line explanations.
