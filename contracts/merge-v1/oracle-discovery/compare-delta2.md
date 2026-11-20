# Oracle delta 2 comparison (sol vs opus vs EXPECTED): rows T1-T4, V1-V4

Two independent answer sets, each written from the packet alone (merge-v1 sections 1-5, section 6 as amended, coverage-a2-v1, ROWS.md, TASK-delta2.md), neither seeing EXPECTED.md or the other.

Counts: the two answer sets agree with each other on all 8 rows. They agree with EXPECTED as first drafted on 5 rows (T2, T4, V1, V2, and T3 apart from the mark below) and add detail EXPECTED lacked on 3; EXPECTED and ROWS were completed to match, and no outcome (VOID, discharge, gap state, pending count) differed anywhere.

| ID | class | difference and settlement |
|---|---|---|
| T1 | ANSWERS-AGREE, EXPECTED completed | Both give `F18` at the disposition and `A20[T] → F20` at S(21); the draft wrote `A20` without its `[T]` mark and did not say F18 precedes S(21). EXPECTED now states both. |
| T2 | ALL-AGREE | none |
| T3 | ALL-AGREE | inherits T1's `[T]` mark |
| T4 | ALL-AGREE, contract sentence added | Both apply a disposition that arrives before its EXIT to the EXIT it names (no hold, uncovered +1, F18). One answerer noted the contract did not say so; DECISION 40 now does ("A disposition delivered before the EXIT it names applies when that EXIT is consumed, which is then not held."). |
| V1 | ALL-AGREE | none |
| V2 | ALL-AGREE | none |
| V3 | ANSWERS-AGREE, EXPECTED and ROWS completed | Both add `FRONTIER(19,Cw)` after REARM and S(20), which the draft omitted (V1 had it). The row said "then REARM" without arguments; one answerer assumed `REARM(21,Cw,attachment_proofs)`, observations at 22. ROWS now states exactly that and EXPECTED carries `→ F19`. |
| V4 | ANSWERS-AGREE, EXPECTED and ROWS completed | As V3. |

The answer files were written against the rows as first drafted; the three completions above change no input semantics the answerers did not already assume.
