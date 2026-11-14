#!/usr/bin/env bash
# Intentionally small construction gate. Optional root supports a seeded copy.
set -eu
root=${1:-$(cd "$(dirname "$0")/.." && pwd)}
status=0
grep -nE 'PGWT_EVIDENCE_(ATTACH_OK|FRONTIER|END)([^[:alnum:]_]|$)' \
    "$root/src/evidence_capture.c" "$root/src/daemon.c" && status=1 || {
        result=$?
        if [ "$result" -ne 1 ]; then exit "$result"; fi
    }
if [ "$status" -ne 0 ]; then
    echo 'FAIL: capture names a forbidden certificate kind'
    exit 1
fi
echo 'capture certificate-kind gate: PASS'
