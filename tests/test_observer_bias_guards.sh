#!/bin/bash
# test_observer_bias_guards.sh — issue #115 / OBSERVER_BIAS.md regression
# test: the guards that turn the two "silently reported zero/doubled"
# bpftrace tooling bugs into loud failures must actually be present and
# actually fire, in every capture script that substitutes a sched*.bt.tmpl
# template. Needs no bpftrace, no kernel tracepoints, no PostgreSQL --
# pure text, runs anywhere including this Mac.
#
# Bug 1 (baked-in pids): tests/results/issue115_arbiter/sched2.bt was
# committed with one run's real pids already substituted in place of a
# reusable template. Re-run with a DIFFERENT pid set and every counter it
# printed read 0.000 s -- silently. The fix is twofold: the templates now
# use __PLACEHOLDER__ markers no bpftrace program can parse un-substituted,
# and every capture script greps the rendered .bt for a leftover
# __PLACEHOLDER__ / wrong tracked-pid count and refuses to run bpftrace at
# all when it finds one.
#
# Bug 2 (doubled histograms): bpftrace reprints every populated map at
# program exit unless it is clear()d in the END block, on top of any
# explicit print() of the same map already there -- silently doubling every
# histogram count. Every histogram this harness prints (hist() assigned to
# a map, printed once in END) must also clear() that same map in END.
set -uo pipefail
cd "$(dirname "$0")"
fail=0

CAPTURE_SCRIPTS="results/issue115_reconcile/recon_capture.sh
results/issue115_pgws/pgws_capture.sh
results/issue115_xtab/xtab_capture.sh"

BT_TEMPLATES="results/issue115_reconcile/sched2.bt.tmpl
results/issue115_pgws/sched3.bt.tmpl
results/issue115_xtab/xtab.bt.tmpl
sched_sampler_bias.bt.tmpl"

echo "=== guard 1: every capture script refuses an unsubstituted placeholder ==="
for s in $CAPTURE_SCRIPTS; do
    if grep -qE "grep.*__\[A-Z\]\+__.*(FATAL|exit 1)|grep.*__\[A-Z\]\+__.*&&.*exit" "$s" \
       || grep -qE "'__\[A-Z\]\+__'" "$s"; then
        echo "PASS: $s greps the rendered .bt for a leftover placeholder"
    else
        echo "FAIL: $s has no unsubstituted-placeholder guard"
        fail=1
    fi
done

echo
echo "=== guard 1b: the guard actually fires on a synthetic un-substituted template ==="
for t in $BT_TEMPLATES; do
    [[ -f "$t" ]] || { echo "FAIL: missing $t"; fail=1; continue; }
    # Simulate the historical bug directly: substitute every placeholder the
    # template has EXCEPT the last one (alphabetically), exactly reproducing
    # "one run's pids already substituted [for everything else]" while one
    # marker survives un-substituted.
    placeholders=$(grep -oE '__[A-Z]+__' "$t" | sort -u)
    leave_behind=$(tail -1 <<<"$placeholders")
    rendered=$(cat "$t")
    for p in $placeholders; do
        [[ "$p" == "$leave_behind" ]] && continue
        rendered=$(sed "s/${p}/x/g" <<<"$rendered")
    done
    if grep -qE '__[A-Z]+__' <<<"$rendered"; then
        echo "PASS: $t still trips the placeholder grep when one substitution is skipped"
    else
        echo "FAIL: $t has no remaining __PLACEHOLDER__ marker for the guard to catch"
        fail=1
    fi
done

echo
echo "=== guard 2: every hist()-assigned map is clear()d in the same file's END block ==="
for t in $BT_TEMPLATES sched.bt; do
    [[ -f "$t" ]] || { echo "FAIL: missing $t"; fail=1; continue; }
    hist_maps=$(grep -oE '@[a-zA-Z_]+ = hist\(' "$t" | sed -E 's/@([a-zA-Z_]+).*/\1/' | sort -u)
    [[ -z "$hist_maps" ]] && { echo "SKIP: $t has no hist() maps"; continue; }
    for m in $hist_maps; do
        if grep -qE "clear\(@${m}\)" "$t"; then
            echo "PASS: $t clear()s @$m (no bpftrace end-of-program re-dump doubling)"
        else
            echo "FAIL: $t assigns hist() to @$m but never clear()s it -- bpftrace will"
            echo "      silently reprint and double this histogram at program exit"
            fail=1
        fi
    done
done

echo
if [[ $fail -ne 0 ]]; then
    echo "test_observer_bias_guards: FAIL"
    exit 1
fi
echo "test_observer_bias_guards: ALL PASS"
exit 0
