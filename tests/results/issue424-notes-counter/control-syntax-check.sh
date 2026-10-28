#!/usr/bin/env bash
# control-syntax-check.sh — does src/control.c still COMPILE after the four
# new metrics keys were added?
#
# WHY THIS IS NEEDED. `make check` on a Mac never builds C (a C test has
# passed twice locally while being uncompilable), and this change touches
# src/control.c, whose real build needs libbpf, a BPF skeleton and
# Linux-only headers. Discovering a typo'd struct field on the gate box
# costs a 40-minute serialised box run. So this stubs the Linux-only
# headers -- declarations only, never implementations, because this is
# -fsyntax-only -- and compiles the real TU against the tree's own real
# headers.
#
# POSITIVE CONTROL, because a syntax check that cannot fail proves nothing:
# the same compile is repeated with one of the new keys' struct fields
# renamed to a member that does not exist. That MUST error. If it does not,
# this script is not compiling what it claims to and refuses.
#
# Usage: tests/results/issue424-notes-counter/control-syntax-check.sh
set -uo pipefail
cd "$(dirname "$0")/../../.." || exit 2

stub=$(mktemp -d)
trap 'rm -rf "$stub"' EXIT
mkdir -p "$stub/inc/sys" "$stub/inc/bpf"

cat > "$stub/inc/sys/epoll.h" <<'EOF'
#ifndef STUB_EPOLL_H
#define STUB_EPOLL_H
#include <stdint.h>
#define EPOLLIN 0x1
#define EPOLLOUT 0x4
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
typedef union epoll_data { void *ptr; int fd; uint32_t u32; uint64_t u64; } epoll_data_t;
struct epoll_event { uint32_t events; epoll_data_t data; };
int epoll_ctl(int, int, int, struct epoll_event *);
int epoll_wait(int, struct epoll_event *, int, int);
int epoll_create1(int);
#endif
EOF
cat > "$stub/inc/sys/timerfd.h" <<'EOF'
#ifndef STUB_TFD_H
#define STUB_TFD_H
#include <time.h>
#define TFD_CLOEXEC 0
#define TFD_NONBLOCK 0
#define TFD_TIMER_ABSTIME 1
int timerfd_create(int clockid, int flags);
struct itimerspec { struct timespec it_interval, it_value; };
int timerfd_settime(int fd, int flags, const struct itimerspec *nv, struct itimerspec *ov);
#endif
EOF
cat > "$stub/inc/lz4.h" <<'EOF'
#ifndef STUB_LZ4_H
#define STUB_LZ4_H
int LZ4_compress_default(const char *src, char *dst, int srcSize, int dstCapacity);
int LZ4_decompress_safe(const char *src, char *dst, int compressedSize, int dstCapacity);
int LZ4_compressBound(int inputSize);
#endif
EOF
cat > "$stub/inc/bpf/libbpf.h" <<'EOF'
#ifndef STUB_LIBBPF_H
#define STUB_LIBBPF_H
#include <stddef.h>
#include <stdint.h>
struct bpf_map;
struct ring_buffer;
int bpf_map__fd(const struct bpf_map *map);
int ring_buffer__consume(struct ring_buffer *rb);
int libbpf_num_possible_cpus(void);
#endif
EOF
cat > "$stub/inc/bpf/bpf.h" <<'EOF'
#ifndef STUB_BPF_H
#define STUB_BPF_H
#define BPF_ANY 0
#define BPF_NOEXIST 1
#define BPF_EXIST 2
int bpf_map_lookup_elem(int fd, const void *key, void *value);
int bpf_map_update_elem(int fd, const void *key, const void *value, unsigned long long flags);
int bpf_map_delete_elem(int fd, const void *key);
int bpf_map_get_next_key(int fd, const void *key, void *next_key);
#endif
EOF
cat > "$stub/inc/pg_wait_tracer.skel.h" <<'EOF'
#ifndef STUB_SKEL_H
#define STUB_SKEL_H
struct bpf_map;
struct pg_wait_tracer_bpf {
    struct { struct bpf_map *state_map, *accum_map, *fail_counters; } maps;
    struct { int exact_cpu_active; } *bss;
};
#endif
EOF

CFLAGS=(-std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-comment
        -Werror=implicit-function-declaration
        -DMSG_NOSIGNAL=0 -DSOCK_NONBLOCK=0 -DSOCK_CLOEXEC=0
        -DPGWT_BUILD_VERSION='"syntax-check"'
        '-Daccept4(a,b,c,d)=accept(a,b,c)' "-I$stub/inc" -Iinclude -Isrc)

rc=0
for tu in control.c map_reader.c summary_writer.c; do
    if ${CC:-cc} -fsyntax-only "${CFLAGS[@]}" "src/$tu" 2>"$stub/$tu.err"; then
        echo "  OK    src/$tu compiles"
    else
        echo "  FAIL  src/$tu"
        sed 's/^/        /' "$stub/$tu.err" | head -12
        rc=1
    fi
done

# POSITIVE CONTROL: rename one of the four new fields to a non-member. The
# compile MUST fail; if it passes, this script is not reaching the lines it
# claims to check and must refuse rather than report a clean bill.
mkdir -p "$stub/neg"
sed 's/open_cpu_notes_total/open_cpu_notes_total_TYPO/g' \
    src/control.c > "$stub/neg/control.c"
if ${CC:-cc} -fsyntax-only "${CFLAGS[@]}" -Isrc "$stub/neg/control.c" \
        2>/dev/null; then
    echo "  REFUSE: the positive control COMPILED. A renamed struct member"
    echo "          was accepted, so this check is not compiling the new"
    echo "          keys and its OK lines above mean nothing."
    exit 2
fi
echo "  OK    positive control: a renamed field is rejected, so the new keys"
echo "        really are being compiled"

if [[ "$rc" -ne 0 ]]; then
    echo "=== control-syntax-check: FAIL ==="
    exit 1
fi
echo "=== control-syntax-check: PASS (syntax only — never a substitute for"
echo "    make box-check, which links and runs) ==="
