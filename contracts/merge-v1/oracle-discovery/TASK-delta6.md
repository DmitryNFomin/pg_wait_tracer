# Task: independent answer set for 15 discovery-admission oracle rows (the unresolved disposition of a cookie-less EXIT that matches only a numeric observation)

You are one of two independent answerers. Do not look for any other answer or "expected" file; work only from the files in this directory:
- CONTRACT-merge-v1-sections1-5.md: merge-v1 sections 1-5 (its §5 defines sources, positions, strict boundaries S(w), release evaluation and the output notation such as D12[U]/[T], F15, VOID20[C0,reason]).
- AMENDMENT.md: merge-v1 §6, DECISIONs 30-45 (discovery admission), as amended, including its clarifications.
- CONTRACT-coverage-a2-v1.md: the A2 coverage contract (for rows that mention A2).
- ROWS.md: the oracle rows, inputs only, with the notation additions.

Answer ONLY these row IDs, in this order:
U1 U2 U3 U4 U5 U6 U7 U8 U9 U10 U11 U12 U13 U14 U15 (and, in the re-check after the settlement: U4 U5 U9 U15 O1)

For each, derive the outputs the amended contract requires: released DATA with [U]/[T] marks and order, FRONTIERs, VOID/FAULT with detection time, counter-vector label and reason, whether the EXIT is published, held or consumed as uncovered (and the uncovered count), the pending count P=L+Q (lifetimes plus numbers), census completeness, the startup gap state (open, or closed with its PERSISTED end), the permanent certification latch (set or not), and any discharge.

Format: one block per row ID: `<ID>: <outputs in the contract's notation>` then at most three lines `why: <DECISION numbers / contract sentences>`. If the amendment does not determine an answer, write `<ID>: UNDETERMINED` and one line saying exactly what is missing. Do not guess silently. Write the complete answer set to the output file named in your instructions. Nothing else.


---

## Second packet of the same delta (rows from the exhaustive enumeration's contract gaps)

# Task: independent answer set for 12 discovery-admission oracle rows (control ordering; one resolving proof per lifetime; conflicting terminal claims; closure from committed evidence only)

You are one of two independent answerers. Do not look for any other answer or "expected" file; work only from the files in this directory:
- CONTRACT-merge-v1-sections1-5.md: merge-v1 sections 1-5 (its §5 defines sources, positions, strict boundaries S(w), release evaluation, TEXT dependencies and holds, and the output notation such as D12[U]/[T], F15, VOID20[C0,reason]).
- AMENDMENT.md: merge-v1 §6, DECISIONs 30-45 (discovery admission), as amended, including its clarifications.
- CONTRACT-coverage-a2-v1.md: the A2 coverage contract (for rows that mention A2).
- ROWS.md: the oracle rows, inputs only, with the notation additions.

Answer ONLY these row IDs, in this order:
G1 G2 G2b G3 G4 G4b G5 G6 G7 G8 N6 P3

Several rows name more than one variant ("Also execute ..."): answer every variant.

For each, derive the outputs the amended contract requires: released DATA with [U]/[T] marks and order, FRONTIERs, VOID/FAULT with detection time, counter-vector label and reason, which control faults and whether it is recorded, the committed lower bound b and terminal discharge x per lifetime (or their absence), any revocation and revoked startup OPEN, the startup gap state (open, or closed with its PERSISTED end), the pending count P=L+Q, census completeness, the permanent certification latch (set or not), uncovered and abandoned counts where relevant, and for G8 what is held and whether a reader window after the last boundary is complete.

Format: one block per row ID: `<ID>: <outputs in the contract's notation>` then at most three lines `why: <DECISION numbers / contract sentences>`. If the amendment does not determine an answer, write `<ID>: UNDETERMINED` and one line saying exactly what is missing. Do not guess silently. Write the complete answer set to the output file named in your instructions. Nothing else.
