TASK for independent answerers: merge-v1 delta 9

Read only ../CONTRACT.md and ROWS.md with delta 9 applied, plus this task. Do not read EXPECTED.md, earlier answer sets, comparisons, product code, product output, implementation tests or review/probe logs.

Derive the answers independently from the contract. Do not implement or change the contract. Report any contradiction or missing premise explicitly; do not resolve it by consulting the product.

Each execution is independent. Expand referenced rows and abbreviations, including every S(w), into primitive inputs and evaluate after each primitive input. Preserve the specified source positions, semantic timestamps, detection times, counter vectors and successful-publication observation point.

Required executions:

- H11g: AD_D19, cookie-less EXIT_D18, TERM_D18, RA, S(26), finish(40,C0), with H11g's binding and continuing-attachment facts.
- H12a: cookie-bearing EXIT_D18, false TERM_D19 naming that exact EXIT, qualified TERM_D18.
- H12b: cookie-less EXIT_D18, false TERM_D19 naming that exact EXIT, qualified TERM_D18, finish(40,C0).
- H12c: H12b without its qualified TERM_D18.
- H12d: D20@P0, TERM_D19, RA, TERM_D21; both controls name the same prospective EXIT transport identity and no EXIT DATA arrives.
- H12e: cookie-bearing EXIT_D18, false TERM_D19, its identical retransmission, qualified TERM_D18, another identical retransmission of the rejected TERM_D19.
- H13: census {D,H}; EXIT_H18, S(19), D20, S(25), TERM_H18, AD_D6, S(30), with the exact attachment and binding facts in ROWS.md.

Also derive these complete existing rows as controls:

- H6b.
- H6c.
- H11e.
- H11f.

Use each row's full text in ROWS.md; the sequences above do not replace its qualifications. In particular, false timestamp controls must name the actual source position required by the row. Do not manufacture a different EXIT-position contradiction. Do not add unlisted input variants.

What to report for every execution:

1. List the expanded primitive inputs. Identify the exact input establishing each fault, hold, accepted disposition, publication and commitment.

2. Give the complete successful merge stream, including TEXT, DATA, FRONTIER and VOID, in order. Include timestamps, fault detection times, vectors and applicable prior_interval_certified markings. List rejected publication calls separately.

3. Give one combined publication timeline containing the initial plain OPEN, all successful merge publications, every END, every revoked OPEN O!, any additional plain OPEN and every VOID. Annotate required counter checkpoints and commitment instants without counting checkpoints as publication records.

4. After every primitive input, report:
   - Epoch active, suspended or exit-required.
   - Certification latch.
   - Historical admitted fact and admission-proof refutation for each relevant lifetime.
   - Retained final terminal claim, named EXIT identity, claimed time and refutation state.
   - Rejected-control refutation history, distinguished from a retained final claim.
   - The exit-disposition accessor for every named EXIT, including a prospective EXIT not yet delivered: NONE, MATCHED or UNRESOLVED, with the associated timestamp and binding where applicable.
   - Each committed b_i and x_i.
   - Pending lifetime/numeric split and total.
   - Census known or unknown.
   - Startup-gap state and historical/current C_close and P_close where defined.

   Report the accessor after each constituent boundary of S(w), not only after the abbreviation as a whole. Distinguish a recorded refuted MATCHED claim from no recorded disposition. Distinguish a claim accepted under the latch from a committed discharge.

5. At every VOID callback entry, give the same public state and the persisted prefix already visible. State required revocation and cache effects. Evaluate startup availability for [50,60) using the existing reader with otherwise valid supplied coverage, attribution and A2 evidence while retaining all ordinary gaps and losses required by the execution. Do not substitute an in-memory proof for persisted coverage records.

6. Report exact diagnostic increments and storage-counter changes, abandonment consumption, uncovered consumption, payload disposal, epoch invalidation and nonzero-exit requirements. State zero explicitly where relevant. A payload dropped by fault handling is not validated release consumption.

7. For H11g, H12b, H12c and H11f, distinguish pending payload records, unfinished-record accounting, unresolved inventory and an unsatisfied final-disposition obligation. Report the accessor and fault total at finish separately from their values after TERM. Explain which retained obligation, if any, justifies a finish fault. For H11g, report pending payload records after TERM and every subsequent input and unfinished_records at finish.

8. Compare H12d with H6c. Identify whether the first terminal control was rejected on delivery or validly recorded before later refutation, and explain the consequences for the later control. Compare H12a with H11e and H12b with H11f without importing an enable/termination contradiction into a row that contains no ADMIT.

9. For H12e, account separately for each identical retransmission. State whether rejected-control history survives the valid replacement and which duplicate or conflict rule applies first. Do not treat a control retransmission as a duplicate DATA envelope.

10. For H13, report the state immediately before and after TERM_H18 and AD_D6. Explain which completed release evidence the terminal control may use and which held prospective evidence it may not use. Inspect the persisted prefix immediately before END and the prefix including END. Report the emitted GAP_CLOSE endpoint and reserved field, and the reader's startup restriction at the endpoint.

11. Supply at least one targeted mutant for each new row H11g, H12a, H12b, H12c, H12d, H12e and H13. Identify the input and exact assertion distinguishing it from the contract result. Include a mutant losing terminal discharge during D's missing-ADMIT hold, a mutant retaining the disposed EXIT as unfinished after REARM, and a mutant checking a later final-claim conflict before rejected-retransmission identity. Use H6c to guard against suppressing conflicts with claims validly recorded before their refutation. This is a design answer; do not claim any mutant was built or executed.

Separately explain Decision 41's END-publisher result requirement. Distinguish a failed END publication leaving no complete reader-visible END from a committed reader-visible END followed by a separate failure observation. Do not treat the latter as a failed closing attempt or infer that an unrelated later fault automatically revokes its closure.

Conclude with any unresolved contract ambiguity, or state that none was needed to derive these executions. Agreement does not establish implementation or platform qualification.

Delta 9b addendum (a fresh answerer, after one sentence of the contract was clarified): derive only the prior_interval_certified marking of every TRANS/MARKER record published in H13, D03, D02a and Q01, with the contract sentence that decides each marking, and say whether any sentence leaves a marking undecided. Use only the amended contract and ROWS.md.
