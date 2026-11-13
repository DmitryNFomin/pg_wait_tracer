# Evidence v1 revision r3 — fault handling (contract only)
Scope: six previously unexercised branches; scenarios 01–33 and explicit VOID behavior unchanged.
Advice 1 ACCEPT — retaining a validated prefix avoids rejecting unrelated usable evidence.
Advice 2 ACCEPT — no replay past an unreadable record; recovery would invent missing evidence.
Advice 3 ACCEPT — capture-local chain termination preserves certified prefixes and independent captures.
Advice 4 ACCEPT — readable counter changes supply their own vector and t, so §4 recovery is computable.
Q1 ACCEPT (§5) — malformed/unknown records stop at their physical boundary; readable counter differences alone become a fully specified VOID.
Q2 ACCEPT (§5) — overlapping/out-of-lifetime ATTACH_OK or END before anchor stops at that offending record, without synthetic loss fields.
Q3 ACCEPT (§5) — END without exactly one addressable run stops at END after dependency resolution; tied-transition withholding stays intact.
Q4 ACCEPT (§5) — FRONTIER t<=H stops at that FRONTIER; rollback does not reset publication high-water.
Q5 ACCEPT (§§1,5,6,7) — the first descriptor binds capture to boot; a conflicting descriptor stops that ID, preserving its earlier prefix.
Q6 ACCEPT (§§4,5) — state/known-entry mismatch stops at TRANS; no later record can repair that capture.
§5 common stop: keep earlier surviving certificates and verified LIFE; discard tentative run suffixes and every later record/file of that ID.
§5 counter exception: consume FRONTIER/ATTACH_OK as VOID(t,vector,null); no certificate/run; reset/decrease or null means unknown, with §4 lifecycle recovery.
§§5,6: resolve delayed dependencies before semantic fault decisions; stop boundaries are physical positions, not guessed detection times.
§7: keep the existing target/query shape; permit deliberately malformed logical records inside intact fixture JSON.
New scenarios 34–37: malformed_record_{prefix,suffix}, unknown_kind_{prefix,suffix}.
New scenarios 38–41: frontier_counter_change_{prefix,suffix}, attach_counter_change_{prefix,suffix}.
New scenarios 42–45: overlapping_attachment_{prefix,suffix}, end_without_run_{prefix,suffix}.
New scenarios 46–49: nonadvancing_frontier_{prefix,suffix}, run_state_mismatch_{prefix,suffix}.
New scenarios 50–53: capture_boot_conflict_{prefix,suffix}, malformed_before_frontier_{prefix,suffix}.
New scenarios 54–56: independent_capture_after_fault, run_duration_mismatch, attachment_before_life.
New scenarios 57–58: nonadvancing_after_rollback, counter_loss_fresh_fork.
Validation: all 58 JSON inputs parse; 01–33 are byte-identical; new fixtures contain no expected answers; original inputs manually checked against changed branches.

## Revision b
1 ACCEPT — overlap must exclude runs ended by END or frozen by VOID; §§4/5 now say so explicitly.
2 ACCEPT — §1 now treats a successor loss_baseline as a restatement of running state, with mismatch stopping at the descriptor.
3 ACCEPT — §4 ignores lifecycle-blocked ATTACH_OK after schema/counter/time checks; §5 ignores TRANS outside all run extents after dependency resolution.
4 ACCEPT — 57 now targets fresh fork 15 after FRONTIER 9, exposing both ignored faults and incorrectly reset H through later certification.
5 ACCEPT — 59–62 isolate continued replay after readable FRONTIER/ATTACH_OK loss, a blocked attachment, and an unattached transition.
6 ACCEPT — §5 rule 1 explicitly includes TRANS and its six required fields; malformed records cannot use the ignore exception.
7 ACCEPT — §4 stops on recovery t below latest VOID.t (63); §5 ignores schema-valid END for a lifecycle-tainted generation (64).
8 ACCEPT — retract the earlier claim of a sufficient changed-branch audit: 12/13/26 do traverse the overlap wording, as does 14; byte identity alone was insufficient.
Sentence audit, §1 successor-baseline addition: 22 restates the unchanged zero vector; 23 stops at the missing file before this check; 24 starts another capture, not a successor; all others have no successor.
Sentence audit, §5 rule 1 replacement: 01–27,31–33 have valid required logical fields, so naming TRANS introduces no fault; 28–30 remain on the legacy path.
Sentence audit, §4 overlap reference and §5 rule 2 replacement: 01–07,09–11,15–19,22–25,31–33 have no competing same-generation run at attachment (02 uses distinct forks; 24 distinct captures); 08 has no attach; 27 ignores certificates; 28–30 are legacy.
Same overlap sentences, repeat paths: 12/13 freeze the prior run before attach 16; 14 freezes PID 202's run before attach 14; 20/21 END the prior run before attach 15; 26 freezes the prior run and blocks attach 16 by lifecycle taint, so none gains an overlap fault or loses its former projection.
Sentence audit, §4 blocked-ATTACH addition: 01–07,09–25,31–33 are untainted at each attach; 26's attach 16 still creates no run, leaving the surviving prefix unchanged; 08 has no attach, 27 ignores certificates, 28–30 are legacy.
Sentence audit, §4 recovery-time addition: 12/13/26 have attach 16 >= latest VOID 14; 14 has attach 14 >= VOID 12; 01–11,15–25,27–33 have no recovery attach, so none enters the new stop branch.
Sentence audit, §5 rule 6 addition: 01–07,09,11–25,32/33 retain their run assignment/discard rules (12/13's TRANS 11 suffix is already discarded by VOID; 32's delayed TRANS 11 is within END 12; 33's TRANS 8 resolves to its later-appended anchor); 26's TRANS 18 cannot extend its frozen run; 27 has no exact certificates; 08/10/31 have no TRANS and 28–30 are legacy.
Sentence audit, §5 rule 3 replacement: 18/19/20/21/32 have untainted ENDs with one addressable run, preserving their pending/certified endpoints; all other 01–33 inputs have no END, so none enters the tainted-END exception.
Inputs: changed 57_nonadvancing_after_rollback; added 59_frontier_counter_recovery, 60_attach_counter_recovery, 61_blocked_attach_fresh_generation, 62_transition_outside_runs, 63_recovery_before_void_time, 64_end_after_blocked_attach (all .json).
Validation: all 64 input JSON files parse; container fields remain input-only; 59–64 logical field shapes checked; 01–33 hashes unchanged; sentence audit and new discriminating paths reviewed manually, without repository code, product output, expected answers, or an evaluator run.
