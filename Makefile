# pg_wait_tracer
# Requires: clang, llvm-strip, bpftool, libbpf-dev, libelf-dev, zlib1g-dev, liblz4-dev

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

BPF_CFLAGS = -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) \
             -I$(INC_DIR) -I$(SRC_DIR) \
             -I/usr/include/$(shell uname -m)-linux-gnu

CFLAGS     = -g -O2 -Wall -Wextra -Wno-unused-parameter \
             -I$(INC_DIR) -I$(SRC_DIR)
LDFLAGS    = -lbpf -lelf -lz -llz4

USER_SRCS  = $(SRC_DIR)/pg_wait_tracer.c \
             $(SRC_DIR)/daemon.c \
             $(SRC_DIR)/backend.c \
             $(SRC_DIR)/discovery.c \
             $(SRC_DIR)/map_reader.c \
             $(SRC_DIR)/event_stream.c \
             $(SRC_DIR)/output.c \
             $(SRC_DIR)/wait_event.c \
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
              $(SRC_DIR)/cmdline.c \
              $(SRC_DIR)/backend_meta.c \
              $(SRC_DIR)/cJSON.c
SERVER_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/server_%.o,$(SERVER_SRCS))
SERVER_LDFLAGS = -lz -llz4 -lm

.PHONY: all clean test-asan test-valgrind bench

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

$(INC_DIR):
	@mkdir -p $(INC_DIR)

# Step 1: Generate vmlinux.h from kernel BTF
$(INC_DIR)/vmlinux.h: | $(INC_DIR)
	@echo "  VMLINUX  $@"
	@$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

# Step 2: Compile BPF program
$(BUILD_DIR)/pg_wait_tracer.bpf.o: $(BPF_DIR)/pg_wait_tracer.bpf.c \
                                    $(INC_DIR)/vmlinux.h \
                                    $(SRC_DIR)/pg_wait_tracer.h | $(BUILD_DIR)
	@echo "  BPF      $@"
	@$(CLANG) $(BPF_CFLAGS) -c $< -o $@
	@$(LLVM_STRIP) -g $@

# Step 3: Generate BPF skeleton
$(INC_DIR)/pg_wait_tracer.skel.h: $(BUILD_DIR)/pg_wait_tracer.bpf.o | $(INC_DIR)
	@echo "  SKEL     $@"
	@$(BPFTOOL) gen skeleton $< > $@

# Step 4: Compile userspace C (all depend on skeleton + shared header)
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(INC_DIR)/pg_wait_tracer.skel.h \
                  $(SRC_DIR)/pg_wait_tracer.h | $(BUILD_DIR)
	@echo "  CC       $@"
	@$(CC) $(CFLAGS) -c $< -o $@

# Step 5: Link
$(TARGET): $(USER_OBJS)
	@echo "  LINK     $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "  DONE     $@"

# pgwt-server: compile WITHOUT skeleton dependency, with -DPGWT_SERVER
$(BUILD_DIR)/server_%.o: $(SRC_DIR)/%.c $(SRC_DIR)/pg_wait_tracer.h | $(BUILD_DIR)
	@echo "  CC       $@ (server)"
	@$(CC) $(CFLAGS) -DPGWT_SERVER -c $< -o $@

pgwt-server: $(SERVER_OBJS)
	@echo "  LINK     $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(SERVER_LDFLAGS)
	@echo "  DONE     $@"

clean:
	rm -rf $(BUILD_DIR) $(TARGET) pgwt-server
	rm -f $(INC_DIR)/vmlinux.h $(INC_DIR)/pg_wait_tracer.skel.h
