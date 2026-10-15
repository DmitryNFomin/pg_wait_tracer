#!/bin/bash
# Bypass suite for #424 deliverable 2.
set -u
WT=/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-afa3ba99b93bf5b11
M=/tmp/pgwt424mut
run() {
    local name=$1 script=$2
    rm -rf $M/w2; mkdir -p $M/w2
    cp $WT/src/event_writer.c $WT/src/event_writer.h $WT/src/trace_closure.h \
       $WT/src/open_ckpt.h $M/w2/
    bash "$script" "$M/w2" || { echo "[$name] MUTATION SCRIPT FAILED (anchor not found)"; return 1; }
    if ! cc -g -O1 -w -DPGWT_SERVER -I $M/w2 -I $WT/src -I /opt/homebrew/include \
         -L /opt/homebrew/lib -o $M/w2/t $WT/tests/test_open_ckpt.c \
         $M/w2/event_writer.c -llz4 2>$M/w2/cc.log; then
        echo "[$name] BUILD FAILED (detected: tree does not compile)"; head -3 $M/w2/cc.log; return 0
    fi
    local out; out=$($M/w2/t 2>/dev/null); local rc=$?
    echo "--- [$name] exit=$rc failing=$(echo "$out" | grep -c 'FAIL(')"
    echo "$out" | grep 'FAIL(' | head -3
    echo "$out" | grep 'passed ==='
    [ "$rc" -eq 0 ] && echo "[$name] *** NOT DETECTED — HOLE IN THE GATE ***"
    return 0
}
for s in "$@"; do run "$(basename "$s" .sed.sh)" "$s"; done
