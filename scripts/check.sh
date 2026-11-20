#!/usr/bin/env bash
# check.sh — the DETERMINISTIC test tier, runnable on a developer Mac with no
# PostgreSQL, no root. CI's full-check runs the full tier on Linux; the
# pre-push hook accepts either mode's passing stamp before `git push`.
#
#   make check        full local tier (includes the Playwright UI suite)
#   make check-fast   cheap guards + slow suites covering branch changes
#
# Everything that needs the Linux build (C units, synthetic-data tests against
# pgwt-server, protocol drift, live capture) runs via `make box-check`.
#
# On success writes .pgwt-check.stamp = "<tree hash>" for the exact working
# tree that passed and .pgwt-check.mode = "fast" or "full" beside it.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

# Running a check should install the push guard for this clone. Respect any
# hooks directory chosen by the developer, and never fail a check over config.
activate_push_guard() {
    local hooks_path status
    if hooks_path=$(git config --get core.hooksPath 2>/dev/null); then
        if [[ "$hooks_path" != "scripts/git-hooks" ]]; then
            printf "warning: core.hooksPath is '%s'; push guard is not active for this clone.\n" "$hooks_path" >&2
        fi
        return
    else
        status=$?
    fi

    if [[ $status -ne 1 ]] || ! git config --list >/dev/null 2>&1; then
        echo "warning: could not read Git config; push guard may not be active for this clone." >&2
    elif git config --local core.hooksPath scripts/git-hooks >/dev/null 2>&1; then
        echo "activated push guard: core.hooksPath=scripts/git-hooks (.git/hooks is bypassed)"
    else
        echo "warning: could not set core.hooksPath; push guard is not active for this clone." >&2
    fi
}
activate_push_guard

FAST=0
[[ "${1:-}" == "--fast" ]] && FAST=1

fail=0
step_name=
step_started=$SECONDS
step_times=()
finish_step() {
    if [[ -n "$step_name" ]]; then
        step_times+=("$((SECONDS - step_started))"$'\t'"$step_name")
    fi
}
print_timings() {
    finish_step
    printf '\nStep timings (elapsed seconds, slowest first):\n'
    if [[ ${#step_times[@]} -gt 0 ]]; then
        printf '%s\n' "${step_times[@]}" | sort -t $'\t' -k1,1nr
    fi
}
trap 'print_timings; print_fast_summary' EXIT
step() {
    finish_step
    step_name=$1
    step_started=$SECONDS
    printf '\n\033[1m=== %s ===\033[0m\n' "$1"
}
run()  { if ! "$@"; then echo "FAIL: $*"; fail=1; fi; }

# BEGIN fast slow-step selector (also exercised in isolation by its self-test)
slow_ran=0
slow_skipped=0
fast_paths_ready=0
fast_all_reason=
fast_changed_paths=()
init_fast_paths() {
    [[ $fast_paths_ready -eq 0 ]] || return
    fast_paths_ready=1
    local base paths_file path shallow
    if [[ "${PGWT_CHECK_FAST_ALL:-0}" == 1 ]]; then
        fast_all_reason='PGWT_CHECK_FAST_ALL=1'
    elif ! shallow=$(git rev-parse --is-shallow-repository 2>/dev/null); then
        fast_all_reason='cannot inspect repository history'
    elif [[ $shallow == true ]]; then
        fast_all_reason='shallow repository; merge base is not reliable'
    elif ! base=$(git merge-base HEAD origin/master 2>/dev/null) || [[ -z "$base" ]]; then
        fast_all_reason='cannot compute merge base with origin/master'
    elif ! paths_file=$(mktemp "${TMPDIR:-/tmp}/pgwt-check-paths.XXXXXX"); then
        fast_all_reason='cannot create changed-path inventory'
    else
        # Compare the base with the working tree (includes committed, staged
        # and unstaged changes). Disable renames so both paths are covered.
        # NUL delimiters preserve spaces, newlines and glob metacharacters.
        if git diff --name-only --no-renames -z "$base" -- > "$paths_file" &&
           git ls-files --others --exclude-standard -z >> "$paths_file"; then
            while IFS= read -r -d '' path; do
                fast_changed_paths+=("$path")
            done < "$paths_file"
        else
            fast_all_reason='cannot enumerate changed or untracked paths'
        fi
        rm -f "$paths_file"
    fi
    if [[ -n "$fast_all_reason" ]]; then
        printf 'fast: running every slow step: %s\n' "$fast_all_reason"
    fi
}
slow_step() {
    local name=$1 patterns=$2 path pattern selected=0
    local -a globs
    shift 2
    if [[ $FAST -eq 0 ]]; then
        selected=1
    else
        init_fast_paths
        if [[ -n "$fast_all_reason" ]]; then
            selected=1
        else
            # Split the declared, space-separated glob list without pathname
            # expansion; matching below treats the changed path literally.
            read -r -a globs <<< "$patterns"
            for path in "${fast_changed_paths[@]}"; do
                for pattern in "${globs[@]}"; do
                    if [[ "$path" == $pattern ]]; then
                        selected=1
                        break 2
                    fi
                done
            done
        fi
    fi
    if [[ $selected -eq 0 ]]; then
        # A skipped suite has no timing row and must not inflate its neighbour.
        finish_step
        step_name=
        slow_skipped=$((slow_skipped + 1))
        printf 'SKIP (fast): %s\n' "$name"
        return 0
    fi
    slow_ran=$((slow_ran + 1))
    step "$name"
    "$@"
}
print_fast_summary() {
    if [[ $FAST -eq 1 ]]; then
        printf '\nFast slow steps: %s ran, %s skipped\n' "$slow_ran" "$slow_skipped"
        echo "note: CI's full-check runs everything."
    fi
}
# END fast slow-step selector

need() { command -v "$1" >/dev/null || { echo "missing: $1 — see CLAUDE.md 'Local setup'"; exit 2; }; }
need node; need go; need python3

# Sampled attribution: deterministic syscall budget and caught regressions.
# Keep suite command spelling intact for the existing literal wiring guards.
check_slow_1() {
run python3 -B tests/test_sampled_attr_io.py
# The live status-batch test's pure parts (perf summary parser, bounds) on outputs
# recorded on the Ubuntu and EL8 gate images; the recorded master must assess RED.
run python3 -B tests/test_sampler_batch_summary.py
# Same portable C dispatch test as the box C tier, built outside the tree.
if dispatch_check_dir=$(mktemp -d "${TMPDIR:-/tmp}/pgwt-sampler-dispatch-check.XXXXXX"); then
    run python3 -B tests/build_sampler_dispatch.py --output "$dispatch_check_dir/test_sampler_dispatch"
    run "$dispatch_check_dir/test_sampler_dispatch"
    rm -rf "$dispatch_check_dir"
else
    echo "FAIL: cannot create sampler dispatch test directory"
    fail=1
fi
}
slow_step "sampled attribution I/O budget and mutants" 'src/* demos/replay-compat.h tests/*sampl* tests/fixtures/* tests/run_all.sh tests/test_backend* tests/test_init_hint* tests/test_counter* tests/*lifecycle* tests/test_spawn* tests/demo_rehearsal* tests/live_daemon_lib.sh tests/testutil.py' check_slow_1

check_slow_2() {
run python3 -B tests/test_backend_attach_race.py --mutants
run python3 -B tests/test_backend_exit_accounting.py
run python3 -B tests/test_init_hint_known_class.py
}
slow_step "watchpoint attach exit races (portable backend C harness)" 'src/* demos/replay-compat.h tests/test_backend* tests/test_init_hint* tests/demo_rehearsal* tests/live_daemon_lib.sh tests/testutil.sh tests/*attach* tests/sampler_batch_perf.py tests/test_sampler_batch_live.py tests/test_preseed_startup_live.py tests/testutil.py tests/mock_server.py tests/server_harness.py tests/test_cmdline.c' check_slow_2

# Portable C core: this script builds/runs the contract C test in a temporary
# directory. The differential compares against the independent reference;
# every numerical mutant must then fail its intended distinguishing case.
# Keep ASan/UBSan coverage in this gate; standalone runs select it via CFLAGS.
step "coverage live gate controls (host, no BPF)"
run python3 -B tests/test_coverage_live_helpers.py
run python3 -B tests/test_coverage_drain.py --mutants
run python3 -B tests/test_libbpf_positions.py

step "capture counter vector, validity and sticky writer (host C, Refs #871)"
run python3 -B tests/test_counter_vector_portable.py
run python3 -B tests/test_counter_failures.py
run python3 -B tests/counter_vector_mutants.py --guard-sweep

core_cflags="${CFLAGS:-} -fsanitize=address,undefined -fno-omit-frame-pointer"
check_slow_3() {
# core-status runs the portable merge/evidence contract, differential, codec,
# redelivery, report, merge index and merge queue suites (including their
# mutants) once in this run.
# A fresh directory binds the self-test's transcripts to this invocation.
# Remove it on green; retain it on red so the printed runner-log paths remain usable.
if core_status_dir=$(mktemp -d "${TMPDIR:-/tmp}/pgwt-check-core-status.XXXXXX"); then
    echo "core status logs: $core_status_dir"
    fail_before_core_status=$fail
    fail=0
    run env PGWT_CORE_STATUS_DIR="$core_status_dir" CFLAGS="$core_cflags" bash scripts/core-status.sh
    run bash tests/test_core_status.sh "$core_status_dir"
    if [[ $fail -eq 0 ]]; then
        rm -rf "$core_status_dir"
    else
        echo "core status logs retained: $core_status_dir"
    fi
    [[ $fail_before_core_status -eq 0 ]] || fail=1
else
    echo "FAIL: core status temporary directory"
    fail=1
fi
run python3 -B tests/test_ledger.py
# core-status ran the merge index and queue proofs; this keeps the queue
# suite's production/instrumented symbol check and informational timings.
run env CFLAGS="$core_cflags" python3 -B tests/test_core_merge_queue.py
}
slow_step "core wait_time, merge and evidence contract status and mutation checks" 'src/core/* contracts/* scripts/core* scripts/tree-hash.sh scripts/ledger.py ledger/* tests/core* tests/test_core* tests/evidence* tests/test_evidence* tests/*oracle* tests/fixtures/* tests/test_ledger.py' check_slow_3

check_slow_4() {
run python3 -B tests/check_server_test_links.py --self-test
run env CFLAGS="$core_cflags" python3 -B tests/test_pilot_route.py --require-browser
run env CFLAGS="$core_cflags" python3 -B tests/test_pilot_route.py --mutants
run bash tests/test_consumer_boundary.sh --self-test
}
slow_step "stateless evidence-v1 pilot route, browser, uint64 and mutations" 'src/* contracts/* demos/replay-compat.h tests/test_pilot_route.py tests/check_server_test_links.py tests/server_build.py Makefile tests/pilot* tests/evidence* tests/fixtures/* tests/*.trace* tests/test_consumer_boundary.sh tests/Makefile web/static/* tests/web_unit/pilot-evidence.test.mjs tests/mock_server.py tests/server_harness.py tests/test_sample_weight_controls.py tests/*.c tests/*.h' check_slow_4

check_slow_5() {
run env CFLAGS="$core_cflags" bash tests/core_merge_adapter_mutants.sh
}
slow_step "C11 synthetic merge adapter: wire goldens, contract replay and rule mutants (Refs #871)" 'src/core/* contracts/merge-v1/* tests/core_merge_adapter_mutants.sh tests/merge_adapter_goldens.h tests/test_core_merge_adapter.c tests/test_core_merge_contract.c' check_slow_5

step "U1b transport wire golden bytes and registered A1/B seams (host C)"
if transport_check_dir=$(mktemp -d "${TMPDIR:-/tmp}/pgwt-transport-check.XXXXXX"); then
    for suite in transport_wire transport_producers percpu_transport; do
        transport_sources=("tests/test_${suite}.c")
        if [[ $suite == percpu_transport ]]; then
            transport_sources+=(src/percpu_transport.c src/merge_capture.c
                               src/core/merge_adapter.c src/core/merge.c)
        fi
        if "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
            -DPGWT_TRANSPORT_HOST_TEST \
            -fno-omit-frame-pointer "${transport_sources[@]}" \
            -o "$transport_check_dir/$suite"; then
            run "$transport_check_dir/$suite"
        else
            echo "FAIL: transport $suite compilation"
            fail=1
        fi
    done
    rm -rf "$transport_check_dir"
else
    echo "FAIL: transport temporary directory"
    fail=1
fi

# Resolve the interpreter up front (same as tests/ui_gallery.sh) so a pyenv
# pin mismatch is reported clearly instead of the UI suite silently skipping
# below (see issue #118). --fast doesn't need Playwright at all.
py=$(python3 -c 'import sys; print(sys.executable)')
if [[ $FAST -eq 0 ]] && ! "$py" -c 'import playwright' 2>/dev/null; then
    echo "playwright missing in $py — pin the interpreter with playwright" \
         "(.python-version / PYENV_VERSION) or install:" \
         "$py -m pip install --user playwright==1.60.0 && $py -m playwright install chromium"
    exit 2
fi

# Same family as the snapshot-history guard: both protect the integrity of
# the .pgwt-check.stamp this script writes at the end. A build product that
# .gitignore does not cover is untracked-but-unignored, so scripts/tree-hash.sh
# counts it and the stamp stops matching the moment anyone runs
# `make -C tests` -- which is how demo-rehearsal counted attempt 2 lost its
# pairing. Lives HERE and not in tests/unit_tests.list on purpose: box-check
# rsyncs the tree WITHOUT .git, so on the box this guard could only refuse for
# an environmental reason, and a gate that is red for the wrong reason gets
# switched off. This Mac and CI are the places it is both meaningful and able
# to run.
check_slow_6() {
run bash tests/test_gitignore_build_products.sh
}
slow_step "bash: .gitignore covers every tests/ build product (tree-hash integrity)" '.gitignore tests/Makefile tests/* scripts/tree-hash.sh' check_slow_6

check_slow_7() {
run bash tests/test_push_guard_denylist.sh
run python3 -B tests/test_check_stamp.py
}
slow_step "bash: push-guard denylist and executable pre-push proofs (#719)" 'tests/test_push_guard_denylist.sh tests/test_pre_push_hook.sh tests/test_check_stamp.py scripts/hooks/push-guard.sh scripts/git-hooks/pre-push scripts/tree-hash.sh scripts/check.sh .claude/settings.json .gitignore' check_slow_7

check_slow_8() {
run bash tests/test_commit_msg_hook.sh
}
slow_step "bash: commit-msg attribution hook fails open and derives Generated-with (#771)" 'tests/test_commit_msg_hook.sh scripts/git-hooks/commit-msg scripts/board-author.py' check_slow_8

check_slow_9() {
run python3 scripts/lead/audit-report-selftest.py
}
slow_step "report audit isolation and contradiction controls (#761)" 'scripts/lead/*' check_slow_9

# Brief and report checks are local, hermetic and require no browser or box.
check_slow_10() {
run bash tests/test_check_brief.sh
}
slow_step "bash: brief sections, budget and live scope evidence" 'tests/test_check_brief.sh scripts/lead/* scripts/tree-hash.sh' check_slow_10

step "agent agreement matches registered hooks and role invocations"
run scripts/check-agent-agreement.sh

step "python: RULE 0 absence vocabulary and evidence scopes"
run python3 tests/test_absence_vocab.py

check_slow_11() {
run scripts/lead/codex-task-selftest.sh
}
slow_step "POSIX shell: Codex task wrapper self-test" 'scripts/lead/* scripts/board-author.py' check_slow_11

check_slow_12() {
run scripts/lead/tick-selftest.sh
}
slow_step "deterministic lead tick offline self-test" 'scripts/lead/*' check_slow_12

step "board-labels trailer authorship regression tests (#771)"
run python3 tests/test_board_author.py

check_slow_13() {
run bash tests/test_pr_ready.sh
}
slow_step "bash: pr-ready ordering and refusal self-test (#701)" 'tests/test_pr_ready.sh scripts/pr-ready.sh scripts/tree-hash.sh' check_slow_13

step "generated tag board offline self-test (#850)"
run scripts/lead/board-selftest.sh

step "PR state drift sweep and disposition self-test (#717)"
run python3 scripts/lead/selftest-pr-state.py

check_slow_14() {
run bash tests/test_box_check_build_provenance.sh
}
slow_step "box build provenance offline self-test (#861)" 'tests/test_box_check_build_provenance.sh scripts/box* scripts/tree-hash.sh scripts/check.sh scripts/lead/selftest.sh Makefile tests/Makefile tests/hetzner* tests/provision-runner.sh' check_slow_14

check_slow_15() {
run bash tests/test_box_check_detached.sh
}
slow_step "detached box-check transport recovery offline self-test" 'tests/test_box_check_detached.sh scripts/box* scripts/tree-hash.sh tests/hetzner* tests/run_all.sh' check_slow_15

check_slow_16() {
run bash scripts/lead/selftest.sh
}
slow_step "lead hooks and box evidence offline self-test (#861)" 'scripts/lead/* scripts/tree-hash.sh' check_slow_16

# The summary format version is the one number in the docs with an
# OPERATIONAL consequence: DEMO_RUNBOOK.md tells the operator which trace
# directories to move aside, and a stale N leaves a directory the server
# then refuses on the summary fast path -- the 900 s Overview silently
# drops to raw, which is the paint latency the runbook exists to avoid.
# It drifted twice in one night (v6 while the code was 8; then a v8
# correction landing on a tree that had merged v9), so it is checked rather
# than proofread.
step "bash: the docs name the PGWT_SUMMARY_VERSION the code defines"
run bash tests/test_docs_summary_version.sh

step "snapshot-history guard self-test"
run tests/test_check_snapshot_history.sh

step "snapshot-history guard (this branch vs origin/master)"
run scripts/check-snapshot-history.sh

# Link-closure guard (#424, 2026-10-15). Adding one call to src/map_reader.c
# compiled everywhere and broke the LINK of test_event_reader and
# test_concurrent_rw on Linux, because both link map_reader.o for its
# accumulator core alone. It cost two ephemeral VMs and two `box-check` exits
# before anyone knew, with every test that ran passing. "It compiles" cannot
# see that; comparing the TU's undefined-symbol set against the merge base
# can, in about a second, here. The guard REFUSES (and so fails this step)
# when it cannot resolve a base, when the base revision will not compile,
# when the compiler is missing, or when the object it built is not the TU it
# claims -- a closure check with no baseline approves everything.
step "link-closure guard self-test"
run tests/test_check_link_closure.sh

step "link-closure guard (src/map_reader.c vs merge base)"
run scripts/check-link-closure.sh

# Runner-registration guard (#433): tests/run_all.sh (make box-check) and
# tests/ci_smoke.sh (CI/nightly) have independent membership, so a test
# wired into ci_smoke.sh alone makes box-check "green" prove nothing about
# it (three real incidents: test_v9_open_cpu_wired registered ci_smoke-only
# twice over, and test_capture_smoke mentioned in run_all.sh only inside a
# comment while ci_smoke.sh actually runs it). Only the SELF-TEST (synthetic
# fixtures) runs here; checking this repository's actual membership remains
# a manual reviewer evidence step, like scripts/check-test-registration.sh.
# The capture-smoke and uprobe gaps are fixed in run_all.sh (#752), with
# both capture modes registered and full mode gated on watchpoint support.
step "runner-registration guard self-test"
run tests/test_check_runner_registration.sh

step "contract guard self-test (#850)"
run bash tests/test_check_contract_guard.sh

step "agent working agreement guard self-test"
run python3 -B tests/test_check_agent_agreement.py

step "test-registration guard self-test"
run bash tests/test_check_test_registration.sh

# Section-registration guard (#686): binary registration stays green when
# one section's dispatcher line disappears. Measured on PR #689 (wired ->
# unwired checks, all still 0 failed / exit 0): test_idle_summary 397 -> 367,
# test_agg_raw_crosscheck 251 -> 211, test_cpu_split_parity 107 -> 66.
# Static reachability from main through same-file call sites catches the
# missing section without built binaries or a Linux box. Discover every
# sectioned C suite automatically; refuse unparseable inputs, and exercise
# the guard's own blind spots here so it cannot silently approve nothing.
check_slow_17() {
run tests/test_check_section_registration.sh
}
slow_step "section-registration guard self-test (bypass suite, #686)" 'tests/test_check_section_registration.sh scripts/check-section-registration*' check_slow_17

step "section-registration guard (every sectioned C suite, #686)"
run scripts/check-section-registration.sh

# #591 bypass suite: tests/test_uprobe_fired.py's exact-window mask gate is a
# LIVE test (ci_smoke.sh and run_all.sh), so its own logic
# would otherwise be provable only on a gate box. The resolver and the
# three-part mask predicate are pure, and this runs them against every way the
# gate could be made unable to SEE: an undeclared/unparseable expectation, an
# absent or non-numeric mask field, phase markers absent where declared
# present AND present where declared absent, an unknown bundle bit, plus two
# subprocess cases proving the real test's main() actually consults the
# resolver and that ci.yml still declares the expectation.
step "exact-window mask gate bypass suite (#591)"
run tests/test_uprobe_mask_gate.py

step "runner known-failing scope and summary self-test (#760)"
run python3 -B tests/test_run_all_known_failing.py

# Runner-image pin guard (2026-10-22). GitHub rolled `ubuntu-latest` to Ubuntu
# 26.04 and four gating jobs went red at once (run 39765852420): Playwright
# 1.60.0 has no chromium for ubuntu26.04-x64, setup-python handed the
# v1.60.0-noble container a Python with no working ssl module, and the newer
# GCC made tests/gen_test_traces.c's implicit unlink() declaration fatal. The
# Playwright version is pinned to the one the 47 committed snapshot baselines
# were rendered with, so the image is pinned instead -- and this guard refuses
# a tree that drifts back to a floating label, or that loses
# -Werror=implicit-function-declaration (the flag that keeps the
# implicit-declaration class fatal on the older, pinned GCC too).
step "runner-image pin guard self-test (bypass suite)"
run bash tests/test_check_runner_pin.sh

# Promoted from an untracked results directory; closes the numpy-presence
# false-negative in the frame detector's bypass probes (issue #519).
step "frame-verify probe suite (bypass suite for scripts/frame_verify.py, #519)"
run python3 tests/test_frame_verify_probes.py

step "runner-image pin guard (this tree)"
run bash scripts/check-runner-pin.sh

step "fast slow-step coverage guard and mutants"
run python3 -B scripts/test-check-fast.py

step "CI full-check job guard and mutants"
run python3 -B scripts/check-ci-full-check.py
run python3 -B scripts/test-ci-full-check.py

# tests/results/ evidence-directory ignore rule (companion to this guard's
# own commit, same failure class as the tree-hash guards above): an agent
# writing a brand-new tests/results/<name>/ directory must not move
# scripts/tree-hash.sh's output, and a tracked tests/results/ file must
# never become invisible to `git status` by also matching an ignore rule.
step "tests/results/ evidence-directory ignore rule"
run bash tests/test_results_evidence_ignore.sh

step "web builder unit tests (node)"
run node --test 'tests/web_unit/*.test.mjs'

step "go bridge (skips server-backed cases without a Linux pgwt-server)"
run bash -c 'cd web && go vet ./... && go test ./...'

step "python: compile every test module"
run python3 -m py_compile tests/*.py

step "python: multi-window scripted snapshots + refusal tests"
run python3 tests/test_multi_window_lib.py

step "python: free_ports self-test"
run python3 tests/test_free_ports.py

check_slow_18() {
run python3 tests/test_tag_checklist.py
}
slow_step "python: tag checklist exact-SHA evidence and blocker refusals" 'tests/test_tag_checklist.py scripts/tag-checklist.py tests/tag_checklist.tsv tests/fixtures/tag_checklist/* scripts/check.sh .github/workflows/* tests/unit_tests.list tests/run_all.sh tests/*.py tests/*.sh tests/web_unit/* src/server.c' check_slow_18

step "tag checklist: real inventory retains every historical ID"
run python3 scripts/tag-checklist.py "$(git rev-parse HEAD)" --check-history

step "tag checklist: workflow_run still targets the CI workflow name"
run python3 -c 'import re; from pathlib import Path; assert re.search(r"(?m)^name: CI\s*$", Path(".github/workflows/ci.yml").read_text()), "tag checklist trigger requires name: CI"'

step "tag checklist: workflow permissions are exactly the required read-only scopes"
run python3 -c 'import re; from pathlib import Path; workflow = Path(".github/workflows/tag-checklist.yml").read_text(); blocks = re.findall(r"(?m)^permissions:\n((?:[ \t]+[^\n]*\n|\n)*)", workflow); expected = ["actions: read", "contents: read", "issues: read", "pull-requests: read"]; assert len(re.findall(r"(?m)^\s*permissions:", workflow)) == 1 and len(blocks) == 1 and sorted(line.strip() for line in blocks[0].splitlines() if line.strip()) == expected, "tag checklist permissions must be exactly contents/actions/issues/pull-requests: read"'

# Fake workload/daemon processes and pg_stat_statements results: no live PG or BPF.
step "python: capture-smoke failure diagnostics (#614)"
run python3 tests/test_capture_smoke_diagnostics.py

step "python: demo-walk recorder refusal/coverage/crop tests"
run python3 tests/test_demo_walk_recorder.py

# Issue #214: tests/demo_workload_coverage.py's per-tab *_populated()
# checkers are pure (no browser, no network, no PG) -- same reasoning as
# the demo-rehearsal bypass-suite step below, run in the fast tier rather
# than waiting for an actual box capture to exercise them.
step "python: demo-workload-coverage unit tests"
run python3 tests/test_demo_workload_coverage.py

# Owner finding, 2026-09-28 (run.id 1790574871): the demo-rehearsal bypass
# suite (tests/test_demo_rehearsal_lib.py) used to run only in the
# box-check/CI tier (tests/unit_tests.list), so a regression in the
# rehearsal's own gating logic -- e.g. its blink verdict silently
# diverging from ui_live_smoke_lib.build_tab_result's -- was only ever
# caught by an actual 30-45 minute rehearsal run, not by `make check`.
# Pure Python, no browser, no network, no Linux/root needed -- belongs in
# the fast deterministic tier.
step "python: demo-rehearsal bypass-suite unit tests"
run python3 tests/test_demo_rehearsal_lib.py

step "python: rehearsal verdict refusal tests"
run python3 tests/test_rehearsal_verdict_hardening.py

step "python: rehearsal workload lifetime and launch refusals (#543)"
run python3 tests/test_rehearsal_workload_lifetime.py

step "python: live-smoke verdict unit tests"
run python3 tests/test_ui_live_smoke_lib.py

step "python: #537 waterfall-readiness diagnostic refusal tests"
run python3 tests/test_diag_537_waterfall.py

step "python: live-smoke failure diagnostic capture (#544)"
run python3 tests/test_ui_live_smoke_failure.py
run python3 tests/test_ui_live_smoke_failure.py --snapshot-js

# #384: the --diag-data per-frame panel-data probe exists in
# tests/ui_live_smoke.py and is reached from tests/ui_live_smoke.sh, but
# tests/demo_rehearsal.py has its OWN run_tab call site -- so the instrument
# could be complete and still produce nothing during a rehearsal. Asserts the
# whole thread (CLI flag -> _run_passes -> every run_tab call) with run_tab
# replaced by a recorder: no browser, no network, milliseconds.
check_slow_19() {
run python3 tests/test_harness_ownership.py
}
slow_step "python: harness ownership, box argv, teardown and Overview REDs (#544/#546)" 'tests/test_harness_ownership.py tests/test_rehearsal_workload_lifetime.py tests/*.sh tests/bridge_owner.py tests/live_loop_workload.py tests/*rehearsal* tests/ui_live_smoke* tests/demo_workload* tests/testutil.py scripts/demo-rehearsal* web/static/* tests/schedstat_lib.py tests/schedstat_probe.py tests/server_harness.py tests/test_capture_smoke.py tests/testutil.py' check_slow_19

step "python: rehearsal --diag-data wiring (#384)"
run python3 tests/test_demo_rehearsal_diag.py

# #386: the Off-CPU* residual decomposition's parsers, window aggregation
# and refusal paths (tests/schedstat_lib.py) plus the /proc reader driven
# against a FIXTURE /proc tree -- pure Python, no Linux, no root, no live
# box, so it belongs in this tier rather than waiting for a 35-minute
# rehearsal. Same reasoning as the bypass-suite step above.
step "python: Off-CPU* run_delay decomposition unit + bypass suite (#386)"
run python3 tests/test_schedstat_lib.py

# The summary-vs-raw parity gate's comparison core and its false-negative
# suite (tests/summary_raw_parity_lib.py). The LIVE half needs pgwt-server
# answering a generated capture on both routes, so it runs in
# tests/run_all.sh's synthetic-data tier; this half is pure Python and is
# where that gate would fail SILENTLY -- both sides taking the same route,
# two empty answers agreeing, an absent field read as equal, an empty
# expected-divergence table approving everything. Same reasoning as the
# bypass-suite steps above: a regression in the gate's own logic must not
# need a box run to surface.
step "python: summary-vs-raw parity comparison core + bypass suite (#408/#379/#319)"
run python3 tests/test_summary_raw_parity_lib.py

# #243: Workload.fire()'s verify flag and live_loop_workload.py's periodic
# verify cadence -- pure Python, fakes psql/sessions, no Linux/root/live-DB
# needed. Belongs in the fast deterministic tier for the same reason as the
# bypass suite above: a regression here (verify silently ignored, or the
# cadence silently reverting to "always" or "never") is otherwise only
# caught by an actual live capture's Sessions-tab row count or, worse, by
# the re-lock loop breaking unnoticed.
step "python: Workload.fire() verify-flag/cadence unit tests (#243)"
run python3 tests/test_workload_verify_flag.py

# Same reasoning as the bypass-suite step above, same owner rule (a
# Mac-side script's unit tests must not live only in the box tier): the
# PG-version gate in scripts/demo-rehearsal.sh (parse_pg_probe_line,
# pg_version_verdict -- the check that refuses a rehearsal run against the
# wrong PostgreSQL major) lives entirely in
# tests/demo_rehearsal_orchestrator_lib.py, pure Python with no browser/
# network/Linux dependency. Without this step a regression there would not
# surface until the next box run.
step "python: demo-rehearsal orchestrator unit tests"
run python3 tests/test_demo_rehearsal_orchestrator_lib.py

# Hermetic launcher fixtures belong in the same local tier as its orchestrator.
step "python: demo-rehearsal launcher provenance unit tests (scripts/test-demo-rehearsal-provenance.py)"
run python3 scripts/test-demo-rehearsal-provenance.py

# tag_cli_args (tests/live_daemon_lib.sh) threads a counted rehearsal's
# --tag through scripts/demo-rehearsal.sh -> ...-remote-run.sh ->
# tests/demo_rehearsal.sh -> demo_rehearsal.py --tag (docs/
# DEMO_REHEARSAL_CRITERIA.md "a counted attempt names the tag it ran
# against"). Pure bash, no Linux/root/live-PostgreSQL needed -- same
# "Mac-side script's unit tests must not live only in the box tier"
# reasoning as the orchestrator step above.
step "bash: tag_cli_args plumbing unit tests"
run bash tests/test_tag_cli_args.sh

# assert_served_ui_matches_tree (tests/live_daemon_lib.sh) refuses a live-tier
# run whose Go bridge serves a web/static OTHER than the tree under test --
# web/main.go embeds the UI into the binary, so a stale web/pgwt would let a
# walk (or a counted demo rehearsal) pass against the wrong UI and report
# green. Hermetic: a throwaway tree plus a python3 http.server, no Go build,
# no PostgreSQL, no root -- so, like the step above, its unit tests do not
# live only in the box tier. The cases are weighted toward FALSE negatives:
# 404, dead server, empty tree, missing dir and missing python3 must each
# REFUSE ("cannot verify"), never pass.
step "bash: served-asset guard unit tests (stale bridge refusal)"
run bash tests/test_served_asset_guard.sh

step "python: gallery provenance and tiered output unit tests"
run python3 tests/test_evidence_output.py

step "CI change-classifier table-driven test (issue #167)"
run bash tests/test_classify_changed_files.sh

step "cross-validate rate-sweep gate unit tests (issue #309)"
run bash tests/test_cross_validate_rate_gate.sh

step "cross-validate workload validity and rounding controls (Refs #968, Refs #675)"
run python3 -B tests/test_cross_validate_validity.py

step "cross-validate tolerance derivation + bypass suite (issues #294/#306/#321)"
run bash tests/test_xval_tolerance_derivation.sh

if [[ $FAST -eq 0 ]]; then
    step "overhead gate liveness and stall evidence controls (#951)"
    run python3 -B tests/test_overhead_harness.py

    # ── Port allocation (so two `make check` runs on this Mac don't collide) ─
    # One free base per run, laid out at FIXED OFFSETS below so every mock
    # server this run spawns gets a run-private port; no lock (that would
    # serialize two ~4-minute runs), see tests/free_ports.py. Allocated here,
    # immediately before the mocks bind, to keep the probe-to-bind window as
    # short as possible. A developer running one suite by hand (no env vars
    # set) keeps today's fixed-default ports. Safe across worktrees; NOT safe
    # for two `make check` runs in the SAME checkout at once (those still
    # race on .pgwt-check.stamp and tests/results/).
    #
    #   PORT MAP (relative to PGWT_PORT_BASE):
    #     PGWT_TEST_PORT  = base+0   test_web_ui.py:       HTTP +0/WS +1, B5 +10/+11, compare +20/+21
    #     PGWT_CHAOS_PORT = base+30  test_web_ui_chaos.py: HTTP +30/WS +31
    #     PGWT_SNAP_PORT  = base+40  reserved for when check.sh runs the
    #                                snapshot suite (test_web_ui_snapshots.py
    #                                isn't invoked here yet): HTTP +40/WS +41,
    #                                sampled +50/+51
    #     PGWT_CHIP_PORT  = base+60 test_chip_label_alignment.py: HTTP +60
    #                                (gallery.html is static — no WS pair)
    #     PGWT_WATERFALL_PORT = base+65 verify_waterfall_fold.py (#415):
    #                                HTTP +65/WS +66 (gap left after CHIP so
    #                                a future WS-pair test can slot into +61)
    #     PGWT_DFG_GEOM_PORT  = base+67 test_dfg_geometry.py: HTTP +67
    #                                (gallery.html is static — no WS pair;
    #                                merge note: agent/web-tables and
    #                                agent/web-graph both independently
    #                                claimed +65 — a genuine collision found
    #                                at integration, not a design choice;
    #                                moved DFG_GEOM here to the next free
    #                                offset rather than picking a "winner")
    PORT_SPAN=70
    PGWT_PORT_BASE=$(python3 tests/free_ports.py "$PORT_SPAN") || { echo "free_ports: could not allocate $PORT_SPAN free ports"; exit 2; }
    [[ $PGWT_PORT_BASE =~ ^[0-9]+$ ]] || { echo "free_ports: non-numeric base '$PGWT_PORT_BASE'"; exit 2; }
    export PGWT_TEST_PORT=$PGWT_PORT_BASE
    export PGWT_CHAOS_PORT=$((PGWT_PORT_BASE + 30))
    export PGWT_SNAP_PORT=$((PGWT_PORT_BASE + 40))
    export PGWT_CHIP_PORT=$((PGWT_PORT_BASE + 60))
    export PGWT_WATERFALL_PORT=$((PGWT_PORT_BASE + 65))
    export PGWT_DFG_GEOM_PORT=$((PGWT_PORT_BASE + 67))
    echo "port base: $PGWT_PORT_BASE (span $PORT_SPAN — TEST=+0 CHAOS=+30 SNAP=+40 CHIP=+60 WATERFALL=+65 DFG_GEOM=+67)"

    step "web UI suite vs mock_server.py (Playwright)"
    run python3 tests/test_web_ui.py
    step "first-info timeout recovery and retry bounds (#758, Playwright)"
    run python3 tests/test_web_ui_info_retry.py
    step "web UI chaos suite (latency jitter / reconnects)"
    run python3 tests/test_web_ui_chaos.py
    step "overlay chip label alignment at DSF 1/2 (issue #213)"
    run python3 tests/test_chip_label_alignment.py
    # #415: the drilled Waterfall readout stays above the fold in the FULL
    # application shell (breadcrumbs/controls/AAS chart included) at the
    # pinned demo viewport -- the isolated fixture gallery is exactly what
    # missed this originally (see the script's own docstring).
    step "Waterfall drilled-readout fold check at the demo viewport (issue #415)"
    run python3 tests/verify_waterfall_fold.py
    step "DFG rendered-geometry guard: real coordinateSystem.dataToPoint, not getOption() (issue #436)"
    run python3 tests/test_dfg_geometry.py

    # issue #245 (TTFP): _navigate_to_tab's ttfp_ms anchor and its wiring
    # into run_tab's build_tab_result call -- imports ui_live_smoke.py,
    # which hard-requires Playwright, so this runs here (already guarded
    # above) rather than via tests/unit_tests.list (that list's CI job
    # installs no Playwright on purpose -- see
    # test_demo_workload_coverage.py's test_import_needs_no_playwright).
    step "ui_live_smoke navigation TTFP anchor (issue #245)"
    run python3 tests/test_ui_live_smoke_nav.py
fi

if [[ $fail -ne 0 ]]; then
    echo; echo "CHECK FAILED"; rm -f .pgwt-check.stamp .pgwt-check.mode; exit 1
fi

# Stamp the exact tree (tracked + untracked, minus ignored) that passed.
mode=full
[[ $FAST -eq 0 ]] || mode=fast
if ! tree_hash=$(scripts/tree-hash.sh) || [[ -z "$tree_hash" ]] ||
   ! printf '%s\n' "$mode" > .pgwt-check.mode ||
   ! printf '%s\n' "$tree_hash" > .pgwt-check.stamp; then
    echo "CHECK FAILED: cannot write tree/mode stamp"
    rm -f .pgwt-check.stamp .pgwt-check.mode
    exit 1
fi
echo; echo "CHECK PASSED ($mode) — stamp $tree_hash"
[[ $FAST -eq 1 ]] && echo "note: --fast skips the UI suite; the push guard accepts it, CI does not."
exit 0
