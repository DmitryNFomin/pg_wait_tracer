# Review of 959f827b (agent/947-coverage-server, a579619c..959f827b): READY-with-nits

Reviewer: Claude (Opus), read-only delta review, 2026-11-21 ~03:50Z. Tree ba8830803a2a7ba607b70c6c10e80511e4c69b99. This is the same tree as /private/tmp/u2b-r12-evidence/final-tree.
Scratch work: SP/rv959/ (tools/, build/{old,new}/, corpus/, *-old.txt/*-new.txt).

## What changed
- The trace reader takes the file's size once per open (fstat, after the .meta count is read). It then reads through a 256 KiB pread buffer instead of one stdio seek and read per record. The cold coverage scan over 24 hourly files drops from about 434k lseek to 0. strace, from the implementer: 1,671 read and 2,405 pread64.
- coverage_trace.c's append path (scan_growth) and scan_file use the same buffered read.
- New tests: the reader snapshot C test (tests/coverage_reader_snapshot.c), a deterministic I/O-count bound (io_count), and a per-record-seek mutant.
- tests/test_capture_smoke.py: the pure-CPU straddle arm may count toward exit 94 only when its own window has a capture_loss and its own drop counters moved. There are 24 new controls for this.

## Evidence
- Check stamp: not run (brief: read-only, no make check).
- Box-check / full tiers, from the implementer and not rerun: they ran on tree d91995dd, NOT on the final tree ba883080.
  - EL8: `Executed: 239 (passed 227, failed 2, refused 4, known-failing 6 (2 xpass))`.
  - Ubuntu: `Executed: 239 (passed 237, failed 1, ...)`.
  - Live UI smoke was PASS on both.
  - Failures: test_block_type_sites.py on both, fixed after that run. EL8 test_evidence_capture_live (tiny arm capture_loss) is still open.
  - The final tree's tiers are READY-NOT-STARTED.
- No UI change.

## Per item (VERIFIED = ran or read file:line here; INFERRED = reasoning only)
1. Equivalence — VERIFIED.
   - I built a579619c and 959f827b on the Mac. All 21 server sources were built per tree, because the struct layout changed.
   - A dump harness compared, per block: open, index, block_info, coverage_record, decode (FNV of the events) and file_span. Per directory it compared the pgwt_cov_trace records plus the default-window verdict. Both servers were also run on every directory with 9 requests (info, aas, time_model, top_events, heatmap, coverage; default and explicit windows), with now_ns normalised.
   - Corpus: 66 repo and test-generated files in 45 directories, including the 24-file day fixture. That is 86,572 blocks and 86,505 coverage records. Output, stderr and server replies were byte-identical.
   - Then 1,615 generated mutants, also byte-identical:
     - truncations at 0, 1, 10, 27-37, mid-block-header, mid-payload, every block boundary of the first and last three blocks, and size-1/-4/-5/-17, each with and without .meta;
     - compressed_size set to 0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, MAX, MAX+1, exactly EOF and EOF+1, on the first, middle and last block;
     - num_events 0, 4097 and 0xFFFFFFFF; block type 3, 7 and 0xFFFF; a payload byte flip;
     - header version 1, 5, 7 and 0xFFFF, and bad magic, on v2/v3/v4/v6 bases;
     - footer count corruption; ten .meta variants;
     - zero-length files;
     - a coverage block starting at every offset 0..97 before the 256 KiB boundary, with and without meta, and torn;
     - total sizes CHUNK-1, CHUNK, CHUNK+1 and 2*CHUNK+1;
     - a real 4096-event SAMPLES block with a 256 KiB-786 KiB payload: it decodes 4096 events identically and its torn variant is refused identically.
   - Mutant results: 586,702 blocks; server verdicts 1,370 complete and 11,102 incomplete, with no difference between old and new. An ASan+UBSan build over the same set found nothing.
   - Growth path: 674 prefix-then-append refresh pairs were identical. A probe confirmed scan_growth was actually taken (334 of 713 probe runs).
   - A sparse 4.6 GB file (444 blocks, last offsets above 2^32) gave identical results.
   - On the implementer's "22 coverage outputs byte-identical": that compared the tests' own PASS text (compatibility.py), which is weak. The results above replace it.
2. Live file — VERIFIED (src/event_reader.c:99-110 and :20-28; coverage_trace.c:165-183).
   - The .meta count is read first, then fstat sets the extent. The writer fflushes a block before it renames .meta (event_writer.c:1026-1032). So every committed block lies inside the extent, and nothing past the extent is ever read.
   - The old path read live bytes, so the new path is equal or stricter. A payload outside the snapshot is refused (-1), as the repair-after-open test pins.
   - Concurrent run: a forked writer wrote 60,000 WATERMARKs with one rotation, while the reader ran open plus refresh in a loop, about 3,600 times per run, over 5 runs of old and new. No torn or BAD record and no non-consecutive watermark appeared in either.
3. Bounds — VERIFIED, read and run.
   - Memory is 256 KiB per open reader. Readers are opened one at a time (replay.c:31-34, server.c:4966-4972).
   - Offsets are uint64 checked against LONG_MAX, and pread takes off_t. SEEK_CUR overflow is checked.
   - Short reads re-fill, EINTR is retried, and pread<0 or EOF gives a short read and then -1, as fread did.
   - Error injection: an error in the first chunk now refuses the open; the old code salvaged one block. An error past the first chunk gives the same prefix salvage as before, cut at 256 KiB instead of stdio-buffer granularity. Meta-indexed files keep the server refusal at server.c:2968.
4. Verdict drift — VERIFIED, none. See the 1,370 + 11,102 verdicts in item 1.
5. I/O test — VERIFIED.
   - It counts calls deterministically: 3,483 on the Mac, the same as the implementer's figure. The bound is 12,096 (test_coverage_consumers.py:914).
   - These mutants fail it: the per-record-seek mutant at 437,386, a 64 B chunk at 955,388, and a 4 KiB (stdio-like) chunk at 19,529.
   - coverage_consumers_mutants.py: 21 built, 21 red, each at its named assertion.
   - It runs in the box tier through tests/unit_tests.list:1136-1137. It does not run in Mac make check; that was already true for this file before the change.
6. Pure-CPU arm — VERIFIED (ran the controls: 24 subtests OK, exit 94 only in the two capture_loss + drops 7->9 rows).
   - window_loss counts only counter samples that lie wholly inside the response's own window. It needs before<after and then coverage_loss.observe, which checks EL8 4.18, incomplete with capture_loss, and drops > 0.
   - Otherwise check() runs normally. Unchanged drops, missing drops, the restart reason, post-SEAL drops, live CPU 0, rc=2, and a wrong trace or AAS value under a covered window all exit 1.
   - The live-CPU and natural-shutdown checks always run.

## Findings
No blockers.
1. nit — src/event_reader.c:63-67. No test covers the new non-EINTR pread error branch. coverage_reader_snapshot.c covers only EINTR and short reads. Scenario, read and run here with injection: pread EIO on the first chunk → the open is refused (correct, as found here). A regression that treated got<0 as EOF would not be caught.
2. Evidence gap, not a code defect: the full tiers have not run on the final tree ba883080. Paste them before the PR.

## Pre-existing, outside this delta (identical at a579619c)
- Truncate one archived hourly file in the day fixture (2025-01-01_05.trace.lz4, 345,874 -> 300,000 bytes). This removes its footer, its last 476 WATERMARKs and its data block.
- Result: time_model over the whole day answers `complete`, fidelity `exact`, DB Time 46,000 ms. The intact directory gives 48,000 ms. Old and new give the same answer.
- Expected: incomplete/withheld under the 6980525535 scope ("withheld... whenever coverage cannot be confirmed").
- NOT SEARCHED: GitHub issues, for whether this is already filed.

## Design questions for the owner
None.
