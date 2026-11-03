#!/usr/bin/env bash
# Manual evidence tool: test committed HEAD in disposable detached worktrees.
# Run from anywhere: bash tests/mutants/683_mutants.sh > tests/mutants/683_mutants.out
set -euo pipefail

REPO=$(git -C "$(dirname "$0")/../.." rev-parse --show-toplevel)
REVISION=$(git -C "$REPO" rev-parse HEAD)
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/683-mutants.XXXXXX")
CHECKOUT="$SCRATCH/checkout"
cleanup() {
    if [[ -e "$CHECKOUT/.git" ]]; then
        git -C "$REPO" worktree remove --force "$CHECKOUT"
    fi
    rm -rf "$SCRATCH"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

new_checkout() {
    git -C "$REPO" worktree add --quiet --detach "$CHECKOUT" "$REVISION"
}
remove_checkout() {
    git -C "$REPO" worktree remove --force "$CHECKOUT"
}
# RESOLVE THE INTERPRETER IN THE REAL REPO, use its ABSOLUTE path in the
# checkout. `.python-version` is untracked (it lives in .git/info/exclude), so
# a freshly added worktree does NOT inherit the pyenv pin and its bare
# `python3` is a system interpreter without `websockets`. The first run of
# this script hit exactly that and the baseline refused -- correctly, but for
# an environment reason rather than a mutation one. A pyenv SHIM path would
# not help either: executed inside the checkout it re-reads the directory's
# (absent) pin. sys.executable is the real binary and bypasses both.
PYTHON=$(cd "$REPO" && python3 -c 'import sys; print(sys.executable)')
[[ -x "$PYTHON" ]] || { echo "REFUSE: cannot resolve a python3 in $REPO"; exit 1; }

# The shell-gate cases run live_daemon_lib.sh, which invokes a BARE `python3`
# -- correct in production, where the gate box (provision-runner.sh) and CI
# both put websockets on that interpreter. Inside a bare checkout it resolves
# to a system python3 without it, so the runner supplies the ENVIRONMENT
# rather than changing the library: put the resolved interpreter's directory
# first on PATH for the checkout only.
run_suite() {
    (cd "$CHECKOUT" && PATH="$(dirname "$PYTHON"):$PATH" \
        "$PYTHON" tests/test_bridge_data_ready.py) >"$SCRATCH/suite.log" 2>&1
}

printf '# #683 mutation evidence against commit %s\n' "$REVISION"
new_checkout
if run_suite; then
    printf 'BASELINE PASS: '
    tail -n 1 "$SCRATCH/suite.log"
else
    cat "$SCRATCH/suite.log"
    echo 'REFUSE: BASELINE FAIL; mutation results would be meaningless.'
    exit 1
fi
remove_checkout

survivors=0
for mutant in deadline num_cpus error_envelope reply_id missing_websockets invalid_timeout probe_status dead_bridge call_site_exit; do
    new_checkout
    # Each replacement must match exactly once; missing/drifted inputs REFUSE.
    python3 - "$CHECKOUT" "$mutant" <<'PY'
from pathlib import Path
import sys

root, name = Path(sys.argv[1]), sys.argv[2]
probe = "tests/bridge_data_ready.py"
library = "tests/live_daemon_lib.sh"
mutants = {
    # 1. Expiry cannot approve an upstream that never answers info.
    "deadline": (probe, '    return 1\n\n\ndef main():',
                 '    return 0\n\n\ndef main():'),
    # 2. A matching reply must contain the UI's required num_cpus field.
    "num_cpus": (probe, '''            elif "num_cpus" not in reply:
                # A matching JSON reply without the UI's required field
                # cannot establish #status.connected.
                seen["last"] = "info reply lacks num_cpus"
''', ''),
    # 3. The bridge's reconnecting/error envelopes cannot establish readiness.
    "error_envelope": (probe, '''            elif "error" in reply:
                # b.up=false sends this envelope even over a healthy WS;
                # presence of error refuses, including null/empty errors.
                seen["last"] = "transport error in info reply: %r" % reply["error"]
''', ''),
    # 4. An unsolicited reply cannot satisfy our request id.
    "reply_id": (probe, '''            elif type(reply.get("id")) is not int or reply["id"] != 1:
                # An unsolicited/other request's answer cannot green ours.
                seen["last"] = "info reply has mismatched id: %r" % reply.get("id")
''', ''),
    # 5. Unavailable tooling must fail closed.
    "missing_websockets": (probe,
        '            raise Refusal("missing websockets module (pip install websockets): %s" % exc) from exc',
        '            return 0'),
    # 6. Zero/negative budgets cannot approve an empty observation.
    "invalid_timeout": (probe,
        '            raise Refusal("bad arguments: --timeout must be finite and > 0")',
        '            return 0'),
    # 7. The shell wrapper must propagate a refusing probe's exit status.
    "probe_status": (library, '    wait "$probe_pid"; rc=$?',
                     '    wait "$probe_pid"; rc=0'),
    # 8. An already-dead bridge must refuse before starting the probe.
    "dead_bridge": (library, '''    if ! kill -0 "$pid" 2>/dev/null; then
        # A dead bridge cannot answer; do not spend the deadline polling it.''',
        '''    if false; then
        # A dead bridge cannot answer; do not spend the deadline polling it.'''),
    # 9. The real UI walk call site must exit when the data precondition fails.
    "call_site_exit": ("tests/ui_live_smoke.sh", '''if ! wait_bridge_data_ready "$BASE_URL" "$BRIDGE_PID" "$BRIDGE_LOG"; then
    exit 1
fi''', '''if ! wait_bridge_data_ready "$BASE_URL" "$BRIDGE_PID" "$BRIDGE_LOG"; then
    : # mutant: exit removed
fi'''),
}
filename, before, after = mutants[name]
path = root / filename
source = path.read_text()
matches = source.count(before)
if matches != 1:
    sys.exit("REFUSE mutant %s: search text found %d times in %s (expected 1)"
             % (name, matches, filename))
path.write_text(source.replace(before, after, 1))
PY
    if run_suite; then
        echo "MUTANT $mutant SURVIVED: suite PASS"
        survivors=$((survivors + 1))
    else
        # A crash/import failure alone is not evidence of a caught mutant.
        # Require unittest to identify the failing controls and finish its run.
        python3 - "$mutant" "$SCRATCH/suite.log" <<'PY'
from pathlib import Path
import re
import sys

name, log = sys.argv[1], Path(sys.argv[2]).read_text()
cases = re.findall(r"^(?:FAIL|ERROR): (test_\w+) ", log, re.MULTILINE)
summary = re.search(r"^test_bridge_data_ready: checks=\d+ failures=[1-9]\d*$",
                    log, re.MULTILINE)
if not cases or not summary:
    print(log)
    sys.exit("REFUSE mutant %s: suite failed without completed failing cases" % name)
print("MUTANT %s CAUGHT: suite FAIL; failing cases: %s" % (name, ", ".join(cases)))
PY
    fi
    remove_checkout
done
if [[ "$survivors" != 0 ]]; then
    echo "REFUSE: $survivors of 9 mutants SURVIVED (named above)."
    exit 1
fi
echo 'RESULT: 9 mutants, 0 survivors'
