# Settle: the challenge to your integration design

Read-only: edit no file. Answer in at most 45 lines as your final message. Mark every claim VERIFIED (file:line read) or INFERRED. scratchpad = SP

You (a previous session) wrote scratchpad/a2go/integ/astra-design.md in answer to scratchpad/a2go/integ/q-design.md. The second adviser challenged it: scratchpad/a2go/integ/fable-challenge.md (45 lines; read all of it). Worktrees: wt-pc = producer 6e6034c7, wt-discm = merge 748218d7.

For each point below answer ACCEPT, or REJECT with the concrete schedule or contract sentence that breaks it. Do not restate your design.

1. **No wire or BPF change.** The challenge's rule: membership_proved(r) := r.cookie==g AND r.seq > B[r.cpu][r.dest], where B is the per-CPU acknowledgement vector of the completed fence taken after discovery_active[pid]=G and before enable, retained in the handoff (today it is overwritten by the post-enable fence), with its conditions (i)-(v). Is it exact generation membership under DECISIONs 36 and 37(a)? Give a breaking schedule if not. Also its claims that baselines_refreshed is false for every marker and that exclusive TEXT attribution follows from the emitter's cookie.
2. **The producer delta** (challenge section 3, "Must add"): is that list sufficient for a sound coordinator? Name anything from your unit 1 that it drops and that soundness (not hardening) needs.
3. **Three units** (U-P producer delta; U-M the two merge nits; U-I coordinator and daemon wiring, with src/open_snapshot.c added and src/core/merge.c kept out): accept or correct the file lists.
4. **Frontier liveness** (challenge section 4, first item): when producer reconciliation is revoked for the rest of the capture, issue TERMINAL_UNRESOLVED for held cookie-less EXITs and ABANDON for installed never-admitted identities at once. Is that rule consistent with DECISIONs 40 and 45? State the exact trigger and what must be fenced first.
5. **Census re-read without revision machinery, final refresh before helpers stop, every capture error turned into a recorded outcome**: accept or correct.
6. **Route-c durable role publication.** The challenge says the merge cannot commit a no-wait-word bound without the publish callback (merge_capture.c:399-400), that the callback's persisted form is a format decision in files master changed after the branch base, and that without it a PostgreSQL 13 capture never closes its startup gap. What is the smallest persisted form that satisfies the contract, does it need a trace format or contract change, and is there a sound in-memory-only interim (the bound commits for the live capture, a reopened file shows the startup gap as open)?
7. **"All 108 rows end to end" overclaims**: which rows are merge-only by construction (the coordinator can never emit their control order)? Give the count and the list.
