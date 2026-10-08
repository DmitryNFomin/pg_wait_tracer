#!/bin/bash
# What the Sessions tab's row count was in the self-test, per box-check log --
# the quantity the burst tier's connection churn changes, and the one that
# drives screenshot cost. Printed next to whether that run's self-test passed.
for f in /Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/*/tests/results/box-check-*.log; do
    grep -q "self-test -- injecting" "$f" 2>/dev/null || continue
    # The self-test walks ONLY the sessions tab, so its PASS line is the LAST
    # "PASS [sessions]" in the log.
    rows=$(grep -o "PASS \[sessions\].*detail': '[0-9]* rows'" "$f" | tail -1 \
           | grep -o "'[0-9]* rows'" | tr -d "'" )
    if grep -q "SELF-TEST FAIL" "$f"; then v=FAIL; else v=pass; fi
    printf '%-6s %-14s %s\n' "$v" "${rows:-?}" "$(basename "$f")"
done | sort
