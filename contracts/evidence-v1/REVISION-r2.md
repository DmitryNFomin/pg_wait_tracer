# Evidence v1 revision r2
Scope: contract and scenario inputs only; freeze is logical replay semantics, not implemented capture.
1. ACCEPT — §§2,4: unconditional prefix retention conflicts with merge-v1's retroactive epoch invalidation.
2. ACCEPT — §5: optional LIFE retention makes unqualified-capture projection nondeterministic.
3. ACCEPT — §4: lifecycle loss and identity recovery need an explicit replay rule.
4. ACCEPT — §2: storage diagnostic strings must be distinguished from merge-v1's closed reason enum.
5. ACCEPT — §3: FRONTIER completeness must cover every source, including all participating CPUs.
6. ACCEPT — §§3,4: evidence/TRANS append order is not guaranteed by merge-v1's DATA ranks.
§§2,4: VOID now requires invalid_from; null preserves certificates, a time revokes earlier certification inclusively.
§2: persist the merge out-of-band invalidation conservatively from the earliest affected epoch proof, or 0.
§4: roll F back to a surviving published frontier; retain publication high-water H across rollback and recovery.
§4: revoke across prior epochs and frozen runs, reconstruct retained portions, and retract dependent cached results.
§4: lifecycle-sensitive slots are 0,1,2,10,12,14; unknown vectors/baselines and retroactive invalidation also taint identity.
§4: restored counters or ATTACH_OK cannot repair tainted identity; require a fresh observed fork and fresh attachment.
§5: retain intact, consistent, unambiguous LIFE independently of exact-capture qualification; otherwise LIFE_INVALID.
§2: reason remains opaque storage metadata; replay uses fields and counters, not diagnostic spellings.
§3: timestamp-resolve anchor and END dependencies before FRONTIER; preserve causal TRANS tie order.
§§1,3: barrier, coherent reread and END proofs remain obligations; no change to frozen merge-v1 is required.
§7 inputs: add invalid_from=null to existing VOIDs; add 31 for late-DATA revocation, 32 for delayed TRANS after END, and 33 for TRANS before ATTACH_OK.
Validation: all 33 inputs parse, VOID fields are present, and fixtures contain only inputs; no answer files created.
