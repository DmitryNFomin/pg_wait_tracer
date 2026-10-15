#!/usr/bin/env bash
# v9-dual-path-probe.sh — compile this branch against BOTH summary-writer
# versions: src/control.c's #if PGWT_SUMMARY_VERSION >= 9 metrics block in
# each of its two branches, and src/map_reader.c (which has no conditional)
# under both, where it must NOT call the dormant v9 producer entry point.
#
# WHY THIS EXISTS. The v9 writer API (pgwt_summary_note_open_ex, struct
# pgwt_summary_open_cpu, the seven open_cpu_* counters) lands on branch
# agent/424-v9, not here, so this branch's code sits behind
# `#if PGWT_SUMMARY_VERSION >= 9`. That keeps THIS branch buildable and
# box-checkable on its own — and it also means this branch's own build never
# compiles the v9 text, which is exactly how a typo in it would reach
# integration unnoticed. So compile it here against v9's REAL header.
#
# It is a SYNTAX/TYPE probe, not a behaviour test: it proves the call matches
# the declaration and that the counter fields exist under those names. The
# behavioural verification is summary_open_cpu_credited_ns != 0 on a live
# capture with an in-flight CPU statement; that needs Linux + PostgreSQL and is
# NOT done here.
#
# THE FALSE NEGATIVE THIS SCRIPT ALREADY HAD, which is why it now checks which
# branch it compiled: the first version shadowed src/summary_writer.h via -I and
# compiled the two TUs from a scratch directory. That does nothing — map_reader.c
# does not include summary_writer.h directly, src/daemon.h does, and a quoted
# include resolves relative to the directory of THE FILE CONTAINING THE
# DIRECTIVE (src/), never the -I list. So the v8 header won both times, the v9
# branch was never compiled, and the probe reported "both branches compile"
# while a deliberately broken 6-argument call to a 7-argument function sat in
# the file. The fix is a whole-tree copy with the header substituted, plus a
# positive control on the preprocessed output: a probe that cannot prove which
# branch it compiled must REFUSE, not pass.
#
# Usage: v9-dual-path-probe.sh [path-to-agent-424-v9-worktree]
set -uo pipefail
cd "$(dirname "$0")/../../.." || exit 2          # repo root (this worktree)
V9=${1:-../agent-a1a141941c73e246b}
HDR="$V9/src/summary_writer.h"

if [[ ! -f "$HDR" ]]; then
    echo "CANNOT VERIFY: v9 header not found at $HDR" >&2
    echo "  pass the agent/424-v9 worktree path as \$1" >&2
    exit 2                                        # refuse, never pass blindly
fi
ver=$(sed -n 's/^#define PGWT_SUMMARY_VERSION  *\([0-9]*\).*/\1/p' "$HDR")
if [[ "$ver" != "9" ]]; then
    echo "CANNOT VERIFY: $HDR declares PGWT_SUMMARY_VERSION='$ver', not 9 —" >&2
    echo "  the v9 branch of the #if would not be the one compiled" >&2
    exit 2
fi
mine=$(sed -n 's/^#define PGWT_SUMMARY_VERSION  *\([0-9]*\).*/\1/p' src/summary_writer.h)
echo "this branch: PGWT_SUMMARY_VERSION=$mine   probe header: $ver"

# Stub headers for the Linux-only APIs these TUs include, so the probe runs on a
# developer Mac. They declare, never implement: this is -fsyntax-only.
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
        -DMSG_NOSIGNAL=0 -DSOCK_NONBLOCK=0 -DSOCK_CLOEXEC=0
        '-Daccept4(a,b,c,d)=accept(a,b,c)' "-I$stub/inc" -Iinclude)

fail=0

# probe <label> <tree-dir> <tu-basename> <want: v8|v9>
#
# The `want` check is the positive control: preprocess the TU and look for the
# call that only the intended branch contains. A probe that compiled the OTHER
# branch is a probe that proves nothing about the one it claims.
probe() {
    local label=$1 tree=$2 tu=$3 want=$4
    local pp="$stub/$want-$tu.i"
    if ! gcc -E "${CFLAGS[@]}" "-I$tree" "$tree/$tu" > "$pp" 2>"$pp.err"; then
        echo "  FAIL $label ($want): preprocessing failed"; sed 's/^/        /' "$pp.err" | head -5
        fail=1; return
    fi
    # Match the CALL, not the declaration. The preprocessed TU also contains
    # v9's prototype, so grepping the bare function name reports "the v9
    # branch compiled" even when the call has been deleted outright — a second
    # false negative this probe had, found by case P4 of
    # v9-probe-bypass.log. The first argument is what makes it the call site.
    local has_ex=0
    grep -q 'pgwt_summary_note_open_ex *( *d->summary_writer' "$pp" && has_ex=1
    # DORMANCY. map_reader.c no longer has a v9 branch: the open-CPU
    # observation was reverted because all three open-CPU counters read zero
    # on a live capture, i.e. the writer was never asked, and a producer
    # whose only evidence is "it compiles" is not a wired producer. So the
    # assertion here is the inverse of what it was — the call must be absent
    # under BOTH writer versions. Re-adding it is a deliberate act that has
    # to come with a live measurement, and it also brings back the
    # link-closure dependency, so this fires first and cheapest.
    if [[ "$tu" == map_reader.c && $has_ex -eq 1 ]]; then
        echo "  FAIL $label ($want): map_reader.c calls" \
             "pgwt_summary_note_open_ex, but the v9 open-CPU producer is" \
             "DORMANT by decision — wire it only with a capture showing" \
             "summary_open_cpu_credited_ns non-zero"
        fail=1; return
    fi
    # control.c has no note_open call; check its v9-only metric key instead.
    if [[ "$tu" == control.c ]]; then
        # The string LITERAL, which only control.c's own emitted code has:
        # the preprocessor strips the header comments that name the counter.
        local has_key=0
        grep -q '"summary_open_cpu_credited_ns"' "$pp" && has_key=1
        if [[ "$want" == v9 && $has_key -eq 0 ]]; then
            echo "  FAIL $label ($want): the v9 metrics block was NOT compiled"
            fail=1; return
        fi
        if [[ "$want" == v8 && $has_key -eq 1 ]]; then
            echo "  FAIL $label ($want): the v9 metrics block leaked into the v8 build"
            fail=1; return
        fi
    fi
    if gcc -fsyntax-only "${CFLAGS[@]}" "-I$tree" "$tree/$tu"; then
        echo "  OK   $label ($want branch confirmed compiled)"
    else
        echo "  FAIL $label ($want)"; fail=1
    fi
}

# v8: this branch's own tree, unmodified.
cp -R src "$stub/v8tree"
echo "v8 path (this branch's own headers):"
# map_reader.c carries no #if any more; it must simply COMPILE against both
# writer versions, and must not call the dormant v9 entry point in either.
probe "src/map_reader.c" "$stub/v8tree" map_reader.c v8
probe "src/control.c"    "$stub/v8tree" control.c    v8

# v9: a whole-tree copy with ONLY summary_writer.h replaced by v9's, so every
# quoted include inside the tree (notably daemon.h -> summary_writer.h) resolves
# to the v9 header.
cp -R src "$stub/v9tree"
cp "$HDR" "$stub/v9tree/summary_writer.h"
echo "v9 path (v9's real summary_writer.h substituted inside the tree):"
probe "src/map_reader.c" "$stub/v9tree" map_reader.c v9
probe "src/control.c"    "$stub/v9tree" control.c    v9

[[ $fail -eq 0 ]] && echo "PROBE PASSED: both #if branches compile, each confirmed" \
                  || echo "PROBE FAILED"
exit $fail
