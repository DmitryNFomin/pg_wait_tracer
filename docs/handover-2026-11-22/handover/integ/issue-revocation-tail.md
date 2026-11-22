Not reproduced: this is both design advisers' reading of the reader code, found while designing the discovery-admission integration. It becomes reachable when discovery admission lands (the merge side that publishes boot-wide revocations is on branch agent/988-discovery-merge, not on master).

**The schedule**

1. A capture closes its startup gap at time 6. The watermark advances to 100; the last watermark's floor is 80.
2. Something later revokes coverage from the boot start. Two paths publish such a boot-wide reopened gap: a process found after the census that should have been listed (oracle row D10), and a contradiction of a committed no-wait-word bound (DECISION 43, row N5).
3. The reopened gap record is written at the end of the current file. The tail of that file is then lost: a crash before the block is committed, a truncated copy, a damaged block.
4. On reopen, the reader's hole for a damaged or missing tail starts at the last watermark's floor (src/coverage.c, the rule also used for missing files). It taints [80, infinity). A window such as [20, 30) stays `complete`, although the capture was revoked from its boot start.

**Why it matters**

`complete` over [20, 30) is then a claim the capture itself withdrew. It needs two unlikely things at once (a retroactive revocation, and the loss of exactly the tail that records it), so it is rare; it is still a wrong `complete`.

**Wanted**

A reader rule on the gap record, decided once for every retroactive revocation rather than per feature. Candidates the advisers named: treat a capture whose revocation history is uncertain (a tail not closed by its certificate, per amendment A6 on issue 947) as revoked from the boot start when the capture could hold a retroactive revocation; or make the revocation durable somewhere a lost tail cannot take it. The first over-withholds after every unclean stop, so the choice needs a measurement of how often tails are unclean in ordinary use.

**Classification**

Open question for the advisers when the reader rule is designed: whether this blocks the tag (criterion c, a documented guarantee) or is an accepted, documented limit. Not labelled tag-blocker yet.

Sources: lead scratchpad `a2go/integ/astra-settle2.md` point 1, `a2go/integ/astra-noslot.md`, `a2go/integ/fable-noslot.md` items 13-14.

Refs #988
