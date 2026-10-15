#!/usr/bin/env bash
# link-closure-check.sh — does src/map_reader.c still link where it used to?
#
# WHAT WENT WRONG, and why a compile check could not see it. map_reader.o is
# linked by two BPF-free unit tests (test_event_reader, test_concurrent_rw) for
# its accumulator core alone. Adding a pgwt_exact_resolve_attr() call to
# map_reader.c compiles perfectly and breaks BOTH of those links on Linux —
# and then wants exact_probe.o, query_text.o and cmdline.o to satisfy it. The
# branch could not be box-checked at all: `make box-check` exited 2 on two
# separate VMs for a link error, with every test that did run passing.
#
# The property that actually matters is not "it compiles" but "map_reader.o
# references no symbol it did not reference before". That is what this checks:
# the UNDEFINED symbol set of map_reader.o at HEAD against the same set at the
# merge base. A new entry is reported by name, whether or not some current test
# target happens to supply it, because the next target that links map_reader.o
# will not.
#
# Usage: link-closure-check.sh [base-ref]        (default: merge-base with
#                                                 origin/master)
# Exit: 0 clean, 1 the closure grew, 2 CANNOT VERIFY (refuses — never passes
#       on an input it could not resolve).
set -uo pipefail
cd "$(dirname "$0")/../../.." || exit 2

TU=${TU:-src/map_reader.c}
CC_BIN=${CC:-gcc}
command -v "$CC_BIN" >/dev/null || {
    echo "CANNOT VERIFY: compiler '$CC_BIN' not found" >&2; exit 2; }
command -v nm >/dev/null || {
    echo "CANNOT VERIFY: nm not found" >&2; exit 2; }

base=${1:-}
if [[ -z "$base" ]]; then
    base=$(git merge-base HEAD origin/master 2>/dev/null)
fi
if [[ -z "$base" ]] || ! git rev-parse --verify --quiet "$base^{commit}" >/dev/null 2>&1; then
    echo "CANNOT VERIFY: cannot resolve a base commit (tried '${1:-merge-base HEAD origin/master}')" >&2
    echo "  a closure check with no baseline approves everything; refusing" >&2
    exit 2
fi
if ! git cat-file -e "$base:$TU" 2>/dev/null; then
    echo "CANNOT VERIFY: $TU does not exist at base $base" >&2
    exit 2
fi
echo "base $(git rev-parse --short "$base")   TU $TU"

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/inc/sys" "$work/inc/bpf"
# Stub headers for the Linux-only APIs, so this runs on a developer Mac. They
# declare, never implement — only the symbol REFERENCES matter here.
cat > "$work/inc/sys/epoll.h" <<'EOF'
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
cat > "$work/inc/sys/timerfd.h" <<'EOF'
#ifndef STUB_TFD_H
#define STUB_TFD_H
#include <time.h>
#define TFD_CLOEXEC 0
#define TFD_NONBLOCK 0
int timerfd_create(int clockid, int flags);
struct itimerspec { struct timespec it_interval, it_value; };
int timerfd_settime(int fd, int flags, const struct itimerspec *nv, struct itimerspec *ov);
#endif
EOF
cat > "$work/inc/lz4.h" <<'EOF'
#ifndef STUB_LZ4_H
#define STUB_LZ4_H
int LZ4_compress_default(const char *s, char *d, int n, int c);
int LZ4_decompress_safe(const char *s, char *d, int n, int c);
int LZ4_compressBound(int n);
#endif
EOF
cat > "$work/inc/bpf/libbpf.h" <<'EOF'
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
cat > "$work/inc/bpf/bpf.h" <<'EOF'
#ifndef STUB_BPF_H
#define STUB_BPF_H
#define BPF_ANY 0
int bpf_map_lookup_elem(int fd, const void *k, void *v);
int bpf_map_update_elem(int fd, const void *k, const void *v, unsigned long long f);
int bpf_map_delete_elem(int fd, const void *k);
int bpf_map_get_next_key(int fd, const void *k, void *n);
#endif
EOF
cat > "$work/inc/pg_wait_tracer.skel.h" <<'EOF'
#ifndef STUB_SKEL_H
#define STUB_SKEL_H
struct bpf_map;
struct pg_wait_tracer_bpf {
    struct { struct bpf_map *state_map, *accum_map, *fail_counters; } maps;
    struct { int exact_cpu_active; } *bss;
};
#endif
EOF

CFLAGS=(-c -std=gnu11 -w -D_GNU_SOURCE "-I$work/inc" -Isrc -Iinclude)

# The base revision's TU is compiled against the CURRENT headers on purpose:
# the question is what THIS source file now references, not whether the whole
# old tree still builds.
git show "$base:$TU" > "$work/base_$(basename "$TU")" || exit 2
if ! "$CC_BIN" "${CFLAGS[@]}" -o "$work/base.o" "$work/base_$(basename "$TU")"; then
    echo "CANNOT VERIFY: the BASE revision of $TU does not compile here —" >&2
    echo "  with no baseline symbol set this check would approve anything" >&2
    exit 2
fi
if ! "$CC_BIN" "${CFLAGS[@]}" -o "$work/head.o" "$TU"; then
    echo "FAIL: $TU does not compile at HEAD"; exit 1
fi

nm -u "$work/base.o" | sed 's/^ *//' | sort -u > "$work/base.txt"
nm -u "$work/head.o" | sed 's/^ *//' | sort -u > "$work/head.txt"

# POSITIVE CONTROL. An empty or near-empty symbol set compares equal to
# anything: if the TU somehow produced no references (wrong file, everything
# preprocessed away), the diff below would be silently green. Require that
# both objects actually reference something and that the file's own anchor
# symbol is DEFINED in the head object.
nb=$(wc -l < "$work/base.txt"); nh=$(wc -l < "$work/head.txt")
if [[ "$nb" -lt 5 || "$nh" -lt 5 ]]; then
    echo "CANNOT VERIFY: symbol sets are implausibly small (base=$nb head=$nh)" >&2
    exit 2
fi
anchor=${ANCHOR:-_pgwt_read_state_map}
if ! nm "$work/head.o" | grep -q " T ${anchor}\| t ${anchor}\| T ${anchor#_}\| t ${anchor#_}"; then
    echo "CANNOT VERIFY: $TU's anchor symbol ${anchor} is not defined in the" >&2
    echo "  object just built — this is not the translation unit it claims" >&2
    exit 2
fi
echo "base references $nb symbols, head $nh; anchor ${anchor} present"

added=$(comm -13 "$work/base.txt" "$work/head.txt")
if [[ -z "$added" ]]; then
    echo "OK: $(basename "$TU") references no symbol it did not reference at base"
    exit 0
fi
echo "FAIL: $(basename "$TU") gained link-time dependencies:"
echo "$added" | sed 's/^/    /'
echo "  every target that links $(basename "${TU%.c}").o must now supply these."
echo "  Either move the caller into its own translation unit (what #424 did"
echo "  with src/open_snapshot.c) or add the objects to each target."
exit 1
