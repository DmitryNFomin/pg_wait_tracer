#!/bin/bash
# rep10.sh -- replicate the SHIPPED 10 Hz cell n times and report the SIGNED
# CPU-share delta (sampled - exact), because the question is direction, not
# magnitude: a fix that overshoots reads consistently positive, noise straddles
# zero. Prints ABSENT for any run whose CPU line is missing; never a 0.
set -u
REPO="${REPO:?set REPO}"
N="${N:-5}"
RATE="${RATE:-10}"
OUT="${OUT:-/root/rep10}"
mkdir -p "$OUT"
for i in $(seq 1 "$N"); do
    o="$OUT/r$i"
    rm -rf "$o"
    bash /root/diag294.sh "$RATE" "$o" "$REPO" > "$o.log" 2>&1
    rc=$?
    line=$(grep -E "^CPU " "$o.log" | head -1)
    if [[ -z "$line" ]]; then
        echo "REP iter=$i exit=$rc CPU-LINE-ABSENT (run not counted)"
        continue
    fi
    echo "$line" | awk -v i="$i" -v rc="$rc" '{
        ex = $2; sa = $3; gsub("%","",ex); gsub("%","",sa);
        printf "REP iter=%s exit=%s exact=%.1f%% sampled=%.1f%% signed_delta=%+.1f pp %s\n",
               i, rc, ex, sa, sa - ex, (sa - ex > 0 ? "(sampler ABOVE exact)" : "(sampler below exact)")
    }'
done
echo "--- signed deltas above; a systematic overshoot is all-positive ---"
