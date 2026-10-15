/* output.h — CLI output formatters for 4 diagnostic views */
#ifndef PGWT_OUTPUT_H
#define PGWT_OUTPUT_H

#include <stdint.h>

struct pgwt_daemon;

/* Should this row be suppressed entirely? (#424)
 *
 * Every table in output.c used to skip a row with `count == 0`, reading zero
 * occurrences as zero information. Once the window selector became OVERLAP
 * that stopped being true: an event whose in-window time is entirely
 * RIGHT-OPEN — one wait still running at `to`, counted later in the window
 * where it closes — legitimately has milliseconds and no completed
 * occurrence. Suppressing it put that time in DB Time and the Time Model
 * with no row to explain it: the total moves and the breakdown cannot
 * account for it, which is the silent-inconsistency class this change set
 * exists to remove, arriving as a side effect of the repair.
 *
 * So: empty in BOTH senses, or it is shown. Shared by all seven call sites
 * (and gated at tests/test_window_overlap.c §12) so they cannot drift back
 * to the count-only test one at a time.
 *
 * The same row shape reaches the web UI from pgwt_compute_top_events, where
 * the server already emits it — src/server.c's handle_top_events writes
 * every row of the result with no count filter of its own. */
static inline int pgwt_row_is_empty(uint64_t count, uint64_t total_ns)
{
    return count == 0 && total_ns == 0;
}

void pgwt_print_header(struct pgwt_daemon *d);
void pgwt_print_time_model(struct pgwt_daemon *d);
void pgwt_print_system_event(struct pgwt_daemon *d);
void pgwt_print_session_event(struct pgwt_daemon *d);
void pgwt_print_histogram(struct pgwt_daemon *d);
void pgwt_print_query_event(struct pgwt_daemon *d);
void pgwt_print_active(struct pgwt_daemon *d);

#endif /* PGWT_OUTPUT_H */
