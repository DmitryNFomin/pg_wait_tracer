# Ambiguities (none blocking; each resolved by the more literal reading)
1. CODEC.md 4/"Types 0 and 2 have both sample-weight fields zero" and header sample_period_ns: for type 0/2 sample_period_ns written 0 (spec only says weights zero; "sample_period_ns" unstated for non-sample types). Alt: copy file nominal period (also 0 here, so same bytes).
2. Sec 4 line 261: type-2 hint timestamps MUST be zero; type 0 first/last = row times (5). Not ambiguous, noted.
3. Sec 8 digest: defined only for a predecessor; the file has no successor, so digest is computed as if this file were the predecessor with previous_data_end=D=646. Alt: none defined.
4. Sec 5: ATTACH_OK "method reread" -> 1 (stated). seed/state "A" -> mapped id 1. LIFE start_known=0 with start=0 allowed (start<=t).
5. Sec 6 type-0 query_id/cpu_ns for the example were given as 0 by the input; cpu_ns uleb 00.
