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
 * silent under-counting. Only a confidently parsed major == 13 attaches the
 * bridge. PG14.0-14.2 is DEFERRED to #830 (exact PG14/15/16 Timeout tables):
 * on PG14 the tracer renders native Timeout id 1 under the same label
 * "CheckpointWriteDelay" as synthetic id 30, with opposite accounting
 * (native counted, synthetic excluded). Unattached means the checkpointer's
 * 100 ms pg_usleep is COUNTED as DB Time (over-counting), and a failed parse
 * leaving major 0 stays disabled. */
int pgwt_needs_ckpt_pacing_probe(int major, int minor)
{
    return major == 13;
}
enum pgwt_pacing_verdict pgwt_pacing_probe_verdict(uint64_t fires,
    uint64_t observations, unsigned threshold)
{
    return !fires && observations && observations >= threshold
        ? PGWT_PACING_INERT : PGWT_PACING_OK;
}
