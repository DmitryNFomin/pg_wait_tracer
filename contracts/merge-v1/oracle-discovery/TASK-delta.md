# Task: independent answer set for the changed and new discovery-admission oracle rows

You are one of two independent answerers. Do not look for any other answer or "expected" file; work only from the files in this directory:
- CONTRACT-merge-v1-sections1-5.md: merge-v1 sections 1-5 (its §5 defines sources, positions, strict boundaries S(w), release evaluation and the output notation such as D12[U]/[T], F15, VOID20[C0,reason]).
- AMENDMENT.md: merge-v1 §6, DECISIONs 30-45 (discovery admission), as amended, including its clarifications.
- CONTRACT-coverage-a2-v1.md: the A2 coverage contract (for rows that mention A2).
- ROWS.md: the oracle rows, inputs only, with the notation additions.

Answer ONLY these row IDs, in this order:
D01 D02c D03 D09 D10 Q02 Q03 Q04 Q04b Q05 Q06 D14a D14b D14c A01 A02 A03 A04 A05 A06 A07 A08 A09 A10 A11 A12 A13 A14 A15 A16 A17 A18 A19 A20 

For each, derive the outputs the amended contract requires: released DATA with [U]/[T] marks and order, FRONTIERs, VOID/FAULT with detection time, counter-vector label and reason, the startup gap state (open, or closed with its PERSISTED end, per DECISIONs 41-42, and the pending count), the lower bound b_i and its route, the count of abandoned records where relevant, numeric answers where the row asks for them (waits, counts, CPU, and whether a requested window is available), and any A2 status asked.

Format: one block per row ID: `<ID>: <outputs in the contract's notation>` then at most two lines `why: <DECISION numbers / contract lines>`. If the amendment does not determine an answer, write `<ID>: UNDETERMINED` and one line saying exactly what is missing. Do not guess silently. Write the complete answer set to the output file named in your instructions. Nothing else.
