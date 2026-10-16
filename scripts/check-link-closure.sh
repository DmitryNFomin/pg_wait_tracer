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
# Runs in `make check`: seconds, Mac-side, no box. The break it catches cost
# two ephemeral VMs and two `box-check` exits before anyone saw it.
#
# Usage: check-link-closure.sh [base-ref]        (default: merge-base with
#                                                origin/master, then
#                                                origin/main, then master)
# Env:   TU      translation unit to check (default src/map_reader.c)
#        ANCHOR  a symbol the TU must DEFINE, as the positive control
#        ROOT    repo root (the self-test points this at a synthetic repo)
# Exit: 0 clean, 1 the closure grew, 2 CANNOT VERIFY (refuses — never passes
#       on an input it could not resolve).
set -uo pipefail
cd "${ROOT:-$(dirname "$0")/..}" || exit 2

TU=${TU:-src/map_reader.c}
CC_BIN=${CC:-gcc}
command -v "$CC_BIN" >/dev/null || {
    echo "CANNOT VERIFY: compiler '$CC_BIN' not found" >&2; exit 2; }
command -v nm >/dev/null || {
    echo "CANNOT VERIFY: nm not found" >&2; exit 2; }

base=${1:-}
if [[ -z "$base" ]]; then
    # In order, and a miss is a refusal rather than a guess: a closure check
    # with no baseline approves everything.
    for ref in origin/master origin/main master main; do
        base=$(git merge-base HEAD "$ref" 2>/dev/null) && [[ -n "$base" ]] && break
        base=
    done
fi
if [[ -z "$base" ]] || ! git rev-parse --verify --quiet "$base^{commit}" >/dev/null 2>&1; then
    echo "CANNOT VERIFY: cannot resolve a base commit (tried '${1:-merge-base with origin/master, origin/main, master, main}')" >&2
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

# POSITIVE CONTROL. Two empty symbol sets compare equal, so a TU that was
# compiled away to nothing — wrong file, or its whole body behind an #ifdef
# that happens to be off — would read as "clean" forever. The check is not a
# threshold on how many symbols look plausible (that number is a guess, and it
# refused legitimately small translation units); it is that BOTH objects
# actually contain this TU, proven by its own anchor symbol being DEFINED in
# each. If the anchor is absent, the object is not what it claims and the
# comparison means nothing.
anchor=${ANCHOR:-_pgwt_read_state_map}
has_anchor() {   # has_anchor <object>
    nm "$1" 2>/dev/null | grep -qE " [TtSs] (${anchor}|${anchor#_})$"
}
for which in base head; do
    if ! has_anchor "$work/$which.o"; then
        echo "CANNOT VERIFY: $TU's anchor symbol ${anchor} is not defined in" >&2
        echo "  the $which object — that object is not the translation unit" >&2
        echo "  it claims, so comparing symbol sets proves nothing" >&2
        exit 2
    fi
done
nb=$(wc -l < "$work/base.txt" | tr -d ' '); nh=$(wc -l < "$work/head.txt" | tr -d ' ')
echo "base references $nb symbols, head $nh; anchor ${anchor} present in both"

added=$(comm -13 "$work/base.txt" "$work/head.txt")
if [[ -z "$added" ]]; then
    echo "OK: $(basename "$TU") references no symbol it did not reference at base"
    exit 0
fi

# ── A new symbol is not automatically a break ────────────────────────────
# The question the guard actually answers is "will every target that links
# this TU's object still link?". A dependency on an object those targets
# ALREADY link is fine — summary_writer.o has been a mandatory companion of
# map_reader.o since #379, for example. So a new symbol may be declared in
# scripts/link-closure-allow.txt together with the object that provides it,
# and the declaration is then CHECKED against tests/Makefile rather than
# believed. The base cannot be moved to silence this, and the allowlist
# cannot silence it either: the Makefile has to agree.
ALLOW=${ALLOW:-scripts/link-closure-allow.txt}
obj="$(basename "${TU%.c}").o"
mk=${LINK_MAKEFILE:-tests/Makefile}
# The Makefile is needed only to VERIFY an allowlist entry. An undeclared
# symbol fails whatever it says, so requiring it up front converted a plain
# failure into a refusal — caught by this guard's own self-test, which has no
# Makefile at that point.

# Logical Makefile lines: continuations joined, recipe attached, so a target
# split over four lines is one record. target|prereqs|recipe
mk_records() {
    awk '
        { line = $0
          while (sub(/\\$/, "", line) && (getline nxt) > 0) line = line " " nxt
          if (line ~ /^[A-Za-z0-9_.\/$(){}%-]+:[^=]/) {
              if (target != "") print target "|" prereqs "|" recipe
              split(line, a, ":"); target = a[1]
              prereqs = substr(line, index(line, ":") + 1); recipe = ""
          } else if (line ~ /^\t/ && target != "") {
              recipe = recipe " " line
          } else if (line ~ /^[[:space:]]*$/ && target != "") {
              print target "|" prereqs "|" recipe; target = ""; prereqs = ""; recipe = ""
          }
        }
        END { if (target != "") print target "|" prereqs "|" recipe }
    ' "$1"
}

# Targets that consume this TU: by OBJECT (they must link the provider) or by
# SOURCE (they compile the TU themselves; -DPGWT_SERVER excludes the daemon
# half, so those are only at risk without it).
records=""
targets_scanned=0
scan_targets() {            # lazily, and only when an entry must be checked
    [[ $targets_scanned -eq 1 ]] && return 0
    if [[ ! -f "$mk" ]]; then
        echo "CANNOT VERIFY: $mk not found, so an allowlist entry cannot be" >&2
        echo "  checked against the targets that consume $obj" >&2
        exit 2
    fi
    records=$(mk_records "$mk")
    scan_targets_body
    targets_scanned=1
}

scan_targets_body() {
# TOKEN comparison, not a regex search. `map_reader.o` as a regex matches
# `map_reader_core.o` (the dot matches the underscore), and map_reader_core.o
# is the -DPGWT_SERVER build — the BPF-free half, which contains none of this
# code. Matching it dragged two innocent targets into the failure the first
# time this ran. Prerequisites are whitespace-separated, so compare whole
# tokens.
has_token() {   # has_token <field> <token>
    local f=" $1 " t=" $2 "
    [[ "${f//$'\t'/ }" == *"$t"* ]]
}
obj_targets=""
src_targets=""
while IFS='|' read -r t pre rec; do
    [[ -z "$t" ]] && continue
    if has_token "$pre" "\$(BUILD)/$obj"; then
        obj_targets="$obj_targets $t"
    elif has_token "$pre" "../src/$(basename "$TU")" && \
         [[ "$rec$pre" != *PGWT_SERVER* ]]; then
        # Compiles the TU itself, daemon half included.
        src_targets="$src_targets $t"
    fi
done <<<"$records"
if [[ -z "$obj_targets" && -z "$src_targets" ]]; then
    echo "CANNOT VERIFY: no target in $mk consumes $obj or $(basename "$TU")," >&2
    echo "  so 'the provider is already linked everywhere' cannot be checked" >&2
    exit 2
fi
}

allow_provider() {   # allow_provider <symbol> -> provider or empty
    local sym=$1 bare=${1#_}
    [[ -f "$ALLOW" ]] || return 0
    awk -v s="$sym" -v b="$bare" '
        /^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
        { gsub(/#.*/, ""); if (NF < 2) next
          n = $1; sub(/^_/, "", n)
          if ($1 == s || $1 == b || n == b) { print $2; exit } }
    ' "$ALLOW"
}

fail=0
accepted=0
while read -r sym; do
    [[ -z "$sym" ]] && continue
    case "$sym" in ___*|_memcpy|_memset|_memmove|_strlen|_snprintf*|___snprintf*|_malloc|_free|_realloc|_calloc)
        # libc/compiler builtins: every link has them.
        continue ;;
    esac
    provider=$(allow_provider "$sym")
    if [[ -n "$provider" ]]; then
        scan_targets
    fi
    if [[ -z "$provider" ]]; then
        echo "FAIL: $(basename "$TU") gained an undeclared link-time dependency:"
        echo "    $sym"
        echo "  Every target that links $obj must supply it. Either move the"
        echo "  caller into its own translation unit (what #424 did with"
        echo "  src/open_snapshot.c), or — if the providing object is already"
        echo "  linked by every such target — declare it in $ALLOW, where the"
        echo "  claim is checked against $mk."
        fail=1
        continue
    fi
    missing=""
    while IFS='|' read -r t pre rec; do
        [[ -z "$t" ]] && continue
        case " $obj_targets " in
            *" $t "*)
                has_token "$pre" "\$(BUILD)/$provider.o" || missing="$missing $t" ;;
        esac
        case " $src_targets " in
            *" $t "*)
                # A source-compiling target may satisfy it from either the
                # object or the provider's source.
                has_token "$pre" "\$(BUILD)/$provider.o" || \
                has_token "$pre" "../src/$provider.c" || \
                missing="$missing $t" ;;
        esac
    done <<<"$records"
    if [[ -n "$missing" ]]; then
        echo "FAIL: $sym is declared in $ALLOW as provided by $provider.o,"
        echo "  but these targets consume $obj WITHOUT linking it:$missing"
        echo "  The declaration is wrong, or those targets need the object."
        fail=1
    else
        echo "ALLOWED: $sym (provided by $provider.o, which every target"
        echo "  consuming $obj already links — verified against $mk)"
        accepted=$((accepted + 1))
    fi
done <<<"$added"

if [[ $fail -ne 0 ]]; then
    exit 1
fi
echo "OK: $(basename "$TU") gained $accepted declared dependency/ies, each"
echo "  already satisfied in every target that consumes $obj"
exit 0
