#!/usr/bin/env bash
# test_orphan_guard.sh -- bypass suite for scripts/hooks/orphan-guard.sh
# (PreToolUse hook) and scripts/hooks/orphan-reap.sh (Stop/SubagentStop
# hook). Per the implementer contract's bypass-suite clause: a case per way
# this gate could refuse nothing while looking like it works.
#
# Background: the owner found background shells listed as running for 13h
# and 23h, and a history of eleven detached watcher scripts that polled for
# up to a day -- an agent's literal-compliance workaround for the
# no-polling rule (the poll moved into a background script that outlived
# the agent). orphan-guard.sh refuses the shapes that create this class;
# orphan-reap.sh kills anything that slips through and is still running
# when the (sub)agent stops.
#
# Every case here talks to the real hook scripts over stdin/stdout/exit
# code, the same contract Claude Code itself uses -- there is no mocked
# decision layer to drift from the real one.
#
# Usage: tests/test_orphan_guard.sh (run from anywhere)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$SCRIPT_DIR/.."
GUARD="$REPO_ROOT/scripts/hooks/orphan-guard.sh"
REAP="$REPO_ROOT/scripts/hooks/orphan-reap.sh"

tests_run=0
tests_passed=0
tests_failed=0

report() {
    local ok="$1" msg="$2"
    tests_run=$((tests_run + 1))
    if [[ "$ok" == "true" ]]; then
        tests_passed=$((tests_passed + 1))
        echo "  PASS: $msg"
    else
        tests_failed=$((tests_failed + 1))
        echo "  FAIL: $msg"
    fi
}

# payload SESSION COMMAND RIB(0|1) -> JSON on stdout
payload() {
    python3 -c '
import json, sys
sid, cmd, rib = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
print(json.dumps({"tool_name": "Bash", "session_id": sid,
                   "tool_input": {"command": cmd, "run_in_background": rib}}))
' "$1" "$2" "$3"
}

# assert_refuse NAME SESSION COMMAND RIB [EXPECT_SUBSTRING]
# EXPECT_SUBSTRING pins the test to the SPECIFIC rule that must fire, not
# just "refused for any reason" -- a command can be refused by the wrong
# rule (e.g. falling through to "not an allowed job shape" once the loop
# check is disabled) and a bare exit-code check would not notice.
assert_refuse() {
    local name="$1" sid="$2" cmd="$3" rib="$4" expect="${5:-}" out rc
    out=$(payload "$sid" "$cmd" "$rib" | "$GUARD" 2>&1 >/dev/null)
    rc=$?
    if [[ $rc -eq 2 && -n "$out" ]]; then ok=true; else ok=false; fi
    if [[ -n "$expect" ]]; then
        [[ "$out" == *"$expect"* ]] || ok=false
    fi
    report "$ok" "$name: refused (exit $rc, want 2; expect substring '$expect'; message: ${out:0:70})"
}

# assert_allow NAME SESSION COMMAND RIB
assert_allow() {
    local name="$1" sid="$2" cmd="$3" rib="$4" out rc
    out=$(payload "$sid" "$cmd" "$rib" | "$GUARD" 2>/tmp/pgwt-orphan-test-stderr.$$)
    rc=$?
    local err; err=$(cat "/tmp/pgwt-orphan-test-stderr.$$" 2>/dev/null); rm -f "/tmp/pgwt-orphan-test-stderr.$$"
    if [[ $rc -eq 0 ]]; then ok=true; else ok=false; fi
    report "$ok" "$name: permitted (exit $rc, want 0; stderr: ${err:0:60})"
}

echo "=== detach keywords: nohup / setsid / disown -- anywhere in the command ==="
assert_refuse "nohup + background" s1 'nohup ./watch.sh &' 0 'detaches a process'
assert_refuse "setsid, no trailing &" s1 'setsid ./watch.sh' 0 'detaches a process'
assert_refuse "bare disown" s1 'disown' 0 'detaches a process'
assert_refuse "disown with job spec" s1 'disown %1' 0 'detaches a process'

echo
echo "=== manual shell backgrounding (&) ==="
assert_refuse "unredirected background job (the exact failing input: sleep 3600 &)" s1 'sleep 3600 &' 0 'must redirect BOTH stdout'
assert_allow "redirected background job (both fds -> file)" s1 'some_cmd >log 2>&1 &' 0
assert_refuse "only stdout redirected, stderr open" s1 'some_cmd >log &' 0 'must redirect BOTH stdout'
assert_refuse "only stderr redirected, stdout open" s1 'some_cmd 2>log &' 0 'must redirect BOTH stdout'

echo
echo "=== loop detection must key on shell syntax, not English words inside a quoted argument (review false-refusal finding) ==="
assert_allow "commit message containing the word 'done' is not a loop" s1 'git commit -m "done: fix bug" >log 2>&1 &' 0
assert_allow "status message containing the word 'while' is not a loop" s1 'echo "waiting while build runs" >log 2>&1 &' 0
assert_allow "commit message containing 'for ... in' text is not a loop" s1 'git commit -m "fixed the for loop in worker.py" >log 2>&1 &' 0
assert_refuse "a real while-loop is still refused even though it also contains the word done" s1 'while ! grep -q done log; do sleep 30; done' 1 'poll loop'

echo
echo "=== ssh leaving a remote background job ==="
assert_refuse "ssh remote command backgrounds itself" s1 'ssh root@gatebox "make box-check EPHEMERAL=1 >log 2>&1 &"' 0 'ssh command backgrounds a job'
assert_allow "plain ssh, no remote backgrounding" s1 'ssh root@gatebox "make check"' 0

echo
echo "=== run_in_background: the three real orphans found on the owner's machine ==="
assert_refuse "specimen 1 shape (until ... do sleep 5; done)" s1 \
    'until [ -f /tmp/check-run1.log ] && tail -1 /tmp/check-run1.log | grep -qE "EXIT:|make.*Error|\$"; do sleep 5; done' 1 'poll loop'
assert_refuse "specimen 3 shape (until grep ...; do sleep 90; done)" s1 \
    'until grep -q "unsat(2) 200" /tmp/x/byoszsvju.output; do sleep 90; done' 1 'poll loop'
assert_refuse "substring-lookalike is not the real command (mentions box-check in a grep pattern/path, still a poll loop)" s1 \
    'until grep -q "run_all summary\|=== SUMMARY\|box-check (ubuntu" /tmp/x/box-check-ephemeral-ubuntu-20260927.log; do sleep 90; done' 1 'poll loop'
assert_refuse "while-loop shape" s1 'while ! grep -q done log; do sleep 30; done' 1 'poll loop'
assert_refuse "bare repeated sleep, no loop keyword" s1 'sleep 30; sleep 30; sleep 30' 1 'poll loop'
assert_refuse "run_in_background of an arbitrary non-make command" s1 'python3 long_running.py' 1 'restricted to'

echo
echo "=== run_in_background: the only permitted shapes ==="
assert_allow "make check" s1 'make check' 1
assert_allow "make check-fast" s1 'make check-fast' 1
assert_allow "make box-check with args" s1 'make box-check OS=el8 PG=16 EPHEMERAL=1 KEEP=1' 1
assert_allow "make box-check with redirect" s1 'make box-check EPHEMERAL=1 >box.log 2>&1' 1
assert_allow "make box-check with tee" s1 'make box-check EPHEMERAL=1 2>&1 | tee box.log' 1
assert_allow "make ui-gallery" s1 'make ui-gallery' 1
assert_refuse "'make check' is a PREFIX, not the whole allowed shape: chaining && after it must not slip through" s1 \
    'make check && rm -rf /tmp/should-not-run' 1 'restricted to'

echo
echo "=== ordinary foreground usage is never touched ==="
assert_allow "make check (foreground)" s1 'make check' 0
assert_allow "make box-check (foreground)" s1 'make box-check OS=ubuntu' 0
assert_allow "make ui-gallery (foreground)" s1 'make ui-gallery' 0
assert_allow "our own tests/*.sh (foreground)" s1 'tests/run_all.sh --require-live' 0
assert_allow "our own scripts/*.sh (foreground)" s1 'scripts/box-check.sh' 0
assert_allow "plain read command" s1 'git status' 0

echo
echo "=== the gate itself must refuse, never silently approve, when it cannot see ==="
out=$(printf '' | "$GUARD" 2>&1 >/dev/null); rc=$?
[[ $rc -eq 2 ]] && ok=true || ok=false
report "$ok" "empty stdin: refused rather than silently allowed (exit $rc)"

out=$(printf 'not json at all' | "$GUARD" 2>&1 >/dev/null); rc=$?
[[ $rc -eq 2 ]] && ok=true || ok=false
report "$ok" "unparseable JSON: refused rather than silently allowed (exit $rc)"

out=$(python3 -c 'import json; print(json.dumps({"tool_name":"Bash","session_id":"s1"}))' | "$GUARD" 2>&1 >/dev/null); rc=$?
[[ $rc -eq 0 ]] && ok=true || ok=false
report "$ok" "missing tool_input (absent command, not a broken one): allowed, exit $rc (there is nothing to refuse)"

out=$(python3 -c 'import json; print(json.dumps({"tool_name":"Write","session_id":"s1","tool_input":{"file_path":"/x"}}))' | "$GUARD" 2>&1 >/dev/null); rc=$?
[[ $rc -eq 0 ]] && ok=true || ok=false
report "$ok" "non-Bash tool_name: allowed, exit $rc (this hook only governs Bash)"

# dependency exiting 127 (command not found): python3 missing from PATH.
minimal_path="$(mktemp -d)"
ln -s /bin/bash "$minimal_path/bash"
ln -s /usr/bin/dirname "$minimal_path/dirname" 2>/dev/null || ln -s /bin/dirname "$minimal_path/dirname" 2>/dev/null || true
ln -s /bin/cat "$minimal_path/cat"
ln -s /bin/cut "$minimal_path/cut" 2>/dev/null || true
out=$(payload s1 'ls' 0 | PATH="$minimal_path" bash "$GUARD" 2>&1 >/dev/null); rc=$?
rm -rf "$minimal_path"
[[ $rc -eq 2 ]] && ok=true || ok=false
report "$ok" "python3 (dependency) missing from PATH (exit 127 path): refused rather than silently allowed (exit $rc)"

echo
echo "=== orphan-reap.sh: kills a live registered pid (matching signature), prunes dead/garbage entries ==="
tmp_pids_dir="$(mktemp -d)"

# Spawn a real background process (plain job control -- same idiom as
# tests/test_check_lock.sh's own holder process) and register it the same
# way orphan-guard.sh's marker would: pid TAB "ps -o lstart=,comm=" for
# that pid, taken while it is still running -- plus a dead pid and a
# garbage line in the same registry file.
sleep 30 >/dev/null 2>&1 &
live_pid=$!
live_sig=$(ps -o lstart=,comm= -p "$live_pid" 2>/dev/null)
pids_file="$tmp_pids_dir/reap-test.pids"
{
    printf '%s\t%s\n' "$live_pid" "$live_sig"
    echo "999999999"   # a pid that (almost certainly) does not exist
    echo "not-a-pid"    # garbage line
} > "$pids_file"

kill -0 "$live_pid" 2>/dev/null && ok=true || ok=false
report "$ok" "setup: the live test process is actually running before reap"

reap_out=$(python3 -c 'import json; print(json.dumps({"session_id":"reap-test"}))' | PGWT_ORPHAN_GUARD_DIR="$tmp_pids_dir" bash "$REAP")
reap_rc=$?
report "true" "reaper ran without error (exit $reap_rc): $reap_out" 2>/dev/null
[[ $reap_rc -eq 0 ]] && ok=true || ok=false
report "$ok" "reaper hook exits 0 (side-effecting cleanup, not a decision gate)"

still_alive=1
kill -0 "$live_pid" 2>/dev/null && still_alive=0
[[ $still_alive -eq 1 ]] && ok=true || ok=false
report "$ok" "reaper actually killed the live registered process with a matching signature (pid $live_pid)"

[[ -f "$pids_file" ]] && ok=false || ok=true
report "$ok" "registry file pruned after reap (must not accumulate like the 563 task-record files)"

rm -rf "$tmp_pids_dir"

echo
echo "=== orphan-reap.sh: a RECYCLED pid (registry entry stale, pid now belongs to an unrelated live process) must be refused, never killed ==="
tmp_pids_dir="$(mktemp -d)"

# Process A is what got registered (its signature is what ends up in the
# file). It exits on its own -- long before Stop fires -- exactly like a
# `make box-check` job that finished 30 minutes into a multi-hour session.
sleep 30 >/dev/null 2>&1 &
old_pid=$!
old_sig=$(ps -o lstart=,comm= -p "$old_pid" 2>/dev/null)
kill -9 "$old_pid" 2>/dev/null
wait "$old_pid" 2>/dev/null

# Process B is unrelated and alive -- standing in for whatever the OS
# later handed a recycled/coincident pid to (a git/node/python fork, or in
# this deterministic test, simply a second process). The registry entry
# below combines a REAL, currently-alive pid (B's) with process A's OLD
# signature -- the exact shape of the bug: an alive pid whose recorded
# identity no longer matches what is actually running there. Deliberately
# a DIFFERENT command (`yes`, not `sleep`) so the mismatch is guaranteed
# regardless of `ps -o lstart=` only having one-second resolution -- two
# `sleep` processes started in the same wall-clock second would otherwise
# share an identical signature and this test would not prove anything.
yes >/dev/null 2>&1 &
new_pid=$!
pids_file="$tmp_pids_dir/recycled-test.pids"
printf '%s\t%s\n' "$new_pid" "$old_sig" > "$pids_file"

kill -0 "$new_pid" 2>/dev/null && ok=true || ok=false
report "$ok" "setup: the unrelated live process is running before reap, registry holds a stale (mismatched) signature for its pid"

reap_out=$(python3 -c 'import json; print(json.dumps({"session_id":"recycled-test"}))' | PGWT_ORPHAN_GUARD_DIR="$tmp_pids_dir" bash "$REAP")
report "true" "$reap_out" 2>/dev/null

still_alive=0
kill -0 "$new_pid" 2>/dev/null || still_alive=1
[[ $still_alive -eq 0 ]] && ok=true || ok=false
report "$ok" "reaper did NOT kill the unrelated live process despite its pid being in the registry (signature mismatch)"

[[ -f "$pids_file" ]] && ok=false || ok=true
report "$ok" "the stale entry is still dropped from the registry (mismatch is not re-tried forever)"

{ kill -9 "$new_pid" 2>/dev/null; wait "$new_pid" 2>/dev/null; } 2>/dev/null || true   # test cleanup, not part of the assertion
rm -rf "$tmp_pids_dir"

echo
echo "=== orphan-reap.sh: absent state must not be mistaken for a live process ==="
out=$(python3 -c 'import json; print(json.dumps({"session_id":"no-such-session-ever"}))' | "$REAP" 2>&1); rc=$?
[[ $rc -eq 0 ]] && ok=true || ok=false
report "$ok" "no registry file for this session: exits cleanly, exit $rc (nothing to reap is not an error)"

out=$(printf 'not json' | "$REAP" 2>&1); rc=$?
[[ $rc -eq 0 ]] && ok=true || ok=false
report "$ok" "unparseable stop-hook input: exits cleanly rather than crashing the stop path, exit $rc"

echo
echo "orphan_guard: $tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
