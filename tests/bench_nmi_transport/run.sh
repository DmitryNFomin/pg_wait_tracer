#!/usr/bin/env bash
# Refs #871: opt-in measurement, never a correctness gate.
set -euo pipefail
if [[ $(uname -s) != Linux ]]; then
    echo 'NMI transport experiment requires Linux' >&2
    exit 2
fi
if [[ $# -lt 1 || $# -gt 4 ]]; then
    echo "usage: $0 OUTPUT.jsonl [REPEATS=5 [SECONDS=30 [TOTAL_BYTES=67108864]]]" >&2
    exit 2
fi
output=$1
repeats=${2:-5}
seconds=${3:-30}
bytes=${4:-67108864}
for value in "$repeats" "$seconds" "$bytes"; do
    if [[ ! $value =~ ^[1-9][0-9]*$ ]]; then
        echo 'repeat count, seconds and bytes must be positive integers' >&2
        exit 2
    fi
done
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# LIBBPF_INC/LIBBPF_LIB and CLANG can be supplied by the box runner.
make -C "$here/.." bench-nmi-transport >&2
# Respect the allowed online affinity set, including containers/taskset.
all_cpus=$(python3 -c 'import os; print(len(os.sched_getaffinity(0)))')
# Refuse accidental overwrite; stderr preserves all libbpf/verifier diagnostics.
(set -o noclobber; : > "$output")
(set -o noclobber; : > "$output.stderr")
for threads in 1 4 "$all_cpus"; do
    if (( threads > all_cpus )); then
        echo "skip threads=$threads: only $all_cpus allowed online CPUs" >> "$output.stderr"
        continue
    fi
    for ((repeat = 1; repeat <= repeats; ++repeat)); do
        for transport in shared percpu-ring perf; do
            echo "transport=$transport threads=$threads repeat=$repeat" >> "$output.stderr"
            status=0
            "$here/nmi_transport" --object "$here/transport.bpf.o" \
                --transport "$transport" --threads "$threads" \
                --duration "$seconds" --bytes "$bytes" \
                >> "$output" 2>> "$output.stderr" || status=$?
            case "$status" in
                0) ;;
                3) echo "unsupported/load failure: transport=$transport; retained JSON and diagnostics" >> "$output.stderr" ;;
                *) echo "measurement failed (exit $status); retained partial output" >&2; exit "$status" ;;
            esac
        done
    done
done
