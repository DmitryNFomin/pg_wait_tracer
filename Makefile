# pg_wait_tracer
# Requires: clang, llvm-strip, bpftool, libbpf-dev, libelf-dev, zlib1g-dev, liblz4-dev

.DEFAULT_GOAL := all

CLANG      ?= clang
LLVM_STRIP ?= llvm-strip
BPFTOOL    ?= bpftool
CC         ?= gcc

SRC_DIR    = src
BPF_DIR    = src/bpf
INC_DIR    = include
BUILD_DIR  = build
TARGET     = pg_wait_tracer

ARCH       := $(shell uname -m | sed 's/x86_64/x86/' | sed 's/aarch64/arm64/')

# ---------------------------------------------------------------------------
# Build version (T7 / TST-11): every binary embeds the exact commit it was
# built from. pgwt-server reports it in the `info` response; the daemon in
# the control-socket `status`; the Go client (pgwt-client target) compares it
# against the server and warns on skew. Overridable for reproducible builds.
# ---------------------------------------------------------------------------
PGWT_BUILD_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

# ---------------------------------------------------------------------------
# libbpf selection (Rocky 8 / RHEL 8 support)
#
# The BPF program and daemon use USDT support: the BPF-side header
# <bpf/usdt.bpf.h> and the userspace function bpf_program__attach_usdt(),
# both of which require libbpf >= 0.8. Ubuntu (CI), Rocky 9 / RHEL 9 (CRB)
# all ship a recent enough libbpf, so the system one is used unchanged.
#
# Rocky 8 / RHEL 8 ships libbpf 0.5.0, which predates USDT entirely (no
# usdt.bpf.h, no attach_usdt symbol). When the system libbpf is too old we
# build a pinned libbpf from source into build/libbpf and link it statically.
# This is detection-driven, so it is a no-op on every distro that already has
# a usable libbpf — no Rocky 9 / Ubuntu / CI behaviour change.
# ---------------------------------------------------------------------------
LIBBPF_VERSION  ?= v1.4.7
BPFTOOL_VERSION ?= v7.4.0

# Probe: does the system libbpf expose bpf_program__attach_usdt AND usdt.bpf.h?
# Delegated to a script so make's $(shell ...) never has to parse C source
# (parentheses in main(void) confuse make's paren matching).
SYSTEM_LIBBPF_HAS_USDT := $(shell CC='$(CC)' sh scripts/detect_libbpf_usdt.sh)

ifeq ($(SYSTEM_LIBBPF_HAS_USDT),yes)
  # System libbpf is recent enough — use it (Rocky 9, Ubuntu, CI: unchanged).
  BPF_LIBBPF_INC =
  LIBBPF_INC     =
  LIBBPF_LIB     = -lbpf
  LIBBPF_DEP     =
  # System bpftool is fine when its libbpf matches the build's libbpf.
  BPFTOOL_DEP    =
else
  # System libbpf lacks USDT (Rocky 8 / RHEL 8: libbpf 0.5.0). Build a pinned
  # libbpf from source and link it statically.
  BUNDLED_LIBBPF_DIR  = $(BUILD_DIR)/libbpf
  BUNDLED_LIBBPF_SRC  = $(BUNDLED_LIBBPF_DIR)/src
  BUNDLED_LIBBPF_INC  = $(BUNDLED_LIBBPF_DIR)/root/usr/include
  BUNDLED_LIBBPF_A    = $(BUNDLED_LIBBPF_DIR)/root/usr/lib64/libbpf.a
  # -I the bundled headers FIRST so usdt.bpf.h and the newer libbpf.h win.
  BPF_LIBBPF_INC = -I$(BUNDLED_LIBBPF_INC)
  LIBBPF_INC     = -I$(BUNDLED_LIBBPF_INC)
  # Static libbpf pulls in libelf and zlib explicitly.
  LIBBPF_LIB     = $(BUNDLED_LIBBPF_A) -lelf -lz
  LIBBPF_DEP     = $(BUNDLED_LIBBPF_A)

  # The BPF object is now compiled against the newer libbpf headers, so the
  # system bpftool (RHEL 8 ships v4.18, libbpf 0.5.0) cannot finalize its BTF
  # when generating the skeleton ("Error finalizing .BTF: -2"). Build a pinned
  # bpftool from source and use it for all bpftool operations. (Same fallback
  # CI already uses when the runner lacks a working bpftool.)
  BUNDLED_BPFTOOL_DIR = $(BUILD_DIR)/bpftool
  BUNDLED_BPFTOOL     = $(BUNDLED_BPFTOOL_DIR)/src/bpftool
  BPFTOOL             = $(BUNDLED_BPFTOOL)
  BPFTOOL_DEP         = $(BUNDLED_BPFTOOL)
endif

BPF_CFLAGS = -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) \
             $(BPF_LIBBPF_INC) \
             -I$(INC_DIR) -I$(SRC_DIR) \
             -I/usr/include/$(shell uname -m)-linux-gnu

CFLAGS     = -g -O2 -Wall -Wextra -Wno-unused-parameter \
             -DPGWT_BUILD_VERSION='"$(PGWT_BUILD_VERSION)"' \
             $(LIBBPF_INC) \
             -I$(INC_DIR) -I$(SRC_DIR)
DEPFLAGS   = -MMD -MP
LDFLAGS    = $(LIBBPF_LIB) -lelf -lz -llz4

USER_SRCS  = $(SRC_DIR)/pg_wait_tracer.c \
             $(SRC_DIR)/daemon.c \
             $(SRC_DIR)/control.c \
             $(SRC_DIR)/effective_cores.c \
             $(SRC_DIR)/provider_full.c \
             $(SRC_DIR)/provider_coop.c \
             $(SRC_DIR)/sampler.c \
             $(SRC_DIR)/exact_probe.c \
             $(SRC_DIR)/escalation.c \
             $(SRC_DIR)/escalation_budget.c \
             $(SRC_DIR)/anomaly.c \
             $(SRC_DIR)/backend.c \
             $(SRC_DIR)/discovery.c \
             $(SRC_DIR)/backend_status_layout.c \
             $(SRC_DIR)/map_reader.c \
             $(SRC_DIR)/event_stream.c \
             $(SRC_DIR)/output.c \
             $(SRC_DIR)/wait_event.c \
             $(SRC_DIR)/spawn.c \
             $(SRC_DIR)/perf_event.c \
             $(SRC_DIR)/cmdline.c \
             $(SRC_DIR)/snapshot.c \
             $(SRC_DIR)/event_writer.c \
             $(SRC_DIR)/event_reader.c \
             $(SRC_DIR)/replay.c \
             $(SRC_DIR)/summary_writer.c \
             $(SRC_DIR)/query_text.c \
             $(SRC_DIR)/backend_meta.c \
             $(SRC_DIR)/cJSON.c

USER_OBJS  = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(USER_SRCS))

# pgwt-server: lightweight replay server (no BPF dependencies)
SERVER_SRCS = $(SRC_DIR)/server.c \
              $(SRC_DIR)/compute.c \
              $(SRC_DIR)/event_reader.c \
              $(SRC_DIR)/event_writer.c \
              $(SRC_DIR)/summary_writer.c \
              $(SRC_DIR)/summary_reader.c \
              $(SRC_DIR)/wait_event.c \
              $(SRC_DIR)/spawn.c \
              $(SRC_DIR)/cmdline.c \
              $(SRC_DIR)/backend_meta.c \
              $(SRC_DIR)/cJSON.c
SERVER_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/server_%.o,$(SERVER_SRCS))
SERVER_LDFLAGS = -lz -llz4 -lm

# Keep incremental builds ABI-safe when a transitive header changes. Stage 3
# adds exact-probe ownership to struct pgwt_daemon, which is embedded across
# several translation units and therefore must never be rebuilt partially.
-include $(USER_OBJS:.o=.d) $(SERVER_OBJS:.o=.d)

.PHONY: all clean test-asan test-valgrind bench pgwt-client

all: $(TARGET) pgwt-server

# ASan build: recompile server with address sanitizer
test-asan:
	$(MAKE) clean
	$(MAKE) pgwt-server CFLAGS="$(CFLAGS) -fsanitize=address -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address"
	$(MAKE) -C tests
	@echo "=== Running tests under ASan ==="
	cd tests && for t in test_wait_event test_cmdline test_bucket; do ./$$t || exit 1; done
	@echo "=== ASan: all tests passed ==="

# Valgrind: run server under valgrind with synthetic data
test-valgrind:
	$(MAKE) -C tests
	@echo "=== Running pgwt-server under Valgrind ==="
	rm -rf /tmp/pgwt-valgrind-test && mkdir -p /tmp/pgwt-valgrind-test
	tests/gen_bench_traces -o /tmp/pgwt-valgrind-test -n 10000
	echo '{"cmd":"time_model","from_ns":0,"to_ns":999999999999999}' | \
		valgrind --leak-check=full --error-exitcode=1 ./pgwt-server /tmp/pgwt-valgrind-test > /dev/null
	rm -rf /tmp/pgwt-valgrind-test
	@echo "=== Valgrind: clean ==="

# Performance benchmark
bench:
	$(MAKE) -C tests
	python3 tests/bench_server.py

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Step 0 (Rocky 8 only): build a pinned libbpf + bpftool from source when the
# system libbpf is too old for USDT. Only referenced when *_DEP is non-empty.
ifneq ($(LIBBPF_DEP),)
$(BUNDLED_LIBBPF_A): | $(BUILD_DIR)
	@echo "  LIBBPF   system libbpf lacks USDT — building $(LIBBPF_VERSION) from source"
	@rm -rf $(BUNDLED_LIBBPF_DIR)
	@git clone -q --depth 1 --branch $(LIBBPF_VERSION) \
		https://github.com/libbpf/libbpf.git $(BUNDLED_LIBBPF_DIR)
	@$(MAKE) -s -C $(BUNDLED_LIBBPF_SRC) BUILD_STATIC_ONLY=1 \
		PREFIX=/usr DESTDIR=$(abspath $(BUNDLED_LIBBPF_DIR))/root install install_uapi_headers
	@echo "  LIBBPF   built $@"
endif

ifneq ($(BPFTOOL_DEP),)
$(BUNDLED_BPFTOOL): | $(BUILD_DIR)
	@echo "  BPFTOOL  system bpftool too old for this BPF object — building $(BPFTOOL_VERSION) from source"
	@rm -rf $(BUNDLED_BPFTOOL_DIR)
	@git clone -q --depth 1 --branch $(BPFTOOL_VERSION) --recurse-submodules \
		https://github.com/libbpf/bpftool.git $(BUNDLED_BPFTOOL_DIR)
	@$(MAKE) -s -C $(BUNDLED_BPFTOOL_DIR)/src
	@echo "  BPFTOOL  built $@"
endif

# Step 1: Generate vmlinux.h from kernel BTF
$(INC_DIR)/vmlinux.h: $(BPFTOOL_DEP)
	@echo "  VMLINUX  $@"
	@mkdir -p $(INC_DIR)
	@$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

# Step 2: Compile BPF program
$(BUILD_DIR)/pg_wait_tracer.bpf.o: $(BPF_DIR)/pg_wait_tracer.bpf.c \
                                    $(INC_DIR)/vmlinux.h \
                                    $(LIBBPF_DEP) \
                                    $(SRC_DIR)/pg_wait_tracer.h | $(BUILD_DIR)
	@echo "  BPF      $@"
	@$(CLANG) $(BPF_CFLAGS) -c $< -o $@
	@$(LLVM_STRIP) -g $@

# Step 3: Generate BPF skeleton
$(INC_DIR)/pg_wait_tracer.skel.h: $(BUILD_DIR)/pg_wait_tracer.bpf.o
	@echo "  SKEL     $@"
	@$(BPFTOOL) gen skeleton $< > $@

# Step 4: Compile userspace C (all depend on skeleton + shared header)
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(INC_DIR)/pg_wait_tracer.skel.h \
                  $(LIBBPF_DEP) \
                  $(SRC_DIR)/pg_wait_tracer.h | $(BUILD_DIR)
	@echo "  CC       $@"
	@$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# Step 5: Link
$(TARGET): $(USER_OBJS)
	@echo "  LINK     $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "  DONE     $@"

# pgwt-server: compile WITHOUT skeleton dependency, with -DPGWT_SERVER
$(BUILD_DIR)/server_%.o: $(SRC_DIR)/%.c $(SRC_DIR)/pg_wait_tracer.h | $(BUILD_DIR)
	@echo "  CC       $@ (server)"
	@$(CC) $(CFLAGS) $(DEPFLAGS) -DPGWT_SERVER -c $< -o $@

pgwt-server: $(SERVER_OBJS)
	@echo "  LINK     $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(SERVER_LDFLAGS)
	@echo "  DONE     $@"

# Go investigation client (runs on the operator's laptop). The -X flag embeds
# the same git-describe version the C binaries carry, so the client/server
# version handshake (T7 / TST-11) compares like with like. Cross-compile with
# e.g. GOOS=darwin GOARCH=arm64 make pgwt-client CLIENT_OUT=pgwt-darwin-arm64
CLIENT_OUT ?= web/pgwt
pgwt-client:
	@echo "  GO       $(CLIENT_OUT) (version $(PGWT_BUILD_VERSION))"
	@cd web && go build -ldflags "-X main.buildVersion=$(PGWT_BUILD_VERSION)" \
		-o $(abspath $(CLIENT_OUT)) .
	@echo "  DONE     $(CLIENT_OUT)"

clean:
	rm -rf $(BUILD_DIR) $(TARGET) pgwt-server
	rm -f $(INC_DIR)/vmlinux.h $(INC_DIR)/pg_wait_tracer.skel.h
