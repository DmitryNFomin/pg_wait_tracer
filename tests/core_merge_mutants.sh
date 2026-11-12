#!/usr/bin/env bash
# Contract mutation checks. Only the intended clean assertion failure kills.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
exec python3 - "$repo" <<'PYTHON'
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

repo = Path(sys.argv[1])
original = (repo / "src/core/merge.c").read_text()
original_test = (repo / "tests/test_core_merge_contract.c").read_text()
# Exact replacements fail closed when the implementation changes.
mutations = [
    ("M1", [("m->queue[eligible].record.t < W", "m->queue[eligible].record.t <= W")], "strict release"),
    ("M2", [("uint64_t F = W - 1;", "uint64_t F = W;")], "inclusive frontier"),
    ("M3", [("if (!m->report.has_frontier || F > m->report.frontier) {",
              "if (eligible && (!m->report.has_frontier || F > m->report.frontier)) {")], "empty eligible batch"),
    ("M4", [("    CMP(t);\n    CMP(pid);", "    CMP(t);\n    CMP(source);\n    CMP(pid);")], "pid before source"),
    ("M5", [("if (r->t > src->proof.cutoff) src->proof.cutoff = r->t;",
              "if (false) src->proof.cutoff = r->t;")], "implicit perf boundary"),
    ("M6", [("if (in->w > m->sources[in->source].accepted_boundary)", "if (true)"),
            ("if (in->w > m->sources[in->source].proof.cutoff)", "if (true)")],
     "needs: accept w=20, then weaker w=10, then DATA t=15 on that source"),
    ("M7", [("    bool breach = internal(reason);",
              "    if (m->report.status == PGWT_MERGE_SUSPENDED) return m->report.status;\n    bool breach = internal(reason);")], "fault during suspension"),
    ("M8", [("            if (perf && src->has_data && r->t < src->last_t)\n"
              "                return late_fault(m, PGWT_MERGE_CLOCK_REGRESSION, in);\n"
              "            if (r->t < src->accepted_boundary)\n"
              "                return late_fault(m, PGWT_MERGE_SEAL_VIOLATION, in);",
              "            if (r->t < src->accepted_boundary)\n"
              "                return late_fault(m, PGWT_MERGE_SEAL_VIOLATION, in);\n"
              "            if (perf && src->has_data && r->t < src->last_t)\n"
              "                return late_fault(m, PGWT_MERGE_CLOCK_REGRESSION, in);")], "clock diagnostic precedence"),
    ("M9", [("m->report.status = breach ? PGWT_MERGE_EXIT_REQUIRED : PGWT_MERGE_SUSPENDED;",
              "m->report.status = breach ? PGWT_MERGE_SUSPENDED : PGWT_MERGE_SUSPENDED;")], "internal breach exit"),
    # M10 changes the sink adapter's observation point: it incorrectly
    # reports the rejected writer attempt as DATA, while still returning
    # failure to the coordinator. The expected fixture remains untouched.
    ("M10", [("            post->counters = counters(f, member(f->reject, \"C\"));\n"
               "            return false;",
               "            post->counters = counters(f, member(f->reject, \"C\"));\n"
               "            captured *attempt = &f->outputs[f->count++];\n"
               "            attempt->out = *out;\n"
               "            memcpy(attempt->payload, out->record.payload, out->record.payload_size);\n"
               "            attempt->payload[out->record.payload_size] = 0;\n"
               "            attempt->out.record.payload = attempt->payload;\n"
               "            return false;")], "rejected DATA reported as output by sink adapter"),
    ("M11", [("if (r->q > a->next_q) return fault(m, PGWT_MERGE_SEQUENCE_GAP, obs);",
               "if (r->q > a->next_q) a->next_q = r->q;")],
     "needs: Q next_q=1, only q=2 eligible after all sources seal its timestamp"),
    # Permit release evaluation during suspension. All other release checks
    # remain intact, including non-advancing-frontier suppression.
    ("M12", [("    if (m->report.status != PGWT_MERGE_OK) return m->report.status;\n    uint64_t W",
               "    if (m->report.status == PGWT_MERGE_EXIT_REQUIRED) return m->report.status;\n    uint64_t W")],
     "needs: suspended, cutoffs=6, no frontier, CHECK with unchanged baseline C0"),
    ("M13", [("        struct source_state *src = &m->sources[r->source];",
               "        if (m->report.status != PGWT_MERGE_OK) return m->report.status;\n"
               "        struct source_state *src = &m->sources[r->source];")],
     "late DATA ignored while suspended; needs row 24"),
    ("M14", [("        pgwt_merge_attachment *a = &generation->proof;",
               "        pgwt_merge_attachment *a = &generation->proof;\n"
               "        if (!a->identity_proved) return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);")],
     "new generation refused; needs row 25"),
    ("M15", [("m->dependencies[m->dependency_count++] = r->dependency_id;",
               "m->dependencies[m->dependency_count++] = 0;")],
     "dependency never becomes available; needs row 26"),
    ("M16", [("        for (size_t s = 0; s < m->config.source_count; ++s)\n"
               "            m->sources[s].proof.cutoff = 0;",
               "        for (size_t s = 0; s < m->config.source_count; ++s) {\n"
               "            m->sources[s].proof.cutoff = 0;\n"
               "            m->sources[s].accepted_boundary = 0;\n"
               "        }")],
     "REARM resets boundaries; needs row 27"),
    ("M17", [("        if (m->report.status == PGWT_MERGE_OK) {\n"
               "            if (check(m, obs) == PGWT_MERGE_EXIT_REQUIRED) return m->report.status;",
               "        if (false) {\n"
               "            if (check(m, obs) == PGWT_MERGE_EXIT_REQUIRED) return m->report.status;")],
     "REARM accepted while active; needs rows 28/31"),
]
# New mutants run only their named direct API case. Each match requires a
# successful build, the REACHED marker and the exact intended assertion.
guard_mutations = [
    ('M18', [('if (r->dependency == PGWT_MERGE_LOST)\n            return fault(m, PGWT_MERGE_DEPENDENCY_LOST, obs);', '')],
     'dependency_lost', 'dependency_lost VOID before any DATA or FRONTIER'),
    ('M19', [('if (in->record.t <= m->frontiers[i].frontier) {',
              'if (i + 1 == m->frontier_count && in->record.t <= m->frontiers[i].frontier) {')],
     'older_epochs', 'seal_violation VOID invalidates both epochs'),
    ('M20', [('    if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);\n'
              '    if (!same_counters(obs.counters, m->baseline))\n'
              '        return fault(m, counter_fault(obs.counters, m->observed), obs);\n'
              '    /* Fixed', '    /* Fixed')],
     'counter_before_batch', 'counter fault VOID stops publication at its checkpoint'),
    ('M21', [('        if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);\n'
              '        if (!same_counters(obs.counters, m->baseline))\n'
              '            return fault(m, counter_fault(obs.counters, m->observed), obs);\n'
              '        if (!v->text)', '        if (!v->text)')],
     'counter_after_data', 'counter fault VOID stops publication at its checkpoint'),
    ('M22', [('        if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);\n'
              '        if (!same_counters(obs.counters, m->baseline))\n'
              '            return fault(m, counter_fault(obs.counters, m->observed), obs);\n'
              '    }\n    return m->report.status;\n}',
              '    }\n    return m->report.status;\n}')],
     'counter_after_frontier', 'counter fault VOID stops publication at its checkpoint'),
    ('M23', [('if (r->q < a->next_q) return fault(m, PGWT_MERGE_SEQUENCE_REUSE, obs);', '')],
     'sequence_reuse', 'sequence_reuse VOID requires exit before DATA'),
    ('M24', [('if (!r->has_q || !a->sequence_proved)', 'if (false)')],
     'sequence_missing', 'missing sequence evidence VOID before DATA'),
    ('M25', [('if (W == 0 || !m->report.qualified)', 'if (W == 0)')],
     'unqualified_source', 'unqualified cutoff holds DATA and FRONTIER'),
    ('M26', [('if (!in->attachment_proofs_fresh ||', 'if (false ||')],
     'rearm_fresh_proofs', 'stale attachment proofs refuse REARM with VOID'),
    ('M27', [('if (generation->creation == 1 && r->rank != PGWT_MERGE_FORK)', 'if (false)')],
     'fork_requires_init', 'invalid creation proof VOID before lifecycle DATA'),
    ('M28', [('} else if (generation->creation && r->rank == PGWT_MERGE_FORK) {\n'
              '            return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);\n        }', '}')],
     'fork_twice', 'invalid creation proof VOID before lifecycle DATA'),
    ('M29', [('        if (m->report.status != PGWT_MERGE_OK && r->rank != PGWT_MERGE_TRANS)\n'
              '                a->lifecycle_seen = true;', '')],
     'suspended_lifecycle', 'invalid creation proof VOID before lifecycle DATA'),
]
compiler = shlex.split(os.environ.get("CC", "cc"))
flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-g"]
flags += shlex.split(os.environ.get("CFLAGS", ""))
env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:exitcode=86",
           UBSAN_OPTIONS="halt_on_error=1:exitcode=87:print_stacktrace=1")
failed = False
killed = 0
with tempfile.TemporaryDirectory(prefix="pgwt-core-merge-mutants-") as tmp:
    work = Path(tmp)
    (work / "src/core").mkdir(parents=True)
    (work / "tests").mkdir()
    shutil.copy2(repo / "src/core/pgwt_merge.h", work / "src/core/pgwt_merge.h")
    def build_and_run(source, test_source=original_test, guard=None, baseline=False):
        (work / "tests/test_core_merge_contract.c").write_text(test_source)
        (work / "src/core/merge.c").write_text(source)
        build = subprocess.run(compiler + flags + ["tests/test_core_merge_contract.c", "src/core/merge.c",
                               "-o", "contract"], cwd=work, env=env, capture_output=True, text=True, timeout=60)
        if build.returncode:
            return "BUILD_ERROR", build.stderr
        args = (["--guard-case", guard[0]] if guard else
                ([] if baseline else ["--fixture-only"]) +
                [str(repo / "contracts/merge-v1/examples.json")])
        run = subprocess.run([str(work / "contract")] + args, cwd=work, env=env,
                             capture_output=True, text=True, timeout=20)
        log = run.stdout + run.stderr
        if re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:", log):
            return "RUNTIME_ERROR", log
        if guard:
            case, assertion = guard
            reached = f"guard {case} REACHED {assertion}"
            mismatch = f"guard {case} MISMATCH {assertion}"
            lines = log.splitlines()
            if (run.returncode == 1 and reached in lines and mismatch in lines and
                    "SETUP_ERROR" not in log and log.count(" MISMATCH ") == 1):
                return "KILLED", mismatch
            if run.returncode == 0 and f"guard {case} PASS" in lines:
                return "SURVIVED", log
            return "RUNTIME_ERROR", log
        if run.returncode == 1 and re.search(r"^row [0-9]+/[NQ] MISMATCH .+", run.stdout, re.M):
            return "KILLED", re.search(r"^row .+ MISMATCH .+", run.stdout, re.M).group(0)
        if run.returncode == 0 and re.search(r"core merge contract: (\d+)/\1 passed", run.stdout):
            return "SURVIVED", log
        return "RUNTIME_ERROR", log
    # A broken baseline must never masquerade as killed mutants.
    try:
        result, detail = build_and_run(original, baseline=True)
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"baseline ERROR: {exc}", file=sys.stderr)
        sys.exit(2)
    if result != "SURVIVED":
        print(f"baseline ERROR ({result}): {detail}", file=sys.stderr)
        sys.exit(2)
    targets = {name: (case, assertion) for name, _, case, assertion in guard_mutations}
    all_mutations = mutations + [(name, edits, case) for name, edits, case, _ in guard_mutations]
    for name, edits, explanation in all_mutations:
        source = original_test if name == "M10" else original
        try:
            for before, after in edits:
                if source.count(before) != 1:
                    raise ValueError("mutation anchor must occur exactly once")
                source = source.replace(before, after, 1)
            result, detail = (build_and_run(original, source) if name == "M10"
                              else build_and_run(source, guard=targets.get(name)))
        except subprocess.TimeoutExpired:
            result, detail = "TIMEOUT", "build or execution timed out"
        except (OSError, ValueError) as exc:
            result, detail = "ERROR", str(exc)
        if result == "KILLED":
            killed += 1
            print(f"{name} KILLED: {detail}", flush=True)
        elif result == "SURVIVED":
            print(f"{name} SURVIVED: {explanation}", flush=True)
            failed = True
        else:
            print(f"{name} {result}: {detail.strip().replace(chr(10), ' ')[:600]}", flush=True)
            failed = True
print(f"core merge mutants: {killed}/{len(all_mutations)} killed", flush=True)
sys.exit(1 if failed else 0)
PYTHON
