#!/bin/sh
# run_atcap_control.sh — the CONTROL for the #327 red evidence.
#
# redgreen-327.txt shows the parent compute.c failing to terminate at all six
# sites. "Exit 124" on its own does not say WHY: a harness that hung for its
# own reasons would look identical. So this rebuilds the SAME driver against
# the SAME parent compute.c with one change — the fixtures hold exactly CAP
# distinct keys instead of CAP+1 — and requires every site to RETURN.
#
# cap+1 hangs, cap returns, same binary otherwise  =>  the missing fullness
# test in the probe loop is the cause, not the fixture or the driver.
#
# usage: run_atcap_control.sh <parent-src-dir> <scratch-dir> [timeout-seconds]
#   <parent-src-dir>  a checkout of the PARENT src/ (git archive <base> src)
set -u
PARENT_SRC="$1"
SCRATCH="$2"
T="${3:-30}"
HERE=$(dirname "$0")
CC="${CC:-cc}"

mkdir -p "$SCRATCH"
# CAP+1 -> CAP in the three N_* fixture sizes. Nothing else differs.
sed 's/ + 1)$/)/' "$HERE/redprobe327.c" > "$SCRATCH/redprobe_atcap.c"
grep -q 'define N_SESSIONS (CAP_SESSIONS)' "$SCRATCH/redprobe_atcap.c" || {
    echo "CONTROL REFUSED: the cap+1 -> cap edit did not apply; this control" >&2
    echo "would then be a second copy of the cap+1 run and prove nothing." >&2
    exit 2
}

$CC -g -O2 -w -I "$PARENT_SRC" -DPGWT_SERVER -o "$SCRATCH/redprobe_atcap" \
    "$SCRATCH/redprobe_atcap.c" "$PARENT_SRC/compute.c" \
    "$PARENT_SRC/wait_event.c" "$PARENT_SRC/idle_rule.c" \
    "$PARENT_SRC/cJSON.c" "$HERE/spawn_stub.c" -lm || {
    echo "CONTROL REFUSED: the at-cap control did not build" >&2
    exit 2
}

echo "=== #327 CONTROL: parent compute.c, fixtures at EXACTLY cap (not cap+1) ==="
echo "parent src: $PARENT_SRC"
rc_all=0
for s in sessions sessions_summary queries queries_summary fingerprints variants; do
    out=$(timeout "$T" "$SCRATCH/redprobe_atcap" "$s" 2>&1)
    rc=$?
    if [ "$rc" -eq 0 ]; then
        v="RETURNED (expected): $(printf '%s' "$out" | grep RETURNED)"
    elif [ "$rc" -eq 124 ]; then
        v="CONTROL FAILED: hung at cap too -- the harness, not the cap, hangs"
        rc_all=1
    else
        v="CONTROL FAILED: exit $rc -- $out"
        rc_all=1
    fi
    printf '  %-18s exit=%-4s %s\n' "$s" "$rc" "$v"
done
exit $rc_all
