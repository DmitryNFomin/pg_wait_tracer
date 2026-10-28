#!/bin/bash
# test_cli.sh — Test CLI argument parsing and validation
# Usage: sudo tests/test_cli.sh [--pid POSTMASTER_PID]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRACER="$SCRIPT_DIR/../pg_wait_tracer"
source "$SCRIPT_DIR/testutil.sh"

PM_PID=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID]"; exit 1 ;;
    esac
done

echo "=== test_cli ==="

passed=0
failed=0

check_exit() {
    local expected=$1
    local actual=$2
    local desc="$3"
    if [[ "$actual" -eq "$expected" ]]; then
        echo "  PASS: $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL: $desc (expected exit $expected, rc=$actual)"
        failed=$((failed + 1))
    fi
}

# #495: replay dispatches before PostgreSQL discovery/BPF setup. Keep this
# rendered-text regression independent of PM_PID and fail if tools are missing.
check_replay_cpu() {
    local generator="$SCRIPT_DIR/gen_test_traces"
    local replay_dir output rc stat ms
    local sys_output row total avgus maxus pct
    if [[ ! -x "$generator" ]]; then
        echo "  FAIL: replay CPU fixture requires executable $generator"
        failed=$((failed + 1))
        return
    fi
    if [[ ! -x "$TRACER" ]]; then
        echo "  FAIL: replay CPU text test requires executable $TRACER"
        failed=$((failed + 1))
        return
    fi
    if ! replay_dir=$(mktemp -d); then
        echo "  FAIL: replay CPU fixture could not create temporary directory"
        failed=$((failed + 1))
        return
    fi
    output=$("$generator" -o "$replay_dir" --inline '{
        "cpu_measured": 1,
        "backends": [{"pid": 1000, "type": "client", "user": "postgres", "db": "testdb"}],
        "events": [
            {"pid": 1000, "ts": 1010000000, "dur": 10000000,
             "old": 0, "new": 167772181, "cpu": 4000000},
            {"pid": 1000, "ts": 1020000000, "dur": 10000000,
             "old": 167772181, "new": 0, "cpu": 4000000}
        ]
    }' 2>&1)
    rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "  FAIL: replay CPU fixture generator failed (rc=$rc): $output"
        failed=$((failed + 1))
        rm -rf -- "$replay_dir"
        return
    fi
    output=$("$TRACER" --replay -T "$replay_dir" --view time_model 2>&1)
    rc=$?
    check_exit 0 "$rc" "--replay measured CPU exits 0"
    if [[ $rc -eq 126 || $rc -eq 127 ]]; then
        echo "  replay tracer could not execute (rc=$rc): $output"
    fi
    # Match complete first and second fields: neither an Off-CPU* row nor
    # an explanatory CPU* footnote can satisfy the measured CPU assertion.
    while read -r stat ms; do
        if awk -v stat="$stat" -v ms="$ms" '
            NF == 3 && $1 == stat && $2 == ms { found = 1 }
            END { exit !found }
        ' <<< "$output"; then
            echo "  PASS: --replay $stat row shows $ms ms"
            passed=$((passed + 1))
        else
            echo "  FAIL: --replay $stat row missing $ms ms (rc=$rc): $output"
            failed=$((failed + 1))
        fi
    done <<'ROWS'
CPU* 4.0
Off-CPU* 6.0
IO 10.0
ROWS

    # #531: the per-event table must print the SAME measured CPU* figure the
    # time model does -- it is the same label, from the same command, over the
    # same records. Before this, the two views of this one fixture read
    # "CPU* 4.0 20.0%" (time_model) and "CPU* 1 10.0 10000.0 10000.0 50.0%"
    # (system_event): one number measured, one wall.
    #
    # Matched ABSOLUTELY, field by field -- the measured 4.0 ms against the
    # 10.0 ms wall, and 4000.0 us in the Avg/Max occurrence columns, which a
    # fix that moved only the total would leave at 10000.0. "The two views
    # agree" is deliberately NOT the assertion: it is satisfied by both being
    # wall, which is exactly the state before #495.
    sys_output=$("$TRACER" --replay -T "$replay_dir" --view system_event 2>&1)
    rc=$?
    check_exit 0 "$rc" "--replay system_event exits 0"
    if [[ $rc -eq 126 || $rc -eq 127 ]]; then
        echo "  replay tracer could not execute (rc=$rc): $sys_output"
    fi
    while read -r row total avgus maxus pct; do
        # LC_ALL=C: the view's rule lines are em dashes, and a multibyte
        # locale makes some awks abort on them ("towc: multibyte conversion
        # failure") -- which would fail this assertion for a reason that has
        # nothing to do with the number being checked. Every field compared
        # here is ASCII, so byte semantics are exact.
        if LC_ALL=C awk -v row="$row" -v total="$total" -v avgus="$avgus" \
               -v maxus="$maxus" -v pct="$pct" '
            NF == 6 && $1 == row && $3 == total && $4 == avgus &&
            $5 == maxus && $6 == pct { found = 1 }
            END { exit !found }
        ' <<< "$sys_output"; then
            echo "  PASS: --replay system_event $row = $total ms, $maxus us max, $pct"
            passed=$((passed + 1))
        else
            echo "  FAIL: --replay system_event $row != $total ms / $avgus / $maxus us / $pct (rc=$rc): $sys_output"
            failed=$((failed + 1))
        fi
    done <<'SYSROWS'
CPU* 4.0 4000.0 4000.0 20.0%
IO:DataFileRead 10.0 10000.0 10000.0 50.0%
SYSROWS

    # #498's Off-CPU* row carries no occurrence columns (em dashes), so it
    # needs its own matcher rather than a wider one that a dash could satisfy.
    if LC_ALL=C awk '
        NF == 6 && $1 == "Off-CPU*" && $3 == "6.0" && $6 == "30.0%" { found = 1 }
        END { exit !found }
    ' <<< "$sys_output"; then
        echo "  PASS: --replay system_event Off-CPU* = 6.0 ms, 30.0%"
        passed=$((passed + 1))
    else
        echo "  FAIL: --replay system_event Off-CPU* != 6.0 ms / 30.0% (rc=$rc): $sys_output"
        failed=$((failed + 1))
    fi

    # The structural consequence, and the user-visible one: this view's %DB
    # column has to sum to 100. Off-CPU* is the on-CPU wall MINUS the measured
    # CPU, so a CPU* row still holding the whole wall double-counts the
    # off-CPU part and the column reads 130%. `rows >= 3` is load-bearing: a
    # view that printed nothing (or "(no data yet)") would otherwise sum to 0
    # and satisfy no-overshoot vacuously, which is a gate that cannot see.
    if LC_ALL=C awk '
        NF == 6 && $6 ~ /^[0-9]+\.[0-9]+%$/ { gsub(/%/, "", $6); sum += $6; rows++ }
        END { printf "  %d numeric %%DB rows summing to %.1f%%\n", rows, sum;
              exit !(rows >= 3 && sum > 99.9 && sum < 100.1) }
    ' <<< "$sys_output"; then
        echo "  PASS: --replay system_event %DB column sums to 100.0 over 3 rows"
        passed=$((passed + 1))
    else
        echo "  FAIL: --replay system_event %DB column does not sum to 100.0 over >=3 rows (rc=$rc): $sys_output"
        failed=$((failed + 1))
    fi

    rm -rf -- "$replay_dir"
}
check_replay_cpu

# #612: a DUPLICATED trace file -- a recovered archive, a hand copy, or two
# writers in one directory -- makes one pid's records interleave out of order,
# and the query_attr resolver then attributes a parse wait to the WRONG
# command: silently, and reported as a successful back-fill. pgwt_run_replay
# refuses such a directory rather than pick an order that does not exist.
#
# This is the ONLY test that exercises that refusal at its call site. The
# predicate itself (pgwt_trace_spans_collide, with four negative controls for
# duplicate / byte copy / reboot / rotation-plus-NTP-step) is unit-tested in
# tests/test_replay_window.c, but nothing links pgwt_run_replay -- so without
# this case the gate could be entirely unreachable and every unit control
# would still pass.
check_replay_duplicate_refusal() {
    local generator="$SCRIPT_DIR/gen_test_traces"
    local dir output rc archive copy
    if [[ ! -x "$generator" ]]; then
        echo "  FAIL: duplicate-trace refusal requires executable $generator"
        failed=$((failed + 1))
        return
    fi
    if [[ ! -x "$TRACER" ]]; then
        echo "  FAIL: duplicate-trace refusal requires executable $TRACER"
        failed=$((failed + 1))
        return
    fi
    if ! dir=$(mktemp -d); then
        echo "  FAIL: duplicate-trace fixture could not create a directory"
        failed=$((failed + 1))
        return
    fi
    archive="$dir/2025-01-01_10.trace.lz4"
    copy="$dir/2025-01-01_11.trace.lz4"
    output=$("$generator" -o "$dir" --inline '{
        "backends": [{"pid": 1000, "type": "client", "user": "postgres", "db": "testdb"}],
        "events": [
            {"pid": 1000, "ts": 1010000000, "dur": 10000000,
             "old": 0, "new": 167772181},
            {"pid": 1000, "ts": 1020000000, "dur": 10000000,
             "old": 167772181, "new": 0}
        ]
    }' --rotate 2025-01-01_10 2>&1)
    rc=$?
    if [[ $rc -ne 0 || ! -f "$archive" ]]; then
        echo "  FAIL: duplicate-trace fixture generator failed (rc=$rc): $output"
        failed=$((failed + 1))
        rm -rf -- "$dir"
        return
    fi

    # POSITIVE CONTROL, and it runs FIRST: one legitimate archive must replay.
    # A refusal that fired on every directory would otherwise make the
    # assertion below pass while replay was broken for everyone.
    output=$("$TRACER" --replay -T "$dir" --view time_model 2>&1)
    rc=$?
    if [[ $rc -eq 0 && "$output" != *"overlapping trace files"* ]]; then
        echo "  PASS: --replay accepts a single archive (no false overlap)"
        passed=$((passed + 1))
    else
        echo "  FAIL: --replay refused a legitimate single archive (rc=$rc): $output"
        failed=$((failed + 1))
    fi

    # The same file byte-copied under another hourly name: identical header,
    # so it overlaps itself in BOTH clocks -- the one shape a single daemon
    # cannot produce. A rotation across an NTP step (wall only) and a reboot
    # (monotonic only) must still be accepted; those are the unit controls.
    if ! cp "$archive" "$copy"; then
        echo "  FAIL: duplicate-trace fixture could not copy the archive"
        failed=$((failed + 1))
        rm -rf -- "$dir"
        return
    fi
    output=$("$TRACER" --replay -T "$dir" --view time_model 2>&1)
    rc=$?
    if [[ $rc -ne 0 && "$output" == *"overlapping trace files"* &&
          "$output" == *"2025-01-01_10.trace.lz4"* &&
          "$output" == *"2025-01-01_11.trace.lz4"* ]]; then
        echo "  PASS: --replay refuses a duplicated trace file, naming both"
        passed=$((passed + 1))
    else
        echo "  FAIL: --replay did not refuse a duplicated trace file (rc=$rc): $output"
        failed=$((failed + 1))
    fi

    rm -rf -- "$dir"
}
check_replay_duplicate_refusal

if [[ -z "$PM_PID" ]]; then
    PM_PID=$(find_postmaster)
fi

# --help exits 0
"$TRACER" --help > /dev/null 2>&1
check_exit 0 $? "--help exits 0"

# --help output contains Usage
output=$("$TRACER" --help 2>&1)
rc=$?
if [[ "$output" == *"Usage"* ]]; then
    echo "  PASS: --help prints Usage"
    passed=$((passed + 1))
else
    echo "  FAIL: --help does not print Usage (rc=$rc)"
    failed=$((failed + 1))
fi

# No args with auto-discovery: runs OK if exactly one PostgreSQL instance is
# up, so use --count 1 to limit. With more than one concurrent instance (a
# gate box runs one cluster per PG major on its own port) auto-discovery is
# ambiguous and the daemon correctly refuses rather than guessing at a
# target — that loud refusal is the other half of this contract, so either
# outcome is accepted, but a refusal must carry the expected message.
if [[ -n "$PM_PID" ]]; then
    output=$(timeout 10 "$TRACER" --count 1 --interval 1 2>&1 >/dev/null)
    rc=$?
    if [[ $rc -eq 0 || $rc -eq 124 ]]; then
        echo "  PASS: no args (auto-discover) runs successfully"
        passed=$((passed + 1))
    elif [[ "$output" == *"Multiple PostgreSQL instances found"* ]]; then
        echo "  PASS: no args refuses loudly with multiple PostgreSQL instances up"
        passed=$((passed + 1))
    else
        echo "  FAIL: no args (auto-discover) rc=$rc: $output"
        failed=$((failed + 1))
    fi

    # Same as above but with an explicit target: must always succeed
    # regardless of how many other PostgreSQL instances are running
    # concurrently.
    timeout 10 "$TRACER" --pid "$PM_PID" --count 1 --interval 1 > /dev/null 2>&1
    rc=$?
    if [[ $rc -eq 0 || $rc -eq 124 ]]; then
        echo "  PASS: --pid <explicit target> runs successfully"
        passed=$((passed + 1))
    else
        echo "  FAIL: --pid <explicit target> rc=$rc"
        failed=$((failed + 1))
    fi
fi

# Invalid PID exits non-zero
"$TRACER" --pid 999999999 > /dev/null 2>&1
check_exit 1 $? "invalid PID exits non-zero"

# Invalid view exits non-zero
"$TRACER" --pid "${PM_PID:-1}" --view invalid_view > /dev/null 2>&1
check_exit 1 $? "invalid view exits non-zero"

# Invalid sort mode exits non-zero
"$TRACER" --pid "${PM_PID:-1}" --sort invalid_sort > /dev/null 2>&1
check_exit 1 $? "invalid sort mode exits non-zero"

# Interval 0 exits non-zero
"$TRACER" --pid "${PM_PID:-1}" --interval 0 > /dev/null 2>&1
check_exit 1 $? "interval 0 exits non-zero"

# --window validation: first window must equal interval
"$TRACER" --window 10s,1m,5m --interval 5 > /dev/null 2>&1
check_exit 1 $? "--window first != interval exits non-zero"

# --window validation: windows must be increasing
"$TRACER" --window 5s,5m,1m > /dev/null 2>&1
check_exit 1 $? "--window non-increasing exits non-zero"

# --window validation: invalid suffix
"$TRACER" --window abc > /dev/null 2>&1
check_exit 1 $? "--window invalid value exits non-zero"

# Valid args with --duration 1 runs and exits cleanly
if [[ -n "$PM_PID" ]]; then
    timeout 10 "$TRACER" --mode full --pid "$PM_PID" --interval 1 --duration 2 \
        --view time_model > /dev/null 2>&1
    rc=$?
    if [[ $rc -eq 0 || $rc -eq 124 ]]; then
        echo "  PASS: valid args run successfully"
        passed=$((passed + 1))
    else
        echo "  FAIL: valid args rc=$rc"
        failed=$((failed + 1))
    fi

    # All views work. Pinned to --mode full: these assert exact-tier CLI
    # output shape (histogram buckets, view headers). The default is now
    # tiered (sampled) where EXACT-required views report "unavailable".
    for view in time_model system_event session_event query_event active; do
        timeout 10 "$TRACER" --mode full --pid "$PM_PID" --interval 1 --duration 2 \
            --view "$view" > /dev/null 2>&1
        rc=$?
        if [[ $rc -eq 0 || $rc -eq 124 ]]; then
            echo "  PASS: --view $view works"
            passed=$((passed + 1))
        else
            echo "  FAIL: --view $view rc=$rc"
            failed=$((failed + 1))
        fi
    done

    # Histogram requires --event
    "$TRACER" --pid "$PM_PID" --view histogram > /dev/null 2>&1
    check_exit 1 $? "histogram without --event exits non-zero"

    # --window with valid args works
    timeout 10 "$TRACER" --mode full --pid "$PM_PID" --window 1s,5s --interval 1 --count 2 \
        > /dev/null 2>&1
    rc=$?
    if [[ $rc -eq 0 || $rc -eq 124 ]]; then
        echo "  PASS: --window with valid args works"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window with valid args rc=$rc"
        failed=$((failed + 1))
    fi

    # --window time_model output contains multi-window column headers
    output=$(timeout 15 "$TRACER" --mode full --pid "$PM_PID" --window 1s,3s \
        --interval 1 --count 4 2>/dev/null)
    rc=$?
    if [[ "$output" == *"Last 1s"* ]]; then
        echo "  PASS: --window time_model output contains window headers"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window time_model output missing 'Last 1s' header (rc=$rc)"
        failed=$((failed + 1))
    fi
    if [[ "$output" == *"% DB"* ]]; then
        echo "  PASS: --window time_model output contains '% DB' header"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window time_model output missing '% DB' header (rc=$rc)"
        failed=$((failed + 1))
    fi

    # --window system_event output contains section headers
    output=$(timeout 15 "$TRACER" --mode full --pid "$PM_PID" --view system_event \
        --window 1s,3s --interval 1 --count 4 2>/dev/null)
    rc=$?
    if [[ "$output" == *"Last 1s"* ]]; then
        echo "  PASS: --window system_event contains 'Last 1s' section"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window system_event missing 'Last 1s' section (rc=$rc)"
        failed=$((failed + 1))
    fi
    if [[ "$output" == *"Last 3s"* ]]; then
        echo "  PASS: --window system_event contains 'Last 3s' section"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window system_event missing 'Last 3s' section (rc=$rc)"
        failed=$((failed + 1))
    fi
    if [[ "$output" == *"Wait Event"* ]]; then
        echo "  PASS: --window system_event contains column headers"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window system_event missing column headers (rc=$rc)"
        failed=$((failed + 1))
    fi

    # --window histogram output contains side-by-side columns
    output=$(timeout 15 "$TRACER" --mode full --pid "$PM_PID" --view histogram \
        --event Client:ClientRead --window 1s,3s --interval 1 --count 4 \
        2>/dev/null)
    rc=$?
    if [[ "$output" == *"Last 1s"* ]]; then
        echo "  PASS: --window histogram contains 'Last 1s' column"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window histogram missing 'Last 1s' column (rc=$rc)"
        failed=$((failed + 1))
    fi
    if [[ "$output" == *"Bucket(us)"* ]]; then
        echo "  PASS: --window histogram contains 'Bucket(us)' column"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window histogram missing 'Bucket(us)' column (rc=$rc)"
        failed=$((failed + 1))
    fi

    # active view output contains correct header and column names
    output=$(timeout 15 "$TRACER" --mode full --pid "$PM_PID" --view active \
        --interval 1 --count 2 2>/dev/null)
    rc=$?
    if [[ "$output" == *"Active Sessions"* ]]; then
        echo "  PASS: active view has 'Active Sessions' header"
        passed=$((passed + 1))
    else
        echo "  FAIL: active view missing 'Active Sessions' header (rc=$rc)"
        failed=$((failed + 1))
    fi
    if [[ "$output" == *"Backend Type"* ]]; then
        echo "  PASS: active view has 'Backend Type' column"
        passed=$((passed + 1))
    else
        echo "  FAIL: active view missing 'Backend Type' column (rc=$rc)"
        failed=$((failed + 1))
    fi
    if [[ "$output" == *"Uptime"* ]]; then
        echo "  PASS: active view has 'Uptime' in header"
        passed=$((passed + 1))
    else
        echo "  FAIL: active view missing 'Uptime' in header (rc=$rc)"
        failed=$((failed + 1))
    fi

    # active view with --sort db_time works
    timeout 10 "$TRACER" --mode full --pid "$PM_PID" --view active --sort db_time \
        --interval 1 --count 1 > /dev/null 2>&1
    rc=$?
    if [[ $rc -eq 0 || $rc -eq 124 ]]; then
        echo "  PASS: --view active --sort db_time works"
        passed=$((passed + 1))
    else
        echo "  FAIL: --view active --sort db_time rc=$rc"
        failed=$((failed + 1))
    fi

    # query_event Mode B header (no workload — just verify header text)
    output=$(timeout 15 "$TRACER" --mode full --pid "$PM_PID" --view query_event \
        --event Client:ClientRead --interval 1 --count 2 2>/dev/null)
    rc=$?
    if [[ "$output" == *"Top Queries for Client:ClientRead"* ]]; then
        echo "  PASS: query_event Mode B has correct header"
        passed=$((passed + 1))
    else
        echo "  FAIL: query_event Mode B missing header (rc=$rc)"
        failed=$((failed + 1))
    fi

    # query_event Mode C header (no workload — just verify header text)
    output=$(timeout 15 "$TRACER" --mode full --pid "$PM_PID" --view query_event \
        --query-id 12345 --interval 1 --count 2 2>/dev/null)
    rc=$?
    if [[ "$output" == *"Wait Profile for query_id 12345"* ]]; then
        echo "  PASS: query_event Mode C has correct header"
        passed=$((passed + 1))
    else
        echo "  FAIL: query_event Mode C missing header (rc=$rc)"
        failed=$((failed + 1))
    fi
else
    echo "  SKIP: no running PG (pass --pid to test valid args)"
fi

echo ""
echo "$passed/$((passed+failed)) tests passed"
[[ $failed -eq 0 ]]
