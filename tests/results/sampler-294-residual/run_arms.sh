#!/bin/bash
# run_arms.sh — issue #294 residual probe: arms A/B/C, n=3 each, INTERLEAVED.
#
# Interleaved (A B C, A B C, A B C) and not blocked (AAA BBB CCC) on purpose:
# the box's own state drifts over 12 minutes (page cache, checkpoint phase,
# autovacuum), so a blocked order aliases arm with time. Interleaving makes the
# drift common-mode across arms.
set -uo pipefail
REPO=/root/probe294/fixtree
ROOT=/root/probe294/results
mkdir -p "$ROOT"
echo "RUN_ARMS start $(date -u +%FT%TZ) fixtree=$(cat $REPO/.FIXTREE_SHA)"
for rep in 1 2 3; do
    for arm in A B C; do
        case "$arm" in
            A) CL=8; RT="" ;;
            B) CL=8; RT=10 ;;
            C) CL=3; RT="" ;;
        esac
        d="$ROOT/arm$arm-r$rep"
        echo "================ ARM $arm rep $rep -> $d ================"
        rm -rf "$d"
        ARM="$arm" CLIENTS="$CL" RT_PRIO="$RT" ESC_SECONDS=60 \
            bash /root/probe294/arm294.sh 10 "$d" "$REPO" 2>&1 | tee "$d.log"
        rc=${PIPESTATUS[0]}
        echo "ARM_EXIT arm=$arm rep=$rep rc=$rc"
        sleep 5
    done
done
echo "RUN_ARMS done $(date -u +%FT%TZ)"
