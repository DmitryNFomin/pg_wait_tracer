/*
 * ticklat: how late does a normal-priority periodic sampler wake up?
 *
 * Sleeps to absolute deadlines every PERIOD_US on CLOCK_MONOTONIC, like a
 * sampler's tick loop, and records lateness = wake time - deadline for each
 * tick.  Deadlines advance by a fixed step (no drift correction), and a tick
 * that wakes after the next deadline still counts once per deadline it
 * covers, as "missed".  Prints one lateness value (ns) per line to stdout.
 *
 * Usage: ticklat PERIOD_US SECONDS > lateness.txt
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

static int64_t ns(const struct timespec *t)
{
    return (int64_t) t->tv_sec * 1000000000LL + t->tv_nsec;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s PERIOD_US SECONDS\n", argv[0]);
        return 2;
    }
    int64_t period = atoll(argv[1]) * 1000LL;
    int64_t total = atoll(argv[2]) * 1000000000LL;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t start = ns(&t), deadline = start + period, missed = 0, ticks = 0;

    while (deadline - start <= total) {
        struct timespec d = { deadline / 1000000000LL, deadline % 1000000000LL };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL) != 0)
            ;
        clock_gettime(CLOCK_MONOTONIC, &t);
        int64_t now = ns(&t);
        printf("%lld\n", (long long) (now - deadline));
        ticks++;
        deadline += period;
        while (deadline < now) {      /* deadlines this wake-up overran */
            deadline += period;
            missed++;
        }
    }
    fprintf(stderr, "ticks %lld missed %lld\n", (long long) ticks, (long long) missed);
    return 0;
}
