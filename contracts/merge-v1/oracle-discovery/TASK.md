# Task: independent answer set for the discovery-admission oracle rows

You are one of two independent answerers. Do not look for any other answer or "expected" file; work only from the four files in this directory:
- CONTRACT-merge-v1.md: the current merge-v1 contract (its §5 defines sources, positions, strict boundaries S(w), release evaluation, output notation such as D12[U]/[T], F15, VOID20[C0,reason]).
- CONTRACT-coverage-a2-v1.md: the A2 coverage contract (for rows that mention A2).
- AMENDMENT.md: the proposed amendment, DECISIONs 30-44 (discovery admission), with its reconciliation notes.
- ROWS.md: the oracle rows, inputs only, with the notation additions.

For EVERY row ID in ROWS.md, derive the outputs the amended contract requires: the released DATA with [U]/[T] certification marks, FRONTIERs, VOID/FAULT with detection time, counter vector and reason, the startup gap state (open, or closed at which C, with the pending count), the lower bound b_i and which route established it, numeric answers where the row asks for them (waits, counts, CPU), and any A2 status asked.

Format: one block per row ID, in ROWS.md order:
`<ID>: <outputs in the contract's notation>` then at most two lines `why: <the DECISION numbers / contract lines that decide it>`.
If a row is ambiguous or the amendment does not determine the answer, write `<ID>: UNDETERMINED` and one line saying exactly what is missing. Do not guess silently.
Write the complete answer set to the output file named in your instructions. Nothing else.
