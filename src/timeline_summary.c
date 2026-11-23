/* timeline_summary.c — S0 stub of the timeline summary (#1006).
 *
 * The prototypes in timeline_summary.h are frozen; S2 replaces these bodies.
 * Every entry point refuses with -ENOSYS (or NULL) so nothing can mistake
 * the stub for an empty summary.
 */
#include "timeline_summary.h"

#include <errno.h>
#include <string.h>

struct pgwt_tl_summary *pgwt_tl_summary_create(void)
{
    errno = ENOSYS;
    return NULL;
}

void pgwt_tl_summary_destroy(struct pgwt_tl_summary *s)
{
    (void)s;
}

int pgwt_tl_summary_add(struct pgwt_tl_summary *s, const struct pgwt_tl_record *rec)
{
    (void)s; (void)rec;
    return -ENOSYS;
}

void pgwt_tl_summary_set_capture(struct pgwt_tl_summary *s,
                                 uint64_t lost_raw_total, int truncated)
{
    (void)s; (void)lost_raw_total; (void)truncated;
}

int pgwt_tl_summary_from_dir(struct pgwt_tl_summary *s, const char *dir)
{
    (void)s; (void)dir;
    return -ENOSYS;
}

size_t pgwt_tl_summary_events(const struct pgwt_tl_summary *s,
                              const struct pgwt_tl_event_row **rows)
{
    (void)s;
    if (rows)
        *rows = NULL;
    return 0;
}

size_t pgwt_tl_summary_queries(const struct pgwt_tl_summary *s,
                               const struct pgwt_tl_query_row **rows)
{
    (void)s;
    if (rows)
        *rows = NULL;
    return 0;
}

void pgwt_tl_summary_get_totals(const struct pgwt_tl_summary *s,
                                struct pgwt_tl_summary_totals *out)
{
    (void)s;
    if (out)
        memset(out, 0, sizeof(*out));
}

int pgwt_tl_summary_equal(const struct pgwt_tl_summary *a,
                          const struct pgwt_tl_summary *b,
                          char *why, size_t whysz)
{
    (void)a; (void)b;
    if (why && whysz)
        snprintf(why, whysz, "%s", "not implemented (S0 stub)");
    return 0;
}

int pgwt_tl_summary_write_json(const struct pgwt_tl_summary *s,
                               const char *path, gid_t trace_gid)
{
    (void)s; (void)path; (void)trace_gid;
    return -ENOSYS;
}

int pgwt_tl_summary_print_text(const struct pgwt_tl_summary *s, FILE *out)
{
    (void)s; (void)out;
    return -ENOSYS;
}
