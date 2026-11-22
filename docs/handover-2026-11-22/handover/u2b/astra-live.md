VERIFIED reference convention: repository paths refer to fb43c3c6; `T:` means ecd3f611, read with `git show HEAD:<path>`; `scratchpad/` is your supplied directory.

VERIFIED — **1. The verdict is right for the explicit request.** `complete` and `live_edge` require `from ≥ S`; rows 23 and 24c deliberately reject pre-capture/restart-spanning windows. Live currently supplies `now − span` unconditionally. (`scratchpad/m871/spec-coverage-label-r2.md:64–67,117–118`; `web/static/lib/state.js:101–105,137–141`)

INFERRED — **Prefer the web fix:** select `[max(now−span, currentBootS), now)`, using `info.from_ns`, conservatively rounded upward to a microsecond with a double-rounding guard. Apply it on initial load, Live activation and refresh/restart. Window selection belongs to the UI; completeness still belongs exclusively to the existing verdict.

VERIFIED — The default server bounds already select the latest boot and derive its S. The pasted CONTROLLING SCOPE DECISION says: “One completeness verdict is computed in one place and every view uses it.” (`src/coverage_trace.c:397–420`; `scratchpad/u2b/brief-u2b.md:47`)

INFERRED — A server-resolved **explicit live-span request** could preserve that same authority, but requires a protocol/spec amendment; silently clipping ordinary explicit windows would violate the existing contract. Neither approach requires a second verdict.

INFERRED — When left-clipped, say **“Live — since capture start HH:MM:SS”**, making clear that the displayed interval is shorter than the selected span. When retention determines S, use “since retained data begins” instead. Never imply that omitted time contributed zero.

VERIFIED — Right clipping already has prescribed wording: numbers are labelled “up to HH:MM:SS”, and the unconfirmed tail remains missing. (`scratchpad/m871/spec-coverage-label-r2.md:183`)

INFERRED — **2. Default Live should start at the current boot.** Users regain current-boot numbers once coverage advances, with a visible restart/start-time notice; previous-boot numbers leave the displayed interval. Keeping the rolling window across restart instead preserves historical context but withholds whole-window metrics until the restart leaves that window. Explicit historical selections should retain restart withholding.

VERIFIED — **3. The real-daemon Live UI gate is the relevant gate**, registered in `tests/run_all.sh:708–713`; its present preparation substitutes a covered URL span before evaluating the page. (`tests/ui_live_smoke.py:694–712,1530–1535`)

INFERRED — Add a mandatory untouched-default-browser case on a young healthy capture, then restart the daemon without changing the user’s controls. Assert visible numbers, actual request bounds, clipping labels and refresh recovery; exercise all ten whole-window routes. Removing the production clamp must make this case red. Keep an explicit cross-restart request as a separate withholding control.

VERIFIED — **The false retention reason follows the specification:** the writer stores boot start separately from file observation; the reader unconditionally assigns `retention` to `[boot_start,S)`. A1 describes retention as leading files being removed. (`src/open_snapshot.c:605`; `src/event_writer.c:903–920`; `src/coverage.c:546`; `scratchpad/m871/spec-coverage-label-r2.md:169`)

INFERRED — **Smallest correct fix: amend the reason rule, then change the reader.** Require evidence of an absent leading file—earliest retained `file_no > 0`—before calling that prefix retention. With file 0 present, keep S conservative and classify the startup prefix as `not_captured`. Changing timestamps merely to eliminate the label would conceal the distinction.

INFERRED — Test identical delayed-BOOT timelines with earliest file 0 versus file 1: the first prefix must be incomplete/`not_captured`, the second incomplete/`retention`; `[S,W)` remains eligible in both. Preserve the existing internal-missing-file case and verify rendered reason text.

VERIFIED — **4. No sustained idle frontier progress is guaranteed.** With no queued/staged work or outstanding round, transport returns without waking helpers; `coverage_cursor` advances only on successful FRONTIER output. The core nevertheless supports advancing through empty batches. (`T:src/percpu_transport.c:648–657`; `T:src/merge_capture.c:278–296,904–905`; `T:src/core/merge.c:668–674`)

VERIFIED — Coverage ticks check counters and promote pending W only when certified-through reaches it; they do not request fresh boundaries. The supplied EL8 log shows the loaded SIGKILL arm passing and the idle startup assertion failing. (`T:src/open_snapshot.c:542–549,768–783`; `scratchpad/a2go/tcov/g1/el8-ecd3f611.log:17988,18000–18003`)

INFERRED — **Smallest mechanism change:** make each 1 s coverage tick request progress toward its pending certification target, reusing/coalescing an existing round. Require the full E/L boundaries, higher-round CPU acknowledgements, counter check and publication before promoting W. Neither wall time nor empty buffers alone certifies idle time.

VERIFIED — Fresh heartbeat observations keep a stalled boot live; its tail is `not_yet_covered`, not automatically `unclean_end`. The pre-existing-session test additionally requires its startup capture-loss gap to remain open. (`scratchpad/m871/spec-coverage-label-r2.md:60–67,120`; `T:tests/test_exact_perf_coverage_live.py:240–250`)

INFERRED — Test sustained idle startup and load→idle progress, plus a withheld boundary acknowledgement that must prevent promotion; retain startup-gap assertions. Measure helper CPU on EL8 and Ubuntu against the provisional ≤1%/helper budget.

VERIFIED / INFERRED — The pasted owner decision explicitly says “nothing is woken on an idle box with nothing waiting”; ≤1%/helper was a proposed budget. Therefore periodic certification demand changes that literal policy and needs an explicit exception; meeting the CPU budget alone does not authorize it. (`scratchpad/u2b/brief-u2b.md:25,36`)