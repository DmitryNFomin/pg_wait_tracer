/* Portable /proc/stat decoding and JSON evidence. Refs #871. */
#ifndef NMI_TRANSPORT_CPU_ACCOUNTING_H
#define NMI_TRANSPORT_CPU_ACCOUNTING_H
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { CPU_FIELDS = 8 };
struct cpu_snapshot { uint64_t ticks[CPU_FIELDS]; };

/* Index possible is the whole-box aggregate; selected CPU rows remain at
 * their physical CPU IDs. Guest/guest_nice are already in user/nice. */
static inline bool transport_cpu_ticks(FILE *f, const int *cpu_ids, int available,
                                       int possible, struct cpu_snapshot *snapshot,
                                       uint64_t *total)
{
    char line[1024];
    int found = 0;
    *total = 0;
    while (fgets(line, sizeof(line), f)) {
        char label[32];
        unsigned long long v[CPU_FIELDS];
        if (sscanf(line, "%31s %llu %llu %llu %llu %llu %llu %llu %llu", label,
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) != 9)
            continue;
        int cpu = -1, used = 0;
        if (!strcmp(label, "cpu")) cpu = possible;
        else if (sscanf(label, "cpu%d%n", &cpu, &used) != 1 || label[used] ||
                 cpu < 0 || cpu >= possible) continue;
        for (int field = 0; field < CPU_FIELDS; ++field) snapshot[cpu].ticks[field] = v[field];
        if (cpu == possible) { ++found; continue; }
        for (int i = 0; i < available; ++i) if (cpu == cpu_ids[i]) {
            *total += v[0] + v[1] + v[2] + v[5] + v[6] + v[7];
            ++found;
        }
    }
    return !ferror(f) && found == available + 1;
}

static inline void transport_report_cpu_ticks(FILE *out, const struct cpu_snapshot *start, const struct cpu_snapshot *end)
{
    const char *names[CPU_FIELDS] = {"user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal"};
    const char *phases[] = {"start", "end", "delta"};
    fputc('{', out);
    for (int phase = 0; phase < 3; ++phase) {
        fprintf(out, "%s\"%s\":{", phase ? "," : "", phases[phase]);
        for (int field = 0; field < CPU_FIELDS; ++field) {
            int64_t value = phase == 0 ? (int64_t)start->ticks[field] :
                phase == 1 ? (int64_t)end->ticks[field] :
                (int64_t)end->ticks[field] - (int64_t)start->ticks[field];
            fprintf(out, "%s\"%s\":%" PRId64, field ? "," : "", names[field], value);
        }
        fputc('}', out);
    }
    fputc('}', out);
}
#endif
