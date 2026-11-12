#!/usr/bin/env bash
# Real edits to temporary source copies. Build/sanitizer/harness failures never
# count as kills. Frozen answers and the checkout's sources stay untouched.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 -B - "$@" <<'PY'
import importlib.util
import contextlib
import io
from pathlib import Path
import re
import shutil
import sys
import tempfile

root = Path.cwd()
spec = importlib.util.spec_from_file_location("evidence_differential", root / "tests/test_core_evidence_differential.py")
diff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diff)

# (name, source text, replacement, intended case, required first mismatch)
mutants = [
    ("E1_no_attach", "out->projection.waits = out->waits;", """
    if (!out->projection.coverage_count && !out->projection.invalid_fields) {
        out->coverage = (pgwt_core_span){out->projection.life_start, UINT64_MAX};
        out->projection.coverage = &out->coverage;
        out->projection.coverage_count = 1;
    }
    out->projection.waits = out->waits;""", "08_attach_failure", "wait_time"),
    ("E2_eof_frontier", "free(history);", """
    if (target_capture && e->coverage_count && n && file->records[n-1].t > out->coverage.end) {
        out->coverage.end = file->records[n-1].t;
        if (e->has_open) e->open.as_of = out->coverage.end;
    }
    free(history);""", "16_crash_with_uncertified_suffix", "wait_time"),
    ("E3_transition_frontier", "free(history);", """
    for (size_t j = 0; target_capture && e->coverage_count && j < count; ++j) {
        if (history[j].record->t > out->coverage.end) {
            out->coverage.end = history[j].record->t;
            if (e->has_open) e->open.as_of = out->coverage.end;
        }
    }
    free(history);""", "16_crash_with_uncertified_suffix", "wait_time"),
    ("E4_frontier_close", "free(history);", """
    if (target_capture && e->has_open) {
        out->waits[e->wait_count++] = (pgwt_core_wait){e->open.event, e->open.start, e->open.as_of};
        e->has_open = false;
    }
    free(history);""", "17_wait_spans_frontiers", "wait_time"),
    ("E5_merge_zero_waits", "if (!null_event(current))", """if (!null_event(current) &&
                !(entry == t->t && waits &&
                  candidate[waits - 1].start == entry &&
                  candidate[waits - 1].end == t->t &&
                  same(candidate[waits - 1].event, current)))""",
     "25_distinct_zero_length_waits", "completed_waits"),
    ("E6_trust_unqualified", "if (!file->qualified) continue;", "; /* trust certificate-shaped records */",
     "27_preempt_rt_unqualified", "wait_time"),
    ("E7_trust_legacy", "free(history);", """
    if (file->version < 5 && count) {
        e->invalid_fields = 0;
        e->life_start = history[0].record->t - history[0].record->duration;
        out->coverage = (pgwt_core_span){e->life_start, history[count - 1].record->t};
        e->coverage = &out->coverage;
        e->coverage_count = 1;
        for (size_t j = 0; j < count; ++j) {
            const pgwt_evidence_record *tr = history[j].record;
            if (!null_event(tr->old_event))
                out->waits[e->wait_count++] = (pgwt_core_wait){tr->old_event, tr->t - tr->duration, tr->t};
        }
        const pgwt_evidence_record *last = history[count - 1].record;
        e->has_open = !null_event(last->new_event);
        e->open = (pgwt_core_open_wait){last->new_event, last->t, last->t};
    }
    free(history);""", "28_legacy_v2", "wait_time"),
    ("E8_shift_coverage", "out->coverage = (pgwt_core_span){attach->t, endpoint};",
     "out->coverage = (pgwt_core_span){attach->t + 1, endpoint};",
     "17_wait_spans_frontiers", "projection.coverage[0][0]"),
    ("E9_accept_end", 'return unsupported(why, "unsupported record kind");',
     'continue; /* ignore unknown storage kinds */',
     "19_end_without_final_frontier", "unsupported"),
]

differential_mutants = [
    ("E10_no_physical_prefix", "history[j].order >= i || ", "",
     "transition_after_frontier", "projection.waits.length"),
    ("E11_no_pid_check", "if (r->kind != PGWT_EVIDENCE_FRONTIER && r->pid != in->pid)",
     "if (0)", "refuse_other_pid_transition", "keys"),
    ("E12_no_duration_check", "(!null_event(current) && t->t - t->duration != entry)", "false",
     "inconsistent_duration", "keys"),
    ("E13_no_old_state_check", "!same(t->old_event, current) ||", "",
     "inconsistent_old_state", "keys"),
    ("E14_no_transition_lifetime", "start <= t->t &&", "true &&",
     "transition_before_fork", "projection.coverage.length"),
    ("E15_no_attach_lifetime", "if (attach->t < life->start || (life->has_exit && attach->t >= life->exit)) {", "if (0) {",
     "refuse_attach_before_fork", "keys"),
    ("E16_no_frontier_advance", "if (has_frontier && r->t <= frontier) {", "if (0) {",
     "refuse_nonadvancing_frontier_15", "keys"),
]


# U2 semantic guard inventory, one independent deletion per row. This also
# repeats the U1 prefix/state gates touched by the replay refactor. Allocation,
# pointer checks and loop mechanics are not evidence-semantic deletions.
# --guard-sweep runs BOTH full suites for every deletion; the default mode
# proves the named distinguishing assertion, never a crash or sanitizer error.
guard_mutants = [
    ('G01_target_capture_count', 'if (same(f->capture, in->capture)) ++targets;', '++targets;', '24_restart_same_pid', 'keys'),
    ('G02_rotation_is_not_restart', 'same(in->files[j].capture, f->capture) || ', '', 'refuse_restart_duplicate_non_target', 'keys'),
    ('G03_restart_clock_domain', '!same(in->files[j].boot, f->boot)', 'false', 'refuse_restart_other_boot', 'keys'),
    ('G04_observed_requires_fork', '!r->has_fork || r->start != r->fork', 'r->start != r->fork', 'refuse_life_observed_no_fork', 'keys'),
    ('G05_observed_start_is_fork', '!r->has_fork || r->start != r->fork', '!r->has_fork', 'refuse_life_observed_fork_mismatch', 'keys'),
    ('G06_provisional_has_no_fork', ': r->has_fork) ||', ': false) ||', 'refuse_life_provisional_with_fork', 'keys'),
    ('G07_exit_not_before_start', 'r->exit < r->start || ', '', 'refuse_life_exit_before_start', 'keys'),
    ('G08_exit_not_after_observation', 'r->exit > r->t', 'false', 'refuse_life_exit_after_observation', 'keys'),
    ('G09_attachment_kind', 'f->records[j].kind == PGWT_EVIDENCE_ATTACH_OK && ', '', '01_ordinary_run', 'keys'),
    ('G10_attachment_generation', ' && key(r, &f->records[j])', '', '02_pid_reuse_delayed_life', 'keys'),
    ('G11_reserved_new_event', ' && !exit_event(r->new_event)', '', '02_pid_reuse_delayed_life', 'keys'),
    ('G12_key_fork_presence', 'a->has_fork == b->has_fork && ', '', 'reuse_provisional_then_observed', 'projection.coverage.length'),
    ('G13_key_fork_timestamp', 'a->fork == b->fork', 'true', '02_pid_reuse_delayed_life', 'keys'),
    ('G14_life_start_immutable', 'g->life->start != r->start ||', '', 'life_lower_bound_changed', 'projection.life'),
    ('G15_life_exit_not_retracted', '!r->has_exit || ', '', 'life_zero_exit_retracted', 'projection.life'),
    ('G16_life_exit_immutable', 'g->life->exit != r->exit', 'false', 'life_exit_changed', 'projection.life'),
    ('G17_life_exit_update_requires_previous_exit', 'g->life->has_exit && ', '', '02_pid_reuse_delayed_life', 'projection.coverage.length'),
    ('G18_discovery_is_first_life', 'if (!g->life) g->discovery = r->t;', 'g->discovery = r->t;', 'provisional_duplicate_discovery', 'projection.coverage.length'),
    ('G19_final_life_version', 'file->version == 5 && r->kind', 'r->kind', 'legacy_life_shaped_records', 'projection.life'),
    ('G20_final_life_kind', 'r->kind == PGWT_EVIDENCE_LIFE &&\n            r->has_fork', 'r->has_fork', '02_pid_reuse_delayed_life', 'projection.life'),
    ('G21_final_key_fork_presence', 'r->has_fork == in->has_fork && ', '', 'reuse_invalid_old_life', 'projection.life'),
    ('G22_final_key_fork_timestamp', 'r->fork == in->fork', 'true', '02_pid_reuse_delayed_life', 'projection.life'),
    ('G23_life_capture_isolation', 'if (target_capture && final.life', 'if (final.life', 'restart_life_exit_isolation', 'projection.life[1]'),
    ('G24_invalid_final_life', 'final.life && !final.invalid', 'final.life', 'life_exit_retracted', 'projection.life'),
    ('G25_prefix_life_dispatch', 'if (r->kind == PGWT_EVIDENCE_LIFE)\n            lifetime', 'if (true)\n            lifetime', '01_ordinary_run', 'projection.coverage.length'),
    ('G26_prefix_attach_dispatch', 'if (r->kind == PGWT_EVIDENCE_ATTACH_OK)\n            g[', 'if (true)\n            g[', '02_pid_reuse_delayed_life', 'keys'),
    ('G27_frontier_dispatch', 'if (r->kind != PGWT_EVIDENCE_FRONTIER) continue;', '', '02_pid_reuse_delayed_life', 'keys'),
    ('G28_assignment_cutoff', ' || t->t > frontier', '', 'frontier_exact_transition', 'projection.waits.length'),
    ('G29_assignment_invalid_life', 'if (!life || g[k].invalid) continue;', 'if (!life) continue;', 'reuse_invalid_old_life', 'projection.coverage.length'),
    ('G30_provisional_discovery_bound', 'life->has_fork ? life->fork : g[k].discovery', 'life->fork', 'provisional_edge_before_discovery', 'projection.coverage.length'),
    ('G31_exit_matching_timestamp', 'life->has_exit && life->exit == t->t', 'life->has_exit', 'exit_unmatched_sentinel', 'projection.coverage.length'),
    ('G32_exit_requires_life_exit', 'life->has_exit && life->exit == t->t', 'life->exit == t->t', 'exit_without_life_exit_at_zero', 'projection.coverage.length'),
    ('G33_ordinary_fork_bound', 'start <= t->t && ', '', '02_pid_reuse_delayed_life', 'projection.coverage.length'),
    ('G34_ordinary_exit_bound', ' && (!life->has_exit || t->t < life->exit)', '', '02_pid_reuse_delayed_life', 'projection.coverage.length'),
    ('G35_unique_assignment', 'if (choices != 1) blocked = true;', '', 'reuse_missing_old_exit', 'projection.coverage.length'),
    ('G36_tracking_invalid_life', 'if (!life || g[k].invalid || !attach) continue;', 'if (!life || !attach) continue;', 'invalid_life_skips_attach', 'keys'),
    ('G37_attach_lower_bound', 'attach->t < life->start || ', '', 'refuse_attach_before_fork', 'keys'),
    ('G38_attach_exit_bound', 'life->has_exit && attach->t >= life->exit', 'false', 'exit_attach_at_exit', 'keys'),
    ('G39_unresolved_identity_blocks', 'blocked || attach->t > frontier', 'attach->t > frontier', 'reuse_missing_old_exit', 'projection.coverage.length'),
    ('G40_attach_before_frontier', 'blocked || attach->t > frontier', 'blocked', 'refuse_last_frontier_before_attach', 'projection.coverage.length'),
    ('G41_endpoint_requires_exit', 'life->has_exit && life->exit < frontier ?', 'life->exit < frontier ?', '01_ordinary_run', 'projection.coverage[0][1]'),
    ('G42_endpoint_capped_by_frontier', 'life->has_exit && life->exit < frontier ?', 'life->has_exit ?', 'exit_after_frontier', 'projection.coverage.length'),
    ('G43_assigned_generation', 'if (assigned[j] != k) continue;', '', '02_pid_reuse_delayed_life', 'keys'),
    ('G44_exit_is_terminal', 'terminal || !same(t->old_event, current)', '!same(t->old_event, current)', 'duplicate_nonwaiting_exit', 'keys'),
    ('G45_coverage_capture_isolation', 'bool target = target_capture && key(', 'bool target = key(', 'restart_same_key_reversed', 'projection.coverage[0][0]'),
    ('G46_coverage_generation_isolation', 'key(g[k].identity, &(pgwt_evidence_record){\n                .has_fork = in->has_fork, .fork = in->fork})', 'true', 'reuse_other_generation_terminal', 'projection.coverage[0][0]'),
    ('G47_invalid_life_suppresses_coverage', 'if (!target || final.invalid) continue;', 'if (!target) continue;', 'life_exit_retracted', 'projection.coverage.length'),
    ('G48_fallback_requires_known_exit', 'e->has_life_end && endpoint', 'endpoint', '01_ordinary_run', 'projection.coverage.length'),
    ('G49_fallback_reaches_exit', 'endpoint >= e->life_end && ', '', 'exit_missing_terminal_fallback', 'projection.coverage.length'),
    ('G50_fallback_requires_missing_terminal', ' && !terminal) continue;', ') continue;', '03_exit_inside_window', 'projection.coverage.length'),
    ('G51_life_start_known_mode', '(r->start_known ? (!r->has_fork || r->start != r->fork) : r->has_fork)', '(!r->has_fork || r->start != r->fork)', '06_preexisting_before_capture', 'keys'),
    ('G52_exit_validation_presence', 'r->has_exit && (r->exit < r->start || r->exit > r->t)', '(r->exit < r->start || r->exit > r->t)', '02_pid_reuse_delayed_life', 'keys'),
    ('G53_generation_lookup', 'if (key(g[i].identity, r)) return i;', 'return i;', '02_pid_reuse_delayed_life', 'projection.coverage.length'),
    ('G54_identity_match', 'if (matches) { assigned[j] = k; ++choices; }', '{ assigned[j] = k; ++choices; }', '02_pid_reuse_delayed_life', 'projection.coverage.length'),
    ('G55_assignment_exit_presence', '!life->has_exit || t->t < life->exit', 't->t < life->exit', '01_ordinary_run', 'projection.coverage.length'),
    ('G56_attach_exit_presence', 'life->has_exit && attach->t >= life->exit', 'attach->t >= life->exit', '01_ordinary_run', 'keys'),
    ('G57_exit_clears_open', 'current = terminal ? (pgwt_core_event){0} : t->new_event;', 'current = t->new_event;', '03_exit_inside_window', 'projection.open'),
    ('G58_key_ignores_absent_fork_bytes', '(!a->has_fork || a->fork == b->fork)', '(a->fork == b->fork)', 'native_provisional_fork_bytes', 'projection'),
    ('G59_final_ignores_absent_fork_bytes', '(!r->has_fork || r->fork == in->fork)', '(r->fork == in->fork)', 'native_provisional_fork_bytes', 'projection'),
    ('G60_new_event_validation', '(!state(r->new_event) && !exit_event(r->new_event))', 'false', 'refuse_transition_empty_new', 'keys'),
    ('G61_observed_fork_not_discovery', 'life->has_fork ? life->fork : g[k].discovery', 'g[k].discovery', 'observed_life_delayed_observation', 'projection.coverage.length'),
    ('G62_capture_failure_propagates', 'if (status != PGWT_REPLAY_OK) goto done;', '', 'inconsistent_duration', 'keys'),
    ('G63_physical_prefix', 'history[j].order >= i || ', '', 'transition_after_frontier', 'projection.waits.length'),
    ('G64_frontier_advance', 'if (has_frontier && r->t <= frontier) {', 'if (false) {', 'refuse_nonadvancing_frontier_15', 'keys'),
    ('G65_duration_consistency', '(!null_event(current) && t->t - t->duration != entry)', 'false', 'inconsistent_duration', 'keys'),
    ('G66_old_state_consistency', '!same(t->old_event, current) ||', '', 'inconsistent_old_state', 'keys'),
    ('G67_reread_reconciles_anchor', 'if (t->t <= attach->t) continue;', '', 'transitions_through_reread', 'keys'),
    ('G68_qualification', 'if (!file->qualified) continue;', '', '27_preempt_rt_unqualified', 'projection.coverage.length'),
    ('G69_legacy_prefix', 'if (file->version != 5) continue;', '', 'legacy_life_shaped_records', 'projection.coverage.length'),
    ('G70_attach_reread_only', 'if (!r->reread || !null_event(r->seed)', 'if (!null_event(r->seed)', 'guard_attach_transition', 'keys'),
    ('G71_attach_seed_null', '!r->reread || !null_event(r->seed) || ', '!r->reread || ', 'guard_attach_seed_nonnull', 'keys'),
    ('G72_attach_state_null', ' || !null_event(r->state))', ')', 'guard_attach_state_nonnull', 'keys'),
    ('G73_ordinary_exit_strict', 't->t < life->exit))', 't->t <= life->exit))', 'guard_ordinary_edge_then_terminal_at_exit', 'projection.coverage.length'),
    ('G74_transition_changes_state', '                       same(r->old_event, r->new_event) || r->duration > r->t', '                       r->duration > r->t', 'guard_same_event_transition', 'keys'),
    ('G75_duration_within_timestamp', 'same(r->old_event, r->new_event) || r->duration > r->t', 'same(r->old_event, r->new_event)', 'guard_duration_exceeds_timestamp', 'keys'),
    ('G76_old_state_valid', '} else if (!state(r->old_event) || ', '} else if (', 'guard_empty_old_state', 'keys'),
    ('G77_life_start_not_future', ' ||\n                    r->start > r->t ||', ' ||', 'guard_life_start_after_observation', 'keys'),
    ('G78_attach_zero_counters', 'if (!zero_counters(&r->counters))\n                    return unsupported(why, "unsupported attachment counters");', '', 'guard_attach_nonzero_counter', 'keys'),
    ('G79_first_physical_discovery', '    if (!g->life) g->discovery = r->t;\n', '    if (!g->life || r->t < g->discovery) g->discovery = r->t;\n', 'guard_first_physical_discovery', 'projection.coverage.length'),
]

contract_guards = {'G58_key_ignores_absent_fork_bytes', 'G59_final_ignores_absent_fork_bytes'}


def differential_case(binary, work, case):
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        status = diff.differential(binary, work, case)
    text = output.getvalue()
    # The differential rejects crashes, sanitizer errors and malformed output.
    valid = bool(re.search(r"^evidence differential: [01] reference cases, [01] unsupported cases, "
                           r"1 differences, seed=0x[0-9a-f]+$", text, re.M))
    return status, text, valid


def contract(binary):
    result = diff.execute([binary, diff.CONTRACT])
    output = result.stdout + result.stderr
    rows = re.findall(r"^evidence \S+ (?:PASS|UNSUPPORTED-OK|FAIL .+)$", output, re.M)
    valid = len(rows) == 34 and not diff.sanitizer_error(output) and " ERROR " not in output
    return result.returncode, output, valid


def full_differential(binary, work):
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        status = diff.differential(binary, work)
    text = output.getvalue()
    valid = bool(re.search(r"^evidence differential: [0-9]+ reference cases, [0-9]+ unsupported cases, "
                           r"[0-9]+ differences, seed=0x[0-9a-f]+$", text, re.M))
    return status, text, valid


def main():
    if sys.argv[1:] not in ([], ["--guard-sweep"]):
        raise RuntimeError("usage: evidence_mutants.sh [--guard-sweep]")
    sweep = bool(sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="pgwt-evidence-mutants-") as temp:
        work = Path(temp)
        base = work / "baseline"
        (base / "src").mkdir(parents=True)
        (base / "tests").mkdir()
        shutil.copytree(root / "src/core", base / "src/core")
        for name in ("test_core_evidence_contract.c", "evidence_json.c", "evidence_json.h"):
            shutil.copy2(root / "tests" / name, base / "tests" / name)
        binary = base / "contract-test"
        diff.build(base, binary)
        status, output, valid = contract(binary)
        if status != 0 or not valid:
            raise RuntimeError("baseline contract failed:\n" + output)
        if diff.differential(binary, work):
            raise RuntimeError("baseline differential failed")
        failed = False
        all_mutants = guard_mutants if sweep else mutants + differential_mutants + guard_mutants
        differential_names = {m[0] for m in differential_mutants + guard_mutants} - contract_guards
        if sweep:
            print("guard | source line | contract | differential | intended assertion | result", flush=True)
        for name, old, new, case, field in all_mutants:
            directory = work / name
            shutil.copytree(base, directory, ignore=shutil.ignore_patterns("contract-test", "*.dSYM"))
            source = directory / "src/core/evidence_replay.c"
            text = source.read_text()
            if text.count(old) != 1:
                raise RuntimeError(f"{name} needs exactly one mutation site")
            line = text[:text.index(old)].count("\n") + 1
            source.write_text(text.replace(old, new, 1))
            executable = directory / "contract-test"
            try:
                diff.build(directory, executable)
                if sweep:
                    cs, co, cv = contract(executable)
                    ds, do, dv = full_differential(executable, directory)
                    if not cv or not dv or cs not in (0, 1) or ds not in (0, 1):
                        raise RuntimeError(f"invalid suite run: contract={cs}, differential={ds}\n{co}\n{do}")
                    if name in contract_guards:
                        intended = re.search(
                            rf"^evidence {re.escape(case)} FAIL ({re.escape(field)}(?:[.\[].*)?)$", co, re.M)
                        killed = cs == 1 and intended
                    else:
                        intended = re.search(
                            rf"^evidence differential {re.escape(case)} MISMATCH ({re.escape(field)}) ", do, re.M)
                        killed = ds == 1 and intended
                    print(f"{name} | {line} | {'FAIL(assertion)' if cs else 'PASS'} | "
                          f"{'FAIL(assertion)' if ds else 'PASS'} | {case}:{field} | "
                          f"{'KILLED' if killed else 'SURVIVED'}", flush=True)
                    if not killed: failed = True
                    continue
                if name in differential_names:
                    status, output, valid = differential_case(executable, directory, case)
                    intended = re.search(
                        rf"^evidence differential {re.escape(case)} MISMATCH ({re.escape(field)}) ",
                        output, re.M)
                else:
                    status, output, valid = contract(executable)
                    intended = re.search(
                        rf"^evidence {re.escape(case)} FAIL ({re.escape(field)}(?:[.\[].*)?)$",
                        output, re.M)
            except (RuntimeError, OSError) as error:
                print(f"{name} SURVIVED/ERROR (build/run failure is not a kill): {error}", flush=True)
                failed = True
                continue
            if status == 1 and valid and intended:
                extra = "; both result objects unchanged" if name == "E8_shift_coverage" else ""
                print(f"{name} KILLED ({case}, {intended[1]}, build OK{extra})", flush=True)
            else:
                print(f"{name} SURVIVED/ERROR (exit {status}; intended comparison not proven)\n{output}", flush=True)
                failed = True
        label = "evidence guard deletions" if sweep else "evidence mutants"
        print(f"{label}: {len(all_mutants)} checked, " + ("FAIL" if failed else "all KILLED"))
        return 1 if failed else 0


try:
    sys.exit(main())
except Exception as error:
    print(f"evidence mutants: harness error (not a kill): {error}", file=sys.stderr)
    sys.exit(2)
PY
