# tests/Makefile — build C unit tests for pg_wait_tracer
# Requires: main project already built (make in parent dir)

CC       ?= gcc
CFLAGS    = -g -O2 -Wall -I../src -I../include
BUILD     = ../build

# libbpf link flags. Defaults to the system shared library; the top-level
# Makefile overrides this (LIBBPF_LIB="path/to/libbpf.a -lelf -lz") on
# Rocky 8 / RHEL 8 where the bundled static libbpf is used instead.
LIBBPF_LIB ?= -lbpf

TESTS     = test_wait_event test_cmdline test_event_writer test_event_reader \
            test_trace_v2 test_sampler test_anomaly test_escalation_budget \
            test_effective_cores \
            test_coop test_bucket \
            test_pg13_resolve test_discovery_pie test_discovery_nopie \
            test_replay_fidelity test_durability test_query_text \
            test_golden_fixture \
            gen_test_traces gen_bench_traces test_concurrent_rw cross_validate \
            dump_markers

.PHONY: all clean check

all: $(TESTS)

# Run the full C unit suite. The list of tests lives in unit_tests.list —
# the SINGLE source of truth shared by CI (ci.yml) and tests/run_all.sh
# (TST-3: two hand-maintained lists had drifted; never maintain a third).
check: all
	@fail=0; \
	while read -r t; do \
	    case "$$t" in ''|\#*) continue ;; esac; \
	    echo "=== $$t ==="; \
	    ./$$t || { echo "FAILED: $$t"; fail=1; }; \
	done < unit_tests.list; \
	exit $$fail

test_wait_event: test_wait_event.c $(BUILD)/wait_event.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -o $@ $^

test_cmdline: test_cmdline.c $(BUILD)/cmdline.o
	$(CC) $(CFLAGS) -o $@ $^

test_event_writer: test_event_writer.c $(BUILD)/event_writer.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4

test_event_reader: test_event_reader.c $(BUILD)/event_reader.o $(BUILD)/event_writer.o \
                   $(BUILD)/map_reader.o $(BUILD)/wait_event.o $(BUILD)/spawn.o $(BUILD)/cJSON.o \
                   $(BUILD)/discovery.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4 $(LIBBPF_LIB) -lelf -lz

test_bucket: test_bucket.c
	$(CC) $(CFLAGS) -o $@ $^

# test_pg13_resolve: PG13 MyProc-path offset table + the runtime "refuse on
# garbage offset" validation guard. Compiles discovery.c directly (the helpers
# under test are BPF-free; discovery.c only needs libelf). Runs anywhere — no
# live PostgreSQL, no BPF skeleton.
test_pg13_resolve: test_pg13_resolve.c ../src/discovery.c ../src/spawn.c \
                   $(BUILD)/wait_event.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -o $@ $^ -lelf -lz

# test_discovery: load-base + ELF symbol resolution — the #24 regression
# class (pgwt_find_load_base_in_maps against committed maps fixtures, plus
# self-resolution of a known global via /proc/self/{exe,maps}). Built TWICE,
# -pie and -no-pie, because PIE vs non-PIE is exactly the axis #24 broke on;
# each build asserts (via the ELF header) that the flag actually took effect.
# Compiles discovery.c directly (BPF-free, needs libelf) — same pattern as
# test_pg13_resolve.
# Compile wait_event.c + cJSON.c FROM SOURCE (not the prebuilt $(BUILD)/*.o):
# the -pie build must link only -fPIE objects, but the main build's objects
# default to non-PIE on EL toolchains (R_X86_64_32S relocations), which fails
# the `-pie` link. Building them here inherits this target's -fPIE/-fno-pie.
DISCOVERY_TEST_DEPS = test_discovery.c ../src/discovery.c ../src/spawn.c \
                      ../src/wait_event.c ../src/cJSON.c

test_discovery_pie: $(DISCOVERY_TEST_DEPS)
	$(CC) $(CFLAGS) -DPGWT_TEST_EXPECT_PIE=1 -fPIE -pie -o $@ $^ -lelf -lz

test_discovery_nopie: $(DISCOVERY_TEST_DEPS)
	$(CC) $(CFLAGS) -DPGWT_TEST_EXPECT_PIE=0 -fno-pie -no-pie -o $@ $^ -lelf -lz

# test_trace_v2: format round-trip (v2 typed blocks). Built against the
# server objects (-DPGWT_SERVER) so it needs no BPF skeleton — buildable on
# a box without bpftool and in CI's server-only jobs.
test_trace_v2: test_trace_v2.c $(BUILD)/server_event_writer.o \
               $(BUILD)/server_event_reader.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4

# test_sampler: sampler core (process_vm_readv read + batch encode + per-pid
# fallback). Compiles sampler.c with -DPGWT_SERVER so only the BPF-free core
# is built — no skeleton, no bpftool, runnable in CI's server-only jobs.
test_sampler: test_sampler.c ../src/sampler.c ../src/anomaly.c
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^

# test_anomaly: anomaly rule engine (AAS-vs-baseline, lock-fraction,
# hysteresis/cooldown, baseline protection, metrics derivation). Compiles
# anomaly.c with -DPGWT_SERVER so only the pure rule core is built — no
# skeleton, no escalation engine, runnable in CI's server-only jobs.
test_anomaly: test_anomaly.c ../src/anomaly.c
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^

# test_escalation_budget: pure rolling-hour budget core (ESC-1/5/6). Compiles
# escalation_budget.c directly — it is BPF-free and daemon-free (depends only
# on escalation.h), so no skeleton, no bpftool: runnable in CI's server-only
# jobs like test_sampler / test_anomaly.
test_escalation_budget: test_escalation_budget.c ../src/escalation_budget.c
	$(CC) $(CFLAGS) -o $@ $^

# test_effective_cores: AAS-1 Stage 2 capacity resolver. All proc/cgroup reads
# target committed fixtures and affinity is injected; it never inspects or
# modifies the host's cgroup configuration.
test_effective_cores: test_effective_cores.c ../src/effective_cores.c
	$(CC) $(CFLAGS) -o $@ $^

# test_coop: cooperative provider stub (A6 interface freeze) — registers,
# advertises EXACT fidelity, start() returns the clean "not available" status.
# Compiles provider_coop.c with -DPGWT_SERVER so it needs no BPF skeleton — no
# bpftool, runnable in CI's server-only jobs.
test_coop: test_coop.c ../src/provider_coop.c
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^

# test_replay_fidelity: fidelity-aware --replay accumulation (T1/FID-5) —
# samples contribute ASH-estimated time (never fabricated histograms),
# markers are skipped. Links the daemon-flavor event_reader.c (which holds
# pgwt_replay_events) against the BPF-free accumulator core: map_reader.c
# compiled with -DPGWT_SERVER strips its libbpf/skeleton half (same pattern
# as sampler.c/anomaly.c), so no bpftool is needed.
map_reader_core.o: ../src/map_reader.c ../src/map_reader.h
	$(CC) $(CFLAGS) -DPGWT_SERVER -c -o $@ $<

test_replay_fidelity: test_replay_fidelity.c ../src/event_reader.c \
                      map_reader_core.o $(BUILD)/server_event_writer.o \
                      $(BUILD)/server_wait_event.o $(BUILD)/server_spawn.o \
                      $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4 -lz

# test_durability: T5 file-lifecycle guarantees (DUR-1/2/3/7/8) — crash
# recovery (kill -9 matrix), collision-safe archive names, size-cap
# retention, meta/footer sanity caps, ~1 s SAMPLES batching. Server build
# (no BPF skeleton) — the writers/readers are BPF-free.
test_durability: test_durability.c $(BUILD)/server_event_writer.o \
                 $(BUILD)/server_event_reader.o \
                 $(BUILD)/server_summary_writer.o \
                 $(BUILD)/server_summary_reader.o \
                 $(BUILD)/server_wait_event.o $(BUILD)/server_spawn.o \
                 $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz

# test_query_text: T5 DUR-4/10 — query_texts.jsonl append + dedup-on-load +
# bounded compaction + cap logging + permissions. Compiles query_text.c
# directly (BPF-free).
test_query_text: test_query_text.c ../src/query_text.c
	$(CC) $(CFLAGS) -o $@ $^

# test_golden_fixture: T7/TST-10 — decode the COMMITTED golden trace bytes
# (tests/fixtures/golden/rev<N>/) and checksum the event stream, pinning
# on-disk format compatibility across future changes. Server build (no BPF
# skeleton) — reader/writer are BPF-free. Runs from tests/ (relative fixture
# path); run_all.sh and CI cd here first.
test_golden_fixture: test_golden_fixture.c $(BUILD)/server_event_reader.o \
                     $(BUILD)/server_event_writer.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz

# Server-build objects for gen_test_traces
SERVER_OBJS = $(BUILD)/server_event_writer.o $(BUILD)/server_summary_writer.o \
              $(BUILD)/server_wait_event.o $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o

gen_test_traces: gen_test_traces.c $(SERVER_OBJS)
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4

gen_bench_traces: gen_bench_traces.c $(SERVER_OBJS)
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4

test_concurrent_rw: test_concurrent_rw.c $(BUILD)/event_reader.o $(BUILD)/event_writer.o \
                    $(BUILD)/map_reader.o $(BUILD)/wait_event.o $(BUILD)/spawn.o $(BUILD)/cJSON.o \
                    $(BUILD)/discovery.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4 $(LIBBPF_LIB) -lelf -lz

# cross_validate: A4 sampled-vs-exact comparison tool. Server build
# (-DPGWT_SERVER) so it needs no BPF skeleton — runs anywhere the server
# builds. Reads a tiered trace dir and reports estimator agreement.
cross_validate: cross_validate.c $(BUILD)/server_event_reader.o \
                $(BUILD)/server_event_writer.o $(BUILD)/server_wait_event.o \
                $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz

# dump_markers: A5 helper — decode escalation-window markers (reason + length)
# from a trace dir. Used by the live anomaly test to prove an ANOMALY-reason
# window was recorded. Server build (no BPF skeleton).
dump_markers: dump_markers.c $(BUILD)/server_event_reader.o \
              $(BUILD)/server_event_writer.o $(BUILD)/server_wait_event.o \
              $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz

clean:
	rm -f $(TESTS) map_reader_core.o
