#!/bin/bash
# probe294.sh -- measure the RESIDUAL read-order window the fix leaves behind.
#
# Scratch tree only; never committed. For every at-risk target the probe takes
# the fresh gate read TWICE, back to back (T3 then T4, with T4-T3 ~ T3-T2):
#   flip_open   = closed at T3, open at T4   -> a command OPENED in one gap
#   flip_closed = open at T3, closed at T4   -> a command CLOSED in one gap
# flip_closed is the direct estimator of what the shipped fix still loses in
# [T2,T3] (a command open at the wait_event read, closed by the gate read), and
# flip_open is what it still over-counts. It also logs the mean T2->T3 gap in ns
# alongside the mean T1->T2 gap the unfixed code had, so the two windows are
# comparable numbers rather than an argument.
set -u
SRC=/root/pgwt-check/agent_sampler-cmd-gate-order
P=/root/pgwt-check/probe294
rm -rf $P; cp -a $SRC $P
cd $P || exit 1

python3 - <<'PY'
import re
p='src/sampler.c'; s=open(p).read()

# Probe counters + gap accumulators, file-scope in the scratch build only.
s = s.replace("""int pgwt_sampler_recheck_cmd_gate(struct pgwt_sample_target *targets,""",
"""unsigned long long pgwt_probe_flip_open, pgwt_probe_flip_closed,
    pgwt_probe_agree_open, pgwt_probe_agree_closed,
    pgwt_probe_gap2_ns, pgwt_probe_gap2_n,
    pgwt_probe_gap1_ns, pgwt_probe_gap1_n;

int pgwt_sampler_recheck_cmd_gate(struct pgwt_sample_target *targets,""", 1)

# Double read: the SAME injected reader, twice.
s = s.replace("""            if (!open_now) {
                local.confirmed_closed++;
                continue;             /* genuinely between commands */
            }""",
"""            {   /* PROBE: a second fresh read, one gap later. */
                int open2 = 0; uint64_t q2 = 0; int v2 = 0;
                if (read_gate(ctx, i, &targets[i], &open2, &q2, &v2)) {
                    if (!open_now && open2)      pgwt_probe_flip_open++;
                    else if (open_now && !open2) pgwt_probe_flip_closed++;
                    else if (open_now)           pgwt_probe_agree_open++;
                    else                         pgwt_probe_agree_closed++;
                }
            }
            if (!open_now) {
                local.confirmed_closed++;
                continue;             /* genuinely between commands */
            }""", 1)

# T2 -> T3 gap: wall ns from the end of the batched wait_event read to each
# fresh gate read.
s = s.replace("""    if (tick_source_enabled) {
        struct pgwt_sampler_recheck_stats rs;""",
"""    if (tick_source_enabled) {
        extern unsigned long long pgwt_probe_gap2_ns, pgwt_probe_gap2_n;
        uint64_t t2 = mono_ns();
        struct pgwt_sampler_recheck_stats rs;""", 1)
s = s.replace("""        d->counters.cmd_gate_order_read_failed_total += rs.read_failed;
    }""",
"""        d->counters.cmd_gate_order_read_failed_total += rs.read_failed;
        if (rs.at_risk) {
            pgwt_probe_gap2_ns += (mono_ns() - t2);
            pgwt_probe_gap2_n  += rs.at_risk;
        }
    }""", 1)

# T1 -> T2 gap: the window the UNFIXED code decided in -- from the first
# target-loop status read of the tick to the end of the batched read.
s = s.replace("""    uint64_t faults_before = s->read_faults_total;""",
"""    {   /* PROBE: the unfixed code's own window, for comparison. */
        extern unsigned long long pgwt_probe_gap1_ns, pgwt_probe_gap1_n;
        if (n > 0 && probe_tick_loop_start) {
            pgwt_probe_gap1_ns += (mono_ns() - probe_tick_loop_start);
            pgwt_probe_gap1_n  += 1;
        }
    }
    uint64_t faults_before = s->read_faults_total;""", 1)
s = s.replace("""    int n = 0;
    int attr_errno = 0;""",
"""    int n = 0;
    int attr_errno = 0;
    uint64_t probe_tick_loop_start = mono_ns();""", 1)
open(p,'w').write(s)

# Expose the probe counters on the metrics socket.
p='src/control.c'; s=open(p).read()
s = s.replace("""    cjson_add_uint64(root, "cmd_gate_order_at_risk_total",""",
"""    {   extern unsigned long long pgwt_probe_flip_open, pgwt_probe_flip_closed,
            pgwt_probe_agree_open, pgwt_probe_agree_closed,
            pgwt_probe_gap2_ns, pgwt_probe_gap2_n,
            pgwt_probe_gap1_ns, pgwt_probe_gap1_n;
        cjson_add_uint64(root, "probe_flip_open", pgwt_probe_flip_open);
        cjson_add_uint64(root, "probe_flip_closed", pgwt_probe_flip_closed);
        cjson_add_uint64(root, "probe_agree_open", pgwt_probe_agree_open);
        cjson_add_uint64(root, "probe_agree_closed", pgwt_probe_agree_closed);
        cjson_add_uint64(root, "probe_gap2_ns", pgwt_probe_gap2_ns);
        cjson_add_uint64(root, "probe_gap2_n", pgwt_probe_gap2_n);
        cjson_add_uint64(root, "probe_gap1_ns", pgwt_probe_gap1_ns);
        cjson_add_uint64(root, "probe_gap1_n", pgwt_probe_gap1_n);
    }
    cjson_add_uint64(root, "cmd_gate_order_at_risk_total",""", 1)
open(p,'w').write(s)
print("patched")
PY

make -j"$(nproc)" >/tmp/probe294-build.log 2>&1 || {
    echo "PROBE-ERROR: build failed"; tail -25 /tmp/probe294-build.log; exit 1; }
echo PROBE_BUILD_OK
