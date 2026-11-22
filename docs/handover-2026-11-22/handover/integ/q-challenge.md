# Challenge: Astra's design for the discovery-admission integration

Read-only: edit no file except your answer. Answer in at most 60 lines. Mark every claim VERIFIED (file:line read) or INFERRED. scratchpad = SP

## Material
- The request Astra answered: scratchpad/a2go/integ/q-design.md (it names the worktrees: wt-discm = merge side 748218d7, wt-pc = producer 6e6034c7, wt-tickfix = transport 0a73d7d0, and the two reviews).
- Astra's design (151 lines): scratchpad/a2go/integ/astra-design.md.
- Contract: contracts/merge-v1/CONTRACT.md §6 (DECISIONs 30-45) in wt-discm.

## What I need challenged
Astra's central claim is that a translation-only integration is unsound: the producer's handoff does not carry enough to build the merge's proof masks, so the work must be TWO units, the first of which extends the producer (complete J/R/G witnesses, binding tokens, sticky fault summary, physical seed facts) and adds a discovery-only internal transport envelope extension carrying emission-time attachment generation, before the daemon wiring can start. That roughly doubles the remaining discovery work, so it has to be right.

1. **Is the gap real?** For each merge proof-mask fact and each control payload field in Astra's two tables, say whether the producer at 6e6034c7 already exposes evidence sufficient to set it soundly (cite where), exposes it only as a summary flag that proves less than the mask claims, or does not expose it at all. Give counts and name the ones in the last two groups. Check Astra's VERIFIED claims that carry the argument: producer record vs merge control (discovery_admission.h:42-108 vs core/pgwt_discovery.h:12-127), numeric resolution records (discovery_producer.c:910-924), no discovery origin in the decoder (percpu_transport.c:100-134), terminal identity vs exit_position, daemon start_ts after the scan (daemon.c:1560-1564, 1724, 1854-1869 vs merge_capture.c:802-810), census reset on state.no_wait_word (merge_capture.c:907-914).
2. **The transport envelope extension.** Is emission-time attachment generation provenance really needed to satisfy the contract (which DECISION requires final-generation membership of the first fresh record, and what does the merge check at merge.c:498-503, 872)? Is there a sound alternative that needs no wire change (for example: generations are separated by completed producer fences the handoff already records, so a record's generation follows from its position relative to those fences in the same source)? If the alternative is sound, state the exact condition; if not, give the schedule that breaks it.
3. **Smallest sound shape.** What is the minimum that must be added to the producer for the integration to be sound, and what in Astra's unit 1 is hardening that can follow or be dropped? Is two units right, or one, or a different split? Name files per unit.
4. **What Astra missed or got wrong**, including anything that contradicts the contract or the two reviews' "notes for the integration unit", and any failure or shutdown path that could raise or hang.
5. **Order.** Given a transport branch (wt-tickfix) that is not on master and a producer fix round now running on discovery_producer.c, what is the order of landing that wastes the least work?

Write your answer to scratchpad/a2go/integ/fable-challenge.md and return it as your final message.
