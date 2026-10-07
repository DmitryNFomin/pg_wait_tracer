#!/bin/bash
# run_arms.sh — issue #294 residual probe: arms A/B/C, n=3 each, INTERLEAVED.
#
# Interleaved (A B C, A B C, A B C) and not blocked (AAA BBB CCC) on purpose:
# the box's own state drifts over ~15 minutes (page cache, checkpoint phase,
# autovacuum), so a blocked order aliases arm with time. Interleaving makes the
# drift common-mode across arms.
#
# SETTLE FIRST. Interleaving equalises arm vs arm, but it does NOT equalise
# rep 1 against rep 3 while a previous job's load is still decaying -- and rep 1
# arm A would absorb the most of it. So wait for the 1-minute load average to
# fall below a threshold before the first arm, and record /proc/loadavg next to
# every arm so the claim "the box was quiet" is checkable per run rather than
# asserted once at the top.
set -uo pipefail
REPO=/root/probe294/fixtree
ROOT=/root/probe294/results
SETTLE_MAX_LOAD="${SETTLE_MAX_LOAD:-0.35}"
mkdir -p "$ROOT"

echo "RUN_ARMS start $(date -u +%FT%TZ) fixtree=$(cat $REPO/.FIXTREE_SHA)"
echo "RUN_ARMS host nproc=$(nproc) kernel=$(uname -r) cpu=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)"

settled=no
for i in $(seq 1 60); do
    l1=$(cut -d' ' -f1 /proc/loadavg)
    if awk -v a="$l1" -v b="$SETTLE_MAX_LOAD" 'BEGIN{exit !(a<b)}'; then
        echo "RUN_ARMS settled after ${i}0s: loadavg1=$l1 < $SETTLE_MAX_LOAD"
        settled=yes; break
    fi
    sleep 10
done
if [[ "$settled" != yes ]]; then
    # A box that never goes quiet must not be measured silently: that is the
    # same contamination that disqualified gate-2.
    echo "RUN_ARMS-ERROR: loadavg1 still $(cut -d' ' -f1 /proc/loadavg) after 600s" >&2
    echo "RUN_ARMS-ERROR: refusing to measure on a box that will not go idle" >&2
    exit 1
fi

for rep in 1 2 3; do
    for arm in A B C; do
        case "$arm" in
            A) CL=8; RT="" ;;
            B) CL=8; RT=10 ;;
            C) CL=3; RT="" ;;
        esac
        d="$ROOT/arm$arm-r$rep"
        echo "================ ARM $arm rep $rep -> $d ================"
        echo "PRE_LOADAVG $(cat /proc/loadavg)"
        rm -rf "$d"
        ARM="$arm" CLIENTS="$CL" RT_PRIO="$RT" ESC_SECONDS=60 \
            bash /root/probe294/arm294.sh 10 "$d" "$REPO" 2>&1 | tee "$d.log"
        rc=${PIPESTATUS[0]}
        echo "ARM_EXIT arm=$arm rep=$rep rc=$rc"
        echo "POST_LOADAVG $(cat /proc/loadavg)"
        sleep 10
    done
done
echo "RUN_ARMS done $(date -u +%FT%TZ)"
