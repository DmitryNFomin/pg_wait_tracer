#!/bin/sh
# Compile AND link against the selected libbpf; never run the probe or silently
# substitute drain return values. These reservation-position APIs require 1.3+.
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
cat > "$work/probe.c" <<'EOF'
#include <bpf/libbpf.h>
/* Volatile pointers retain symbol relocations even with optimization/LTO. */
static struct ring *(*volatile get_ring)(struct ring_buffer *, unsigned int) = ring_buffer__ring;
static unsigned long (*volatile producer)(const struct ring *) = ring__producer_pos;
static unsigned long (*volatile consumer)(const struct ring *) = ring__consumer_pos;
int main(void)
{
    struct ring *r = get_ring((struct ring_buffer *)0, 0);
    return producer(r) != consumer(r);
}
EOF
# Flags come from the Makefile's already selected system or bundled toolchain.
# The generated program is only linked. It performs no BPF operation on a host.
if ! ${CC:-cc} ${CPPFLAGS:-} ${CFLAGS:-} -Werror=implicit-function-declaration \
        "$work/probe.c" ${LIBBPF_LIB:--lbpf} -o "$work/probe"; then
    echo 'ERROR: coverage fences require libbpf >= 1.3 with ring_buffer__ring, ring__producer_pos and ring__consumer_pos (headers AND library).' >&2
    exit 1
fi
printf '%s\n' 'OK: libbpf reservation-position APIs compile and link'
