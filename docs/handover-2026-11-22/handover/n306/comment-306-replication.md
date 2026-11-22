**2026-11-21: the service-placement under-read was re-measured today without knowing it had been measured here on 19 November; the 306-a implementation starts now**

What happened: the required Ubuntu box tier failed on `test_cross_validate_tiered` in 6 of 42 CI jobs over two days. I investigated it on issue 675 as a new problem (three measurements on the gate boxes) and did not read this issue's 19 November comments first. The second adviser found them. Today's numbers replicate them:

| Launch (pgwt-gate, Ubuntu 6.8, master 7140a517, tracer and pgbench together) | 10 Hz median | 100 Hz median |
|---|---|---|
| login session over ssh | about 5 | about 4 |
| default system service (the CI runner's shape), three sets of 3 | 7.1, 7.8, 7.6 | 8.4, 8.4, 8.7 |
| service in its own top-level slice | 4.9 | 4.7 |
| service with CPUWeight=10000 | 2.9 | 1.9 |

Details and limits: issue 675, comments of 2026-11-21 (7064598820, 7064977101, 7065436327).

State of the owner's decision "306 - a" (comment 7004963488):
- Not implemented. Master 7140a517 leaves the sampler's scheduling unchanged by default (src/daemon.c:697-703) and its message and README.md:1625-1633 still say the nice override does not remove the bias, which the 19 November service-context measurements contradict. INSTALL.md's unit example has no scheduling line and the tree ships no unit file.
- Both things it was waiting for have merged (the coverage-writer PR and the stalled-workload validity rule, PR 974).
- The implementation unit is being dispatched now: sampler at nice -10 by default; a startup warning when the tracer and PostgreSQL are in different CPU groups, with the accuracy figures withdrawn for that setup in the documentation; the cross-validation test running the tracer as a plain system unit with the load generator outside, with the layout and the nice value read back.
- The test's 8.8 pp bound is not changed in that unit. Re-deriving it from the shipped configuration (pre-registered before its confirmation runs, as tests/xval_tolerance.sh requires) is the step after it.

What 306-a does not close: about 2 to 4 pp remain at the best placement measured (nice -10 in a shared group 3.5 to 4.2; maximum weight 1.9 to 2.9; SCHED_FIFO 3.9 in this issue's original experiment). That residual is this issue's original question and stays open.

Refs #306
