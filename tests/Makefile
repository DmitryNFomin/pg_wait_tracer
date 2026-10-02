# tests/Makefile — build C unit tests for pg_wait_tracer
# Requires: main project already built (make in parent dir)

CC       ?= gcc
CFLAGS    = -g -O2 -Wall -I../src -I../include
BUILD     = ../build

# libbpf link flags. Defaults to the system shared library; the top-level
# Makefile overrides this (LIBBPF_LIB="path/to/libbpf.a -lelf -lz") on
# Rocky 8 / RHEL 8 where the bundled static libbpf is used instead.
LIBBPF_LIB ?= -lbpf

TESTS     = test_wait_event test_cmdline test_spawn test_event_writer test_event_reader \
            test_trace_v2 test_sampler test_anomaly test_escalation_budget \
            test_exact_probe \
            test_effective_cores \
            test_coop test_bucket \
            test_backend_status_layout \
            test_synthetic_query \
            test_pgss_resolver \
            test_pg13_resolve test_discovery_pie test_discovery_nopie \
            test_replay_fidelity test_durability test_query_text \
            test_golden_fixture test_live_accum test_window_clip \
            test_executions_attribution \
            test_top_queries_percentile \
            test_variants_p95 \
            gen_test_traces gen_bench_traces test_concurrent_rw cross_validate \
            dump_markers wait_cpu_canary test_wait_cpu_canary

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
	    case "$$t" in \
	        *.py) python3 "$$t" ;; \
	        *) ./"$$t" ;; \
	    esac || { echo "FAILED: $$t"; fail=1; }; \
	done < unit_tests.list; \
	exit $$fail

test_wait_event: test_wait_event.c $(BUILD)/wait_event.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -o $@ $^

test_cmdline: test_cmdline.c $(BUILD)/cmdline.o
	$(CC) $(CFLAGS) -o $@ $^

test_spawn: test_spawn.c ../src/spawn.c
	$(CC) $(CFLAGS) -o $@ $^

test_event_writer: test_event_writer.c $(BUILD)/event_writer.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4

test_event_reader: test_event_reader.c $(BUILD)/event_reader.o $(BUILD)/event_writer.o \
                   $(BUILD)/map_reader.o $(BUILD)/wait_event.o $(BUILD)/spawn.o $(BUILD)/cJSON.o \
                   $(BUILD)/discovery.o $(BUILD)/backend_status_layout.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4 $(LIBBPF_LIB) -lelf -lz

test_bucket: test_bucket.c
	$(CC) $(CFLAGS) -o $@ $^

# test_live_accum: issues #97/#98 — live-view interval accounting shared by
# the closed-record path and the open state_map scan, the marker-driven
# command gate, and the saturating ring delta behind the multi-window view.
# Compiles map_reader.c with -DPGWT_SERVER (the BPF-free accumulator core)
# and snapshot.c directly; compute.c for the pgwt_tag_events cross-check
# (#98: the live rule must match the server's majority rule on the same
# stream — the test stubs its only foreign symbol, pgwt_visit_summaries);
# wait_event.o only for the idle/hidden classification. Runs anywhere.
test_live_accum: test_live_accum.c ../src/map_reader.c ../src/snapshot.c \
                 ../src/compute.c \
                 $(BUILD)/wait_event.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^

# test_window_clip: windowed aggregates must count only the part of each
# event that lies inside the requested window. The raw loader selects on END
# timestamp, so a wait already running when the window opened arrives whole;
# pgwt_compute_aas clipped it, the Overview/Top-N summers did not (+1.28% on a
# 60 s rehearsal window, unbounded under a lock pileup). Same build shape as
# test_live_accum: compute.c compiled directly with -DPGWT_SERVER, its only
# foreign symbol (pgwt_visit_summaries) stubbed in the test. Runs anywhere.
test_window_clip: test_window_clip.c ../src/compute.c \
                  $(BUILD)/wait_event.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -lm

# test_pg13_resolve: PG13 MyProc-path offset table + the runtime "refuse on
# garbage offset" validation guard. Compiles discovery.c directly (the helpers
# under test are BPF-free; discovery.c only needs libelf). Runs anywhere — no
# live PostgreSQL, no BPF skeleton.
# test_executions_attribution: #222 review round 3 item 2 — the per-pid
# "is the top row the one that just started" bit (compute.c
# exec_pid_state.top_attributable) must be cleared by an EXEC_START whose
# timestamp falls past the caller's to_ns, even though that start keeps no
# row. Unreachable through pgwt-server (its loader stops at ts > to), so it
# is pinned at the only layer that can observe it: pgwt_compute_executions
# called directly. Same link set as test_live_accum; stubs
# pgwt_visit_summaries, compute.c's only foreign symbol. Runs anywhere.
test_executions_attribution: test_executions_attribution.c ../src/map_reader.c \
                             ../src/snapshot.c ../src/compute.c \
                             $(BUILD)/wait_event.o $(BUILD)/spawn.o \
                             $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^

# test_top_queries_percentile: #265 — the Queries tab's p95/p99 sort. The
# O(n^2) exchange sort in handle_top_queries became qsort; the entire safety
# argument is that the output is BIT-IDENTICAL, so this is a differential
# against a verbatim copy of the deleted sort (section 1), the percentile
# index arithmetic at its guard boundaries (2), the (int)(a-b) comparator
# trap (3), and a false-negative suite including a mutation probe that
# proves section 1 can go red (4). Header-only subject, no link deps, no
# I/O, no clock — runs anywhere.
test_top_queries_percentile: test_top_queries_percentile.c ../src/percentile.h
	$(CC) $(CFLAGS) -o $@ test_top_queries_percentile.c

# test_variants_p95: #271 + #269 — pgwt_compute_variants sampled execution
# times only while exec_count <= 10000 but read min(exec_count, cap) of a
# realloc'd (never zeroed) array, so the p95 came out of slots that were
# never written (~9.1 years on a real capture); the same lines sorted with
# an O(n^2) exchange sort. Same link shape as test_window_clip (compute.c
# with -DPGWT_SERVER, pgwt_visit_summaries stubbed in the test).
# -Wl,--wrap=realloc lets sections 4.7/4.8 inject a transient and a total
# allocation failure — the test FAILS if the interposer never fires, so a
# toolchain where --wrap silently did nothing cannot pass blind.
test_variants_p95: test_variants_p95.c ../src/compute.c ../src/percentile.h \
                   $(BUILD)/wait_event.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ test_variants_p95.c ../src/compute.c \
	    $(BUILD)/wait_event.o $(BUILD)/spawn.o $(BUILD)/cJSON.o \
	    -Wl,--wrap=realloc -lm

test_pg13_resolve: test_pg13_resolve.c ../src/discovery.c ../src/spawn.c \
                   ../src/backend_status_layout.c \
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
                      ../src/backend_status_layout.c \
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
# CPU-demand CUSUM, hysteresis/cooldown, baseline protection, metrics
# derivation). Compiles anomaly.c with -DPGWT_SERVER so only the pure rule core
# is built — no skeleton, no escalation engine, runnable in CI's server-only
# jobs.
test_anomaly: test_anomaly.c ../src/anomaly.c
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^

# test_escalation_budget: pure rolling-hour budget core (ESC-1/5/6). Compiles
# escalation_budget.c directly — it is BPF-free and daemon-free (depends only
# on escalation.h), so no skeleton, no bpftool: runnable in CI's server-only
# jobs like test_sampler / test_anomaly.
test_escalation_budget: test_escalation_budget.c ../src/escalation_budget.c
	$(CC) $(CFLAGS) -o $@ $^

test_exact_probe: test_exact_probe.c ../src/exact_probe.c \
                  ../src/backend_status_layout.c ../src/spawn.c \
                  ../src/cmdline.c ../src/backend_meta.c ../src/cJSON.c
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^

# test_effective_cores: AAS-1 Stage 2 capacity resolver. All proc/cgroup reads
# target committed fixtures and affinity is injected; it never inspects or
# modifies the host's cgroup configuration.
test_effective_cores: test_effective_cores.c ../src/effective_cores.c
	$(CC) $(CFLAGS) -o $@ $^

# Stage 1 PgBackendStatus discovery is BPF-free.  The test feeds synthetic
# readelf streams into the strict parser and exercises both architecture
# tables plus the pure coherency/degrade validator.
test_backend_status_layout: test_backend_status_layout.c \
                            ../src/backend_status_layout.c ../src/spawn.c
	$(CC) $(CFLAGS) -o $@ $^

test_synthetic_query: test_synthetic_query.c ../src/synthetic_query.c
	$(CC) $(CFLAGS) -o $@ $^

test_pgss_resolver: test_pgss_resolver.c ../src/pgss_resolver.c \
                    ../src/query_text.c ../src/synthetic_query.c ../src/spawn.c
	$(CC) $(CFLAGS) -DPGWT_TEST -o $@ $^ -pthread

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
	$(CC) $(CFLAGS) -DPGWT_TEST -o $@ $^ ../src/synthetic_query.c -pthread

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
                    $(BUILD)/discovery.o $(BUILD)/backend_status_layout.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4 $(LIBBPF_LIB) -lelf -lz

# cross_validate: A4 sampled-vs-exact comparison tool. Server build
# (-DPGWT_SERVER) so it needs no BPF skeleton — runs anywhere the server
# builds. Reads a tiered trace dir and reports estimator agreement.
cross_validate: cross_validate.c $(BUILD)/server_event_reader.o \
                $(BUILD)/server_event_writer.o $(BUILD)/server_wait_event.o \
                $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz

# wait_cpu_canary: #202 helper — per-class Sum(cpu_ns) / Sum(duration) over a
# trace dir, flagging CPU recorded inside a PURE-SLEEP wait (Lock, Timeout)
# and any event with cpu_ns > duration_ns (src/compute.c's wait branch never
# clamps that, so cpu_clamped_ms vouches for CPU gaps only). Server build.
wait_cpu_canary: wait_cpu_canary.c $(BUILD)/server_event_reader.o \
                 $(BUILD)/server_event_writer.o $(BUILD)/server_wait_event.o \
                 $(BUILD)/server_compute.o $(BUILD)/server_summary_reader.o \
                 $(BUILD)/server_summary_writer.o \
                 $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz

# test_wait_cpu_canary: the canary's own decision logic, including every way
# it could be satisfied without looking at anything (no events, nothing
# measured, UNKNOWN folded in as 0). Includes wait_cpu_canary.c with
# -DWCC_NO_MAIN; no trace file, no BPF, runs anywhere.
test_wait_cpu_canary: test_wait_cpu_canary.c wait_cpu_canary.c \
                      $(BUILD)/server_event_reader.o \
                      $(BUILD)/server_event_writer.o \
                      $(BUILD)/server_wait_event.o \
                      $(BUILD)/server_compute.o \
                      $(BUILD)/server_summary_reader.o \
                      $(BUILD)/server_summary_writer.o \
                      $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -DWCC_NO_MAIN -o $@ test_wait_cpu_canary.c \
	      $(BUILD)/server_event_reader.o $(BUILD)/server_event_writer.o \
	      $(BUILD)/server_wait_event.o $(BUILD)/server_compute.o \
	      $(BUILD)/server_summary_reader.o $(BUILD)/server_summary_writer.o \
	      $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o -llz4 -lz

# dump_markers: A5 helper — decode escalation-window markers (reason + length)
# from a trace dir. Used by the live anomaly test to prove an ANOMALY-reason
# window was recorded. Server build (no BPF skeleton).
dump_markers: dump_markers.c $(BUILD)/server_event_reader.o \
              $(BUILD)/server_event_writer.o $(BUILD)/server_wait_event.o \
              $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz

clean:
	rm -f $(TESTS) map_reader_core.o
