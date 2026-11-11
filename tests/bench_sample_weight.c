/* Real-writer cost probe. Compile this source against each baseline's
 * sampler.c, idle_rule.c and event_writer.c with -DPGWT_SERVER.
 * Inputs are precomputed outside the CPU timer. No reading/decompression is
 * timed. DISRUPTIONS adds empty/failed ticks, a stall, rotation and escalation.
 * Uses the reviewer's srand(863), +/-10% jitter, 20-200us handler latency. */
#include "sampler.h"
#include "event_writer.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>

struct tick {
    uint64_t ts, weight;
    int count;
};

static double cpu_seconds(void)
{
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 +
           u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s DIR BACKENDS SECONDS DISRUPTIONS\n", argv[0]);
        return 2;
    }
    int nb = atoi(argv[2]), secs = atoi(argv[3]), disrupt = atoi(argv[4]);
    if (nb < 1 || nb > 4096 || secs < 1 || secs > 3600 ||
        (disrupt != 0 && disrupt != 1)) return 2;
    const uint64_t p = 100000000, base = 10000000000000ULL;
    int capacity = secs * 12 + 1;  /* >= maximum 10Hz +/-10% tick count */
    struct tick *ticks = calloc((size_t)capacity, sizeof(*ticks));
    struct pgwt_trace_event *samples = calloc((size_t)capacity * nb, sizeof(*samples));
    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    if (!ticks || !samples || !w || pgwt_writer_init(w, argv[1], 18, 24, NULL))
        return 2;
#if PGWT_TRACE_VERSION >= 4
    w->nominal_sample_period_ns = p;
#endif
    uint32_t evs[] = {0, 0x0A000001u, 0x05000002u, 0x03000000u, 0x0800000Bu};
    srand(863);
    uint64_t now = base, last = 0;
    int nticks = 0;
    while (now - base < (uint64_t)secs * 1000000000ULL) {
        if (nticks == capacity) return 2;
        struct tick *t = &ticks[nticks];
        t->ts = now;
        t->weight = pgwt_sampler_effective_period(p, last, now);
        last = now;
        /* The sampler advances last_tick even on idle or failed-read ticks. */
        t->count = disrupt && (nticks % 997 == 30 || nticks % 997 == 31) ? 0 : nb;
        for (int i = 0; i < nb; i++) {
            samples[(size_t)nticks * nb + i] = (struct pgwt_trace_event){
                .timestamp_ns = now, .pid = 1000 + i,
                .new_event = evs[(i + nticks) % 5 * (rand() % 3 == 0)],
                .query_id = 100 + i % 7,
            };
        }
        nticks++;
        now += p - p / 10 + (uint64_t)rand() % (p / 5 + 1) +
               20000 + (uint64_t)rand() % 180000;
        /* Short captures preserve the reviewer's ~70s/one-backend shape;
         * hour-long runs exercise the >60s clamp too. */
        if (disrupt && nticks == 100)
            now += secs < 120 ? 500000000ULL : 61000000000ULL;
    }

    int blocks = 0;
    uint64_t escalation_start = 0;
    double start = cpu_seconds();
    for (int k = 0; k < nticks; k++) {
        struct tick *t = &ticks[k];
        if (t->count && pgwt_writer_push_samples(w, samples + (size_t)k * nb,
                                                t->count, t->weight)) return 3;
        if (disrupt && (k == 50 || k == 70)) {
            uint32_t kind = k == 50 ? PGWT_MARKER_ESCALATE_START : PGWT_MARKER_ESCALATE_END;
            if (k == 50) escalation_start = t->ts;
            else {
                struct pgwt_trace_event e = {.timestamp_ns = t->ts, .pid = 1000,
                    .old_event = 0x0A000001u, .new_event = 0x0A000001u,
                    .duration_ns = t->ts - escalation_start, .query_id = 100};
                if (pgwt_writer_push_event(w, &e)) return 3;
            }
            struct pgwt_trace_event e = {.timestamp_ns = t->ts,
                                         .old_event = kind, .new_event = kind};
            if (pgwt_writer_push_event(w, &e)) return 3;
        }
        if (disrupt && k == 20) {
            if (pgwt_writer_publish(w)) return 3;
            blocks += w->num_blocks;
            w->force_rotate = true;
            if (pgwt_writer_check_rotation(w) < 0) return 4;
        }
    }
    pgwt_writer_close(w);
    blocks += w->num_blocks;
    double elapsed = cpu_seconds() - start;

    uint64_t bytes = 0;
    DIR *d = opendir(argv[1]);
    if (!d) return 4;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strstr(ent->d_name, ".trace") || strstr(ent->d_name, ".meta")) continue;
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", argv[1], ent->d_name);
        struct stat st;
        if (!stat(path, &st)) bytes += st.st_size;
    }
    closedir(d);
    printf("backends=%d seconds=%d disruptions=%d ticks=%d blocks=%d bytes=%llu cpu=%.6f\n",
           nb, secs, disrupt, nticks, blocks, (unsigned long long)bytes, elapsed);
    pgwt_writer_destroy(w);
    free(w); free(samples); free(ticks);
    return 0;
}
