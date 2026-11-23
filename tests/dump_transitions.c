/* dump_transitions.c — export the trap reference of a full-mode trace for
 * the timeline comparator (#1006 unit 1, S5).
 *
 * Reads a `--mode full` trace directory (the trap reference of the
 * two-tracer acceptance run) and prints one line per TRANSITIONS row:
 *     <pid> <t0> <t1> <event>
 * with t0 = timestamp_ns - duration_ns, t1 = timestamp_ns (CLOCK_MONOTONIC
 * ns) and event the 0x%08x wait_event_info of the interval (0 = no wait).
 * Markers and COVERAGE blocks are skipped. tests/timeline_compare.py reads
 * this output. Modelled on dump_markers.c; server build (no BPF), linked
 * by tests/Makefile with the same objects.
 *
 * S0: stub with the frozen command line; S5 implements the export.
 */
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 2 || argv[1][0] == '-') {
        fprintf(stderr, "usage: %s <trace_dir>\n"
                "  prints '<pid> <t0_ns> <t1_ns> <0xEVENT>' per TRANSITIONS row\n",
                argv[0]);
        return 2;
    }
    fprintf(stderr, "dump_transitions: not implemented yet (#1006 S5)\n");
    return 3;
}
