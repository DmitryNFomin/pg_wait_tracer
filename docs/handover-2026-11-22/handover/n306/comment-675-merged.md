**After PR 1004 (sampler at nice -10 by default; issue 306, decision "a"): the cross-validation test passes on EL8 in CI; this issue's own measure is not re-measured yet**

On the EL8 gate box, in the PR's own CI (run 44972248327, started by the Actions runner), `test_cross_validate_tiered` read 2.63 / 3.66 / 2.78 / 2.75 pp at 10 / 50 / 100 / 200 Hz against the 8.8 pp bound, and was reported as an unexpected pass of its scoped known-failing entry (`test_cross_validate_tiered|675|el8@4.18|exit=79`). On a clean-boot EL8 development VM ten runs read 2.26 to 3.86 pp (issue 306, comment 7068935283). Before the change the same test read 9 to 10 pp on EL8.

What that does and does not say about this issue:
- This issue is stated against a third-party referee: sampled on-CPU time over the kernel's schedstat total (S/G 0.50 on 4.18 against 0.79 on 6.8). The cross-validation test compares the sampled tier with the exact tier, not with the kernel, so its passing does not show S/G has moved.
- The referee experiment is being re-run on master (default nice -10 against nice 0, service context, both kernels). Until its result is here the known-failing entry stays and this issue stays open.

Refs #675
