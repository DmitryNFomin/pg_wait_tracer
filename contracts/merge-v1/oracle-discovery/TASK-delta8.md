# TASK for independent answerers: merge-v1 delta 8

Read only ../CONTRACT.md and ROWS.md with delta 8 applied, plus this task.
Do not read EXPECTED.md, earlier answer sets, comparisons, product code,
product output, implementation tests or review/probe logs.

Derive the answers independently from the contract. Do not implement or
change the contract. Report a contradiction or missing premise explicitly;
do not resolve it by consulting the product.

Each named execution is independent. Expand referenced rows and all
abbreviations, including S(w), into primitive inputs and evaluate after
each primitive input. Use the source positions, detection times, counter
vectors and successful-publication observation point defined by the
contract and ROWS.md.

## Required executions

Derive these complete rows:

- V4 and amended N14.
- H11d, H11e and H11f.
- G5 and G7.
- H1a, H8a, H10a and H11b.
- H11a, including the EXIT-disposition accessor before and after TERM.

Derive these additional existing executions as over-reach guards:

- R11.
- H1d.
- H6b and H6c.
- T2.
- R7(a) only.
- G8.
- J18.

Also derive these two explicitly specified publication-boundary executions:

1. V4-post-FRONTIER-counter:
   Use the ordinary V4 starting state and
   `AB_D, TERM_D18, EXIT_D18@L, S(19)`.
   Override V4's writer rejection: EXIT_D18 and F18 publish successfully
   with C0, then the counter checkpoint immediately following F18 observes
   Ce at detection time 20, before END publication.
   All publication calls succeed.
   Continue with
   `REARM(21,Ce,fresh valid A/B attachment_proofs), TERM_D18, S(20)`,
   with observations after REARM at detection time 22 and vector Ce.
   No other input, counter change or operation failure occurs.

2. G7-cut-after-END:
   Use G7 through the L boundary of S(19), with all validations,
   publications and counter checkpoints successful.
   Retain the persisted prefix immediately after the complete END becomes
   reader-visible; stop execution before any subsequent in-memory
   bookkeeping and before G7's later ADMIT and confirmation.
   Derive what commitment that prefix represents under Decision 41.
   Do not infer additional persistence beyond the stipulated
   reader-visible record or a reader guarantee beyond its supplied
   ordinary coverage evidence.

Do not expand parameter families or add unlisted variants.

## One over-reach guard per normative sentence

C1–C5 identify the CONTRACT.md edits in delta 8. Sentence numbers count
the sentences in each edit's inserted or replacement text; C4.5 is
retained wording and is listed as a regression guard as well.

| Sentence | Guard execution | Boundary to examine |
|---|---|---|
| C1.1 | R11 | An open startup gap must not prohibit an otherwise valid EXIT release or advancing FRONTIER. |
| C2.1 | H1d | Equal enable and terminal timestamps must not become an enable/termination contradiction. |
| C2.2 | H6c | Rejection of a new control must not erase an earlier recorded terminal claim or suppress a genuine conflict with it. |
| C3.1 | H6b | The MATCHED rule for the enable/termination contradiction must not accept a control rejected for a different contradiction already established on delivery. |
| C3.2 | T2 | Finish must still fault when a cookie-less EXIT actually lacks a final disposition. |
| C4.1 | G5 | END-last must not require a repeated, non-advancing FRONTIER when a later control uses completed release evidence. |
| C4.2 | V4-post-FRONTIER-counter | Successful FRONTIER publication alone must not permit END or discovery commitment before its remaining checkpoint succeeds. |
| C4.3 | G7-cut-after-END | A complete reader-visible END must not become provisional merely because subsequent in-memory bookkeeping did not execute. |
| C4.4 | R7(a) | A later independent fault must not erase an earlier successfully committed, unrefuted closure. |
| C4.5 | G8 | A control using completed EXIT-release evidence must not borrow prospective bounds from another held batch to close the gap. |
| C4.6 | H8a | Removing failed-attempt rollback must not remove the revoked OPEN required for contradiction of an already committed bound. |
| C4.7 | J18 | END-last must not weaken refusal after rejected supporting EXIT publication. |
| C5.1 | H8a | The stated persistence limit must not excuse omission or postponement of a successful revoked OPEN before the revoking VOID callback. |

For C5.1 additionally explain the persisted-prefix limit without inventing
another oracle row: distinguish H8a's normal successful revocation from a
process stopping, or its retraction write failing, before any revoked OPEN
becomes persistent. State what remains in the file and what the existing
reader can establish from that prefix under its other coverage conditions.
Do not claim that an unpersisted in-memory revocation changes those bytes.

## What to report for every execution

Provide:

1. The expanded primitive inputs and the exact input establishing each
   fault, hold, publication and commitment.

2. The complete successful merge stream: TEXT, DATA, FRONTIER and VOID,
   in order, with timestamps, detection times, vectors and applicable
   prior_interval_certified markings.
   List rejected publication calls separately; never count them as
   successful outputs.

3. One combined publication timeline containing the initial plain OPEN,
   every successful TEXT/DATA/FRONTIER publication, every END, every
   revoked OPEN O!, any additional plain OPEN, and every VOID.
   Annotate counter-checkpoint positions and commitment instants without
   treating checkpoints as publication records.
   A collection of separate merge and gap lists is insufficient.

4. State after every primitive input:
   epoch active/suspended/exit-required, certification latch,
   historical admitted fact and admission-proof refutation,
   retained terminal claim and its refutation,
   named EXIT's final disposition/accessor value,
   each committed b_i and x_i,
   pending lifetime/numeric split,
   census known/unknown,
   startup-gap state and historical/current C_close and P_close where
   defined.
   Distinguish no recorded disposition from a recorded MATCHED disposition
   whose proof is refuted.

5. At every VOID callback entry:
   the same public state, the persisted prefix already visible,
   required revocation/cache effects, and the existing reader's
   startup-coverage result.
   Evaluate [50,60) with otherwise valid coverage, attribution and A2
   evidence, while retaining all ordinary gaps and losses required by
   the execution.
   Do not substitute an in-memory bound for persisted gap records.

6. Exact diagnostic increments and storage-counter changes;
   abandoned-record and uncovered-record consumption;
   payload disposal versus validated consumption;
   epoch invalidation and nonzero-exit requirements.
   State zero explicitly where relevant.

7. For H11e, state which terminal claim remains after the false TERM and
   why the later TERM is or is not accepted as the final disposition.
   For H11f, report the disposition and fault total at finish separately
   from the state after TERM.
   For H11a and H11d, identify exactly when MATCHED becomes observable.

8. For V4, N14 and V4-post-FRONTIER-counter, inspect every successful
   publication prefix of the closing attempt, including the last prefix
   before the failure and the prefix at VOID entry.
   For successful closure executions, inspect the prefix immediately
   before END and the prefix including END.
   Explain whether any prefix contains a complete END for a closure whose
   acceptance checks have not yet succeeded.
   Report the persisted END endpoint and reserved field whenever END
   exists.

9. At least one targeted mutant per new row H11d–H11f, identifying the
   input and exact assertion that distinguishes it from the contract
   result; also identify mutants for END-before-FRONTIER,
   END-before-the-post-FRONTIER-checkpoint, and suppression of a
   successfully published FRONTIER after later END rejection.
   This is a design answer: do not claim a mutant was built or executed.

Conclude with any unresolved contract ambiguity, or state that none was
needed to derive these executions. Agreement is not implementation or
platform qualification.
