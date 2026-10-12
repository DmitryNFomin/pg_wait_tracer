The 13-record discrepancy is entirely a harness category-input difference.

The independent harness (`../scratchpad/harness.c`) decodes current.trace and
calls pgwt_tag_events(B, n, NULL, 0). The server calls pgwt_tag_events with
103 PID categories loaded from backends.jsonl. The audit decodes the same
file and changes only that argument:

- NULL categories: 2,101,977 visible transitions.
- Server categories: 2,101,990 visible transitions.
- Actual server loader: 2,101,990 visible transitions.

Exactly 13 records differ in admission. Every one belongs to a distinct
autovac_worker (metadata in discrepancy-backends.jsonl). NULL categories treat
these workers as foreground and relabel their CPU to hidden NONCMD_CPU
(0x05ffffff). Real categories set MAINT (0x8), so their CPU remains visible.
Twelve close to event 0x09000007; one closes to 0x0a000011. None closes to
ClientRead, explaining the identical CPU->ClientRead count of 275,072.

Both the reader and actual loader contain 4,307,056 records. Stable sorting
(the decoded file has 1,070 adjacent timestamp inversions) preserves both
category variants' totals exactly. Thus neither window selection, phantom
EXIT dropping, marker counting, nor ordering explains any of the delta.
All 13 are listed in discrepancy-audit.log; zero remain unexplained.

Reproduce from the repository root on Linux:

    make pgwt-server
    cc -g -O2 -Wall -Isrc -DPGWT_SERVER -include assert.h \
      -o /tmp/cx340-audit tests/results/cx340-safety/discrepancy-audit.c \
      build/server_{compute,block_agg,event_reader,event_writer,summary_writer,summary_reader,wait_event,idle_rule,spawn,cmdline,backend_meta,cJSON}.o \
      -lz -llz4 -lm
    /tmp/cx340-audit /path/to/demo_rehearsal/trace

The stored audit and benchmark ran on macOS arm64 using the prior temporary
POSIX compilation shims; the full product build is gated on Linux.
