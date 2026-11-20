#!/bin/sh
# Print "yes" only when the system libbpf has USDT AND reservation positions.
# Older systems (including USDT-capable libbpf before 1.3) use pinned v1.4.7.
# This is a compile/link probe only: it never runs BPF on the build host.
# USDT_HEADER, CPPFLAGS, CFLAGS and LIBBPF_LIB can select a test toolchain.
set -eu

HDR="${USDT_HEADER:-/usr/include/bpf/usdt.bpf.h}"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

cat > "$tmpdir/probe.c" <<'C'
#include <bpf/libbpf.h>
/* Volatile pointers keep every symbol reference, even with optimization. */
static __typeof__(&bpf_program__attach_usdt) volatile usdt = bpf_program__attach_usdt;
static struct ring *(*volatile get_ring)(struct ring_buffer *, unsigned int) = ring_buffer__ring;
static unsigned long (*volatile producer)(const struct ring *) = ring__producer_pos;
static unsigned long (*volatile consumer)(const struct ring *) = ring__consumer_pos;
int main(void)
{
    return !usdt || !get_ring || !producer || !consumer;
}
C

if [ -r "$HDR" ] && ${CC:-gcc} ${CPPFLAGS:-} ${CFLAGS:-} \
        -Werror=implicit-function-declaration "$tmpdir/probe.c" \
        ${LIBBPF_LIB:--lbpf} -o "$tmpdir/probe.out" >/dev/null 2>&1; then
    echo yes
else
    echo no
fi
