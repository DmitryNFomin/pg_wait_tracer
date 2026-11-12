#!/usr/bin/env bash
# Real edits to temporary source copies. Build/sanitizer/harness failures never
# count as kills. Frozen answers and the checkout's sources stay untouched.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 -B - <<'PY'
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
    ("E1_no_attach", "if (!attach || attach->t > frontier || e->invalid_fields) continue;", """
        if (!attach && !e->invalid_fields) {
            out->coverage = (pgwt_core_span){e->life_start, frontier};
            e->coverage = &out->coverage;
            e->coverage_count = 1;
            continue;
        }
        if (!attach || attach->t > frontier || e->invalid_fields) continue;""",
     "08_attach_failure", "wait_time"),
    ("E2_eof_frontier", "e->waits = out->waits;", """
    if (e->coverage_count && file->record_count) {
        uint64_t tail = file->records[file->record_count - 1].t;
        if (tail > out->coverage.end) {
            out->coverage.end = tail;
            if (e->has_open) e->open.as_of = tail;
        }
    }
    e->waits = out->waits;""", "16_crash_with_uncertified_suffix", "wait_time"),
    ("E3_transition_frontier", "e->waits = out->waits;", """
    for (size_t j = 0; e->coverage_count && j < count; ++j) {
        uint64_t latest = history[j].record->t;
        if (latest > out->coverage.end) {
            out->coverage.end = latest;
            if (e->has_open) e->open.as_of = latest;
        }
    }
    e->waits = out->waits;""", "16_crash_with_uncertified_suffix", "wait_time"),
    ("E4_frontier_close", "e->waits = out->waits;", """
    if (e->has_open) {
        out->waits[e->wait_count++] = (pgwt_core_wait){e->open.event, e->open.start, e->open.as_of};
        e->has_open = false;
    }
    e->waits = out->waits;""", "17_wait_spans_frontiers", "wait_time"),
    ("E5_merge_zero_waits", "if (!null_event(current))", """if (!null_event(current) &&
                !(entry == t->t && e->wait_count &&
                  out->waits[e->wait_count - 1].start == entry &&
                  out->waits[e->wait_count - 1].end == t->t &&
                  same(out->waits[e->wait_count - 1].event, current)))""",
     "25_distinct_zero_length_waits", "completed_waits"),
    ("E6_trust_unqualified", "if (!file->qualified) continue;", "; /* trust certificate-shaped records */",
     "27_preempt_rt_unqualified", "wait_time"),
    ("E7_trust_legacy", "e->waits = out->waits;", """
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
    e->waits = out->waits;""", "28_legacy_v2", "wait_time"),
    ("E8_shift_coverage", "out->coverage = (pgwt_core_span){attach->t, frontier};",
     "out->coverage = (pgwt_core_span){attach->t + 1, frontier};",
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
    ("E14_no_transition_lifetime", "if (t->t < e->life_start) {", "if (0) {",
     "refuse_transition_before_fork", "keys"),
    ("E15_no_attach_lifetime", "if (attach->t < e->life_start) {", "if (0) {",
     "refuse_attach_before_fork", "keys"),
    ("E16_no_frontier_advance", "if (has_frontier && r->t <= frontier) {", "if (0) {",
     "refuse_nonadvancing_frontier_15", "keys"),
]


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
    valid = len(rows) == 33 and not diff.sanitizer_error(output) and " ERROR " not in output
    return result.returncode, output, valid


def main():
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
        all_mutants = mutants + differential_mutants
        for name, old, new, case, field in all_mutants:
            directory = work / name
            shutil.copytree(base, directory, ignore=shutil.ignore_patterns("contract-test", "*.dSYM"))
            source = directory / "src/core/evidence_replay.c"
            text = source.read_text()
            if text.count(old) != 1:
                raise RuntimeError(f"{name} needs exactly one mutation site")
            source.write_text(text.replace(old, new, 1))
            executable = directory / "contract-test"
            try:
                diff.build(directory, executable)
                if name in {m[0] for m in differential_mutants}:
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
        print(f"evidence mutants: {len(all_mutants)} checked, " + ("FAIL" if failed else "all KILLED"))
        return 1 if failed else 0


try:
    sys.exit(main())
except Exception as error:
    print(f"evidence mutants: harness error (not a kill): {error}", file=sys.stderr)
    sys.exit(2)
PY
