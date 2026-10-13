/* test_offcpu_decompose.c — the Off-CPU* decomposition's own decision logic,
 * including every way it could be satisfied WITHOUT looking at anything.
 *
 * The thing under test is tests/offcpu_decompose.c's pure pair
 * (ocd_window_ns / ocd_classify): the claim that compute.c's Off-CPU*
 * residual is exactly the per-gap `clipped_dur - min(cpu_ns*frac, dur)` sum.
 * The headline case (case 7) does not assert that claim against a hand
 * computation -- it drives the REAL pgwt_compute_time_model over a synthetic
 * window and asserts the decomposition reproduces its offcpu_ms to the ns.
 * That is the only assertion that can detect the decomposition drifting away
 * from the product, which is the whole value of the tool.
 *
 * Included with -DOCD_NO_MAIN; no trace file, no BPF, runs anywhere.
 */
#include "offcpu_decompose.c"

#include <assert.h>
#include <math.h>
#include <stdio.h>

#define WEI(cls, id) (((uint32_t)(cls) << 24) | (uint32_t)(id))
#define IO_WAIT      WEI(0x0A, 0)     /* IO class: a real wait */
#define ACTIVITY     WEI(0x05, 0)     /* Activity class: idle by the rule */

static int failures;

static void chk(int cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
    else        printf("ok: %s\n", what);
}

static struct pgwt_trace_event mk(uint32_t pid, uint32_t old_ev, uint32_t new_ev,
                                  uint64_t ts, uint64_t dur, uint64_t cpu)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.pid = pid; e.old_event = old_ev; e.new_event = new_ev;
    e.timestamp_ns = ts; e.duration_ns = dur; e.cpu_ns = cpu;
    return e;
}

int main(void)
{
    double resid, dur, clamp;

    /* 1. UNKNOWN cpu_ns: the whole gap is booked as CPU*, residual 0. The
     *    reason code must still say UNKNOWN -- reporting it as a measured 0
     *    is how "no data" turns into "clean". */
    struct pgwt_trace_event e = mk(10, 0, IO_WAIT, 1000000, 1000000,
                                   PGWT_CPU_NS_UNKNOWN);
    chk(ocd_classify(&e, 0, 0, &resid, &dur, &clamp) == OCD_UNKNOWN &&
        resid == 0.0 && dur == 1000000.0, "UNKNOWN cpu_ns -> UNKNOWN, resid 0");

    /* 2. Measured 0 on a gap: the WHOLE gap lands in the residual. */
    e = mk(10, 0, IO_WAIT, 1000000, 400000, 0);
    chk(ocd_classify(&e, 0, 0, &resid, &dur, &clamp) == OCD_ZERO_CPU &&
        resid == 400000.0, "cpu_ns == 0 -> whole gap is residual");

    /* 3. Partial: the deficit, nothing more. */
    e = mk(10, 0, IO_WAIT, 1000000, 400000, 300000);
    chk(ocd_classify(&e, 0, 0, &resid, &dur, &clamp) == OCD_PARTIAL &&
        resid == 100000.0, "0 < cpu < dur -> deficit only");

    /* 4. cpu > dur is the clamp, not a negative residual. */
    e = mk(10, 0, IO_WAIT, 1000000, 400000, 500000);
    chk(ocd_classify(&e, 0, 0, &resid, &dur, &clamp) == OCD_CLAMPED &&
        resid == 0.0 && clamp == 100000.0, "cpu > dur -> clamped, resid 0");

    /* 5. The three kinds of record that are NOT gaps. A wait keeps its label
     *    (its cpu_ns is observability only); an idle record is outside DB
     *    Time; an io_worker record is outside both. If any of these leaked in,
     *    the decomposition would stop matching the product. */
    e = mk(10, IO_WAIT, 0, 1000000, 400000, 10000);
    chk(ocd_classify(&e, 0, 0, &resid, &dur, &clamp) == OCD_NOT_GAP,
        "a WAIT record is not a gap");
    e = mk(10, ACTIVITY, 0, 1000000, 400000, 0);
    chk(ocd_classify(&e, 0, 0, &resid, &dur, &clamp) == OCD_NOT_GAP,
        "an IDLE record is not a gap");
    e = mk(10, 0, IO_WAIT, 1000000, 400000, 0);
    e.flags = PGWT_EVENT_FLAG_IO_WORKER;
    chk(ocd_classify(&e, 0, 0, &resid, &dur, &clamp) == OCD_NOT_GAP,
        "an io_worker record is not a gap");

    /* 6. Zero overlap with the window. This is the false negative the first
     *    version of the tool shipped with: such a record contributes 0 ns
     *    either way, so it cannot be caught by a residual assertion -- but
     *    counted as a gap it made every window report the WHOLE capture's
     *    gap count, and an empty window print "0.000%" and exit 0. */
    e = mk(10, 0, IO_WAIT, 1000000, 400000, 0);
    chk(ocd_classify(&e, 5000000, 6000000, &resid, &dur, &clamp) == OCD_NOT_GAP
        && dur == 0.0, "record outside the window is not a gap");

    /* 7. RECONCILIATION against the real compute.c, over a window that cuts
     *    a gap in half (the straddler case ocd_window_ns exists for).
     *
     *    Window [2.0s, 4.0s). Records, all one pid:
     *      - gap  closing at 1.5s, dur 1.0s, cpu 0.2s   -> entirely BEFORE
     *      - gap  closing at 3.0s, dur 2.0s, cpu 0.5s   -> half in window
     *      - wait closing at 3.5s, dur 0.5s, cpu 0.01s  -> in window, a wait
     *      - gap  closing at 4.5s, dur 1.0s, cpu 0.25s  -> half in window
     *      - idle closing at 5.0s, dur 0.5s             -> idle, after
     *    The two straddlers' in-window halves are 1.0s and 0.5s of gap with
     *    0.25s and 0.125s of in-window CPU, so Off-CPU* must be
     *    0.75 + 0.375 = 1.125s. Asserted against tm.offcpu_ms, not against
     *    that arithmetic. */
    struct pgwt_trace_event ev[5] = {
        mk(7, 0,        IO_WAIT, 1500000000ULL, 1000000000ULL, 200000000ULL),
        mk(7, 0,        IO_WAIT, 3000000000ULL, 2000000000ULL, 500000000ULL),
        mk(7, IO_WAIT,  0,       3500000000ULL,  500000000ULL,  10000000ULL),
        mk(7, 0,        IO_WAIT, 4500000000ULL, 1000000000ULL, 250000000ULL),
        mk(7, ACTIVITY, 0,       5000000000ULL,  500000000ULL,          0ULL),
    };
    uint64_t from_ns = 2000000000ULL, to_ns = 4000000000ULL;
    struct pgwt_filter f; memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm; memset(&tm, 0, sizeof(tm));
    pgwt_compute_time_model(ev, 5, &f, from_ns, to_ns, 2000.0, &tm);

    double mine = 0.0;
    int n_gaps = 0;
    for (int i = 0; i < 5; i++) {
        enum ocd_reason r = ocd_classify(&ev[i], from_ns, to_ns,
                                         &resid, &dur, &clamp);
        if (r == OCD_NOT_GAP) continue;
        mine += resid;
        n_gaps++;
    }
    printf("   compute.c offcpu_ms=%.6f  decomposition=%.6f  gaps=%d\n",
           tm.offcpu_ms, mine / 1e6, n_gaps);
    chk(tm.has_measured_cpu, "synthetic window has measured CPU");
    chk(n_gaps == 2, "only the two straddling gaps counted");
    chk(fabs(mine / 1e6 - tm.offcpu_ms) < 1e-6,
        "decomposition reproduces compute.c's offcpu_ms exactly");
    chk(tm.offcpu_ms > 1124.0 && tm.offcpu_ms < 1126.0,
        "and that number is the 1.125 s the window arithmetic predicts");
    free(tm.rows);

    /* 8. A window with no records at all: compute.c reports offcpu_ms 0 AND
     *    has_measured_cpu, so "nothing here" is indistinguishable from
     *    "clean" on the response alone. The decomposition must find no gaps,
     *    which is what the tool's refusal is keyed on. */
    memset(&tm, 0, sizeof(tm));
    pgwt_compute_time_model(ev, 5, &f, 20000000000ULL, 21000000000ULL,
                            1000.0, &tm);
    int n2 = 0;
    for (int i = 0; i < 5; i++)
        if (ocd_classify(&ev[i], 20000000000ULL, 21000000000ULL,
                         &resid, &dur, &clamp) != OCD_NOT_GAP) n2++;
    chk(tm.db_time_ms == 0.0 && n2 == 0,
        "empty window: no DB Time and no gaps (the refusal's trigger)");
    free(tm.rows);

    printf(failures ? "\nFAILED (%d)\n" : "\nPASS\n", failures);
    return failures ? 1 : 0;
}
