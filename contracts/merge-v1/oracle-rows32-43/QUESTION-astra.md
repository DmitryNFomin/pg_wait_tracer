# Task (Astra, contract author): draft the smallest merge-v1 contract increment that admits backends the daemon attaches directly at fork, without fabricating INIT

Read #871 comments 6858126984 (M1: 18,872 direct attaches in 80,603 connections under `pgbench -C` on EL8/PG13) and
6858197492 (both advisers' positions, including Fable's proposal and your own constraints). Use
`gh api repos/DmitryNFomin/pg_wait_tracer/issues/871/comments --paginate`. Read contracts/merge-v1/ on origin/master
(CONTRACT.md, DECISIONS, examples.json, README), src/core/merge.c and pgwt_merge.h, and the direct-attach code at
src/backend.c:900-950. Code: `git show origin/master:<path>`.

Fable's proposal: amend DECISION 25a so that a valid FORK record plus a lifetime cookie allocated in the fork-seeded
BPF state is the creation proof. INIT then only times the attach. State created by any path other than on_fork stays
`unknown_evidence`.

Your earlier position: a real attachment protocol with REARM certifies only subsequent coverage, and avoiding routine
VOID needs a separately reviewed contract/API increment.

Write, as a contract revision draft (do not edit the repository; write your answer only):
1. Decide whether a sound increment exists, and if so, the exact text change (§ and DECISION numbers) with the
   argument for why it cannot certify a wrong number. Show the failure cases it must still VOID: pid reuse, a state
   created outside on_fork, a FORK record lost, and an attach before the cookie is visible.
2. New examples.json rows (inputs and expected outputs) that pin the change, including at least one row that must
   stay VOID.
3. What the producer (BPF and daemon) must supply, and whether the core API (`pgwt_merge.h`) changes.
4. If you conclude no sound increment exists, say so plainly and state the cost: routine epoch suspension under
   connection churn.
At most 40 lines. Mark [Verified] (file:line) and [Inferred]. Fable will challenge the draft independently.
# Task (Astra, contract author): revision r2 of your merge-v1 direct-attach increment

Your draft is /private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/27cc85b4-040d-47d2-ba40-e0c59ac54ec1/scratchpad/m871/astra-25a.md.
Fable's independent challenge (ACCEPT-WITH-CHANGES, six points) is fable-25a-challenge.md in the same folder. The
original task is q-25a.md there. Read all three, then re-read the cited code (`git show origin/master:<path>`,
origin/master 4e634818).

Write revision r2 as the full replacement text: DECISION 25, 25a, 25b, 25c and any §3/§4 sentences, the new
examples.json rows, the producer obligations, the API change and the adapter truth table. Give a disposition for each
of the six points: ACCEPT, or REJECT with a concrete reason. A rejection must show why Fable's failure case cannot
occur, or why it cannot produce a wrong number. Every row must have exact inputs and expected outputs in the §5
shorthand, including the uncertified-first-record marking if you accept point 1. State plainly what remains VOID
until the producer work lands. At most 60 lines. Mark [Verified] (file:line) and [Inferred]. Do not edit the
repository.
