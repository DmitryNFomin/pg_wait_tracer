/* Refs #871: reproduce PERF_SAMPLE_RAW framing without Linux or libbpf. */
#define _POSIX_C_SOURCE 200809L
#include "reader.h"
#include "cadence.h"
#include "cpu_accounting.h"
#include "staging.h"
#include "request.h"
#include <stdatomic.h>
#include <time.h>
#include <assert.h>
#include <stdio.h>

struct parked_test {
    struct transport_request_channel channel;
    atomic_uint wakes;
};
static void *parked_loop(void *arg)
{
    struct parked_test *t = arg;
    while (transport_request_wait(&t->channel))
        atomic_fetch_add_explicit(&t->wakes, 1, memory_order_release);
    return NULL;
}
static void test_requests(void)
{
    /* Hand-counted residence examples; semantic stamp and arrival use very
     * different values so producer age cannot pass as consumer held age. */
    struct transport_staging s = {.sources = 2};
    s.queues = calloc(s.sources, sizeof(*s.queues));
    assert(s.queues);
    assert(transport_stage(&s, 0, 1000000, 100));
    assert(transport_stage(&s, 1, 900000, 110));
    assert(s.depth == 2 && s.max_depth == 2 && s.arrived == 2);
    transport_release(&s, 900000, 120);
    assert(s.released == 0 && s.depth == 2); /* strict equality stays held */
    transport_release(&s, 900001, 130);
    assert(s.released == 1 && s.depth == 1 && s.max_held_age_ns == 20);
    assert(transport_oldest_age(&s, 140) == 40); /* oldest arrival, not lowest t */
    transport_release(&s, 1000001, 170);
    assert(s.released == 2 && s.depth == 0 && s.max_held_age_ns == 70);
    assert(transport_oldest_age(&s, 1000) == 0);
    /* Wrap, then grow a nonempty circular queue. */
    for (uint64_t i = 0; i < 250; ++i) assert(transport_stage(&s, 0, i, 200));
    transport_release(&s, 200, 210);
    for (uint64_t i = 250; i < 800; ++i) assert(transport_stage(&s, 0, i, 220));
    assert(s.depth == 600 && s.max_depth == 600);
    transport_release(&s, 800, 230);
    assert(s.depth == 0 && s.released == 802 && s.arrived == 802);
    assert(s.max_held_age_ns == 70);
    assert(s.held_ages.count == s.released);
    assert(transport_p99(&s.held_ages) == 30); /* 802 ages: 50 at 30, one at 70 */
    transport_staging_free(&s);

    struct transport_samples samples = {0};
    assert(transport_p99(&samples) == 0);
    /* 1..100 in descending order: rank 99, not the maximum or an average. */
    for (uint64_t i = 100; i; --i) assert(transport_sample(&samples, i));
    assert(transport_p99(&samples) == 99);
    assert(transport_sample(&samples, 101));
    assert(transport_p99(&samples) == 100); /* ceil(.99 * 101) == 100 */
    free(samples.values);

    const uint64_t ms = 1000000;
    assert(!transport_request_due(false, 10, 10, 1, 49*ms, 50*ms, 100*ms, 0));
    assert(transport_request_due(false, 10, 10, 1, 50*ms, 50*ms, 100*ms, 0));
    assert(!transport_request_due(true, 10, 10, 1, 99*ms, 50*ms, 100*ms, 0));
    assert(!transport_request_due(false, 11, 10, 1, 99*ms, 50*ms, 100*ms, 0));
    assert(transport_request_due(false, 0, 0, 1, 50*ms, 50*ms, 100*ms, 0));
    assert(!transport_request_due(false, 0, 0, 0, 0, 50*ms, 100*ms, 0));
    assert(!transport_request_due(false, 51*ms, 0, 0, 0, 50*ms, 100*ms, 50*ms));
    assert(transport_request_due(false, 50*ms, 0, 0, 0, 50*ms, 100*ms, 50*ms));
    assert(!transport_request_due(true, 50*ms, 0, 0, 0, 50*ms, 100*ms, 50*ms));
    assert(!transport_request_due(false, 11, 10, 1, 1, 50*ms, 100*ms, 50*ms));
    assert(transport_request_due(false, 10, 10, 1, 0, 0, 100*ms, 0));

    struct parked_test t = {0};
    atomic_init(&t.wakes, 0);
    assert(transport_request_init(&t.channel) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, parked_loop, &t) == 0);
    struct timespec pause = {.tv_nsec = 25000000};
    nanosleep(&pause, NULL);
    assert(atomic_load_explicit(&t.wakes, memory_order_acquire) == 0);
    /* A spurious wake is not a request. */
    pthread_mutex_lock(&t.channel.mutex);
    pthread_cond_signal(&t.channel.condition);
    pthread_mutex_unlock(&t.channel.mutex);
    nanosleep(&pause, NULL);
    assert(atomic_load_explicit(&t.wakes, memory_order_acquire) == 0);
    transport_request_signal(&t.channel);
    for (int i = 0; i < 1000 && !atomic_load_explicit(&t.wakes, memory_order_acquire); ++i) {
        struct timespec tick = {.tv_nsec = 1000000};
        nanosleep(&tick, NULL);
    }
    assert(atomic_load_explicit(&t.wakes, memory_order_acquire) == 1);
    nanosleep(&pause, NULL);
    assert(atomic_load_explicit(&t.wakes, memory_order_acquire) == 1);
    transport_request_stop(&t.channel);
    assert(pthread_join(thread, NULL) == 0);
    assert(atomic_load_explicit(&t.wakes, memory_order_acquire) == 1);
    transport_request_destroy(&t.channel);
}
static void test_schedstat(void)
{
    const int selected[] = {0, 3};
    uint64_t running[4] = {0};
    for (int version = 14; version <= 18; ++version) {
        FILE *f = tmpfile();
        assert(f);
        fprintf(f, "version %d\ntimestamp 1234\n"
                   "cpu0 1 2 3 4 5 6 7000000001 8 9\n"
                   "domain0 0 999999999999\n"
                   "cpu3 11 12 13 14 15 16 17000000001 18 19\n", version);
        rewind(f);
        assert(transport_schedstat(f, selected, 2, 4, running) == (version >= 15 && version <= 17));
        assert(running[0] == 7000000001 && running[3] == 17000000001);
        fclose(f);
    }
    const char *bad[] = {
        "version 15\ncpu0 0 0 0 0 0 0 1 0 1\n", /* CPU3 absent */
        "version 15\ncpu0 0 0 0 0 0 0 0 0 0\ncpu3 0 0 0 0 0 0 0 0 0\n", /* disabled */
        "version 15\ncpu0 0 0 0 0 0 0 bad 0 0\ncpu3 0 0 0 0 0 0 1 0 1\n",
        "cpu0 0 0 0 0 0 0 1 0 1\ncpu3 0 0 0 0 0 0 1 0 1\n" /* version unknown */
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        FILE *f = tmpfile();
        assert(f);
        fputs(bad[i], f); rewind(f);
        assert(!transport_schedstat(f, selected, 2, 4, running));
        fclose(f);
    }
}

int main(void)
{
    test_requests();
    test_schedstat();
    /* perf header is 8 bytes; raw length is 4; data starts at offset 12,
     * not 16. Nonzero framing padding is allowed, unlike record.pad. */
    _Alignas(8) unsigned char event[88] = {0};
    unsigned char *sample = event + 8;
    struct transport_record record = { .ts = 123, .cpu = 2, .tid = 456, .seq = 1 };
    uint32_t size = 68;
    memcpy(sample, &size, sizeof(size));
    memcpy(sample + sizeof(size), &record, sizeof(record));
    memset(sample + sizeof(size) + sizeof(record), 0xa5, 4);
    struct transport_record decoded;
    assert(transport_decode_perf(&decoded, sample, 72));
    assert(memcmp(&record, &decoded, sizeof(record)) == 0);
    assert(decoded.cpu == 2 && transport_record_valid(&decoded, 456));
    for (size_t n = 0; n < 80; ++n)
        if (n != 72) assert(!transport_decode_perf(&decoded, sample, n));
    for (uint32_t n = 0; n < 80; ++n) {
        memcpy(sample, &n, sizeof(n));
        assert(transport_decode_perf(&decoded, sample, 72) == (n == 68));
    }
    assert(!transport_record_valid(&record, 0));
    assert(!transport_record_valid(&record, 457));
    decoded = record; decoded.ts = 0;
    assert(!transport_record_valid(&decoded, 456));
    decoded = record; decoded.seq = 0;
    assert(!transport_record_valid(&decoded, 456));
    decoded = record; decoded.tid = 0;
    assert(!transport_record_valid(&decoded, 456));
    for (size_t i = 0; i < sizeof(record.pad); ++i) {
        decoded = record; decoded.pad[i] = 1;
        assert(!transport_record_valid(&decoded, 456));
    }
    /* Hand-computable per-site accounting: attempts 1..6, delivered 2,4.
     * Initial gap 1, internal gap 3, final gaps 5,6 -> four missing records.
     * A final failed helper requires no later LOST callback. */
    struct transport_cpu_result cpu = { .boundary_seen_ns = 100 };
    struct transport_record data = { .ts = 10, .seq = 2, .tid = 456, .site = TRANSPORT_DATA };
    transport_observe(&cpu, &data, 110);
    data.ts = 12; data.seq = 4;
    transport_observe(&cpu, &data, 120);
    struct transport_counts counts = { .attempts = 6, .successes = 2, .failures = 4, .eagain = 4 };
    uint64_t loss = 0, gaps = 0;
    assert(transport_reconcile(&cpu.site[TRANSPORT_DATA], &counts, &loss, &gaps));
    assert(loss == 4 && gaps == 4);
    assert(transport_reconcile(&cpu.site[TRANSPORT_DATA], &counts, &loss, &gaps));
    assert(loss == 4 && gaps == 4); /* finalization is idempotent */
    counts.attempts = 3;
    assert(!transport_reconcile(&cpu.site[TRANSPORT_DATA], &counts, &loss, &gaps));
    counts.attempts = 6; counts.successes = 3;
    assert(!transport_reconcile(&cpu.site[TRANSPORT_DATA], &counts, &loss, &gaps));
    counts.successes = 2; counts.other = 1;
    assert(!transport_reconcile(&cpu.site[TRANSPORT_DATA], &counts, &loss, &gaps));
    counts.other = 0;
    data.ts = 13; data.seq = 3;
    transport_observe(&cpu, &data, 130);
    assert(!transport_reconcile(&cpu.site[TRANSPORT_DATA], &counts, &loss, &gaps));
    assert(cpu.site[TRANSPORT_DATA].reorders == 1);

    cpu = (struct transport_cpu_result){ .boundary_seen_ns = 100 };
    struct transport_record boundary = { .ts = 20, .seq = 1, .site = TRANSPORT_BOUNDARY };
    assert(transport_record_valid(&boundary, 0)); /* an idle CPU has no DATA tid */
    boundary.tid = 456;
    assert(!transport_record_valid(&boundary, 456));
    boundary.tid = 0;
    transport_observe(&cpu, &boundary, 150);
    data.ts = 20; data.seq = 1;
    transport_observe(&cpu, &data, 160); /* strict boundary: equality allowed */
    assert(cpu.seal_violations == 0 && cpu.regressions == 0);
    boundary.ts = 19; boundary.seq = 2;
    transport_observe(&cpu, &boundary, 200); /* weaker promise cannot retract */
    assert(cpu.boundary == 20 && cpu.regressions == 1);
    data.ts = 19; data.seq = 2;
    transport_observe(&cpu, &data, 210);
    assert(cpu.seal_violations == 1 && cpu.regressions == 2);
    assert(cpu.site[TRANSPORT_BOUNDARY].gaps == 0);
    assert(cpu.site[TRANSPORT_DATA].gaps == 0); /* sites have independent seqs */
    boundary.ts = 30; boundary.seq = 3;
    transport_observe(&cpu, &boundary, 300);
    transport_boundary_gap(&cpu, 450);
    assert(cpu.max_boundary_gap_ns == 150); /* includes trailing silence */
    assert(cpu.max_boundary_stamp_gap_ns == 10);
    assert(cpu.site[TRANSPORT_BOUNDARY].delivered == 3);
    counts = (struct transport_counts){ .attempts = 1, .failures = 1, .enospc = 1 };
    struct transport_site_result silent = {0};
    assert(transport_reconcile(&silent, &counts, &loss, &gaps));
    assert(loss == 1 && gaps == 1); /* no delivered record, no LOST callback */
    decoded = record; decoded.site = TRANSPORT_SITES;
    assert(!transport_record_valid(&decoded, 456));
    cpu = (struct transport_cpu_result){ .boundary_seen_ns = 100 };
    boundary.ts = 20; boundary.seq = 1;
    transport_observe(&cpu, &boundary, 110);
    boundary.seq = 2; /* repeated stamp is not boundary advancement */
    transport_observe(&cpu, &boundary, 150);
    transport_boundary_gap(&cpu, 220);
    assert(cpu.boundary_seen_ns == 110 && cpu.max_boundary_gap_ns == 110);
    /* DATA must keep the qualified frontier alive while the old explicit
     * boundary diagnostic still shows 150 ns silence. Unqualified ring DATA
     * supplies no implicit boundary. */
    for (int qualified = 0; qualified < 2; ++qualified) {
        cpu = (struct transport_cpu_result){ .boundary_seen_ns = 100,
            .measurement_start_ns = 100, .frontier_seen_ns = 100, .qualified_perf = qualified };
        for (uint64_t i = 1; i <= 3; ++i) {
            data.ts = 100 + i * 40; data.seq = i;
            transport_observe(&cpu, &data, 100 + i * 50);
        }
        transport_boundary_gap(&cpu, 250);
        transport_frontier_end(&cpu, 260);
        assert(cpu.max_boundary_gap_ns == 150);
        assert(cpu.max_frontier_gap_ns == (qualified ? 50 : 160));
        assert(cpu.max_frontier_stamp_gap_ns == (qualified ? 40 : 160));
        assert(cpu.frontier_ns == (qualified ? 220 : 0));
        assert(cpu.frontier_seen_ns == (qualified ? 250 : 100));
    }
    /* An equal stamp, weaker stamp, duplicate sequence or seal violation
     * cannot refresh the consumer's frontier-arrival timestamp. */
    data.ts = 220; data.seq = 4;
    transport_observe(&cpu, &data, 280);
    assert(cpu.frontier_seen_ns == 250);
    data.ts = 219; data.seq = 5;
    transport_observe(&cpu, &data, 290);
    assert(cpu.frontier_seen_ns == 250);
    data.ts = 230; data.seq = 5;
    transport_observe(&cpu, &data, 300);
    assert(cpu.frontier_seen_ns == 250);
    cpu.boundary = 250;
    data.ts = 240; data.seq = 6;
    transport_observe(&cpu, &data, 310);
    assert(cpu.frontier_seen_ns == 250);
    boundary.ts = 300; boundary.seq = 1;
    transport_observe(&cpu, &boundary, 320);
    assert(cpu.frontier_seen_ns == 320 && cpu.frontier_ns == 300);
    transport_frontier_end(&cpu, 400);
    assert(cpu.max_frontier_gap_ns == 80 && cpu.max_frontier_stamp_gap_ns == 100);

    assert(transport_first_deadline(100, 0) == 100);
    assert(transport_first_deadline(100, 999999) == 1000099);
    assert(transport_next_deadline(100, 100, 200, 10) == 110);
    assert(transport_next_deadline(100, 135, 200, 10) == 140);
    assert(transport_next_deadline(100, 250, 200, 10) == 200);
    assert(transport_next_deadline(195, 195, 200, 10) == 200);
    FILE *stat = tmpfile();
    assert(stat);
    fputs("cpu 11 22 33 44 55 66 77 88 999 999\n"
          "cpu0 1 2 3 4 5 6 7 8 999 999\n"
          "cpu1 100 100 100 100 100 100 100 100\n"
          "cpu3 10 20 30 40 50 60 70 80\n"
          "intr 999 999 999 999 999 999 999 999\n", stat);
    rewind(stat);
    struct cpu_snapshot snapshots[5] = {0};
    int selected[] = {0, 3};
    uint64_t busy = 0;
    assert(transport_cpu_ticks(stat, selected, 2, 4, snapshots, &busy));
    assert(busy == 297); /* excludes idle, iowait, guest and unselected CPU1 */
    for (int i = 0; i < CPU_FIELDS; ++i) {
        assert(snapshots[0].ticks[i] == (uint64_t)(i + 1));
        assert(snapshots[3].ticks[i] == (uint64_t)(i + 1) * 10);
        assert(snapshots[4].ticks[i] == (uint64_t)(i + 1) * 11);
    }
    rewind(stat);
    selected[1] = 2; /* a missing participating CPU refuses the snapshot */
    assert(!transport_cpu_ticks(stat, selected, 2, 4, snapshots, &busy));
    fclose(stat);
    stat = tmpfile();
    assert(stat);
    fputs("cpu0 1 2 3 4 5 6 7 8\n", stat);
    rewind(stat);
    assert(!transport_cpu_ticks(stat, selected, 1, 4, snapshots, &busy)); /* missing aggregate */
    fclose(stat);
    stat = tmpfile();
    assert(stat);
    transport_report_cpu_ticks(stat, &snapshots[3], &snapshots[0]);
    rewind(stat);
    char json[1024] = {0};
    assert(fread(json, 1, sizeof(json) - 1, stat) > 0);
    assert(strcmp(json,
        "{\"start\":{\"user\":10,\"nice\":20,\"system\":30,\"idle\":40,\"iowait\":50,\"irq\":60,\"softirq\":70,\"steal\":80},"
        "\"end\":{\"user\":1,\"nice\":2,\"system\":3,\"idle\":4,\"iowait\":5,\"irq\":6,\"softirq\":7,\"steal\":8},"
        "\"delta\":{\"user\":-9,\"nice\":-18,\"system\":-27,\"idle\":-36,\"iowait\":-45,\"irq\":-54,\"softirq\":-63,\"steal\":-72}}") == 0);
    fclose(stat);
    puts("nmi transport reader: PASS (framing, fields, per-site/trailing loss, seals, regressions, boundary/frontier gaps, phase, deadlines, CPU ticks, schedstat, strict staging, parked/coalesced requests)");
    return 0;
}
