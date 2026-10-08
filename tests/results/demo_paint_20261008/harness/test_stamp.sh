#!/usr/bin/env bash
# Bypass suite for stamp.py's workload_alive rule. Pure: a temp walk JSON and
# synthetic conditions files, no box, no Chrome, no PostgreSQL.
#
# Why it exists: workload_alive is the field paint_analyse.py refuses on, and
# on 2026-10-08 it was computed by `alive == 3` -- a magic number coupled by
# convention to another script's loop body. Adding the burst tier made the real
# count 4, and a perfectly valid walk was refused. The safe failure direction
# that time was luck: `alive >= 3` would instead have APPROVED a walk with the
# burst tier dead. Every case below is a way the rule could approve something
# it should not, or refuse something it should not.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
W=$(mktemp -d "${TMPDIR:-/tmp}/pgwt_stamp_test_XXXXXX")
trap 'rm -rf "$W"' EXIT
pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $*"; fail=$((fail + 1)); }

alive_lines() { for i in $(seq 1 "$1"); do echo "alive $((1000 + i)): proc$i"; done; }

# run_case LABEL EXPECT_TRUE CONDITIONS_TEXT
run_case() {
    local label="$1" expect="$2" cond="$3"
    local walk="$W/walk.json" cf="$W/cond.txt"
    echo '{"label":"t","results":{}}' >"$walk"
    printf '%s\n' "$cond" >"$cf"
    python3 "$HERE/stamp.py" "$walk" "$cf" >/dev/null 2>&1
    local got
    got=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['workload_alive'])" "$walk")
    if [[ "$got" == "$expect" ]]; then
        ok "$label -> workload_alive=$got"
    else
        bad "$label -> workload_alive=$got, expected $expect"
        python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print('        basis:', d.get('workload_alive_basis'))" "$walk"
    fi
}

echo "stamp.py workload_alive rule"

# The real 2026-10-08 input: four tiers, all alive, count emitted.
run_case "4 alive, procs_expected=4 (the real r1 input)" True \
    "capture_age_s=1112
procs_expected=4
$(alive_lines 4)"

# The exact regression: the count grows, the rule must follow it.
run_case "5 alive, procs_expected=5 (a fifth tier added later)" True \
    "capture_age_s=10
procs_expected=5
$(alive_lines 5)"

# A dead tier must refuse however many are alive.
run_case "3 alive + 1 DEAD, procs_expected=4" False \
    "capture_age_s=10
procs_expected=4
$(alive_lines 3)
DEAD 9999 -- this run's numbers are NOT valid"

# Fewer alive than expected: a tier never started.
run_case "3 alive, procs_expected=4 (a tier never started)" False \
    "capture_age_s=10
procs_expected=4
$(alive_lines 3)"

# The gate-cannot-see cases: a count of zero must never be satisfiable, and
# "0 == 0" is exactly how such a rule accidentally approves nothing-checked.
run_case "procs_expected=0, nothing checked" False \
    "capture_age_s=10
procs_expected=0"
run_case "procs_expected=1, one tier only" False \
    "capture_age_s=10
procs_expected=1
$(alive_lines 1)"
run_case "procs_expected unparseable" False \
    "capture_age_s=10
procs_expected=abc
$(alive_lines 4)"

# Legacy files (no count line): the floor still refuses a short list.
run_case "legacy: 4 alive, no procs_expected" True \
    "capture_age_s=10
$(alive_lines 4)"
run_case "legacy: 3 alive, no procs_expected" False \
    "capture_age_s=10
$(alive_lines 3)"
run_case "legacy: 4 alive + 1 DEAD, no procs_expected" False \
    "capture_age_s=10
$(alive_lines 4)
DEAD 9999 -- not valid"

# Empty and missing conditions: an absent answer is not a yes.
run_case "empty conditions file" False ""

walk="$W/walk2.json"
echo '{"label":"t","results":{}}' >"$walk"
python3 "$HERE/stamp.py" "$walk" "$W/does-not-exist.txt" >/dev/null 2>&1
got=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['workload_alive'])" "$walk")
if [[ "$got" == "False" ]]; then
    ok "missing conditions file -> workload_alive=False"
else
    bad "missing conditions file -> workload_alive=$got, expected False"
fi

echo
echo "stamp.py: $pass passed, $fail failed"
[[ $fail -eq 0 ]] || exit 1
