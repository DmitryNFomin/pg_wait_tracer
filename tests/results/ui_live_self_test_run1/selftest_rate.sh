#!/bin/bash
# Base rate of the live-UI-smoke self-test's failure across every box-check log
# on this Mac that actually RAN the self-test.
tot=0; fail=0; pass=0
for f in /Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/*/tests/results/box-check-*.log; do
    grep -q "self-test -- injecting" "$f" 2>/dev/null || continue
    tot=$((tot + 1))
    if grep -q "SELF-TEST FAIL" "$f"; then
        fail=$((fail + 1))
        echo "FAIL  $f"
    else
        pass=$((pass + 1))
        echo "pass  $(basename "$f")"
    fi
done
echo
echo "box-check logs that ran the self-test: $tot (passed $pass, failed $fail)"
