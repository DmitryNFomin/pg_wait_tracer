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
