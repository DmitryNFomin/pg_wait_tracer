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

if [[ -z "$PM_PID" ]]; then
    PM_PID=$(find_postmaster)
fi

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
        echo "  FAIL: $desc (expected exit $expected, got $actual)"
        failed=$((failed + 1))
    fi
}

# --help exits 0
"$TRACER" --help > /dev/null 2>&1
check_exit 0 $? "--help exits 0"

# --help output contains Usage
output=$("$TRACER" --help 2>&1)
if [[ "$output" == *"Usage"* ]]; then
    echo "  PASS: --help prints Usage"
    passed=$((passed + 1))
else
    echo "  FAIL: --help does not print Usage"
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
        echo "  FAIL: no args (auto-discover) returned exit code $rc: $output"
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
        echo "  FAIL: --pid <explicit target> returned exit code $rc"
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
        echo "  FAIL: valid args returned exit code $rc"
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
            echo "  FAIL: --view $view returned exit code $rc"
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
        echo "  FAIL: --window with valid args returned exit code $rc"
        failed=$((failed + 1))
    fi

    # --window time_model output contains multi-window column headers
    output=$(timeout 15 "$TRACER" --mode full --pid "$PM_PID" --window 1s,3s \
        --interval 1 --count 4 2>/dev/null || true)
    if [[ "$output" == *"Last 1s"* ]]; then
        echo "  PASS: --window time_model output contains window headers"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window time_model output missing 'Last 1s' header"
        failed=$((failed + 1))
    fi
    if [[ "$output" == *"% DB"* ]]; then
        echo "  PASS: --window time_model output contains '% DB' header"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window time_model output missing '% DB' header"
        failed=$((failed + 1))
    fi

    # --window system_event output contains section headers
    output=$(timeout 15 "$TRACER" --mode full --pid "$PM_PID" --view system_event \
        --window 1s,3s --interval 1 --count 4 2>/dev/null || true)
    if [[ "$output" == *"Last 1s"* ]]; then
        echo "  PASS: --window system_event contains 'Last 1s' section"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window system_event missing 'Last 1s' section"
        failed=$((failed + 1))
    fi
    if [[ "$output" == *"Last 3s"* ]]; then
        echo "  PASS: --window system_event contains 'Last 3s' section"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window system_event missing 'Last 3s' section"
        failed=$((failed + 1))
    fi
    if [[ "$output" == *"Wait Event"* ]]; then
        echo "  PASS: --window system_event contains column headers"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window system_event missing column headers"
        failed=$((failed + 1))
    fi

    # --window histogram output contains side-by-side columns
    output=$(timeout 15 "$TRACER" --mode full --pid "$PM_PID" --view histogram \
        --event Client:ClientRead --window 1s,3s --interval 1 --count 4 \
        2>/dev/null || true)
    if [[ "$output" == *"Last 1s"* ]]; then
        echo "  PASS: --window histogram contains 'Last 1s' column"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window histogram missing 'Last 1s' column"
        failed=$((failed + 1))
    fi
    if [[ "$output" == *"Bucket(us)"* ]]; then
        echo "  PASS: --window histogram contains 'Bucket(us)' column"
        passed=$((passed + 1))
    else
        echo "  FAIL: --window histogram missing 'Bucket(us)' column"
        failed=$((failed + 1))
    fi

    # active view output contains correct header and column names
    output=$(timeout 15 "$TRACER" --mode full --pid "$PM_PID" --view active \
        --interval 1 --count 2 2>/dev/null || true)
    if [[ "$output" == *"Active Sessions"* ]]; then
        echo "  PASS: active view has 'Active Sessions' header"
        passed=$((passed + 1))
    else
        echo "  FAIL: active view missing 'Active Sessions' header"
        failed=$((failed + 1))
    fi
    if [[ "$output" == *"Backend Type"* ]]; then
        echo "  PASS: active view has 'Backend Type' column"
        passed=$((passed + 1))
    else
        echo "  FAIL: active view missing 'Backend Type' column"
        failed=$((failed + 1))
    fi
    if [[ "$output" == *"Uptime"* ]]; then
        echo "  PASS: active view has 'Uptime' in header"
        passed=$((passed + 1))
    else
        echo "  FAIL: active view missing 'Uptime' in header"
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
        echo "  FAIL: --view active --sort db_time returned exit code $rc"
        failed=$((failed + 1))
    fi

    # query_event Mode B header (no workload — just verify header text)
    output=$(timeout 15 "$TRACER" --mode full --pid "$PM_PID" --view query_event \
        --event Client:ClientRead --interval 1 --count 2 2>/dev/null || true)
    if [[ "$output" == *"Top Queries for Client:ClientRead"* ]]; then
        echo "  PASS: query_event Mode B has correct header"
        passed=$((passed + 1))
    else
        echo "  FAIL: query_event Mode B missing header"
        failed=$((failed + 1))
    fi

    # query_event Mode C header (no workload — just verify header text)
    output=$(timeout 15 "$TRACER" --mode full --pid "$PM_PID" --view query_event \
        --query-id 12345 --interval 1 --count 2 2>/dev/null || true)
    if [[ "$output" == *"Wait Profile for query_id 12345"* ]]; then
        echo "  PASS: query_event Mode C has correct header"
        passed=$((passed + 1))
    else
        echo "  FAIL: query_event Mode C missing header"
        failed=$((failed + 1))
    fi
else
    echo "  SKIP: no running PG (pass --pid to test valid args)"
fi

echo ""
echo "$passed/$((passed+failed)) tests passed"
[[ $failed -eq 0 ]]
