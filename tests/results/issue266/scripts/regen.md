# How the issue #266 evidence was produced

Everything below ran on a throwaway Hetzner **cx33** created from the
`pgwt=gate-snapshot` image (`pgwt-266-blink`, id 170060361, deleted at the
end of the run). The two persistent gate boxes were both holding
`/tmp/pgwt-box-check.lock` for CI at the time (loads 4.64 and 1.16, each with
a live `Runner.Worker`), which is the documented condition for using a
throwaway VM.

`make box-check` was **not** run: this is a self-test reliability
investigation needing many repetitions of one tab's walk, not one pass of the
whole live tier.

## 1. The box-local repeat driver

`tests/ui_live_smoke.sh` brings up pgbench, the lock/sleep workload, the
`--mode full` daemon and the Go bridge, then runs the 11-tab walk followed by
one self-test. Repeating the self-test 14 times needs the stack up once, so a
copy of that script was placed on the box with exactly two edits:

* the 11-tab walk invocation replaced by `SMOKE_RC=0` (it is not what is
  being measured, and it costs ~10 min per iteration), and
* the single self-test invocation wrapped in a `for` loop over
  `$PGWT_SELFTEST_REPEAT`, each iteration writing its own
  `tests/results/ui_live_self_test_$n`.

Nothing else differs -- the whole start-up/teardown path, the daemon flags,
the workload and the `ui_live_smoke.py` command line (`--inject-blink-ms
400`, defaults for everything else) are the file's own. `scripts/
ui_live_smoke_repeat.patch` here is the diff, so the driver can be rebuilt
from whatever `tests/ui_live_smoke.sh` says at the time.

    rsync -az --delete --exclude .git --exclude tests/results ./ root@BOX:/root/pgwt-266/
    ssh root@BOX 'cd /root/pgwt-266 && flock /tmp/pgwt-box-check.lock \
        bash -c "make -j4 && make -C tests && make pgwt-client"'
    # BEFORE: the parent commit; AFTER: the fix. 14 iterations each.
    ssh root@BOX 'cd /root/pgwt-266 && flock /tmp/pgwt-box-check.lock \
        sudo env PGWT_SELFTEST_REPEAT=14 tests/ui_live_smoke_repeat.sh --pg-version 17'

## 2. The tables

    python3 scripts/extract.py before before/
    python3 scripts/extract.py after  after/

`tables/` holds the output of exactly those two commands.

## 3. The deliberate reds

Mac, `--mock` (the same `ui_live_smoke.py`, against `tests/mock_server.py`):

    # old design, window moved 700 -> 600ms, nothing else changed
    git -C <parent-checkout> show 6bfe7d3:tests/ui_live_smoke.py   # then s/+ 700/+ 600/
    python3 tests/ui_live_smoke.py --mock --inject-blink-ms 400    # -> ratio 0.0, exit 1

    # new design: a target frame with no preceding frame -> REFUSES
    python3 tests/ui_live_smoke.py --mock --inject-blink-ms 400 --inject-frame 0

    # new design: an overlay that covers no pixels -> geometry proven, NOT caught
    python3 tests/ui_live_smoke.py --mock --inject-blink-ms 400 --inject-blink-frac 0.0

    # new design: the last sweep frame as the target -> still proven, still caught
    python3 tests/ui_live_smoke.py --mock --inject-blink-ms 400 --inject-frame 4

And the unit tests against the parent tree, where both new names are absent:

    python3 tests/test_ui_live_smoke_lib.py   # AttributeError: inject_stimulus_verdict
    python3 tests/test_ui_live_smoke_nav.py   # AttributeError: _BlinkInjector

## Note on the Mac vs the box

The Mac's panel capture costs 17-73 ms; the VM's 103-264 ms; `pgwt-gate`'s
210-344 ms (issue #266's own measurement). The flake therefore does NOT
reproduce on the Mac at all, and did not reproduce on this VM either (14/14
before-runs passed) -- which is the whole reason the before/after evidence
here is the measured GEOMETRY, with a pass count as a secondary check, rather
than a pass count alone.
