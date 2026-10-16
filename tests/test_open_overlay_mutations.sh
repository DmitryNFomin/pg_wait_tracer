#!/usr/bin/env bash
# test_open_overlay_mutations.sh — can tests/test_open_overlay.c actually SEE?
#
# A gate going green proves nothing about a gate nobody has watched fail. The
# defect this repo keeps finding is the FALSE NEGATIVE: a check that was
# unreachable, skipped, or satisfied without checking anything. So this suite
# breaks src/open_overlay.c one guard at a time and requires the unit test to
# go RED for every single break:
#
#   - the uniform precedence rule, so a closed interval is counted twice
#   - the clip to the snapshot, so time past the observation is invented
#   - the absent `pids` array read as "nothing open"
#   - the route/tier refusal, so the summary route double counts
#   - the io_worker exclusion
#   - unmeasured CPU degraded into a measured zero
#   - an uninitialised overlay reporting `included`
#   - a degraded overlay rendering a zero in-flight total instead of absence
#   - the committed_through coverage check
#   - complete:false accepted
#   - the OVERALL socket deadline degraded into a per-I/O one (this one HANGS,
#     and a hang is counted as a failure, never as a pass)
#   - markers treated as interval closes
#   - the daemon-generation check
#   - per-entry field validation
#   - the admitted-record view treated as optional
#   - exact u64 string parsing degraded through a double
#   - the same-pid overlap guard
#
# Plus one NEGATIVE control: a comment-only edit must leave the test GREEN.
# Without that, "every edit fails" would look identical to "every guard works".
#
# Pure C and a compiler; no Linux, no BPF, no PostgreSQL. Runs on the Mac tier
# and on the box. If the compiler is missing the suite FAILS — a bypass suite
# that skips itself is the very thing it exists to prevent.
set -u

cd "$(dirname "$0")" || exit 2
SRC=../src/open_overlay.c
TEST=test_open_overlay.c
CC_BIN=${CC:-cc}

fail=0
pass_n=0

if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "REFUSING: no C compiler ($CC_BIN) — cannot prove the overlay test can fail"
    exit 1
fi
for f in "$SRC" "$TEST" ../src/cJSON.c ../src/open_overlay.h; do
    [ -f "$f" ] || { echo "REFUSING: missing $f"; exit 1; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/pgwt_overlay_mut.XXXXXX") || exit 2
trap 'rm -rf "$work"' EXIT

# Run a command with a hard watchdog. A mutant that HANGS (e.g. the overall
# deadline turned into an unbounded poll) must be reported as detected, not
# waited on forever and not silently skipped.
run_bounded() {   # run_bounded <seconds> <cmd...>
    local secs=$1; shift
    "$@" >"$work/run.log" 2>&1 &
    local pid=$!
    ( sleep "$secs"; kill -9 "$pid" 2>/dev/null ) >/dev/null 2>&1 &
    local wd=$!
    wait "$pid"; local rc=$?
    kill "$wd" 2>/dev/null; wait "$wd" 2>/dev/null
    return $rc
}

build_and_run() {  # build_and_run <mutated-source> <binary>; echoes verdict
    local src=$1 bin=$2
    if ! "$CC_BIN" -g -O1 -w -I../src -o "$bin" "$TEST" "$src" ../src/cJSON.c \
            >"$work/build.log" 2>&1; then
        echo "BUILD-FAILED"
        return
    fi
    if run_bounded 25 "$bin"; then
        echo "GREEN"
    else
        echo "RED"
    fi
}

# ── 0. the control: the real source must build and pass ─────────
verdict=$(build_and_run "$SRC" "$work/control")
if [ "$verdict" != "GREEN" ]; then
    echo "REFUSING: the unmutated overlay test is not green ($verdict)"
    sed -n '1,40p' "$work/build.log" "$work/run.log" 2>/dev/null
    exit 1
fi
echo "control (unmutated)                     : GREEN as required"

# ── the mutation table ──────────────────────────────────────────
# name<TAB>perl-expression   (expected RED unless the name starts with "control")
mutations=$(cat <<'MUTS'
precedence-rule-blind	s@if \(s && s->seen && s->max_wall_ns > s_from\)@if (0)@
clip-past-snapshot	s@uint64_t end_cap = req->to_wall_ns < snapshot_wall \? req->to_wall_ns : snapshot_wall;@uint64_t end_cap = req->to_wall_ns;@
absent-pids-is-empty	s@if \(!cJSON_IsArray\(pids\)\) \{@if (0) {@
route-tier-gate-off	s@if \(!pgwt_overlay_wanted\(req, why, sizeof\(why\)\)\) \{@if (0) {@
io-worker-included	s@if \(it->cat_flag & PGWT_EVENT_FLAG_IO_WORKER\)@if (0)@
absent-cpu-becomes-measured	s@            else if \(snap->cpu_accounting && !has_sent\)@            else if (0)@
cpu-drift-check-off	s@            else if \(snap->cpu_accounting && has_sent && sent != it->cpu_ns\)@            else if (0)@
cpu-span-clamp-off	s@            sp->cpu_ns = c > sp->wall_ns \? sp->wall_ns : c;@            sp->cpu_ns = c;@
classify-uses-instantaneous-gate	s@    if \(it->cmd_sweep_open\) \{\n        uint64_t a = it->cmd_anchor_ns@    if (it->cmd_open) {\n        uint64_t a = it->cmd_anchor_ns@
classify-no-seen-shortcircuit	s@    if \(!it->cmd_seen\)\n        return 1;@    if (0)\n        return 1;@
classify-no-gate-present-fallback	s@    if \(!it->cmd_gate_present\)\n        return it->cmd_open != 0;@    if (0)\n        return it->cmd_open != 0;@
classify-no-clamp	s@    if \(open_ns > wall\)\n        open_ns = wall;@    if (0)\n        open_ns = wall;@
verdict-crosscheck-off	s@            else if \(it->in_command != it->cmd_in_window\)@            else if (0)@
cmd-open-ns-crosscheck-off	s@            else if \(it->cmd_gate_present &&@            else if (0 \&\&@
anchor-consistency-off	s@    if \(want_snap_wall != snap->snapshot_wall_ns \|\|@    if (0 \&\&@
open-count-crosscheck-off	s@    if \(json_u64\(root, "open_count", &open_count\) == 0 && open_count != \(uint64_t\)n\) \{@    if (0) {@
status-ignored	s@    if \(strcmp\(status, "included"\) != 0\) \{@    if (0) {@
empty-category-accepted	s@    if \(!cJSON_IsString\(it\) \|\| !it->valuestring \|\| !it->valuestring\[0\]\)@    if (!cJSON_IsString(it) || !it->valuestring)@
bool-accepts-anything	s@    if \(cJSON_IsNumber\(it\) && \(it->valuedouble == 0.0 \|\| it->valuedouble == 1.0\)\) \{\n        \*out = it->valuedouble != 0.0;\n        return 0;\n    \}\n    return -1;@    *out = 1;\n    return 0;@
init-fails-open	s@ov->status = PGWT_OVERLAY_UNAVAILABLE;\n    set_reason\(ov->reason, sizeof\(ov->reason\), "overlay not requested"\);@ov->status = PGWT_OVERLAY_INCLUDED;\n    set_reason(ov->reason, sizeof(ov->reason), "");@
degraded-renders-zero	s@cJSON_AddBoolToObject\(o, "excludes_in_flight", 1\);@cJSON_AddBoolToObject(o, "excludes_in_flight", 1); cJSON_AddNumberToObject(o, "in_flight_ns", 0);@
committed-prefix-ignored	s@if \(snap->committed_through_ns < it->open_start_ns\) \{@if (0) {@
incomplete-accepted	s@if \(!cJSON_IsTrue\(complete\)\) \{@if (0) {@
per-io-timeout-not-overall	s@poll\(&p, 1, \(int\)\(left > 1000 \? 1000 : left\)\);@poll(&p, 1, -1);@
markers-close-intervals	s@if \(PGWT_IS_MARKER\(ev->new_event\) \|\| PGWT_IS_MARKER\(ev->old_event\)\)\n            continue;@if (0)\n            continue;@
daemon-restart-ignored	s@if \(req->expect_generation && req->expect_generation != snap->daemon_generation\) \{@if (0) {@
entry-validation-skipped	s@if \(bad\) \{@if (0 \&\& bad) {@
record-view-optional	s@if \(!rec \|\| \(rec->n > 0 && !rec->events\)\) \{@if (0) {@
u64-string-through-double	s@unsigned long long v = strtoull\(s, &end, 10\);@unsigned long long v = (unsigned long long)(double)strtoull(s, &end, 10);@
same-pid-overlap-allowed	s@if \(spans\[j\].from_wall_ns < sp->to_wall_ns &&@if (0 \&\&@
attach-queries-on-refusal	s@    if \(!pgwt_overlay_wanted\(req, why, sizeof\(why\)\)\) \{\n        /\* Refuse BEFORE any I/O@    if (0) {\n        /* Refuse BEFORE any I/O@
stale-snapshot-accepted	s@if \(req->from_wall_ns >= snapshot_wall\) \{@if (0) {@
filtered-request-unfiltered	s@if \(req->filters_active && !req->accept\) \{@if (0) {@
filter-callback-ignored	s@if \(req->accept && !req->accept\(sp, req->accept_user\)\)@if (0)@
control-comment-only	s@/\* ── small helpers@/* ── SMALL HELPERS@
MUTS
)

printf '%s\n' "$mutations" | while IFS=$'\t' read -r name expr; do
    [ -n "$name" ] || continue
    mut="$work/$name.c"
    echo "$name" >> "$work/seen"
    if ! perl -0777 -pe "$expr" "$SRC" > "$mut"; then
        printf '%-40s: PERL-FAILED (the mutation could not even be applied)\n' "$name"
        echo "$name" >> "$work/failures"
        continue
    fi

    if cmp -s "$SRC" "$mut"; then
        # A mutation that did not apply would "pass" without testing anything:
        # exactly the vacuous gate this suite exists to catch.
        printf '%-40s: NOT-APPLIED (the pattern no longer matches the source)\n' "$name"
        echo "$name" >> "$work/failures"
        continue
    fi

    verdict=$(build_and_run "$mut" "$work/$name.bin")
    case "$name:$verdict" in
        control-*:GREEN)
            printf '%-40s: GREEN as required (a comment-only edit is not detected)\n' "$name" ;;
        control-*:*)
            printf '%-40s: %s — the suite reacts to edits that change nothing\n' "$name" "$verdict"
            echo "$name" >> "$work/failures" ;;
        *:RED)
            printf '%-40s: RED as required\n' "$name" ;;
        *:BUILD-FAILED)
            printf '%-40s: BUILD-FAILED — not evidence the test detects it\n' "$name"
            sed -n '1,8p' "$work/build.log"
            echo "$name" >> "$work/failures" ;;
        *)
            printf '%-40s: GREEN — the test CANNOT SEE this defect\n' "$name"
            echo "$name" >> "$work/failures" ;;
    esac
done

if [ -s "$work/failures" ]; then
    echo
    echo "FAILED: $(wc -l < "$work/failures" | tr -d ' ') mutation(s) not detected:"
    cat "$work/failures"
    exit 1
fi

n=$(printf '%s\n' "$mutations" | grep -c .)
seen=0
[ -f "$work/seen" ] && seen=$(wc -l < "$work/seen" | tr -d ' ')
if [ "$seen" != "$n" ]; then
    # The loop stopped early. Reporting "all green" here would be exactly the
    # false negative this file exists to prevent.
    echo "FAILED: only $seen of $n mutations were attempted - the suite did not finish"
    exit 1
fi
echo
echo "test_open_overlay_mutations: $n mutations, all accounted for"
exit 0
