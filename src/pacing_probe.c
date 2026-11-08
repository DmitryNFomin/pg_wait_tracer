#include "pacing_probe.h"
#include <stdlib.h>
#include <string.h>
void pgwt_parse_pg_version(const char *line, int *major, int *minor)
{
    *major = 0;
    *minor = -1;
    if (!line) return;
    const char *p = strstr(line, "PostgreSQL");
    if (!p) return;
    p += strlen("PostgreSQL");
    while (*p == ')' || *p == ' ') p++;
    char *end;
    long m = strtol(p, &end, 10);
    if (end == p || m < 1 || m > 99) return;
    *major = (int)m;
    if (*end == '.' && end[1] >= '0' && end[1] <= '9') {
        long n = strtol(end + 1, &end, 10);
        if (n >= 0 && n <= 9999) *minor = (int)n;
    }
}
/* Fail safe toward visible over-counting, as required by idle_rule.h, never
 * silent under-counting. Only a confidently parsed major == 13, or major == 14
 * with a parsed minor in [0,3), attaches the bridge. An unknown PG14 minor
 * (14beta/14rc/14devel), or a failed parse leaving major 0, is not applicable:
 * the bridge is NOT attached, so the checkpointer's 100 ms pg_usleep is COUNTED
 * as DB Time. No parse failure can make this predicate exclude time that the
 * native path would count. */
int pgwt_needs_ckpt_pacing_probe(int major, int minor)
{
    return major == 13 || (major == 14 && minor >= 0 && minor < 3);
}
enum pgwt_pacing_verdict pgwt_pacing_probe_verdict(uint64_t fires,
    uint64_t observations, unsigned threshold)
{
    return !fires && observations && observations >= threshold
        ? PGWT_PACING_INERT : PGWT_PACING_OK;
}
