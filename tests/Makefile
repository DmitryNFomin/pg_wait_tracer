# tests/Makefile — build C unit tests for pg_wait_tracer
# Requires: main project already built (make in parent dir)

CC       ?= gcc
CFLAGS    = -g -O2 -Wall -I../src -I../include
BUILD     = ../build

# libbpf link flags. Defaults to the system shared library; the top-level
# Makefile overrides this (LIBBPF_LIB="path/to/libbpf.a -lelf -lz") on
# Rocky 8 / RHEL 8 where the bundled static libbpf is used instead.
LIBBPF_LIB ?= -lbpf

TESTS     = test_wait_event test_cmdline test_backend_lifecycle test_spawn test_event_writer test_event_reader \
            test_trace_v2 test_sampler test_anomaly test_escalation_budget \
            test_exact_probe \
            test_effective_cores \
            test_coop test_bucket \
            test_backend_status_layout \
            test_synthetic_query \
            test_pgss_resolver \
            test_pg13_resolve test_discovery_pie test_discovery_nopie \
            test_replay_fidelity test_durability test_query_text \
            test_golden_fixture test_live_accum test_window_clip test_time_model_absence \
            test_executions_attribution \
            test_top_queries_percentile \
            test_event_sort \
            test_top_queries_context \
            test_variants_p95 \
            test_pid_cap \
            test_tag_refusal \
            test_probe_bounds \
            test_interference_bounds \
            test_concurrency_bounds \
            test_idle_accounting test_idle_summary \
            test_agg_raw_crosscheck \
            test_io_worker_summary test_io_worker_parity \
            test_cpu_split_parity \
            test_straddler_accounting \
            test_exec_index \
            test_interval_index \
            test_alloc_tune \
            gen_test_traces gen_bench_traces test_concurrent_rw cross_validate \
            dump_markers wait_cpu_canary test_wait_cpu_canary \
            wait_cpu_scan wait_cpu_fixture \
            test_summary_tick_flush \
            test_block_agg bench_block_agg test_transitions_loader \
            test_top_wait_backfill \
            test_window_overlap \
            test_open_overlay \
            test_replay_window

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

test_wait_event: test_wait_event.c $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -o $@ $^

# Exercise the production lifecycle functions, replacing only hardware opens
# and map deletes. The capture objects exist after the required parent build.
LIFECYCLE_OBJS = $(filter-out $(BUILD)/pg_wait_tracer.o $(BUILD)/pg_wait_tracer.bpf.o $(BUILD)/server_%.o,$(wildcard $(BUILD)/*.o))
test_backend_lifecycle: test_backend_lifecycle.c $(LIFECYCLE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -Wl,--wrap=pgwt_open_watchpoint -Wl,--wrap=pgwt_open_watchpoint_disabled -Wl,--wrap=bpf_map_delete_elem $(LIBBPF_LIB) -lelf -lz -llz4 -pthread -lm

test_cmdline: test_cmdline.c $(BUILD)/cmdline.o
	$(CC) $(CFLAGS) -o $@ $^

test_spawn: test_spawn.c ../src/spawn.c
	$(CC) $(CFLAGS) -o $@ $^

test_event_writer: test_event_writer.c $(BUILD)/event_writer.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4

# summary_writer.o since #379: the daemon build of map_reader.c calls
# pgwt_summary_note_open from pgwt_read_state_map (that loop is the daemon's
# only open-interval source), so every target linking map_reader.o needs it.
test_event_reader: test_event_reader.c $(BUILD)/event_reader.o $(BUILD)/event_writer.o \
                   $(BUILD)/map_reader.o $(BUILD)/summary_writer.o \
                   $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o \
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
                 $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^

# test_window_clip: windowed aggregates must count only the part of each
# event that lies inside the requested window. The raw loader selects on END
# timestamp, so a wait already running when the window opened arrives whole;
# pgwt_compute_aas clipped it, the Overview/Top-N summers did not (+1.28% on a
# 60 s rehearsal window, unbounded under a lock pileup). Same build shape as
# test_live_accum: compute.c compiled directly with -DPGWT_SERVER, its only
# foreign symbol (pgwt_visit_summaries) stubbed in the test. Runs anywhere.
test_window_clip: test_window_clip.c ../src/compute.c \
                  $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -lm

# #387: out-of-window / zero-duration CPU records cannot supply a measured
# signal. Same BPF-free build and summary visitor stub as test_window_clip.
test_time_model_absence: test_time_model_absence.c ../src/compute.c \
                  $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
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
                             $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o \
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

# test_event_sort: the loader's order restoration (src/event_sort.h). The
# fixed-width bottom-up mergesort was 29.88% of warm backend self time
# (1779-1955 ms per request on a real 5.7M-event window); it is now an
# in-place natural chain merge with an O(n log n) fallback. The whole safety
# argument is that the output is BIT-IDENTICAL, so this is a differential
# against a verbatim private copy of the deleted sort, plus an explicit tie
# order, the fallback hand-off, and a false-negative suite with a five-way
# mutation probe. Header-only subject, no link deps, no I/O, no clock.
test_event_sort: test_event_sort.c ../src/event_sort.h ../src/pg_wait_tracer.h
	$(CC) $(CFLAGS) -o $@ test_event_sort.c

# test_top_queries_context: the Queries tab's per-row CONTEXT resolution and
# QUERY_SYNTH flag, which handle_top_queries resolved with TWO full passes
# over the window PER ROW (1778 ms of the response at the 900 s default) and
# now resolves in one. Header-only, like percentile.h above: pure in-memory
# arrays, a verbatim copy of the deleted query_context_for_events as the
# oracle, seven mutants, the bypass suite and a non-vacuity ledger.
test_top_queries_context: test_top_queries_context.c ../src/query_ctx_index.h \
                          ../src/qid_index.h ../src/pg_wait_tracer.h
	$(CC) $(CFLAGS) -o $@ test_top_queries_context.c

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
                   $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ test_variants_p95.c ../src/compute.c \
	    $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o \
	    -Wl,--wrap=realloc -lm

# test_pid_cap: #275 — the two per-pid execution-lifecycle tables that
# silently dropped every pid past the 512th (handle_top_queries' pid_st[512],
# pgwt_compute_variants' MAX_PIDS), plus the fixed 1024-slot query-id table
# in the same scan whose probe loop never terminated when full. Same link
# shape as test_variants_p95 (compute.c with -DPGWT_SERVER, pgwt_visit_summaries
# stubbed in the test). Allocation failures are injected through
# PGWT_TEST_ALLOC_FAIL (src/pid_index.h), the same env-var shape server.c
# already uses for PGWT_TEST_LOAD_ALLOC_FAIL — no --wrap needed, so the
# binary behaves identically on every toolchain.
test_pid_cap: test_pid_cap.c ../src/compute.c ../src/pid_index.h \
              ../src/percentile.h \
              $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ test_pid_cap.c ../src/compute.c \
	    $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o -lm

# test_tag_refusal: #363 — pgwt_tag_events was declared `void` and had two
# ways to stop partway (a working-allocation failure, or more distinct pids
# than its 4096-slot per-pid table holds), each leaving the records it never
# reached with their raw old_event/query_id while returning normally. That
# array is the oracle, so the result was confidently wrong, not short. Same
# link shape as test_pid_cap (compute.c with -DPGWT_SERVER,
# pgwt_visit_summaries stubbed in the test). Allocation failures ride
# PGWT_TEST_LOAD_ALLOC_FAIL — the LOADER's injection variable, not
# PGWT_TEST_ALLOC_FAIL, because this pass runs inside the raw loader and
# tests/test_data_tag_refusal.py drives the same two points through the real
# pgwt-server binary with the same variable.
test_tag_refusal: test_tag_refusal.c ../src/compute.c ../src/pid_index.h \
              ../src/percentile.h \
              $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ test_tag_refusal.c ../src/compute.c \
	    $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o -lm

# test_concurrency_bounds: #276 — pgwt_compute_concurrency materialised
# candidate intervals into a fixed 100,000-entry array filled in ARRIVAL
# order, so a 900 s window rendered its first ~60 s and reported the rest as
# max: 0 ("fidelity": "exact" beside it), plus four more silent bounds
# (ev_pids[64], pids[128], burst_cap=256, pids[64] per burst). Same link
# shape as test_pid_cap (compute.c with -DPGWT_SERVER, pgwt_visit_summaries
# stubbed in the test). The file carries a VERBATIM COPY of the deleted
# phase 1, so the defect goes red here on the very fixtures that prove the
# fix; allocation failures ride PGWT_TEST_ALLOC_FAIL (src/test_alloc_fail.h),
# so the binary behaves identically on every toolchain.
test_concurrency_bounds: test_concurrency_bounds.c ../src/compute.c \
              ../src/triple_map.h ../src/test_alloc_fail.h \
              ../src/pid_index.h ../src/percentile.h \
              $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ test_concurrency_bounds.c ../src/compute.c \
	    $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o -lm

# test_probe_bounds: #327 — six open-addressing tables in compute.c whose
# probe loop had no fullness test, so the (size+1)-th distinct key made it
# spin forever and hung every tab at once (top sessions raw + summary, top
# queries raw + summary, fingerprints, variants). Plus #347's shape: an
# accumulator used as the occupancy test, which let a zero-duration record's
# slot be re-keyed and its transitions inherited by the next key.
# Same link shape as test_pid_cap (compute.c with -DPGWT_SERVER), except that
# pgwt_visit_summaries is a RECORD-SERVING stub here rather than a `return
# -1` one — the two summary sites can only be reached through it, and a stub
# that delivers nothing would make those sections vacuous. Allocation
# failures ride PGWT_TEST_ALLOC_FAIL (src/test_alloc_fail.h), so the binary
# behaves identically on every toolchain; no --wrap.
test_probe_bounds: test_probe_bounds.c ../src/compute.c \
              ../src/qid_index.h ../src/pid_index.h \
              ../src/test_alloc_fail.h ../src/percentile.h \
              $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ test_probe_bounds.c ../src/compute.c \
	    $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o -lm

# #359: interference's former 4096-slot pair table must return at cap and
# cap+1, with exact per-pair totals and whole-result allocation refusals.
test_interference_bounds: test_interference_bounds.c ../src/compute.c \
              ../src/triple_map.h ../src/test_alloc_fail.h \
              $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ test_interference_bounds.c ../src/compute.c \
	    $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o -lm

test_pg13_resolve: test_pg13_resolve.c ../src/discovery.c ../src/spawn.c \
                   ../src/backend_status_layout.c \
                   $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/cJSON.o
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
# idle_rule.c joins the list because wait_event.c now CALLS into it
# (pgwt_timeout_name_is_pacing / pgwt_idle_rule_set_timeout_mask, to rebuild
# the Timeout pacing mask from the active name table) -- the predicates moved
# out of wait_event.c, so linking wait_event.c alone no longer resolves. Like
# the rest of this list it is a SOURCE, not a $(BUILD) object: the -pie link
# needs -fPIE objects and the main build's are not.
DISCOVERY_TEST_DEPS = test_discovery.c ../src/discovery.c ../src/spawn.c \
                      ../src/backend_status_layout.c \
                      ../src/wait_event.c ../src/idle_rule.c ../src/cJSON.c

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
# idle_rule.c is linked directly rather than via $(BUILD): this target
# deliberately compiles its sources rather than depending on the daemon
# build, and src/idle_rule.c is a ~110-line BPF-free TU with no deps of
# its own -- which is exactly why the idle rule lives there.
test_sampler: test_sampler.c ../src/sampler.c ../src/anomaly.c ../src/idle_rule.c
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^

# test_anomaly: anomaly rule engine (AAS-vs-baseline, lock-fraction,
# CPU-demand CUSUM, hysteresis/cooldown, baseline protection, metrics
# derivation). Compiles anomaly.c with -DPGWT_SERVER so only the pure rule core
# is built — no skeleton, no escalation engine, runnable in CI's server-only
# jobs.
# test_anomaly also links wait_event.o + idle_rule.o now: src/anomaly.c no
# longer carries its own inline copy of the idle rule (it calls the shared
# pgwt_is_idle_event), and the test sweeps the whole Timeout class on BOTH the
# PG13 and PG18 tables, which needs pgwt_init_event_names. Still BPF-free and
# still runs anywhere.
test_anomaly: test_anomaly.c ../src/anomaly.c $(BUILD)/wait_event.o $(BUILD)/idle_rule.o \
              $(BUILD)/spawn.o $(BUILD)/cJSON.o
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
                      $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o $(BUILD)/server_spawn.o \
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
                 $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o $(BUILD)/server_spawn.o \
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
              $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o

gen_test_traces: gen_test_traces.c $(SERVER_OBJS)
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4

gen_bench_traces: gen_bench_traces.c $(SERVER_OBJS)
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4

# summary_writer.o: see test_event_reader above (#379).
test_concurrent_rw: test_concurrent_rw.c $(BUILD)/event_reader.o $(BUILD)/event_writer.o \
                    $(BUILD)/map_reader.o $(BUILD)/summary_writer.o \
                    $(BUILD)/wait_event.o $(BUILD)/idle_rule.o $(BUILD)/spawn.o $(BUILD)/cJSON.o \
                    $(BUILD)/discovery.o $(BUILD)/backend_status_layout.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4 $(LIBBPF_LIB) -lelf -lz

# cross_validate: A4 sampled-vs-exact comparison tool. Server build
# (-DPGWT_SERVER) so it needs no BPF skeleton — runs anywhere the server
# builds. Reads a tiered trace dir and reports estimator agreement.
cross_validate: cross_validate.c $(BUILD)/server_event_reader.o \
                $(BUILD)/server_event_writer.o $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o \
                $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz

# wait_cpu_canary: #202 helper — per-class Sum(cpu_ns) / Sum(duration) over a
# trace dir, flagging CPU recorded inside a PURE-SLEEP wait (Lock, Timeout)
# and any event with cpu_ns > duration_ns (src/compute.c's wait branch never
# clamps that, so cpu_clamped_ms vouches for CPU gaps only). Server build.
wait_cpu_canary: wait_cpu_canary.c $(BUILD)/server_event_reader.o \
                 $(BUILD)/server_event_writer.o $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o \
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
                      $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o \
                      $(BUILD)/server_compute.o \
                      $(BUILD)/server_summary_reader.o \
                      $(BUILD)/server_summary_writer.o \
                      $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -DWCC_NO_MAIN -o $@ test_wait_cpu_canary.c \
	      $(BUILD)/server_event_reader.o $(BUILD)/server_event_writer.o \
	      $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o $(BUILD)/server_compute.o \
	      $(BUILD)/server_summary_reader.o $(BUILD)/server_summary_writer.o \
	      $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o -llz4 -lz

# wait_cpu_scan / wait_cpu_fixture: criteria item 2.5's per-class wait-CPU
# scan over a FULL raw walk of a trace dir, and the synthetic-trace generator
# its bypass suite drives it with. Same tool+test shape as wait_cpu_canary
# above: both are helpers invoked by test_wait_cpu_scan.py (in
# unit_tests.list), so test_unit_list_coverage.py exempts them by the
# `wait_cpu_` prefix rather than demanding they be listed as tests.
# Registered here deliberately: an instrument nothing builds is an
# instrument nothing notices rotting -- this one's mutant anchor already
# went stale once (M6, fixed in show_red.py) between two commits.
wait_cpu_scan: wait_cpu_scan.c $(BUILD)/server_event_reader.o \
               $(BUILD)/server_event_writer.o \
               $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o \
               $(BUILD)/server_cJSON.o $(BUILD)/server_spawn.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz -lm

wait_cpu_fixture: wait_cpu_fixture.c $(BUILD)/server_event_writer.o \
                  $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o \
                  $(BUILD)/server_cJSON.o $(BUILD)/server_spawn.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz -lm

# dump_markers: A5 helper — decode escalation-window markers (reason + length)
# from a trace dir. Used by the live anomaly test to prove an ANOMALY-reason
# window was recorded. Server build (no BPF skeleton).
dump_markers: dump_markers.c $(BUILD)/server_event_reader.o \
              $(BUILD)/server_event_writer.o $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o \
              $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz

# test_alloc_tune: src/alloc_tune.h. Header-only, so this links nothing else.
# The subject is an allocator property (a ~274 MB block served from the brk
# heap, not returned on free, and re-touched without page faults), measured
# against the same sequence run untuned in the same process -- the control is
# what lets the test fail. Pre-tuned MALLOC_* in the environment, an
# allocation too small to be mmap'd, or an unreadable sbrk() each REFUSE.
#
# ../src/alloc_tune.h IS a prerequisite, and has to be. The subject is a
# header the test #includes, so without it here `make` reports the binary
# up to date after the header changes and silently runs the STALE one: three
# mutations of alloc_tune.h (M_MMAP_MAX not set, M_TRIM_THRESHOLD tiny,
# success reported without calling mallopt) all stayed GREEN against a
# pre-built tests/ directory before this line existed. Same shape as
# test_event_sort and test_top_queries_context above.
test_alloc_tune: test_alloc_tune.c ../src/alloc_tune.h
	$(CC) $(CFLAGS) -o $@ $<

# test_summary_tick_flush: #277 — the daemon tick flushed the IN-PROGRESS
# second and left the accumulator's counters in place, so the rest of that
# second was written again as a superset and pgwt_visit_summaries summed both
# (1.47x on a 121 s window, up to 2.16x at -i 1). Drives the real writer and
# the real reader over synthetic seconds; every tick is an explicit
# now_mono_ns, so nothing here depends on wall time. Server build (no BPF).
test_summary_tick_flush: test_summary_tick_flush.c \
                         $(BUILD)/server_summary_writer.o \
                         $(BUILD)/server_summary_reader.o \
                         $(BUILD)/server_event_writer.o \
                         $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o \
                         $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz

# test_idle_accounting: the timer-sleep/idle accounting in the RAW compute path
# -- DB Time and the class rows after six Timeout events became pacing sleeps,
# the pacing events staying VISIBLE, the Idle row and its NAMED children (the
# 807 s of CheckpointWriteDelay must not become an unexplained number), the
# conservation identity asserted by COMPONENT (the identity alone cannot fail:
# Off-CPU* is the residual), CPU measured on and adjacent to a pacing wait, and
# a bypass suite for every way a "time was excluded" assertion is satisfied by
# 0 == 0. Same build shape as test_window_clip: compute.c compiled directly
# with -DPGWT_SERVER, its only foreign symbol (pgwt_visit_summaries) stubbed in
# the test. Runs anywhere -- no live PG, no BPF.
test_idle_accounting: test_idle_accounting.c ../src/compute.c \
                      $(BUILD)/wait_event.o $(BUILD)/idle_rule.o \
                      $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -lm

# test_idle_summary: the SUMMARY fast path, which is the one the demo's 900 s
# window actually takes (should_use_summaries). Changing the idle predicate
# alone would have left that window reporting the OLD DB Time, because the
# per-second class totals were precomputed LUMPED at the writer and the read
# path subtracted only Client:ClientRead back out. Drives the REAL writer and
# the REAL reader over a 150 s synthetic stream and compares every summary
# aggregate against the raw path (unfiltered, query-filtered and
# Timeout-class-filtered), checks the writer-side v3 exclusion on the records
# themselves, and pins the accounting-VERSION preflight plus its bypass suite
# (a v2 window, a truncated header, a missing dir, an empty dir, a writer that
# wrote nothing, a non-overlapping window). Server build, no BPF.
test_idle_summary: test_idle_summary.c ../src/compute.c \
                   $(BUILD)/server_summary_writer.o \
                   $(BUILD)/server_summary_reader.o \
                   $(BUILD)/server_event_writer.o \
                   $(BUILD)/wait_event.o $(BUILD)/idle_rule.o \
                   $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz -lm

# test_agg_raw_crosscheck: the AGGREGATE-vs-RAW differential gate. pgwt-server
# answers the same question from pre-aggregated per-second records or by
# materialising every raw event, and the failure mode that matters is the two
# silently disagreeing -- FID-2 shipped exactly that. For every
# pgwt_compute_*_from_summaries() in src/compute.h this computes BOTH paths
# over the SAME committed blocks and the SAME window and asserts bit-exact
# agreement, field by field, with no blanket epsilon; a new aggregate path
# with no differential turns it red (it parses ../src/compute.h and refuses a
# header it cannot read or parse). Carries mutation probes proving each
# comparator can see a one-nanosecond / one-count / one-cell / one-row
# difference, and a bypass suite for every way the comparison can be starved
# into "0 == 0". Server build, no BPF, no live PG; ~0.1 s.
test_agg_raw_crosscheck: test_agg_raw_crosscheck.c ../src/compute.c \
                   $(BUILD)/server_summary_writer.o \
                   $(BUILD)/server_summary_reader.o \
                   $(BUILD)/server_event_writer.o \
                   $(BUILD)/wait_event.o $(BUILD)/idle_rule.o \
                   $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz -lm

# test_io_worker_summary: io_workers must be outside the SUMMARY model too
# (#315, #376; summary v6). Same build shape as test_idle_summary -- the real
# writer, the real reader and compute.c, server build, no BPF, no live PG.
# Section 4 reads ../src/event_stream.c and ../src/escalation.c as TEXT (the
# daemon needs libbpf and a BPF skeleton, so its two summary producers cannot
# be linked here), which is why the recipe needs no daemon objects.
test_io_worker_summary: test_io_worker_summary.c ../src/compute.c \
                   $(BUILD)/server_summary_writer.o \
                   $(BUILD)/server_summary_reader.o \
                   $(BUILD)/server_event_writer.o \
                   $(BUILD)/wait_event.o $(BUILD)/idle_rule.o \
                   $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz -lm

# test_io_worker_parity: the RAW-vs-SUMMARY parity gate for io_worker time
# (#376 + #315). The raw path drops io_worker records from the time model
# while the summary writer's accum_event adds their time to idle_ns and to the
# DB-Time classes, so the same window answers Idle and DB Time two ways. RED
# until the writer agrees. The io_worker property is produced the way
# production produces it -- unflagged events into the REAL
# pgwt_summary_push_event (as src/event_stream.c pushes them) and
# pgwt_tag_events against a backends.jsonl registry resolved through
# pgwt_backend_type_flag (as src/server.c tags every raw window) -- never by
# pre-setting PGWT_EVENT_FLAG_IO_WORKER, which is the trap commit 8705412
# records. Three window cases (excluding the capture's final second,
# including it, the full span), handle_heatmap's two routes pinned against
# each other, mutation probes, and a bypass suite in which every way the
# comparison can be starved must REFUSE. An ACCEPTANCE TIER runs against a
# real PG18 io_method=worker capture when one is named
# (PGWT_PARITY_TRACE=<trace dir> or argv[1]). Server build, no BPF, no live
# PG; ~0.3 s for the unit tier.
test_io_worker_parity: test_io_worker_parity.c ../src/compute.c \
                   $(BUILD)/server_summary_writer.o \
                   $(BUILD)/server_summary_reader.o \
                   $(BUILD)/server_event_writer.o \
                   $(BUILD)/server_event_reader.o \
                   $(BUILD)/server_cmdline.o \
                   $(BUILD)/wait_event.o $(BUILD)/idle_rule.o \
                   $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz -lm

# test_cpu_split_parity: the RAW-vs-SUMMARY parity gate for the CPU ROWS
# (#408, summary v7). The raw time model emits "CPU (running)" +
# "CPU (waiting for a core)" wherever a record carried a measured cpu_ns, and
# charges a foreground we==0 interval outside a command to Idle
# (PGWT_WEI_NONCMD_CPU). The summary record stored neither the measurement nor
# the relabel, so the same window answered with ONE "CPU*" row that was also
# 3.54% too large -- the runqueue signal disappeared from the Overview on
# every window of 120 s or more. Neither property is written into the
# fixture: the relabel comes from the production classifier on BOTH sides
# (pgwt_tag_events for the raw oracle, the producer's eff_event for the
# writer), and the fixture's intervals are contiguous because both command
# gates bank command-open time onto the pid's next record. Bypass suite: an
# all-UNKNOWN fixture, a fixture with nothing to relabel, a producer that
# resolves NOTHING (the reachability probe -- it must go RED, or eff_event is
# dead code), a window holding no records, a deleted summary dir, a v6 file in
# the window, a truncated v7 record (the scalars are the LAST bytes, so a
# short block is exactly what would silently restore the v6 behaviour) and an
# illegal eff_event. Server build, no BPF, no live PG; ~0.2 s.
test_cpu_split_parity: test_cpu_split_parity.c ../src/compute.c \
                   $(BUILD)/server_summary_writer.o \
                   $(BUILD)/server_summary_reader.o \
                   $(BUILD)/server_event_writer.o \
                   $(BUILD)/server_event_reader.o \
                   $(BUILD)/server_cmdline.o \
                   $(BUILD)/wait_event.o $(BUILD)/idle_rule.o \
                   $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz -lm

# test_straddler_accounting: #379 / summary v8 -- a long wait's DURATION must
# land in the seconds it occupied, not whole in the second it closed in. The
# rule is a watermark: the producer notes what each pid has open and the
# writer credits each completed second as it is written, the closing record
# contributing only the remainder. Asserted: placement (ten seconds, 1 s
# each), CONSERVATION (exactly the duration, once -- not 1+2+...+10, which is
# what the live view's open-stretch rule would give a persistent writer),
# occurrence quantities staying whole and once (count, histogram bucket,
# max_ns), both tick/close orders, and the two deliberate refusals (queries[]
# and an on-CPU gap, whose measured split spreading would destroy). Bypass
# suite: no notes at all (the reachability probe -- it must fall back to v7,
# or the credit path is dead code), a stale note, an exit, a mismatched note,
# an io_worker, a full table, a missing trace dir (the read-back must refuse)
# and a late first note. Server build, no BPF, no live PG; ~0.2 s.
test_straddler_accounting: test_straddler_accounting.c \
                   $(BUILD)/server_summary_writer.o \
                   $(BUILD)/server_summary_reader.o \
                   $(BUILD)/server_event_writer.o \
                   $(BUILD)/server_event_reader.o \
                   $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o \
                   $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz -lm

# test_exec_index: the EXECUTIONS INDEX (paint-latency plan, Phase 3,
# src/exec_index.c). A differential against the real pgwt_compute_executions
# over every block split of every fixture, asserting the ten boundary fields
# BIT-EXACTLY, plus the bypass suite (an empty/unsealed/wrong-version index, a
# skipped or reordered block, an out-of-order record, a window outside
# coverage and every injected allocation failure must REFUSE, never answer)
# and the measured O(executions in window) claim. Server build, no BPF, no
# live PG; ~0.2 s.
#
test_exec_index: test_exec_index.c ../src/exec_index.c ../src/compute.c \
                   $(BUILD)/wait_event.o $(BUILD)/idle_rule.o \
                   $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -lm

# test_block_agg: paint-latency Phase 1 — the per-committed-block transition
# aggregate (src/block_agg.c). The oracle is the SHIPPING raw implementation,
# pgwt_compute_transitions() in ../src/compute.c, compiled in directly with
# -DPGWT_SERVER exactly as test_window_clip does, with its one foreign symbol
# (pgwt_visit_summaries) stubbed in the test — so the comparison is against
# the product rather than against a copy or against this module's own
# accumulator. §9 additionally writes a real multi-block trace with the real
# writer and reads it back with the real reader, driving the same
# pgwt_block_agg_window_from_reader() the server calls, hence event_reader /
# event_writer / -llz4. No BPF, no live PG; runs anywhere; ~1 s.
test_block_agg: test_block_agg.c ../src/block_agg.c ../src/compute.c \
                $(BUILD)/server_event_reader.o $(BUILD)/server_event_writer.o \
                $(BUILD)/wait_event.o $(BUILD)/idle_rule.o \
                $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lm

# Exercise the actual server gate/loader/response against reader-decoded
# block aggregates from the same two-block file (command state crosses blocks).
TRANSITIONS_LOADER_OBJS = $(addprefix $(BUILD)/server_,compute.o block_agg.o \
    event_reader.o event_writer.o summary_writer.o summary_reader.o \
    wait_event.o idle_rule.o spawn.o cmdline.o backend_meta.o cJSON.o)
test_transitions_loader: test_transitions_loader.c ../src/server.c $(TRANSITIONS_LOADER_OBJS)
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $< $(TRANSITIONS_LOADER_OBJS) -lz -llz4 -lm

# test_top_wait_backfill: #319 — handle_top_sessions' and handle_top_queries'
# raw backfill of top_wait over the summary-derived page. Same server.c-
# included recipe as test_transitions_loader (the real writer, the real
# summary writer, the real handlers).
test_top_wait_backfill: test_top_wait_backfill.c ../src/server.c $(TRANSITIONS_LOADER_OBJS)
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $< $(TRANSITIONS_LOADER_OBJS) -lz -llz4 -lm

# test_window_overlap: #424 — the raw loader selected non-sample records by
# CLOSE time, so a wait still running at the window's right edge contributed
# nothing. Admission is now OVERLAP and the per-occurrence statistics are
# gated on the close, together. Same server.c-included recipe (the real
# writer, the real loader, the real compute).
test_window_overlap: test_window_overlap.c ../src/server.c $(TRANSITIONS_LOADER_OBJS)
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $< $(TRANSITIONS_LOADER_OBJS) -lz -llz4 -lm

# bench_block_agg: the measurement behind Phase 1's falsifiable prediction
# (O(events in window) -> O(blocks x distinct pairs)). Deliberately NOT in
# unit_tests.list: a wall-clock assertion on a shared runner is runner noise
# dressed up as a gate. Built so the number can be reproduced on the gate box.
bench_block_agg: bench_block_agg.c ../src/block_agg.c ../src/compute.c \
                 $(BUILD)/server_event_reader.o $(BUILD)/server_event_writer.o \
                 $(BUILD)/wait_event.o $(BUILD)/idle_rule.o \
                 $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lm

# test_interval_index: the CONCURRENCY INTERVAL INDEX (paint-latency plan,
# Phase 4, src/interval_index.c). A differential against the real
# pgwt_compute_concurrency over every block split of every fixture, asserting
# peak_sessions[], peak_event[], every burst's pid list and bursts_total
# BIT-EXACTLY at two bucket resolutions, plus the bypass suite (a filtered
# request, an empty/unsealed/wrong-version index, a skipped or reordered
# block, an out-of-order record, a record outside its block's bounds, a
# window outside coverage, a window selecting nothing and every injected
# allocation failure must REFUSE, never answer) and the measured compaction.
# `./test_interval_index oneblock` is a DELIBERATE falsification of its own
# non-vacuity ledger and must exit nonzero.
# Server build, no BPF, no live PG; ~1 s.
#
test_interval_index: test_interval_index.c ../src/interval_index.c \
                   ../src/compute.c \
                   $(BUILD)/wait_event.o $(BUILD)/idle_rule.o \
                   $(BUILD)/spawn.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -lm

#
# #424 live half: the request-scoped open-interval overlay (src/open_overlay.c).
# Pure C plus cJSON and one AF_UNIX round-trip against a forked fixture
# daemon -- no BPF, no libbpf, no PostgreSQL, no root, so it also builds and
# runs on a developer Mac. Companion: tests/test_open_overlay_mutations.sh
# breaks one guard at a time (the precedence rule, the snapshot clip, the
# route/tier refusal, the absent-pids read as empty, the overall socket
# deadline, ...) and requires THIS binary to go red for each.
test_open_overlay: test_open_overlay.c ../src/open_overlay.c ../src/cJSON.c
	$(CC) $(CFLAGS) -o $@ $^
# test_replay_window: #424 — `--replay --from/--to` selects by OVERLAP and
# clips, while per-occurrence statistics stay selected by the record's own
# close timestamp. Covers BOTH halves (the record rule in pgwt_replay_events
# and the block scan in pgwt_replay_file, which must look PAST --to or the
# record rule is unreachable), plus a bypass suite for the lookahead budget,
# an unreadable block header and a SAMPLES tail. Same build shape as
# test_replay_fidelity: the daemon-flavour event_reader.c against the BPF-free
# accumulator core, so no bpftool is needed.
test_replay_window: test_replay_window.c ../src/event_reader.c \
                    map_reader_core.o $(BUILD)/server_event_writer.o \
                    $(BUILD)/server_wait_event.o $(BUILD)/server_idle_rule.o \
                    $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4 -lz

clean:
	rm -f $(TESTS) test_exec_index map_reader_core.o
