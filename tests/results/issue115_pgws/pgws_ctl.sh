#!/bin/bash
# pgws_ctl.sh [REPEATS] — two controls for the pg_wait_sampling bias result.
#
# The main run (pgws_run.sh) found, over 3 repeats:
#   saturated    collector bias -2.67 pp of wall, pgsa poller -4.27 pp
#   unsaturated  collector bias -2.02 pp of wall, pgsa poller -1.32 pp
# The unsaturated collector figure cannot be scheduler-induced: unsaturated
# its wakeup->running delay is 8-16 us and the run queue holds 2.9 % of
# on-or-runnable time, so there is almost nothing for the scheduler to
# displace. The remaining candidate is ALIASING. The collector self-corrects
# its period from elapsed time, so it samples at exactly 100.00 Hz; a
# strictly periodic sampler against a quasi-periodic workload (2 clients at
# ~600 transactions/s each = a 1.67 ms cycle, exactly 1/6 of the 10 ms
# period) can lock onto a phase. The pg_stat_activity poller sleeps a fixed
# 20 ms AFTER variable work, so its period jitters and it aliases less.
#
# Both controls change ONLY pg_wait_sampling.profile_period (PGC_SIGHUP, so
# no restart and no other difference between runs):
#   C1  20 ms (50 Hz)   — the poller's rate. If the saturated bias is
#                         unchanged, "it is smaller only because it samples
#                         twice as often" is ruled out by measurement.
#   C2   1 ms (1000 Hz) — ten times the default. Scheduling bias is a
#                         property of WHEN each sample lands, so it should
#                         not move with rate; an aliasing artefact should.
set -uo pipefail
N=${1:-2}
ESC=${ESC:-45}
mkdir -p /root/arb/pgwsctl
run_at() {
    local period=$1 clients=$2 tag=$3 i=$4
    psql -U postgres -p 5418 -d postgres -q \
        -c "ALTER SYSTEM SET pg_wait_sampling.profile_period = $period" \
        -c "SELECT pg_reload_conf()" >/dev/null 2>&1
    sleep 2
    local got
    got=$(psql -U postgres -p 5418 -d postgres -tAc "SHOW pg_wait_sampling.profile_period" | tr -d '[:space:]')
    [[ "$got" == "${period}ms" || "$got" == "$period" ]] || {
        echo "FATAL: profile_period is '$got', wanted $period"; return 1; }
    local D=/root/arb/pgwsctl/$tag$i
    echo "=== $tag$i profile_period=${period}ms clients=$clients esc=$ESC ==="
    ESC=$ESC CLIENTS=$clients OUT=$D bash /root/arb/pgws_capture.sh 2>&1 | tail -2
    ESC_S=$ESC bash /root/arb/pgws_analyze.sh "$D" "$tag$i" > "/root/arb/pgwsctl/$tag$i.txt" 2>&1
    grep -E 'SCHEDULING BIAS\[pgws_probe|^PGWS |eff_rate|^CDELAY' "/root/arb/pgwsctl/$tag$i.txt"
}
for i in $(seq 1 "$N"); do
    run_at 20 8 satp20  "$i"
    run_at  1 8 satp01  "$i"
    run_at 20 2 unsatp20 "$i"
    run_at  1 2 unsatp01 "$i"
done
psql -U postgres -p 5418 -d postgres -q \
    -c "ALTER SYSTEM RESET pg_wait_sampling.profile_period" \
    -c "SELECT pg_reload_conf()" >/dev/null 2>&1
echo "restored profile_period: $(psql -U postgres -p 5418 -d postgres -tAc 'SHOW pg_wait_sampling.profile_period')"
echo "CTL DONE"
