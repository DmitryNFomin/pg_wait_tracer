/* Live readback shares the product's CPU-group rule. No BPF needed. */
#include "cpu_group.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: cpu_group_readback PID\n");
        return 1;
    }
    char *end;
    errno = 0;
    long n = strtol(argv[1], &end, 10);
    pid_t pid = !errno && !*end && n > 0 && n <= 2147483647 ? (pid_t)n : -1;
    struct pgwt_cpu_group g = pgwt_cpu_group_read("/proc", "/sys/fs/cgroup", pid);
    if (!g.version) {
        printf("unknown\t%s\n", g.reason);
        return 1;
    }
    printf("v%d\t%s\n", g.version, g.path);
    return 0;
}
