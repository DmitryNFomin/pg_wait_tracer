#!/usr/bin/env python3
"""Daemon capacity recovery schedules and mutants; host C only, no kernel work.

A kill requires a clean build, a named reached case, and its intended assertion.
All altered sources and binaries live in an external temporary directory.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-DPGWT_TRANSPORT_HOST_TEST", "-Isrc"]
FLAGS += shlex.split(os.environ.get("CFLAGS", ""))
ENV = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:exitcode=86",
           UBSAN_OPTIONS="halt_on_error=1:exitcode=87:print_stacktrace=1")
SOURCES = ["tests/capture_recovery_host.c", "src/percpu_transport.c",
           "src/core/merge_adapter.c", "src/core/merge.c"]
MUTANTS = [
    ("backlog_kept", "src/percpu_transport.c",
     "t->count = t->queue_head = t->release_left = 0;",
     "t->release_left = 0; /* old queue retained */", "arrival",
     "pgwt_transport_pending_records(d.transport)==0"),
    ("stale_baseline", "src/percpu_transport.c",
     "counters(t); /* a NEW full read, never the pre-VOID borrowed baseline */",
     "/* pre-VOID snapshot reused */", "baseline",
     "d.reads>reads && pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED"),
    ("stale_attachment_proofs", "src/merge_capture.c", None, None, "proofs",
     "pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED"),
    ("omit_pause_time", "src/merge_capture.c",
     "c->pause_ns += c->pause_end - c->pause_start;", "/* pause time lost */", "arrival",
     "!cov.open && cov.last_end>=cov.last_start && cov.pause_ns>0"),
    ("omit_unavailable_time", "src/merge_capture.c",
     "s.unavailable_ns = s.pause_ns;",
     "s.unavailable_ns = 0;", "arrival", "c.covered_ns+c.unavailable_ns==c.window_ns"),
    ("rearm_lifetime", "src/transport_diagnostics.h",
     "(pool == PGWT_POOL_TRANSPORT_QUEUE || pool == PGWT_POOL_CAPTURE_ARRIVAL ||",
     "(pool == PGWT_POOL_CAPTURE_LIFETIME || pool == PGWT_POOL_TRANSPORT_QUEUE || pool == PGWT_POOL_CAPTURE_ARRIVAL ||",
     "lifetime", "pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED"),
    ("rearm_noncapacity", "src/transport_diagnostics.h",
     "return reason == PGWT_MERGE_ALLOCATION_FAIL &&",
     "return reason == PGWT_MERGE_UNKNOWN_EVIDENCE || reason == PGWT_MERGE_ALLOCATION_FAIL &&",
     "noncapacity", "pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED"),
    ("trust_preproof_data", "src/merge_capture.c", "(c->rearm_at && e->kind != MA_TEXT &&",
     "(false && c->rearm_at && e->kind != MA_TEXT &&", "arrival",
     "pgwt_capture_stats(d.capture).released_data==before+1"),
    ("uncapped_backoff", "src/percpu_transport.c",
     "return previous >= PGWT_TRANSPORT_RECOVERY_CAP_NS / 2 ?\n        PGWT_TRANSPORT_RECOVERY_CAP_NS : previous * 2;",
     "return previous * 2;", "backoff",
     "pgwt_transport_recovery_delay(delay)<=UINT64_C(5000000000)"),
    ("retained_backlog_kept", "src/merge_capture.c",
     "c->retained_count = c->staged = 0;", "c->staged = 0;", "retained",
     "pgwt_capture_pending_records(d.capture)==0"),
    ("hide_retained_seal", "src/merge_capture.c",
     "if (c->rearm_at && pgwt_capture_report(c).status == PGWT_MERGE_OK &&",
     "if (false && c->rearm_at && pgwt_capture_report(c).status == PGWT_MERGE_OK &&",
     "retained_seal", "pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED"),
    ("proof_start_as_lower_bound", "src/merge_capture.c",
     "if (c->config.now) now = c->config.now(c->config.ctx);",
     "/* old attempt start reused as proof time */", "proof_time",
     "d.capture->rearm_at==proof_end"),
    ("rearm_after_proof_fault", "src/merge_capture.c",
     "if (!pgwt_capture_capacity_paused(c)) return 0; /* acquisition observed another fault */",
     "/* ignore the acquisition's non-capacity fault */", "proof_fault",
     "pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED"),
    ("skip_paused_checks", "src/percpu_transport.c", "if (pgwt_transport_service_due(now, t->recovery_check_at, t->recovery_checked)) {",
     "if (false && pgwt_transport_service_due(now, t->recovery_check_at, t->recovery_checked)) {", "counter_change",
     "pgwt_capture_report(d.capture).faults[PGWT_MERGE_COUNTER_CHANGE]==1"),
    ("reuse_pre_rearm_round", "src/percpu_transport.c", "t->round += 2;",
     "/* retired round reused */", "stale_round",
     "pgwt_capture_stats(d.capture).released_data==before"),
    ("skip_unknown_rearm_check", "src/merge_capture.c",
     "    /* Every observed vector reaches CHECK, including unreadable observations",
     "    if (!s || !s->valid) return 0;\n    for (size_t i=0;i<PGWT_MERGE_COUNTERS;++i) if (!s->component_valid[i]) return 0;\n"
     "    /* Every observed vector reaches CHECK, including unreadable observations",
     "baseline", "pgwt_capture_report(d.capture).faults[PGWT_MERGE_COUNTER_UNKNOWN]==1"),
    ("skip_unknown_backoff_check", "src/percpu_transport.c",
     "int rc = transport_deliver(t, &check, 0, now);\n            if (rc < 0) return rc;\n        }\n        return 1;",
     "int rc = t->snapshot.valid ? transport_deliver(t, &check, 0, now) : 0;\n            if (rc < 0) return rc;\n        }\n        return 1;",
     "baseline", "pgwt_capture_report(d.capture).faults[PGWT_MERGE_COUNTER_UNKNOWN]==1"),
    ("skip_unknown_daemon_poll", "src/merge_capture.c",
     "    /* Decide cadence before reading any counter component.",
     "    if (pgwt_capture_capacity_paused(d->merge_capture)) return 0;\n"
     "    /* Decide cadence before reading any counter component.",
     "baseline", "d.reads>reads && pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED"),
    ("corrupt_covered_time", "src/merge_capture.c",
     "s.covered_ns = s.epoch_invalid ? 0 : c->covered_ns;",
     "s.covered_ns = s.epoch_invalid ? 0 : c->covered_ns + 1;",
     "arrival", "c.covered_ns+c.unavailable_ns==c.window_ns"),
]


def run(command, tree, check=True):
    result = subprocess.run(command, cwd=tree, env=ENV, text=True, capture_output=True)
    if check and result.returncode:
        raise AssertionError(f"{shlex.join(map(str, command))}\n{result.stdout}{result.stderr}")
    return result


def build(tree):
    binary = tree / "recovery"
    run(shlex.split(os.environ.get("CC", "cc")) + FLAGS + SOURCES + ["-o", str(binary)], tree)
    return binary


with tempfile.TemporaryDirectory(prefix="pgwt-capacity-recovery-") as temporary:
    tree = Path(temporary)
    shutil.copytree(ROOT / "src", tree / "src")
    (tree / "tests").mkdir()
    for name in ["capture_recovery_host.c", "test_percpu_transport.c", "test_core_merge_adapter.c",
                 "test_core_merge_contract.c", "merge_adapter_goldens.h"]:
        shutil.copy2(ROOT / "tests" / name, tree / "tests" / name)
    baseline = run([str(build(tree))], tree)
    assert "capacity recovery: 16/16 PASS" in baseline.stdout
    for line in baseline.stdout.splitlines():
        if line.startswith(("exact-perf:", "capacity recovery:", "stale-round", "counter-unknown", "recovery ", "corrupted coverage:")):
            print(line)
    for name, path, before, after, case, assertion in MUTANTS:
        file = tree / path
        source = file.read_text()
        if name == "stale_attachment_proofs":
            # Deliberately assert freshness from the old lifetime IDs, without
            # invoking the run's proof acquisition mechanism.
            start = source.index("    if (!c->config.test_attach_premise) return 0; /* production A2 pending */")
            end = source.index("    observation(c, now, s);", start)
            mutated = source[:start] + """    for (size_t i = 0; i < c->lifetime_count; ++i)
        c->fresh_proofs[i] = (pgwt_merge_attachment){.pid=c->lifetimes[i].pid,
            .g=c->lifetimes[i].g,.identity_proved=true};
""" + source[end:]
        else:
            assert source.count(before) == 1, f"{name}: mutation anchor must occur once"
            mutated = source.replace(before, after)
        # Keep the policy predicate parenthesized in the permissive mutant so
        # -Werror still builds it; compilation failure is never a kill.
        if name == "rearm_noncapacity":
            mutated = mutated.replace("reason == PGWT_MERGE_ALLOCATION_FAIL &&\n", "(reason == PGWT_MERGE_ALLOCATION_FAIL &&\n")
            mutated = mutated.replace("pool == PGWT_POOL_CORE_STAGING);", "pool == PGWT_POOL_CORE_STAGING));")
        file.write_text(mutated)
        binary = build(tree)
        result = run([str(binary), case], tree, False)
        assert (result.returncode == 1 and f"REACHED recovery {case}" in result.stdout
                and f"transport ASSERT {case}: {assertion} (line " in result.stderr
                and not any(x in result.stderr for x in ["AddressSanitizer", "runtime error:"])), (
                    f"{name}: not killed by intended assertion\n{result.stdout}{result.stderr}")
        if name in ("corrupt_covered_time", "omit_unavailable_time"):
            assert "time_identity=0" in result.stdout, result.stdout
            print(next(line for line in result.stdout.splitlines() if "time_identity=0" in line))
        if name == "reuse_pre_rearm_round":
            assert "released_after_old_EL=1" in result.stdout, result.stdout
            print(next(line for line in result.stdout.splitlines() if line.startswith("stale-round")))
        print(f"KILLED {name}: built, reached {case}, failed {assertion}")
        file.write_text(source)
    print(f"capacity recovery mutants: {len(MUTANTS)}/{len(MUTANTS)} killed")
