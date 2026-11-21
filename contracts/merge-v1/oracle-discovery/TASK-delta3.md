# Task: independent answer set for the 38 new discovery-admission oracle rows (no-wait-word route; unbound enumerated PIDs; the exit filter)

You are one of two independent answerers. Do not look for any other answer or "expected" file; work only from the files in this directory:
- CONTRACT-merge-v1-sections1-5.md: merge-v1 sections 1-5 (its §5 defines sources, positions, strict boundaries S(w), release evaluation and the output notation such as D12[U]/[T], F15, VOID20[C0,reason]).
- AMENDMENT.md: merge-v1 §6, DECISIONs 30-45 (discovery admission), as amended, including its clarifications.
- CONTRACT-coverage-a2-v1.md: the A2 coverage contract (for rows that mention A2).
- ROWS.md: the oracle rows, inputs only, with the notation additions.

Answer ONLY these row IDs, in this order:
N1 N2 N3 N4 N5 N6 N7 N8 N9 N10 N11 N12 N13 N14 N15 N16 J1 J2 J3 J4 J5 J6 J7 J8 J9 J10 J11 J12 J13 J14 J15 J16 J17 J18 J19 J20 J21 J22 

For each, derive the outputs the amended contract requires: released DATA with [U]/[T] marks and order, FRONTIERs, VOID/FAULT with detection time, counter-vector label and reason, the startup gap state (open, or closed with its PERSISTED end, per DECISIONs 41-42, and the pending count), the lower bound b_i and its route, the count of abandoned records where relevant, numeric answers where the row asks for them (waits, counts, CPU, and whether a requested window is available), and any A2 status asked.

Format: one block per row ID: `<ID>: <outputs in the contract's notation>` then at most two lines `why: <DECISION numbers / contract lines>`. If the amendment does not determine an answer, write `<ID>: UNDETERMINED` and one line saying exactly what is missing. Do not guess silently. Write the complete answer set to the output file named in your instructions. Nothing else.
