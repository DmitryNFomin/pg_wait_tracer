# Task: independent answer set for 18 discovery-admission oracle rows (forward-only certification latch; order-independent refutation; the PostgreSQL 13 no-slot helpers)

You are one of two independent answerers. Do not look for any other answer or "expected" file; work only from the files in this directory:
- CONTRACT-merge-v1-sections1-5.md: merge-v1 sections 1-5 (its §5 defines sources, positions, strict boundaries S(w), release evaluation and the output notation such as D12[U]/[T], F15, VOID20[C0,reason]).
- AMENDMENT.md: merge-v1 §6, DECISIONs 30-45 (discovery admission), as amended, including its clarifications.
- CONTRACT-coverage-a2-v1.md: the A2 coverage contract (for rows that mention A2).
- ROWS.md: the oracle rows, inputs only, with the notation additions.

Answer ONLY these row IDs, in this order:
R1 R2 R3 R4 R5 R6 R7 R8 R9 R10 R11 R12 N1 N10 N14 P1 P2 P3

Rows R1-R12 are parameterised: answer every parameter combination the row names (group combinations that have the same answer and say which they are).

For each, derive the outputs the amended contract requires: released DATA with [U]/[T] marks and order, FRONTIERs, VOID/FAULT with detection time, counter-vector label and reason, the committed lower bound b and terminal discharge x (or their absence), the startup gap state (open, or closed with its PERSISTED end, per DECISIONs 41-42), any revoked startup OPEN, the pending count P=L+Q, census completeness, the permanent certification latch (set or not), the count of abandoned or uncovered records where relevant, and for N1, N10, P1, P3 what is listed or counted for the process.

Format: one block per row ID: `<ID>: <outputs in the contract's notation>` then at most three lines `why: <DECISION numbers / contract sentences>`. If the amendment does not determine an answer, write `<ID>: UNDETERMINED` and one line saying exactly what is missing. Do not guess silently. Write the complete answer set to the output file named in your instructions. Nothing else.
