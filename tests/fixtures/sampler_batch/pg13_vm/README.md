Recorded file-decoding regression evidence (Refs #866)

`el8-4.18-file.script.txt` is an unedited contiguous prefix of
`tmp/p866-candidate/pg13-far-candidate/perf.trace` from
`tests/results/session-20261116/p866-pg13-live2/el8/ev.tgz` on the lead's Mac.
The complete recording is 1,149,450 bytes; this 39,569-byte slice retains
51 tick markers and every intervening readv entry/exit (50 complete ticks,
150 syscall pairs). It ends immediately after marker 51.

Full recording SHA256: `6aa7cba87e87fd1a86b73efa5ad2e2e0591eef949ec908bd41a2b5650cbc1064`.
Slice SHA256: `28b0d35b1ccb4fe6f7748460fc7c7306f68e7b3ffb5c8b8ae75e44e0c89bb78d`.
Kernel/perf: EL8 4.18; tick event `pgwt866_6907_7473:tick`, sampler tid 7473,
postmaster 6925, wait reader 6928. Each complete tick has two postmaster
status calls and one wait-shaped call (492 iovecs, 1968 returned bytes).
There are zero possible discovery calls: the original live run failed its
pipe-readiness check before opening the far-row workload. The fixture proves
decoding and steady-state call shape; it cannot qualify the churn gate.

`ubuntu-6.8-ticks.script.txt` is all 20 unedited lines from
`tests/results/session-20261116/p866-pg13-live2/ubuntu/raw20.txt` on the lead's
Mac, 1,320 bytes. Kernel 6.8, `perf script --ns` with padded columns.
SHA256: `58da02e1ca0f31277b50a264b8a58d172d4a538841e1582e4dc0da42ca3bbf5c`.
These are tick lines only, not Ubuntu syscall evidence.

Portable regression: `python3 -B tests/test_sampler_batch_summary.py`
(`recorded_file_vm_cases`, `file_capture_cases`, `pg13_selection_cases`).
