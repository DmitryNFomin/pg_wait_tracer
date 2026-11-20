#!/usr/bin/env python3
"""Portable old/new wire replay, production-token guards and non-gating timing.

Only C host code runs. Old sources and their transitive headers are vendored
byte-for-byte from d926bf22 with pinned hashes; current schedules drive both.
Temporary trees, binaries and qualification streams live outside the repo.
"""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / "tests/fixtures/capture-d926bf22"
# Frozen migration sources, never a numerical oracle.
BASE_SHA256 = {
    "src/anomaly.h": "ce327fda14725461ea99862f79b5d72321ae1b1fbbd1ea91c1b24fd236eec227",
    "src/backend.h": "1ec6e5c0c805141a86612de3af5d0dbce5f142baf1371ebb6c8435fd503fc67a",
    "src/backend_meta.h": "46da3175873c7e88c4b319454d805afc0d914f71b5aaeec02b502a51ede6a5c0",
    "src/backend_status_layout.h": "e528ab4ddf0655b522a2a706e275c6e3375e07253469b2df30bca9df79b0ebb5",
    "src/capture_counters.h": "3ed84044d0ec62c1e7bf49c75ddaf4ce766f43015ccdd207010c2408b1ea6c61",
    "src/cmdline.h": "01838a8938587ef89f4837e8bb097beef099c2ae49ded918dc4e32c1e916c9f4",
    "src/core/merge.c": "b51b3c8cdf21e744c38cf95ad2a137efdee7309ad8d9e743e8cc019bf7191897",
    "src/core/merge_adapter.c": "2cf761181210ed843887bc5f60f9d4d0d57e4b33222aff4e9f60b613caa413da",
    "src/core/pgwt_diagnostics.h": "ee5cd8ae6a1860b202ffadc35e668b5cb7f49fc7574e8605c524940d226060e5",
    "src/core/pgwt_merge.h": "16bb9187a5cc36c00d850a80f71c68dbf3058240cb830e8c8694a381b365cf20",
    "src/core/pgwt_merge_adapter.h": "228ccff912e63b3f31b09a636db65ed2a2982ee1aa772f450af8dc75cdbedeff",
    "src/counter_vector.c": "c0409270be11991ce314e046358f95a5d8d0cbfd8ea3153f06349b62b32a58c4",
    "src/counter_vector.h": "208856883f714ce973357240d8f0bc334761f7f69ed2847c19832205d5a1f311",
    "src/daemon.h": "7a492d6633581e5af876ce295fd90eb9fae2e04f03461a0f19fb629734365675",
    "src/effective_cores.h": "dc66c39a6775bec4ce954ba63a373316f58707bf37cad23cf45f479cfac97aa6",
    "src/escalation.h": "3914ec12e44f78520783eca658fab6f2a0720246f2d651b2e04049840a64a231",
    "src/event_stream.h": "5ffc6de1f0fd265d490674e2a5a0d033986405e16c81935706a97879bb144f9d",
    "src/event_writer.h": "c0f6003a82d7998363ebbd8e650230f41656f8573387fd9744118eef528f7ec0",
    "src/exact_probe.h": "9ecb22419dfe16fc2fad02d3f4aef13698f20c656e0c3387b6c55155b9261c7e",
    "src/map_reader.h": "57d1d5b00f686fb5426dcc90b6a744c9637f00c89ae2447696cad3b1219db3de",
    "src/merge_capture.c": "8327802b19389c5c6c1151ef79ddcec66908aafaa74258943bd6033207e75d49",
    "src/merge_capture.h": "252e0e6fe66b9eb2501828179d68bc977e649446f9a8e27fb2971239dee01437",
    "src/open_ckpt.h": "40ea087c31e0b1d62b8b4cda817d980521e6da88212a92d3ca7d8f4f63a2aeb7",
    "src/percpu_transport.c": "7585ea848889359576e74c16ba7c9510cb0499a5435d1021c01843fd5d2b0761",
    "src/percpu_transport.h": "12579da8fac01b16f4e8a99c9efd4c2b654acea855d462799c8211c5e315890e",
    "src/pg_wait_tracer.h": "9ddb16c08ea36d7ab966092f005859a86b3be7c10bf91c7ffa85ce4f6d79e43c",
    "src/pgss_resolver.h": "2318d615826aef518285754768742f2ad2fd427ccdcfd90cecfe0cc62807410e",
    "src/pgwt_transport_wire.h": "ca6b9a3f8d51c100ae3322b842eb4cddb7086d069b50726fb3b7adccbfb2e897",
    "src/phase_capability.h": "7ff4943ad036607e21fef7b7003b741368f6b2f3585085a0ff5f64feb02f2028",
    "src/provider.h": "37a73651e47b126195436492bbb1142b868996375634a2b5cc59f3620b624283",
    "src/query_attr.h": "6fa38053b9ba344d69444bc11998462ece3fcfab3f0b4d1cf5c401387606ff74",
    "src/query_text.h": "2612f41bdd1d7f119b2d75cfbbc845d549254ba23f9aee7a18bf0af582205232",
    "src/sampler.h": "9834ce0eb7c782707c68b01f39bf5579f077b66622764f4b84da429981c6053c",
    "src/snapshot.h": "1b2aadda4ee519b008ac774ccd81a34e7d622159e7bdf3e0b8eb6cb51112856f",
    "src/summary_writer.h": "8a5868f004554390d286280a98b62ef6deecd9902a8ed94f638bb141301dbbc6",
    "src/synthetic_query.h": "697e9c1ad460d0d72f7504d6b4a553b24eb4482ed338bcc84795a705fc52d931",
    "src/trace_closure.h": "b876b99757c4a9e9d0a3cc69f83b710a808520305284bf9dc2979771b6a8b362",
    "src/transport_budget.h": "f6a96abd15a214ee6d1d98209d6dddaac9031d8b83b020113709bae6606e48d7",
    "src/transport_diagnostics.h": "343f5c26eba12ca535d46b0e8feab8dff6eaa67194cb6a684832c1bb2d4bd028"
}
FLAGS = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-DPGWT_TRANSPORT_HOST_TEST"]
FLAGS += shlex.split(os.environ.get("CFLAGS", ""))
CC = shlex.split(os.environ.get("CC", "cc"))
ENV = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:exitcode=86",
           UBSAN_OPTIONS="halt_on_error=1:exitcode=87:print_stacktrace=1")
TESTS = ["capture_bookkeeping_harness.c", "test_percpu_transport.c",
         "test_core_merge_adapter.c", "test_core_merge_contract.c", "merge_adapter_goldens.h"]



def run(command, directory=ROOT, check=True):
    result = subprocess.run(command, cwd=directory, env=ENV, text=True, capture_output=True)
    if check and result.returncode:
        raise AssertionError(f"command failed: {shlex.join(map(str, command))}\n{result.stdout}{result.stderr}")
    return result


def build(tree, legacy=False):
    command = CC + FLAGS + (["-DPGWT_CAPTURE_LEGACY"] if legacy else [])
    command += ["-Isrc", "tests/capture_bookkeeping_harness.c", "src/percpu_transport.c",
                "src/core/merge_adapter.c", "src/core/merge.c", "-o", str(tree / "replay")]
    run(command, tree)
    return tree / "replay"


def evidence(binary, tree):
    stream = tree / "merge-qualification.bin"
    result = run([str(binary), "identity", str(stream)], tree)
    assert "released_data=513 released_text=8" in result.stderr
    assert result.stderr.count("exact-perf: pool_stats ") == 10
    assert result.stderr.count("exact-perf: phase ") == 5
    assert result.stderr.count("exact-perf: reconcile ") == 3
    lines = result.stderr.splitlines(keepends=True)
    accounting = [line for line in lines if line.startswith(("exact-perf: coverage ", "exact-perf: wire_identity "))]
    if tree.name == "new":
        assert len(accounting) == 2
        assert "capacity_pauses=0 recovery_attempts=0 rearms=0" in accounting[0]
        assert "time_identity=1" in accounting[0] and "ok=1" in accounting[1]
    # The new per-run report is additive; it does not change any prior bytes.
    diagnostics = "".join(line for line in lines if line not in accounting)
    return stream.read_bytes(), result.stdout, diagnostics


with tempfile.TemporaryDirectory(prefix="pgwt-capture-bookkeeping-") as temporary:
    work = Path(temporary)
    old, new = work / "old", work / "new"
    # Validate every frozen byte before building either version. No git or
    # historical object is needed on the box or after a squash merge.
    names = BASE_SHA256
    for name, expected in BASE_SHA256.items():
        actual = hashlib.sha256((BASE / name).read_bytes()).hexdigest()
        if actual != expected:
            raise RuntimeError(f"pinned base checksum mismatch: {name}: expected {expected}, got {actual}")
    for tree, legacy in [(old, True), (new, False)]:
        for name in names:
            dest = tree / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            if legacy:
                data = (BASE / name).read_bytes()
                dest.write_bytes(data)
                # Keep the pinned migration files intact. GCC 8 diagnoses the
                # conditional zero compound literal in a designated initializer;
                # use the equivalent assignment in this temporary build only.
                if name in ("src/merge_capture.c", "src/core/merge.c"):
                    text = dest.read_text()
                    owner, variable = ("c", "in") if name.endswith("merge_capture.c") else ("m", "out")
                    before = (",\n        .allocation = reason == PGWT_MERGE_ALLOCATION_FAIL ? "
                              f"{owner}->diagnostics.allocation : (pgwt_allocation_failure){{0}}}};")
                    after = ("};\n    if (reason == PGWT_MERGE_ALLOCATION_FAIL) "
                             f"{variable}.allocation = {owner}->diagnostics.allocation;")
                    assert text.count(before) == 1, name
                    dest.write_text(text.replace(before, after))
            else:
                shutil.copy2(ROOT / name, dest)
        if not legacy:
            shutil.copy2(ROOT / "src/init_loss_diag.h", tree / "src/init_loss_diag.h")
            shutil.copy2(ROOT / "src/attach_timeline.h", tree / "src/attach_timeline.h")
            # daemon.h gained the portable coverage tracker in U1. These are
            # build dependencies only; the pinned migration bytes stay intact.
            for header in ("core/pgwt_discovery.h", "core/pgwt_a2_population.h",
                           "open_snapshot.h", "cJSON.h"):
                shutil.copy2(ROOT / "src" / header, tree / "src" / header)
        for name in TESTS:
            (tree / "tests").mkdir(exist_ok=True)
            shutil.copy2(ROOT / "tests" / name, tree / "tests" / name)
    old_binary, new_binary = build(old, True), build(new)
    before, after = evidence(old_binary, old), evidence(new_binary, new)
    assert before == after, "byte identity: qualification bytes or stats/diagnostics differ"
    print(f"old/new identity: PASS; merge-qualification.bin {len(after[0])} bytes "
          f"sha256={hashlib.sha256(after[0]).hexdigest()}; all {len(after[2].splitlines())} diagnostics lines byte-identical")
    for binary, tree in [(old_binary, old), (new_binary, new)]:
        result = run([str(binary), "local_snapshot"], tree)
        assert "REACHED bookkeeping local_snapshot" in result.stdout
    for guard in ["duplicate", "clock"]:
        result = run([str(new_binary), guard], new)
        assert f"REACHED bookkeeping {guard}" in result.stdout
    print("bookkeeping guards: 4/4 PASS (reuse/prefix bytes, duplicate, published clock, local failure snapshot)")

    capture = new / "src/merge_capture.c"
    original = capture.read_text()
    # Put the original hash probe back on arrival (not a build-error mutant).
    original_old = (old / "src/merge_capture.c").read_text()
    hash_probe = original_old[original_old.index("static size_t arrival_hash("):
                              original_old.index("static void held_age(")]
    hash_probe = hash_probe.replace("    size_t i = arrival_hash", "    ++((struct pgwt_merge_capture *)c)->arrival_probes;\n    size_t i = arrival_hash")
    restored = original.replace("    uint64_t *arrival_positions;",
                                "    uint64_t *arrival_positions;\n    size_t *arrival_buckets;")
    restored = restored.replace("static void held_age(", hash_probe + "static void held_age(")
    restored = restored.replace("    c->arrivals = calloc(cfg->capacity, sizeof(*c->arrivals));",
                                "    c->arrivals = calloc(cfg->capacity, sizeof(*c->arrivals));\n"
                                "    c->arrival_buckets = calloc(c->arrival_slots, sizeof(*c->arrival_buckets));")
    restored = restored.replace("if (!c->lifetime_index", "if (!c->arrival_buckets || !c->lifetime_index")
    restored = restored.replace("        size_t duplicate =", "        (void)arrival_slot(c, e->source, e->position);\n        size_t duplicate =")
    restored = restored.replace("free(c->arrival_positions)", "free(c->arrival_buckets); free(c->arrival_positions)")
    mutants = [
        ("stale_local_snapshot", original.replace(
            "            retain_snapshot(c); /* local failure may invalidate the borrowed vector */", ""),
         "local_snapshot", "!s.valid && s.component_valid[0] && s.component_valid[14] && s.value[14]==0"),
        ("restore_hash_probe", restored, "identity", "d.capture->arrival_probes==0"),
        ("omit_token_free", original.replace(
            "a->next = c->arrival_free; c->arrival_free = index - 1;",
            "a->next = c->arrival_free; /* token free omitted */"),
         "identity", "POOL_EXHAUSTION pool=capture_arrival"),
        ("stale_published_clock", original.replace(
            "o->d = (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;",
            "o->d = d->merge_capture->budget.started;"), "clock", "o.d>=before && o.d<=after"),
        ("drop_duplicate_check", original.replace("        if (duplicate) {", "        if (false && duplicate) {"),
         "duplicate", "pgwt_capture_received_token(d.capture,&e,20,&token)==-EIO"),
    ]
    for name, source, guard, assertion in mutants:
        assert source != original, f"{name}: missing anchor"
        capture.write_text(source)
        binary = build(new)
        result = run([str(binary), guard], new, False)
        assert (result.returncode == 1 and f"REACHED bookkeeping {guard}" in result.stdout
                and assertion in result.stderr and "transport ASSERT" in result.stderr
                and not any(x in result.stderr for x in ["AddressSanitizer", "runtime error:"])), (
                    f"{name}: not killed at intended assertion\n{result.stdout}{result.stderr}")
        print(f"KILLED {name}: built, reached {guard}, failed {assertion}")
    # A valid mutant that only corrupts the restored prefix must fail byte
    # comparison, even though the core and every completion counter stay OK.
    capture.write_text(original.replace("memcpy(payload, a->prefix, sizeof(a->prefix));",
                                        "memset(payload, 0, sizeof(a->prefix));"))
    binary = build(new)
    corrupted = evidence(binary, new)
    assert corrupted[0] != before[0] and corrupted[1:] == before[1:]
    print("KILLED corrupt_payload_prefix: built, replayed identity, qualification bytes differ")
    print("bookkeeping mutants: 6/6 killed at intended assertions")
    capture.write_text(original)
    new_binary = build(new)
    # Timing is intentionally outside the identity and correctness gates.
    # Both binaries use the same 1,360,001-record stream and compiler options.
    for name, binary, tree in [("old", old_binary, old), ("new", new_binary, new)]:
        result = run([str(binary), "bench"], tree)
        print(f"host benchmark {name}: {result.stdout.splitlines()[-1]} (not a gate)")
