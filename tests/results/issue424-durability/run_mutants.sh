#!/bin/bash
# Bypass suite for #424 deliverable 1: each mutant is a way the closure gate
# could stop DETECTING while still looking present. A mutant that produces no
# FAIL line is a hole in the gate.
set -u
WT=/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-afa3ba99b93bf5b11
M=/tmp/pgwt424mut
run() {   # run <name> <sed-script-file>
    local name=$1 script=$2
    rm -rf $M/w; mkdir -p $M/w
    cp $WT/src/event_writer.c $WT/src/event_writer.h $WT/src/trace_closure.h $M/w/
    bash "$script" "$M/w" || { echo "[$name] MUTATION SCRIPT FAILED"; return 1; }
    if ! cc -g -O1 -w -DPGWT_SERVER -I $M/w -I $WT/src -I /opt/homebrew/include \
         -L /opt/homebrew/lib -o $M/w/t $WT/tests/test_trace_closure.c \
         $M/w/event_writer.c -llz4 2>$M/w/cc.log; then
        echo "[$name] BUILD FAILED (counts as detected: the tree does not compile)"
        head -3 $M/w/cc.log
        return 0
    fi
    local out
    out=$($M/w/t 2>/dev/null)
    local rc=$?
    local fails
    fails=$(echo "$out" | grep -c 'FAIL(')
    echo "--- [$name] exit=$rc failing-assertions=$fails"
    echo "$out" | grep 'FAIL(' | head -4
    echo "$out" | grep 'passed ==='
    if [ "$rc" -eq 0 ]; then echo "[$name] *** NOT DETECTED — HOLE IN THE GATE ***"; fi
}
for s in "$@"; do run "$(basename "$s" .sed.sh)" "$s"; done
