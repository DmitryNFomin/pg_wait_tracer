# A2 r3 answer-set comparison

Sets: A = answers-codex.txt, B = answers-claude.txt, D = ANSWERS.txt (designer, not independent).
Line references: `M:` = in/merge-v1-CONTRACT-master.md, `A:` = in/merge-v1.txt, `C:` = in/coverage-a2-v1.txt, `R:` = in/ROWS.txt (file line numbers).

## Normalisation

- ROWS.txt lists 77 lettered variants: 35 merge (M44..M63e; there is no M47) and 42 coverage (C1..C24).
- A writes `M44[N]`/`M44[Q]` (70 merge lines, one per mode) and `C1[default]`, `C18a[raw]` and so on (42 lines). For every M variant, A's N and Q lines are textually identical (checked with a script).
- B writes one line per variant (`M44 | N and Q: ...`, `C1 | ...`): 35 + 42 lines.
- D writes one line per variant and says "Merge answers apply identically in modes N and Q": 35 + 42 lines.
- No set leaves out a variant, and no set repeats an id. Each set's id list was diffed against the ROWS id list, and all three match exactly.
- Every M variant was compared in both modes. All three sets give the same answer for N and Q, so each M variant counts once below.

## Counts

**77 variants in total. A=B on all compared fields: 73. A=B=D: 73.**
(Counting only status, reason and note values, without the C18e UI-string field: A=B 74 and A=B=D 74.)
D agrees with B on every variant. All 35 merge variants agree in all three sets: output sequence, VOID time, vector and reason, epoch state, and the uncertified M55 prefix.

## DISAGREEMENTS

### D1. C4b: certificate N=0,U=1 with one valid pair Q(100,106); W=[0,1000)
- A: Unavailable(untraced_invalid), note=1/6 ms.
- B: Unavailable(untraced_invalid), note values withheld.
- D: Unavailable(untraced_invalid), note values withheld.
- Deciding lines: C:L39 (0<=U_k<=N_k; U_k must agree with the ledger), C:L71 (invalid covers contradictory certificates), C:L82-L83 (the note is computed when "the required omission inventory and its identities and timestamps are complete and consistent"), C:L91 (note returned "even when" the status is limit, unqualified or base failure), C:L93 (withhold if "the required omission inventory is invalid, incomplete or pending"), C:L126 (A2_BUCKET "certifies finality, not a sample").
- Classification: **(ii) CONTRACT AMBIGUITY.** The contract never defines "omission inventory". If it means the retained pairs only, the pair set is complete and consistent here: U=1 matches the one pair, and only N is wrong. That gives A's 1/6. If it includes the cohort certificate, which is what certifies that the pair set is complete (L126), then an invalid certificate makes the inventory invalid. That gives B's and D's "withheld". L91's list does not name invalid, but "even when" does not make the list closed.
- Minimal clarifying sentence (also decides D2 and D3): "The required omission inventory consists of the retained pairs and the final A2_BUCKET certificate of every required cohort; it is complete and consistent only when each such certificate exists, is valid, and agrees with its pairs." **Yields B/D (withhold).** The opposite sentence ("the omission inventory is the set of retained pairs; certificate faults affect status only") yields A.

### D2. C14c: C1 inputs, but the J_0 certificate was removed by retention; no live unfinished-reconciliation record
- A: Unavailable(untraced_incomplete), note=2/12 ms.
- B: Unavailable(untraced_incomplete), note values withheld.
- D: Unavailable(untraced_incomplete), note values withheld.
- Deciding lines: C:L72 (incomplete covers missing "certificates or other finality evidence"), C:L77 (no live ledger means incomplete), C:L83, C:L93 (withhold when incomplete; "do not label a known subset as the complete upper bound"), C:L125-L127 (the certificate certifies finality; WATERMARK and SEAL cannot replace it), C:L131 (retention). The fixture default R:L139 says reconciliation is complete, and A relied on it.
- Classification: **(ii) CONTRACT AMBIGUITY.** All three agree on the status reason. They differ only on whether a missing certificate makes the *omission inventory* incomplete. A reads the R:L139 stipulation of completed reconciliation as the inventory still being complete. B and D read it as the reader being unable to establish that completeness without the certificate, so the two pairs might be a known subset. The text leans toward B/D (L72 treats the certificate as finality evidence, and L126 makes it the finality authority), but it does not say explicitly that the note's completeness test includes it.
- Clarifying sentence: the same one as D1. **Yields B/D (withhold).**

### D3. C24: certificate N=2,U=1; Q(100,106); complete retained FORK evidence shows 3 births, the other two attached
- A: Unavailable(untraced_invalid), note=1/6 ms.
- B: Unavailable(untraced_invalid), note values withheld.
- D: Unavailable(untraced_invalid), note values withheld.
- Deciding lines: C:L39 (N_k must agree with complete retained FORK evidence), C:L71, C:L83, C:L91, C:L93.
- Classification: **(ii) CONTRACT AMBIGUITY.** This has the same root as D1. Here the case for A is stronger, because independent FORK evidence proves that the birth set and the dispositions are complete, so the pair set is demonstrably complete even though the certificate's N is wrong. The text still does not say whether a contradictory certificate invalidates the inventory.
- Clarifying sentence: the same one as D1. **Yields B/D (withhold).** If the owner instead wants A's answer here but not in C14c, the sentence would need a second clause: "...or, when the certificate is invalid only in N_k, when complete retained FORK evidence independently establishes the cohort's births and dispositions." That clause would yield A for C24 and C4b, and B/D for C14c.

### D4. C18e (UI route): the UI string only; status and note values agree (3/18 ms, untraced_limit)
- A: "3 backends not traced; at most 18 backend-ms omitted."
- B: "3 backends not traced; at most 18 ms backend-time omitted."
- D: gives no UI string.
- Deciding line: C:L96-L97. The template is the literal “N backends not traced; at most X backend-time omitted.”
- Classification: **(i) AUTHOR ERROR (A)**, for the fixed words. A replaced the literal "backend-time" with "backend-ms", so its string does not match the template. A related **(ii) ambiguity** remains: the contract does not say how X is formatted (unit, ns or ms, precision). B chose "18 ms".
- Clarifying sentence: "X is untraced_backend_ns_upper rendered as a duration with its unit (for example '18 ms'); the remaining words are literal." **Yields B.**

### Unscored differences (diagnostics, not status, reason or note)
- A lists receive_ns-fork_ns for every omission, including omissions that do not overlap W or lie outside the required cohorts (C5a, C5c, C17, C19b-c, C22a-b). It gives "delay=(unavailable,1)" for C14b and "delay=8" for the invalid C21 declaration. B lists receive delays for most rows but leaves them out in some (C14b, C21, C22a-b) and adds cohort N/U diagnostics instead. C:L21-L22 requires a labelled diagnostic but does not say for which omissions. B's ambiguity 10 raises this point; see the next section.

## AMBIGUITIES listed by the authors, merged

| # | Ambiguity (source) | Status |
|---|---|---|
| 1 | Invalid certificate vs invalid omission inventory, C4b/C24 (A1, B7) | Disagreement D1/D3. **Needs clarification.** |
| 2 | Missing certificate with completed reconciliation: is the note computable? C14c (A2, B9) | Disagreement D2. **Needs clarification** (same sentence as D1). |
| 3 | M52c: EXIT (30,2) without its own FORK, fault or hold? (A3, B5) | No disagreement; all give VOID20[C0,unknown_evidence]. **Decided** by A:L2 (missing FORK is unknown evidence), A:L5 (fork_ns<=exit_ns, so any FORK for (30,2) has t<=12<13 and is lost once L passes 13) and M:L110 (lost dependency causes VOID). |
| 4 | M50c/M53a-c: a genuine INIT does not bypass contradictory stored-INIT evidence (A4, B2 second half) | No disagreement. **Decided** by A:L11 (a delivered INIT must agree exactly) and A:L7-L8 (c)-(d). |
| 5 | M46: FORK+INIT is also an ordinary DECISION 25 creation proof; does A2 still emit UNTRACED? (B2) | No disagreement; all emit UNTRACED. The text supports this through A:L32 ("release FORK and any genuine INIT normally, then UNTRACED") and C:L10 ("Count every admitted never-attached lifetime"). It does not address how A2 interacts with M:L54 and row 25. **Needs clarification (minor):** "A FORK+INIT-created lifetime that meets A2(a)-(e) is an A2 lifetime and receives UNTRACED; the DECISION 25 creation route does not displace it." That sentence yields the current answers. |
| 6 | When FAULT(unknown_evidence) is detected for refused M rows: EXIT arrival, reconciliation or batch validation (B3) | No disagreement. The output is the same here because W=0 until the last boundary and d=20 throughout. **Needs clarification** only for a future row that has an eligible release before detection. A:L26 and A:L2 ("before further trusted release") give no instant. |
| 7 | M52b: whether S(13) boundaries after CHECK(Cx) are C0 observations, which would be a Cx-to-C0 reset and further VOIDs (B4) | No disagreement; all give a single VOID20[Cx,counter_change]. The fixture conflicts with itself: R:L26 ("every unstated observation [is] C0") against R:L37 ("no unstated ... counter changes"). M54b (after an unreadable CHECK) has the same exposure. **Needs clarification (fixture):** "After a stated CHECK, every later observation retains that vector (as in M54a)." That sentence yields the current answers. |
| 8 | M55: whether the uncertified FORK prefix is part of the output sequence (B6) | No disagreement. **Decided** by R:L61 ("including any prefix before a fault"), M:L116 and A:L35. |
| 9 | C14b: incomplete (missing pair member) vs invalid (certificate U=2 against one complete pair; invalid has higher precedence) (B8) | No disagreement; all give untraced_incomplete and withhold. Precedence C:L71 > C:L72 makes this a real conflict: C:L72 names "missing required retained pairs" specifically, while C:L39 and C:L117 could make the U mismatch a contradiction. **Needs clarification:** "A certificate/ledger mismatch caused only by a pair member missing from retention is untraced_incomplete, not untraced_invalid." That sentence yields the current answers. |
| 10 | Qualification identity: an applicable qualification is stipulated without an identifier (A5) | No disagreement; both handle it symbolically. A fixture gap against C:L21, not a contract ambiguity. Optional fixture sentence: "The stipulated qualification identity is the symbol QSTIP." |
| 11 | How many omissions get the receive_ns-fork_ns diagnostic (B10) | This is the unscored difference above. **Needs clarification** if diagnostics are tested: "Return receive_ns-fork_ns for each omitted lifetime in a required cohort." |
| 12 | Row-id format `<row id>[<variant>]` (B1) | Notation only. **Decided** by this normalisation; no contract change. |
| 13 | Format of X in the UI string (new, from D4) | Disagreement D4. **Needs clarification** (sentence in D4). |
