# #369 contract experiment, 2026-10-13

The evidence supports analysis A on the **existing contract and current viewer
impact**: d231d18 breaks the pinned empty-not-failed contract, while its routing
prevents supported Sessions requests from exercising that changed contract.
Two qualifications matter: master really does reach summaries for explicit
CPU=0 (the separate #345 bug), and the UI **does display** a compute refusal.
Neither “always forces raw” nor “nobody reads the failure” is literally true.
No production source or production behavior was changed by this experiment.

Pinned revisions:

| Label | Commit | Purpose |
|---|---|---|
| master | `8a52a510e47c8592fd2315b99e8094258c83fe0c` | origin/master at experiment time |
| d231d18 | `d231d189da237d0b51e2e13301a9524e315b678a` | disputed revision requested by the owner |
| restored | `6a5154ff0601179c50aa85b1711fabbcfeb8630d` | already-existing #369 contract restoration, with master merged; preservation check only |

The experiment branch starts at 612bd98. Origin/master 8a52a51 differs from
612bd98 only in two demo GIFs. PR #369 had already moved beyond d231d18; that
does not change which revision was used for the requested comparison.

1. **Reachability: real server, actual trace and summary files.**

   [experiment_369.py](../../experiment_369.py) builds each exported revision's
   real pgwt-server. The main fixture has 300 one-second exact transitions,
   alternating CPU and IO, for pid 1000/query 100. The requested 320-second
   range qualifies for summaries. An unfiltered control actually enters the
   summary compute function, so a short range, absent summaries, or fidelity
   fallback cannot explain the filtered results.

   Separate GNU linker wrappers log entry to raw and summary Sessions compute
   without changing the source. Every Sessions JSON response is asserted equal
   to the corresponding response from the unmodified binary. Info is also
   compared, except its process-specific real-time `now_ns` field.

   | Filter | master compute | d231d18 compute | restored compute |
   |---|---|---|---|
   | none, control | summary | summary | summary |
   | class=IO | raw | raw | raw |
   | event_id=167772181 (IO:DataFileRead) | raw | raw | raw |
   | query_id=100 | raw | raw | raw |
   | event_id=0 (explicit CPU) | **summary** | raw | raw |
   | pid=1000 | raw | raw | raw |
   | class=NotAClass | raw | raw | raw |

   The same route matrix also passed over a genuinely empty 200-second window.
   Raw entries are logged; this is stronger evidence than absence of a summary
   log alone. Full traces: [master](master.routes.stderr.log),
   [d231d18](d231d18.routes.stderr.log), [restored](restored.routes.stderr.log).

   Consequently the **changed refusal is unreachable through d231d18's normal
   Sessions routing**. Master's explicit CPU exception is reachable, but
   master destroys filter presence during parsing, treats it as unfiltered,
   and returns incorrect totals. The CPU presence/routing fix removes that
   exception independently of the summary-result contract.

   This is direct server protocol execution with synthetic disk fixtures, not
   a live PostgreSQL capture or a full BPF/CI gate. The server, routing, loaders,
   summary writer/reader, compute, and JSON serializer are real.

2. **Captured responses and what users see.**

   A valid IO filter is unsupported by the *Sessions summary representation*,
   but supported by the Sessions endpoint through raw fallback. The identical
   request issued to each unmodified binary was:

   ```json
   {"id":3,"cmd":"top_sessions","from":9990000000000,"to":10310000000000,"filters":{"class":"IO"}}
   ```

   Full master response:

   ```json
   {"id":3,"fidelity":"exact","rows":[{"pid":1000,"db_time_ms":150000,"cpu_pct":0,"wait_pct":100,"top_wait":"IO:DataFileRead","top_wait_id":167772181,"type":"client","user":"u","db":"d"}]}
   ```

   Full d231d18 response:

   ```json
   {"id":3,"fidelity":"exact","rows":[{"pid":1000,"db_time_ms":150000,"cpu_pct":0,"wait_pct":100,"top_wait":"IO:DataFileRead","top_wait_id":167772181,"type":"client","user":"u","db":"d"}]}
   ```

   All requests and full responses are retained:
   [master requests](master.production.requests.jsonl),
   [master responses](master.production.responses.jsonl),
   [d231d18 requests](d231d18.production.requests.jsonl),
   [d231d18 responses](d231d18.production.responses.jsonl).
   The normal class-filtered UI displays the same populated table in both.
   The CPU-filtered long-window response changes from 300000 ms and 50% CPU
   to 150000 ms and 100% CPU, as expected for the separate #345 repair.

   To observe the disputed fallback, a **separate, explicit fault-injection
   pass** redirects the raw compute call to the revision's actual summary
   compute function. It supplies the same directory, bounds and parsed filter,
   then lets the real server serializer handle its result. These responses
   **are not responses from normal production routing**. The injected pass
   retains the handler's raw-loaded exact fidelity; it does not pretend to
   change the handler's local `from_summaries` variable.

   Full forced-summary master response for the same class request:

   ```json
   {"id":3,"fidelity":"exact","rows":[]}
   ```

   Full forced-summary d231d18 response:

   ```json
   {"id":3,"error":"top sessions compute failed","code":"compute_failed","hint":"narrow the time range and retry","fidelity":"exact"}
   ```

   [experiment_369_ui.py](../../experiment_369_ui.py) serves each revision's
   actual unmodified web/static files and replays the captured Sessions
   envelopes through WebSocket transport in headless Chromium. The mock
   supplies unrelated app commands. This is a rendering experiment, not a
   live Go-bridge integration test. **All ten rendering cases passed**;
   their replayed envelopes also match the final Linux captures.

   | Case | master UI | d231d18 UI |
   |---|---|---|
   | normal class / CPU filter | populated table | populated table |
   | normal invalid class / empty window | No data for selected range | No data for selected range |
   | forced unsupported class summary | No data for selected range | **compute error card, code, hint and Retry** |

   Screenshots: [master empty fallback](master.forced-summary-class.png),
   [d231d18 error fallback](d231d18.forced-summary-class.png).
   DOM observations: [ui-observations.json](ui-observations.json).

   The failure flag is read by server.c, which emits an error envelope.
   lib/transport.js rejects that envelope, and app.js paints the pane error
   card. sessions.js's empty model does not swallow the error. Thus the
   suggested “identical empty table with an unread failure flag” is falsified.
   The error remains generic: it does not explain unsupported filters, and
   narrowing the range would not repair a summary representation's missing
   dimension if routing were still wrong.

3. **The house rule and the two green-test claims.**

   [Issue #318](https://github.com/DmitryNFomin/pg_wait_tracer/issues/318)
   specifically concerns unsupported **class/event Sessions summary filters**,
   not just missing trace data. It proposes filtering summaries or routing
   to raw. Its premise that per-session event totals exist was corrected in
   [the landed #326 fix](https://github.com/DmitryNFomin/pg_wait_tracer/pull/326)
   and commit 87054128: session summaries do not carry those dimensions;
   route to raw, with no rows as the internal fail-safe if that guard regresses.
   The source explicitly names that empty fallback as the house rule.
   #327 later pins `!failed && num_rows == 0` at test_probe_bounds.c:1018.
   Saved primary texts: [issue-318.json](issue-318.json), [pr-326.json](pr-326.json).

   An unsupported dimension is **inability of this representation to answer**,
   not evidence that there are no matching sessions. An empty summary result
   must not be interpreted as a measured zero. Nevertheless, #318's landed
   implementation deliberately chose no rows as its guarded internal fallback;
   it does cover the disputed case. At the endpoint, valid filters are
   answerable from raw data, so they do not require user-visible refusal.
   Changing this internal contract for a future routing regression would be
   an intentional policy change, not a consequence established by this test.

   | Unchanged pinned test | Outcome |
   |---|---|
   | master | 87 checks, 0 failures |
   | d231d18 | 87 checks, **1 failure**, line 1018 |
   | existing restored revision | 87 checks, 0 failures |

   A separate direct compute probe asserts **all three fields**, for class,
   nonzero event, CPU and query filters with both one record and no records.
   master returns `(0,0,NULL)` for class/event/query; d231d18 returns
   `(1,-1,NULL)` for all four, even with no records; the restored revision
   returns `(0,0,NULL)` for all four. Master's populated CPU case returns a
   row because it cannot represent explicit CPU filter presence.

   Analysis B's green result is reproducible: changing just line 1018's
   expectation in a temporary copy to assert `failed`, `num_rows == -1`,
   and `rows == NULL` makes d231d18 pass **87/87**. The repository test remains
   unchanged. [Changed-expectation log](d231d18.changed-expectation.log).
   This establishes consistency with a new expectation, not compatibility
   with the existing one or a production benefit.

4. **The routing option and preservation of #345.**

   Refusing the unsupported **summary plan**, then falling back to raw, is
   already the endpoint's implemented third option. d231d18 completes its
   CPU-presence guard. Rejecting the whole valid Sessions request would deny
   an answer that raw data can provide. A shared summary-capability predicate
   could make this routing invariant harder to drift; that would be a separate
   change, and none was implemented here. Request validation for invalid class
   names would also be a separate policy; both revisions currently return
   successful empty rows for `NotAClass`.

   Independent exact fixtures demonstrate that #345 does not depend on the
   disputed summary return contract:

   | CPU-filtered request | master | d231d18 | existing restored revision |
   |---|---|---|---|
   | Time Model, 21 ms mixed-work fixture | **21 ms** | **3 ms** | **3 ms** |
   | execution table | **3 executions** | **1 CPU execution** | **1 CPU execution** |
   | execution scatter | **3 executions** | **1 CPU execution** | **1 CPU execution** |

   The mixed-work fixture contains exactly 3 ms CPU, 14 ms IO and 4 ms Lock.
   The separate execution fixture has three starts, only one with CPU work.
   Full requests/responses are in each revision's cpu-work and cpu-executions
   JSONL files. [Machine-readable verdict](summary.json).

   Analysis A therefore has empirical support for preserving the existing
   internal contract while retaining #345. Analysis B accurately describes a
   more explicit internal refusal and a green test under changed expectations;
   it has no observed current routing benefit. A future preference for a loud
   failure if routing regresses is a design decision. The evidence does not
   make that preference a current production requirement.

Execution provenance and cleanup:

- At 06:35 UTC both persistent gate flocks were free. By the nonblocking
  acquisition attempt, gate-2 was held; at 06:38 gate-1's lock belonged to
  runner PID 851660 and its make process 851662. Gate-2 had root sleep PID
  1370826 holding the lock, with CI runner bash/flock 1373900/1373901 queued.
  No test run was started on a persistent gate box.
- Created **170030058**, `2.28.132.229`, from gate snapshot **433799516**.
  DMI serial confirmed the id. Each experiment invocation acquired its flock.
  [Creation](vm-create.log), [final Linux run](linux-run.log).
- Source hashes verified against the exact git revisions: **88 master,
  85 d231d18, 88 restored** source/header/build/test files. Unmodified binary
  hashes are included in [source-provenance.json](source-provenance.json).
- Early harness failures are retained as linux-initial-build,
  linux-initial-comparison, linux-clock-comparison and linux-fixture-duplication
  logs. Fixes used server-build helper objects for the C unit link, preserved
  distinct output filenames, excluded info's real clock from equality, and
  regenerated fixtures in clean directories to avoid archived duplicates.
  Those attempts are not reported as passing. The final run passes its
  experiment assertions while explicitly preserving d231d18's pinned failure.
- Deleted only **170030058**, with API-confirmed deletion:
  [vm-delete.log](vm-delete.log). No other VM was deleted or swept.

Reproduction on a Linux box: export each pinned revision to a separate clean
directory and write its full SHA to `EXPERIMENT_REVISION` in that directory;
run `python3 tests/experiment_369.py MASTER D231D18 OUTPUT [RESTORED]` under
the box flock. Copy its text evidence locally, then run
`python3 tests/experiment_369_ui.py OUTPUT` with Playwright Chromium installed.
The UI script exports the two pinned revisions itself. The focused experiment
is not registered as a permanent unit or live test and does not modify the
production source, the existing test assertion, or any gate tolerance.
