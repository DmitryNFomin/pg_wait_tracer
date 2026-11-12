You are an independent expected-answer author. Use ONLY CONTRACT.md in this directory.
Do not read any other file, repository, source code or prior answers; do not search the filesystem.

For each of the 20 example rows at the end of CONTRACT.md, write the expected output exactly as the
contract defines it: the ordered list of released outputs (TEXT, DATA, FRONTIER F<n>, VOID<d>), or
`hold`, and whether the daemon exits. Where the contract gives two modes (N = sequence-free,
Q = sequence-bearing), answer both. Show the arithmetic for every frontier value (W, F=W-1).

Then list every place where the contract is ambiguous or does not determine the answer, with the
row number and the two readings. Do not resolve an ambiguity by guessing; mark the row AMBIGUOUS.

Output: markdown, one section per row: `## Row N`, `Output: ...`, `Exit: yes/no`, `Reasoning: ...`
(at most 6 lines), then `## Ambiguities`.
