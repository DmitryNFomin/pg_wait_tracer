# Challenge 2: Astra's interface specification for the discovery integration (before I freeze it)

Read-only: edit no file except your answer. At most 60 lines. Mark every claim VERIFIED (file:line read) or INFERRED. scratchpad = SP

## Material
- The specification (267 lines): scratchpad/a2go/integ/astra-spec.md. The request it answers, with the points both advisers already settled: scratchpad/a2go/integ/q-spec.md. Earlier: astra-design.md, fable-challenge.md (yours, a previous session), astra-settle.md.
- Worktrees: wt-pc (producer 6e6034c7), wt-discm (merge 748218d7). Master (346ccc95) has the A2 coverage record codec and writers (src/coverage_record.h, src/event_writer.c: pgwt_writer_coverage_untraced_lifetime, ..._a2_bucket) and contracts/coverage-a2-v1/.

## Questions
1. **Section A against section B.** Is every field section B consumes present in section A with a usable type, clock domain and "unknown" encoding, and is anything in A consumed by nothing (dead weight for the producer round)? List mismatches only.
2. **Section D, the persisted form.** Astra chooses a capture-bound sidecar file with a new contract directory, a BOOT capability bit and a separate persistence unit touching event_writer, event_reader, coverage_record.h and coverage.c. Is there a smaller sound form: for example one new record kind in the existing COVERAGE block written through the existing checked writer (what would the contract change be: contracts/coverage-a2-v1 or none), or reuse of an existing record? Compare on: bytes and files touched, what an old reader does, revocation, rotation and retention, and how many contract commits it needs. Recommend one.
3. **The interim sentence.** Astra says that until the persistence unit lands, a PostgreSQL 13 lifetime that needs the no-wait-word route stays pending and the startup gap stays open, live and on reopen. On PG13 the logger, stats collector and archiver live for the whole capture. State plainly what a PG13 user would then see in every window (is every whole-window number withheld for the whole capture?), citing where an open startup gap is turned into a verdict. If so, the persistence unit is on the critical path for "EL8 4.18 and PG13 must work" and cannot be deferred: confirm or refute.
4. **Open point 1 (exact start for a rebound pre-existing PID).** The merge wants an exact monotonic start; the producer's rebind has /proc tick observations. Which real situations reach REBIND (how common), what happens to them under "retain pending" (does the startup gap stay open for the capture?), and what is the smallest sound way to complete R?
5. **Section E counts** and the other open points in G: say where you disagree with the recommended answer, and whether any open point is truly the owner's (money, machines, release, exclusions) rather than the advisers'.
6. **Sizing.** With this specification, how many implementation rounds do U-P, U-I and the persistence unit each plausibly need (each round is one fresh implementer with a written brief, about 5-7 hours, followed by one independent review), and which parts can run at the same time without touching the same files?

Write your answer to scratchpad/a2go/integ/fable-challenge2.md and return it as your final message.
