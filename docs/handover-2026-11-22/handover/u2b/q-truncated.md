# Question: a truncated archived trace file inside the window still answers "complete" with a short number

Read-only. At most 18 lines; every claim VERIFIED (file:line read, or a command you ran) or INFERRED. Repository <repo>; scratchpad = SP.

## Found (review scratchpad/u2b/review-959f827b.md, section "Pre-existing", VERIFIED by that reviewer on the Mac; its tools are under scratchpad/rv959/)
On branch agent/947-coverage-server (the coverage label; tip 959f827b, read with `git show "959f827b:<path>"`): take the day fixture of 24 archived hourly files, truncate ONE archived file (2025-01-01_05.trace.lz4, 345,874 -> 300,000 bytes: it loses its footer, its last 476 WATERMARK records and its data block). `time_model` over the whole day then answers coverage `complete`, fidelity `exact`, DB Time 46,000 ms; the intact directory gives 48,000 ms. Identical before and after the reader change of this round. On master there is no coverage label, so the same short number appears with no claim at all.

## Context
Coverage spec r2 and its amendments A1-A5 (find the spec: `git ls-tree -r --name-only 959f827b | grep -i -E 'coverage|spec'`; issue 947's comments carry A4 and A5: `gh api repos/DmitryNFomin/pg_wait_tracer/issues/947/comments --jq '.[].body'`); the verdict is `pgwt_coverage_verdict` in src/coverage.c; reasons in use include capture_loss, restart, retention, not_captured, missing_file. Owner scope decision on issue 947: "Their whole-window numbers are withheld, the same as for any window whose coverage cannot be confirmed." Related open issue 333 (a truncated current.trace reads as "No data"; low priority, not reachable by normal daemon operation).

## Questions
1. Why does the verdict say complete: is it because the following file's WATERMARK records certify capture continuity (cumulative), so the verdict has no notion of a retained file whose stored data is damaged? Point to the code (file:line) and say exactly which files and structures a verdict consults.
2. What is the right rule: a retained file overlapping the window that fails a structural check (no footer / block count below its .meta / a frame that ends early / a checksum failure) makes its time range incomplete, with which reason (reuse `missing_file`, or a new `damaged_file`), and what range exactly (the whole file's nominal range, or from the last intact block)? Is the spec silent on this (then it is an amendment A6) or does an existing row already require it?
3. How reachable is it in normal operation (crash or power loss during rotation or compression, a full disk, an interrupted copy of a capture directory to another machine, retention deleting while compressing)? Which of these does the writer already guard (fsync, rename-after-write)? This decides severity: tag-blocker (a wrong number under an explicit "complete") or documented limit.
4. The cheapest sound detection on the read path that does not undo this round's fix (no per-record seeks; the cold scan now reads in 256 KiB chunks): what does the reader already know after its scan (footer seen, block count, last valid offset) and where should the verdict get it from?
5. Tests: the rows to add to the verdict's oracle (inputs only, then expected), and the mutant that must turn red.
6. Is anything here an owner decision, or is it the lead's call under the existing scope decision?
