# timing arm diagnosis (measurement; nothing in the repository changed or committed)

Raw logs: vm-timing-raw.log, mac-timing-raw.log in this directory. Copies of the test with per-command prints and the bound turned into a no-op were used (archives in /private/tmp/claude-501/x-<sha>; on the VM /root/timing-<sha>, /root/perf).

## Q1 origin
`git log -S'24-file request under 50 ms' agent/947-coverage-server` -> only 80b88663 (2026-11-20 "Preserve live coverage totals and cache coverage refreshes"). MEASURED.
Evidence PASS lines (grep "PASS timing" in /private/tmp/u2b-r*-evidence), per request max/mean:
- r5 (consumers.log): 10.534 / 3.894
- r7 ubuntu 22.293-31.154 / 6.4-7.2 (host root@<ip>); r7 el8 22.5-22.6 / 6.0-6.6 (root@<ip>, then EL8)
- r8 ubuntu 21.78-22.41 / 6.3-6.4 (<ip>); r8 el8 20.3-24.2 / 5.7-5.8
- r9: no PASS timing line in r9 evidence (grep found none).
- r10 el8 (<ip>) 21.5-28.5 / 6.1-6.9 PASS; r10 ubuntu (<ip>, a new Ubuntu VM) FAIL aas 143.17 ms; r11 ubuntu FAIL aas 119.9-129.8.
The evidence prints only the max, not which command; the bound was never failing on the earlier hosts. The failing Ubuntu VM is a different machine than the r7/r8 Ubuntu (<ip>) and the IP <ip> was EL8 in r7-r9 (server 176957296 gone, 404 in r9).

## Q2 VM (root@<ip>), 5 runs x 3 commits interleaved, per-command first-5 (info/aas/time_model/top_events/heatmap, ms), MEASURED (run.sh loop; log vm-timing-raw.log)
137aa637: r1 28.7/120.3/30.3/9.3/9.3; r2 30.1/128.6/26.1/17.4/12.4; r3 21.9/129.7/33.2/9.1/15.7; r4 27.2/140.2/25.5/16.9/10.9; r5 22.7/152.2/23.0/10.3/9.2
3b53b034: r1 27.2/127.8/24.9/18.4/10.6; r2 28.0/135.2/26.7/13.8/10.0; r3 21.0/119.1/18.4/9.5/11.0; r4 25.8/129.1/19.6/10.5/12.1; r5 22.2/118.1/17.5/10.7/10.3
a579619c: r1 21.2/112.8/15.6/10.3/10.1; r2 24.6/118.3/19.5/9.0/9.2; r3 21.1/115.7/19.0/11.2/11.2; r4 21.8/130.5/18.7/9.6/9.8; r5 22.5/132.4/18.4/10.1/11.6
Later aas calls (requests 7,12,17) are 10-21 ms on every run. So: first aas = 113-152 ms at ALL three commits (round 8 included, it also fails here); warm requests 9-20 ms. Only the first aas breaks 50 ms; time_model first call 15-33 ms.

## Q3 Mac (Apple M4, 10 cpu), tip a579619c, 5 runs (mac-timing-raw.log)
first aas 18.5, 10.3, 11.3, 21.1, 16.6 ms; later aas 3.4-4.5; all other commands 2.7-6.8. Whole arm max 10.3-21.1, mean 3.6-4.8. PASS.

## Q4 where aas spends time on the VM (perf exists; /root/perf)
- Scaling of the FIRST aas (fresh server, info first), hours of data in window 1/6/12/24: 22-34 / 46-71 / 71-75 / 112-132 ms; second aas 9-20 ms. Buckets 12 vs 120: same (116-128 vs 112-147); 1200 buckets only raises the WARM call (21-32 ms). => cost scales with files / WATERMARK count (linear, ~5 ms per hourly file = 3,600 coverage blocks), not with buckets.
- strace of one server: syscalls between the info reply and the first aas reply: 86,569 lseek, 2,376 read, 219 openat; second aas: 72 lseek, 216 read. 86,569 ~= 86,400 WATERMARKs (+ ~170). Counted per request, not per file open. (INFERRED link to source: src/block_agg.c ~line 851 loops every block calling pgwt_reader_block_info = fseek+fread, WATERMARK blocks only then `continue`; coverage_trace.c scan_block does the same per record at refresh. I did not test by changing code.)
- perf record -F4999 -g over 30 fresh server processes (each info + one aas), self time relative to comm server: pgwt_reader_coverage_record 23.9%, one unresolved libc address 7.2%, _IO_fread 6.7%, kernel __fdget_pos 5.6%, _IO_file_seekoff 5.4%, ext4_llseek 4.9%, fseek 3.3%, read 3.2%, walk_next 2.1%. Inclusive: pgwt_reader_block_info 98.5%, pgwt_cov_trace_refresh 51.5%, pgwt_reader_decode_block_info 48.8%. Kernel lseek family is the largest kernel cost (strace: lseek 432k calls = 95% of syscall time). The profile includes startup/info passes, so the split coverage-refresh vs aggregation is ~50/50 by inclusive share across the process; per aas request the syscall counts above place the cold cost in the per-block header read loop (86.4k seek+read), not in the verdict (pgwt_coverage_verdict self 0.26%) and not in AAS bucketing (warm aas = 9-10 ms).
- So: the verdict computation is negligible; the cost is reading block headers of 24 files (per-WATERMARK fseek+fread, ~86.4k libc seeks) on the first request after the files are first seen; AAS bucketing is the warm ~10 ms.
- INFERRED (untested): the VM makes syscalls expensive (Meltdown PTI, Retbleed IBRS, MDS clear CPU buffers, KVM Skylake, all shown by lscpu), explaining ~10x vs the M4 and vs earlier hosts; I did not measure raw syscall cost or compare against the earlier hosts.

## Q5 CPU
VM <ip>: Intel Xeon Processor (Skylake, IBRS, no TSX), 4 vCPU, 1 thread/core, KVM, 16 MiB L3, Ubuntu 24.04, gcc 13.3; load 0.00 at start. Mac: Apple M4, 10 cpus. Earlier machines: the old evidence records only the Hetzner type "cx33" (grep), no lscpu/nproc; vendor/CPU unknown. NOT RUN: lscpu on <ip> / <ip> (not asked to touch other machines; r9 says r7-r9 VMs are gone).

## Not run
No comparison of raw syscall cost; no code change experiment; el8 not run; perf was not restricted to the aas request alone.
