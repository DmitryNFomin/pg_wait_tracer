# Issue #266 — status as of this commit

## Root cause (credible, matches the observed ~1-in-3 rate)

The #252 self-test's injected overlay was shown/hidden via a browser-side
`setTimeout` at absolute times computed **before the sweep ran**:
`showAtMs = mount_at_ms + 700`, `hideAtMs = showAtMs + 400` (window
`[mount+700, mount+1100]`). The targeted sweep sample's real capture lands
at `mount+1008` to `mount+1014` (achieved offset), and the screenshot call
itself costs 210-344ms (mostly Playwright's own stability wait, which
finishes near the END of that call, not the start). That leaves only
~87ms of margin against a 344ms call before the window's close — so the
real pixel-sampled instant could land after the overlay was already
hidden, about 1 run in 3 on identical code.

## The fix (committed, this branch, this commit)

`tests/ui_live_smoke.py`: `_capture_sweep_offset` replaces the scheduled
guess. It shows the overlay **synchronously immediately before** the
capture call it targets, and hides it **synchronously after** that call
returns (holding open for at least the configured `blink_ms` by sleeping
any remainder first). The overlay is guaranteed visible for the real
captured instant by construction, never by a timing bet, regardless of
how long any single capture call takes.

`tests/test_ui_live_smoke_inject.py` (new, 11/11 passing): unit-tests the
show/hide bracket against a fake Page with a virtual millisecond clock —
proves show happens at-or-before the capture starts and hide happens
at-or-after it finishes, including the case where a capture outlasts the
configured `blink_ms`. Demonstrated red against the parent tree
(`AttributeError: module 'ui_live_smoke' has no attribute
'_capture_sweep_offset'`) — confirms the test exercises genuinely new code.

`make check` (full, this tree): **PASSED**, stamp
`aec97bd19185e4917af0fbe243062b9d587d4c0d`.

## What is PROVEN

- The mechanism diagnosis (above), from direct measurement cited in the
  issue plus code reading.
- The fix's internal logic in isolation: the show/hide bracket is
  synchronous and correctly ordered relative to the capture, via unit
  tests with a controllable clock, not a live browser.
- At least one real red control DID run successfully on gate-2 (before
  this VM was spun up) and passed: with the gate deliberately blinded
  (`ratio` forced to `0.0` for the injected tick), the self-test correctly
  reported `SELF-TEST FAIL` / `caught=False`. See
  `tests/results/issue-266-evidence/red-control.log` in this worktree if
  still present, or re-run to reproduce.
- At least 3 live runs on the FIXED tree (gate-2 + one EPHEMERAL
  box-check on PG17) reported `SELF-TEST PASS`, `caught=True`,
  `ratio=0.199796126401631` each time — consistent with the fix catching
  the injected blink reliably when nothing else is wrong.

## What is NOT proven, and why

**The full n>=12 consecutive-pass validation the issue's "done when" bar
requires was NOT completed.** Hetzner is at its account server cap, and
this issue's remaining validation (12 clean runs + a valid live red
control, run standalone to avoid the cost of 12 full box-checks) needs a
dedicated VM for several hours. Against the ~30h merge cutoff for four
queued wiring steps, that trade was judged not worth it — the cheaper
path is to eat the occasional flaky box-check re-run (~2h total) rather
than spend ~8h of VM time proving this now. **Do not claim this fix is
validated at n>=12. It is credible, unit-tested, and has produced
consistent early PASSes, but the statistical bar from the issue is open.**

## A driver bug that cost real time — do not repeat it

My own throwaway evidence-collection script (not part of the committed
fix) called:

```
subprocess.run(['flock', '/tmp/pgwt-box-check.lock', '--',
                 'bash', 'tests/ui_live_smoke.sh', '--pg-version', '18'])
```

`flock FILE -- CMD...` is **invalid syntax** for this `flock` — it does
not accept `--` as a separator between the lockfile and the command; it
tried to execve a program literally named `--` and failed. The log's
entire content was `flock: failed to execute --: No such file or
directory`. **`ui_live_smoke.sh` never ran at all.** This produced
`self_test=MISSING` with `rc=69`, which briefly got reported as "the fix
failed its own red control" — it did not fail; it was never executed. The
correct invocation drops the `--`:
`flock('/tmp/pgwt-box-check.lock', 'bash', 'tests/ui_live_smoke.sh', ...)`.
If you resume this validation, smoke-test the exact subprocess argv
against `flock lockfile bash -c 'echo OK'` BEFORE trusting it to an
unattended multi-run loop — this one cost a full round of confusion.

## To resume this later

1. Get a VM (persistent box when idle, or `make box-check EPHEMERAL=1
   KEEP=1 OS=ubuntu PG=17` when both are busy).
2. Run the live red control once (force `ratio = 0.0` for the injected
   tick in a scratch copy of `ui_live_smoke.py`, confirm `SELF-TEST FAIL`,
   restore). This is the one that decides whether the fix lands — a
   self-test that cannot catch an injected blink is worse than the flake.
3. Run `tests/ui_live_smoke.sh --pg-version N` standalone >=12 times on
   the unmodified fixed tree and report the raw PASS/FAIL tally — do not
   stop early on the first failure, report the full count.
4. Optionally, get a comparison rate on the PARENT tree (pre-`a427f6a`)
   the same way, to measure the before-rate rather than quote the issue.
