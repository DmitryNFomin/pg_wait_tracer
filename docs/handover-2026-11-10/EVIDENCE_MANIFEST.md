# Measurement evidence manifest, 2026-11-10

The measurements published this week on #381, #837 and #839 were taken from
captures that are **not in this repository**: they are hundreds of megabytes,
and they sat under `/private/tmp` on the machine the work was done on, which
does not survive a reboot. No decision was obtained about moving or deleting
them before the handover ended, so this manifest is the durable record instead.

It exists for two jobs:

- **identification.** If a copy of one of these directories turns up on a disk,
  a backup or another machine, the sizes and the first sixteen hex digits of
  each file's SHA-256 below say whether it is the same data the published
  numbers came from, or something else.
- **honesty.** If the data is gone, this is the proof of what was lost, so that
  nobody re-derives a number from a different capture and reports it as the same
  measurement. Treat a number whose evidence is listed here but absent as
  unverified, not as confirmed.

What each directory backs:

| directory | backs |
|---|---|
| `sidecar-measure-20261110` | round 1 of the `backends.jsonl` work: the capture behind #839, and the raw-versus-summary control that redirected #381 |
| `sidecar-r2-20261110` | round 2: the aligned/unaligned window table on #381 and the four-arm sidecar table on #839 |
| `idle-r3-20261110` | the #381 Idle decomposition (boundary placement +2445.590751 ms, boundary classification -0.144408 ms, interior remainder 0.000000 ms) |
| `pgwt-837-evidence` | #837's severity run. **Its `ordinary/` and `diverse/` captures have no second copy anywhere** |
| `pgwt-837-classfiltered-evidence` | #837's camera-quantisation round, which established that the UI widths need captures of about 28.6 and 57.3 minutes |

A second copy of round 1's capture existed in the session scratchpad at the time
of writing (`salvage-sidecar-20261110/intact/`), byte-identical by SHA-256 for
`current.trace` and `2026-11-10_06.trace.lz4`; that location is volatile too.

Sizes are bytes. Hashes are the first sixteen hex digits of SHA-256. Only files
that carry a result are listed -- requests, responses, scripts, calculations,
manifests, traces and summaries -- not the whole tree.

```
### /private/tmp/sidecar-measure-20261110  (250M)
        5605  4791a8eb78eb2ddf  sidecar-measure-20261110/calculations.txt
        2391  b8034df926a34b05  sidecar-measure-20261110/capture.sh
         535  fe1d426bc4d07bcf  sidecar-measure-20261110/cleanup.sh
         387  7dff86652e6d0c93  sidecar-measure-20261110/control-A.json
         136  ee37be294308426a  sidecar-measure-20261110/info.command.txt
          31  07cf367928f87bbb  sidecar-measure-20261110/info.request.json
         204  53b8e6dca706acea  sidecar-measure-20261110/info.response.json
         928  0e593b0a1fa7de80  sidecar-measure-20261110/intact.manifest.json
         146  ed257c7280816706  sidecar-measure-20261110/intact.raw.command.txt
         176  4b2fbd7ad0e518dc  sidecar-measure-20261110/intact.raw.oracle.command.txt
          97  a05ffd7f595259cb  sidecar-measure-20261110/intact.raw.oracle.request.json
         251  e0ce93acb598b181  sidecar-measure-20261110/intact.raw.oracle.response.json
          97  a05ffd7f595259cb  sidecar-measure-20261110/intact.raw.request.json
        5917  cf8feaabd1e98768  sidecar-measure-20261110/intact.raw.response.json
         146  1601ebb61304141f  sidecar-measure-20261110/intact.summary.command.txt
         176  d1183d27f786657d  sidecar-measure-20261110/intact.summary.oracle.command.txt
          97  a05ffd7f595259cb  sidecar-measure-20261110/intact.summary.oracle.request.json
        5396  70b99a0af85772dd  sidecar-measure-20261110/intact.summary.oracle.response.json
          97  a05ffd7f595259cb  sidecar-measure-20261110/intact.summary.request.json
        5396  70b99a0af85772dd  sidecar-measure-20261110/intact.summary.response.json
       20996  9ef61d336badd136  sidecar-measure-20261110/intact/2026-11-10_06.summary.lz4
    10095580  2e7095d1775048f4  sidecar-measure-20261110/intact/2026-11-10_06.trace.lz4
       16089  e4e5f9c008e32870  sidecar-measure-20261110/intact/backends.jsonl
      290528  5b16fa621b52a352  sidecar-measure-20261110/intact/current.summary
   163383825  cb09661756f309ea  sidecar-measure-20261110/intact/current.trace
        2306  6a4f34e2625a0012  sidecar-measure-20261110/intact/query_texts.jsonl
        5638  3756ffd95386c832  sidecar-measure-20261110/intact/wait_event_names.json
         151  741434d9626d3887  sidecar-measure-20261110/local-transfer-verification.json
        4791  07fc7a900fe1cdb4  sidecar-measure-20261110/measure.py
          87  efe5273503dd86ba  sidecar-measure-20261110/provenance-command.txt
          55  cedd4a755444b7e5  sidecar-measure-20261110/raw-removals.json
       21834  14126aaafad0669a  sidecar-measure-20261110/results.json
      322414  00c4f44ce42f735c  sidecar-measure-20261110/source-git-objects.json
         105  c2757cbb5be1f433  sidecar-measure-20261110/source-verification.json
         695  790d32e6b545f721  sidecar-measure-20261110/summarize.py
         461  83a494be33d98394  sidecar-measure-20261110/type-counts.json
         505  9bf1216f29f96446  sidecar-measure-20261110/verify-source.py
          83  c9261baeb11f629c  sidecar-measure-20261110/window.json

### /private/tmp/sidecar-r2-20261110  (2.0M)
         285  8890a027d9efb75e  sidecar-r2-20261110/142.132.191.18.lock-check.json
         549  e172245c65cd8aaa  sidecar-r2-20261110/2.28.45.47.lock-check.json
         562  9ebe89050008f9b5  sidecar-r2-20261110/46.225.142.12.lock-check.json
        2095  b48811f39811a332  sidecar-r2-20261110/audit_evidence.py
        7614  87530ea98a1ce9ea  sidecar-r2-20261110/calculations.txt
         159  03cb092796a16774  sidecar-r2-20261110/cleanup-final.json
         591  cd3d3d49b7e82398  sidecar-r2-20261110/cleanup.sh
        9379  cefce7551e7e3361  sidecar-r2-20261110/COMMANDS.txt
         559  5fe3529058087b16  sidecar-r2-20261110/grid.audit.json
       17092  2f969611de696d6f  sidecar-r2-20261110/grid.jsonl
        3867  2ed78509c5e927c7  sidecar-r2-20261110/index_evidence.py
        7820  9405acc8bd6e7fd6  sidecar-r2-20261110/measure_r2.py
         425  d9dbb54d00fac95e  sidecar-r2-20261110/partial-kept.json
        1135  d96a3f07df3ee36d  sidecar-r2-20261110/provenance.json
          55  cedd4a755444b7e5  sidecar-r2-20261110/raw-removals.json
         388  c294d274fb49fbe5  sidecar-r2-20261110/repair_window_metadata.py
         146  4d19ccfd4c28129b  sidecar-r2-20261110/s1.full.aligned.raw.command.txt
         176  4e91a9e83c6d0116  sidecar-r2-20261110/s1.full.aligned.raw.oracle.command.txt
         309  f087b21d51dadc0f  sidecar-r2-20261110/s1.full.aligned.raw.oracle.process.json
         101  7034d60ae656bb6a  sidecar-r2-20261110/s1.full.aligned.raw.oracle.request.json
         251  e0ce93acb598b181  sidecar-r2-20261110/s1.full.aligned.raw.oracle.response.json
         279  462c97df8c8916f6  sidecar-r2-20261110/s1.full.aligned.raw.process.json
         101  7034d60ae656bb6a  sidecar-r2-20261110/s1.full.aligned.raw.request.json
        5921  2fca75d4bbef0346  sidecar-r2-20261110/s1.full.aligned.raw.response.json
         437  fae1d68e2f1a8b65  sidecar-r2-20261110/s1.full.aligned.raw.route.json
         154  ddbffacf46ce10d8  sidecar-r2-20261110/s1.full.aligned.summary.command.txt
         184  bf828125e1847761  sidecar-r2-20261110/s1.full.aligned.summary.oracle.command.txt
         317  8eba7c9abdee737d  sidecar-r2-20261110/s1.full.aligned.summary.oracle.process.json
         101  ea3d61e5268a18dc  sidecar-r2-20261110/s1.full.aligned.summary.oracle.request.json
        5425  9504c5428753d770  sidecar-r2-20261110/s1.full.aligned.summary.oracle.response.json
         287  fc03a921f48720db  sidecar-r2-20261110/s1.full.aligned.summary.process.json
         101  ea3d61e5268a18dc  sidecar-r2-20261110/s1.full.aligned.summary.request.json
        5425  9504c5428753d770  sidecar-r2-20261110/s1.full.aligned.summary.response.json
        6020  f4f21eb4f4b90b67  sidecar-r2-20261110/s1.full.aligned.summary.route.json
         148  3d820cdaa533b4d6  sidecar-r2-20261110/s1.full.requested.raw.command.txt
         178  1f741eb1df525356  sidecar-r2-20261110/s1.full.requested.raw.oracle.command.txt
         311  d37a25a711db69dc  sidecar-r2-20261110/s1.full.requested.raw.oracle.process.json
         101  ea3d61e5268a18dc  sidecar-r2-20261110/s1.full.requested.raw.oracle.request.json
         251  e0ce93acb598b181  sidecar-r2-20261110/s1.full.requested.raw.oracle.response.json
         281  762ac4db67878dc4  sidecar-r2-20261110/s1.full.requested.raw.process.json

### /private/tmp/idle-r3-20261110  (445M)
        2512  e14a9f3fd0028f40  idle-r3-20261110/additivity-detail.json
        2512  e14a9f3fd0028f40  idle-r3-20261110/additivity-detail.txt
         146  49b5a4c18609ce55  idle-r3-20261110/additivity.json
        1988  4fb671d1babca028  idle-r3-20261110/additivity.py
       11693  7167e110a04c9b02  idle-r3-20261110/analysis.json
        1168  455e188615ba82bd  idle-r3-20261110/analyze.py
      561984  f5b8ef8efc34511e  idle-r3-20261110/background-records.json
         500  2a1129984258618d  idle-r3-20261110/calculations.txt
         460  3994d6a4fdcbc738  idle-r3-20261110/classification-buckets.json
         261  564b92cb5ef349b6  idle-r3-20261110/cleanup.json
        5292  cd41edb58719d028  idle-r3-20261110/COMMANDS.txt
         150  8a0063f1fdeeead0  idle-r3-20261110/complete/additivity.json
         175  e424276051ad322f  idle-r3-20261110/complete/one000.raw.command.txt
         101  4b94d86f19099d70  idle-r3-20261110/complete/one000.raw.request.json
        3837  d3ab5b6dacb5090c  idle-r3-20261110/complete/one000.raw.response.json
         175  75dc53d30e4ee605  idle-r3-20261110/complete/one001.raw.command.txt
         101  fb35ecab3289fc16  idle-r3-20261110/complete/one001.raw.request.json
        3850  e39ec1f1bedf6c82  idle-r3-20261110/complete/one001.raw.response.json
         175  424a4cd27506c050  idle-r3-20261110/complete/one002.raw.command.txt
         101  149114ff6cbf9bee  idle-r3-20261110/complete/one002.raw.request.json
        3891  b9ed6403cb10eb27  idle-r3-20261110/complete/one002.raw.response.json
         175  fc3d2360fe5168fc  idle-r3-20261110/complete/one003.raw.command.txt
         101  08eb9bfe0fd7750b  idle-r3-20261110/complete/one003.raw.request.json
        4158  3edb72758c5d6f15  idle-r3-20261110/complete/one003.raw.response.json
         175  7a46688abffe0672  idle-r3-20261110/complete/one004.raw.command.txt
         101  b5601c90dae94954  idle-r3-20261110/complete/one004.raw.request.json
        4325  cf268a43b80bf6d2  idle-r3-20261110/complete/one004.raw.response.json
         175  41624d7b6f11b53e  idle-r3-20261110/complete/one005.raw.command.txt
         101  65e6ba69ba93e5e3  idle-r3-20261110/complete/one005.raw.request.json
        3861  98308409f1265a76  idle-r3-20261110/complete/one005.raw.response.json
         175  b5d46958e92c0d30  idle-r3-20261110/complete/one006.raw.command.txt
         101  e169eceb7c2d0fc8  idle-r3-20261110/complete/one006.raw.request.json
        3899  1a3d4587ca11f1f7  idle-r3-20261110/complete/one006.raw.response.json
         175  c5b97019b5b7cdb6  idle-r3-20261110/complete/one007.raw.command.txt
         101  3fe660698fd27bc5  idle-r3-20261110/complete/one007.raw.request.json
        3996  a61b2bae6ec40820  idle-r3-20261110/complete/one007.raw.response.json
         175  f224d2ff12ea66cc  idle-r3-20261110/complete/one008.raw.command.txt
         101  146ae9be2d929b31  idle-r3-20261110/complete/one008.raw.request.json
        4621  b25d48b5bbc845c9  idle-r3-20261110/complete/one008.raw.response.json
         175  dc19a6d2c1b640a0  idle-r3-20261110/complete/one009.raw.command.txt

### /private/tmp/pgwt-837-evidence  (404M)
         672  e907d4c74e89c3d0  pgwt-837-evidence/00-provenance.sh
         449  5656027c64d2d742  pgwt-837-evidence/01-build.sh
         106  f3d640414e0ef36c  pgwt-837-evidence/02-ordinary.sh
         106  c1603bb7ba2fca66  pgwt-837-evidence/03-measure-ordinary.sh
         105  e377af4f4ec2b0f7  pgwt-837-evidence/04-diverse.sh
         221  7a8bd0eab74ef38b  pgwt-837-evidence/04a-measure-ordinary.sh
         105  cd91bd437bb873c5  pgwt-837-evidence/05-measure-diverse.sh
        2187  dd8535c1137076c3  pgwt-837-evidence/06-verify-archive.sh
         421  717efb7fa641027b  pgwt-837-evidence/07-repeat-diverse.sh
         105  cd91bd437bb873c5  pgwt-837-evidence/08-measure-diverse.sh
        1924  2683423b753738c2  pgwt-837-evidence/09-verify.sh
        1212  0485ee0c84968ec1  pgwt-837-evidence/10-archive-cleanup.sh
        2817  d4cbebb76072d254  pgwt-837-evidence/attempts.txt
        3654  28f5e0c48518b1da  pgwt-837-evidence/capture.py
          41  928431f61cbf885f  pgwt-837-evidence/commit.txt
        5678  2f3646e4ad2a0d79  pgwt-837-evidence/diverse-before-memory-correction.py
         236  01ef6e28053884b8  pgwt-837-evidence/diverse-failed-oom/info.json
       73672  a3ba49d96c6c9455  pgwt-837-evidence/diverse-failed-oom/metrics.jsonl
     2542297  60a9ce5643992d7e  pgwt-837-evidence/diverse-failed-oom/requests-responses.jsonl
       19046  242560ac51741d35  pgwt-837-evidence/diverse-failed-oom/status.jsonl
         176  767dae6b94fc1f21  pgwt-837-evidence/diverse-failed-oom/window.json
        5719  9d452b61078ab6c0  pgwt-837-evidence/diverse.py
      514564  82ae574afb11ae64  pgwt-837-evidence/diverse/buckets-2147483648.json
       42094  97ca9d2c0a83e85b  pgwt-837-evidence/diverse/buckets-34359738368.json
      288594  2dcbb8fe1cbaeef3  pgwt-837-evidence/diverse/buckets-4294967296.json
         236  9af1e6122022dbce  pgwt-837-evidence/diverse/info.json
       77786  1d2e0bd420d344ed  pgwt-837-evidence/diverse/metrics.jsonl
     6251948  1fa38964dee313da  pgwt-837-evidence/diverse/requests-responses.jsonl
       20105  885362f115d4ee35  pgwt-837-evidence/diverse/status.jsonl
       21280  363b75a140c92c57  pgwt-837-evidence/diverse/summary.json
         176  6edd75839fe8c2e3  pgwt-837-evidence/diverse/window.json
         672  e907d4c74e89c3d0  pgwt-837-evidence/done/00-provenance.sh
         449  5656027c64d2d742  pgwt-837-evidence/done/01-build.sh
         106  f3d640414e0ef36c  pgwt-837-evidence/done/02-ordinary.sh
         106  c1603bb7ba2fca66  pgwt-837-evidence/done/03-measure-ordinary.sh
         105  e377af4f4ec2b0f7  pgwt-837-evidence/done/04-diverse.sh
         221  7a8bd0eab74ef38b  pgwt-837-evidence/done/04a-measure-ordinary.sh
         105  cd91bd437bb873c5  pgwt-837-evidence/done/05-measure-diverse.sh
         421  717efb7fa641027b  pgwt-837-evidence/done/07-repeat-diverse.sh
         105  cd91bd437bb873c5  pgwt-837-evidence/done/08-measure-diverse.sh

### /private/tmp/pgwt-837-classfiltered-evidence  ( 36K)
        2037  ec84e15de2dc83b0  pgwt-837-classfiltered-evidence/attempts.txt
        4291  03622d688d50c83d  pgwt-837-classfiltered-evidence/box-locks.json
        6411  3071b7506417838c  pgwt-837-classfiltered-evidence/camera.json
         479  9ca04c0a4d71d01f  pgwt-837-classfiltered-evidence/cleanup.json
         611  4e0e03547c84f23e  pgwt-837-classfiltered-evidence/local-state.json
         114  a8c882f3e49edae7  pgwt-837-classfiltered-evidence/start.txt

```
