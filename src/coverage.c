/* Coverage label verdict: coverage spec r2 §2 plus Amendment A1.
 * Refs #947, #871.
 *
 * Pure: every derived fact (holes, taint, E_b, liveness) is recomputed from
 * the caller's records by a pass over them; nothing is cached or allocated.
 * Windows are half-open [from, to); a boot's covered piece [S_b, E_b] is
 * closed, and uncovered ranges are reported as half-open [from, to) wall
 * intervals (a measure-zero endpoint is not represented).
 *
 * Choices where §2 is silent and no example row decides (each fails
 * closed): a hole or taint with no earlier WATERMARK starts at boot_start;
 * a boot with no WATERMARK and no SEAL has no covered piece and adds no
 * unclean_end, restart or not_yet_covered range (§5 C7: "BOOT but no
 * WATERMARK => not_captured"); "meets a boot" uses the covered piece
 * [S_b, E_b]; T_b and holes apply to a point whichever boot it lies in; the
 * first restart in time order names restart_cause. clock_step has no rule
 * text and is never emitted. */
#include "coverage.h"

enum { BAD_TAINT = 1, BAD_HOLE = 2 };
/* What a covered piece must not meet (§2 complete, live_edge). */
#define COVER_EXCLUDES (BAD_TAINT | BAD_HOLE)

static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }

/* Non-empty interior intersection of [a,b) and [c,d) (A1 item 7). */
static int meets(int64_t a, int64_t b, int64_t c, int64_t d)
{
    return max64(a, c) < min64(b, d);
}

static int64_t wall(const struct pgwt_cov_boot *b, int64_t t)
{
    return t == PGWT_COV_INF ? t : t + b->wall_offset;
}

static int64_t floor_of(int kind, int64_t fd, int64_t fi)
{
    return kind == PGWT_COV_FLOOR_IDLE ? fi : fd;
}

/* One pass over a boot's records: yields valid records and holes. */
struct walk {
    const struct pgwt_cov_boot *b;
    int kind;
    size_t i;
    int started, dead, have_w, hole_open;
    int64_t file_no, w, fd, fi, hole_from;
};

enum { W_END, W_REC, W_HOLE };

static void walk_init(struct walk *w, const struct pgwt_cov_boot *b, int kind)
{
    *w = (struct walk){0};
    w->b = b;
    w->kind = kind;
}

static int64_t hole_start(const struct walk *w)
{
    /* §2 Holes (C2): records closing in the lost stretch started at or
     * after the floor of the last valid W, so the loss begins there. */
    return w->have_w ? floor_of(w->kind, w->fd, w->fi) : w->b->boot_start;
}

static void open_hole(struct walk *w)
{
    if (!w->hole_open) {
        w->hole_open = 1;
        w->hole_from = hole_start(w);
    }
}

/* Mono times. A missing file_no, an unreadable file, a bad record or a
 * non-monotone W opens a hole that the next readable file's BOOT
 * observation closes, else it runs to INF. Leading files removed by
 * retention are not a hole (A1 item 5). */
static int walk_next(struct walk *w, const struct pgwt_cov_rec **rec,
                     size_t *idx, int64_t *lo, int64_t *hi)
{
    while (w->i < w->b->n_recs) {
        size_t k = w->i++;
        const struct pgwt_cov_rec *r = &w->b->recs[k];
        if (r->kind == PGWT_COV_REC_FILE) {
            int lost = !r->c || (w->started && r->b != w->file_no + 1);
            w->started = 1;
            w->file_no = r->b;
            w->dead = !r->c;
            if (lost)
                open_hole(w);
            if (r->c && w->hole_open) {
                w->hole_open = 0;
                *lo = w->hole_from;
                *hi = r->a;
                return W_HOLE;
            }
            continue;
        }
        if (w->dead)
            continue;
        int bad = r->kind < PGWT_COV_REC_WATERMARK || r->kind > PGWT_COV_REC_SEAL ||
                  (r->kind == PGWT_COV_REC_WATERMARK && w->have_w && r->a < w->w);
        if (bad) {
            open_hole(w);
            w->dead = 1;
            continue;
        }
        if (r->kind == PGWT_COV_REC_WATERMARK) {
            w->have_w = 1;
            w->w = r->a;
            w->fd = r->b;
            w->fi = r->c;
        }
        *rec = r;
        *idx = k;
        return W_REC;
    }
    if (w->hole_open) {
        w->hole_open = 0;
        *lo = w->hole_from;
        *hi = PGWT_COV_INF;
        return W_HOLE;
    }
    return W_END;
}

/* §2 Taint (C6): the floor of the last valid WATERMARK with W <= the gap
 * start, whatever was appended in between; else boot_start. */
static int64_t taint_from(const struct pgwt_cov_boot *b, int kind,
                          int64_t start, size_t gap_idx)
{
    struct walk w;
    const struct pgwt_cov_rec *r = 0;
    size_t k = 0;
    int64_t lo = 0, hi = 0, from = b->boot_start;
    int st;
    (void)gap_idx;
    walk_init(&w, b, kind);
    while ((st = walk_next(&w, &r, &k, &lo, &hi)) != W_END)
        if (st == W_REC && r->kind == PGWT_COV_REC_WATERMARK && r->a <= start)
            from = floor_of(kind, r->b, r->c);
    return from;
}

static int64_t gap_end(const struct pgwt_cov_boot *b, int kind, int64_t id)
{
    struct walk w;
    const struct pgwt_cov_rec *r = 0;
    size_t k = 0;
    int64_t lo = 0, hi = 0;
    int st;
    walk_init(&w, b, kind);
    while ((st = walk_next(&w, &r, &k, &lo, &hi)) != W_END)
        if (st == W_REC && r->kind == PGWT_COV_REC_GAP_CLOSE && r->c == id)
            return r->a;
    return PGWT_COV_INF;
}

/* Iterates a boot's taint and hole intervals, in wall time. */
static int bad_next(struct walk *w, int64_t *lo, int64_t *hi, int *cls)
{
    const struct pgwt_cov_rec *r = 0;
    size_t k = 0;
    int st;
    while ((st = walk_next(w, &r, &k, lo, hi)) != W_END) {
        if (st == W_HOLE) {
            *cls = BAD_HOLE;
        } else if (r->kind == PGWT_COV_REC_GAP_OPEN) {
            *cls = BAD_TAINT;
            *lo = taint_from(w->b, w->kind, r->a, k);
            *hi = gap_end(w->b, w->kind, r->c);
        } else {
            continue;
        }
        *lo = wall(w->b, *lo);
        *hi = wall(w->b, *hi);
        return 1;
    }
    return 0;
}

static int bad_hit(const struct pgwt_cov_boot *b, int kind, int cls,
                   int64_t from, int64_t to)
{
    struct walk w;
    int64_t lo = 0, hi = 0;
    int c = 0;
    walk_init(&w, b, kind);
    while (bad_next(&w, &lo, &hi, &c))
        if ((c & cls) && meets(lo, hi, from, to))
            return 1;
    return 0;
}

static void edge(int64_t *next, int64_t x, int64_t t)
{
    if (t > x && t < *next)
        *next = t;
}

static void bad_edges(const struct pgwt_cov_boot *b, int kind, int64_t x,
                      int64_t *next)
{
    struct walk w;
    int64_t lo = 0, hi = 0;
    int c = 0;
    walk_init(&w, b, kind);
    while (bad_next(&w, &lo, &hi, &c)) {
        edge(next, x, lo);
        edge(next, x, hi);
    }
}

/* Per-boot facts of §2 Model, Live and Covered piece, in wall time. */
struct sum {
    int64_t start, S, W, fd, fk, E;
    int have_w, have_e, sealed, live;
};

static void summarise(const struct pgwt_cov_boot *b, int kind, int64_t now,
                      struct sum *s)
{
    struct walk w;
    const struct pgwt_cov_rec *r = 0;
    size_t k = 0;
    int64_t lo = 0, hi = 0, w_final = 0;
    int st, seal_seen = 0;
    walk_init(&w, b, kind);
    while ((st = walk_next(&w, &r, &k, &lo, &hi)) != W_END)
        if (st == W_REC && r->kind == PGWT_COV_REC_SEAL) {
            seal_seen = 1;
            w_final = r->a;
        }
    *s = (struct sum){0};
    s->start = wall(b, b->boot_start);
    /* S = max(boot_start, BOOT observation of the earliest retained file). */
    s->S = b->n_recs ? max64(s->start, wall(b, b->recs[0].a)) : s->start;
    s->have_w = w.have_w;
    s->W = wall(b, w.w);
    s->fd = wall(b, w.fd);
    s->fk = wall(b, floor_of(kind, w.fd, w.fi));
    /* Sealed = SEAL present AND closure CLEAN (§1 Clean shutdown). */
    s->sealed = seal_seen && b->closure_clean;
    s->live = !s->sealed && b->same_kernel_boot &&
              b->status != PGWT_COV_STATUS_MISMATCH &&
              now - wall(b, b->last_obs) <= PGWT_COV_LIVE_L_NS;
    if (s->sealed) {
        s->have_e = 1;
        s->E = wall(b, w_final);
    } else if (s->live) {
        /* The floor is not applied while live: open waits will still close. */
        s->have_e = s->have_w;
        s->E = s->W;
    } else {
        s->have_e = s->have_w;
        s->E = s->fk;
    }
}

/* Reason for the segment [x, nx), which no boundary crosses; -1 = covered. */
static int classify(const struct pgwt_cov_model *m, int kind, int64_t now,
                    int64_t x, int64_t nx, size_t *pair)
{
    unsigned hit = 0;
    int have_pair = 0;
    for (size_t i = 0; i < m->n_boots; i++) {
        const struct pgwt_cov_boot *b = &m->boots[i];
        struct sum s, t = {0};
        int64_t next_s = PGWT_COV_INF;
        summarise(b, kind, now, &s);
        if (s.have_e && meets(s.S, s.E, x, nx) &&
            !bad_hit(b, kind, COVER_EXCLUDES, x, nx))
            return -1;
        if (i + 1 < m->n_boots) {
            summarise(&m->boots[i + 1], kind, now, &t);
            next_s = t.S;
        }
        if (bad_hit(b, kind, BAD_TAINT, x, nx))
            hit |= 1u << PGWT_COV_R_CAPTURE_LOSS;
        if (bad_hit(b, kind, BAD_HOLE, x, nx))
            hit |= 1u << PGWT_COV_R_MISSING_FILE;
        if (!s.sealed && !s.live && s.have_e && meets(s.E, next_s, x, nx))
            hit |= 1u << PGWT_COV_R_UNCLEAN_END;
        if (meets(s.start, s.S, x, nx))
            hit |= 1u << PGWT_COV_R_RETENTION;
        if (i + 1 < m->n_boots && s.have_e && meets(s.E, next_s, x, nx)) {
            hit |= 1u << PGWT_COV_R_RESTART;
            if (!have_pair) {
                have_pair = 1;
                *pair = i;
            }
        }
        if (s.live && s.have_e && s.E <= x)
            hit |= 1u << PGWT_COV_R_NOT_YET_COVERED;
    }
    for (size_t i = 0; i < m->n_legacy; i++)
        if (meets(m->legacy[i].from, m->legacy[i].to, x, nx))
            hit |= 1u << PGWT_COV_R_NOT_RECORDED;
    for (int r = 0; r < PGWT_COV_N_REASONS; r++)
        if (hit & (1u << r))
            return r;
    return PGWT_COV_R_NOT_CAPTURED;
}

static int restart_cause(const struct pgwt_cov_boot *a,
                         const struct pgwt_cov_boot *b)
{
    if (!a->postmaster_start || !b->postmaster_start)
        return PGWT_COV_RESTART_UNKNOWN;
    return a->postmaster_start != b->postmaster_start ? PGWT_COV_RESTART_POSTGRES
                                                      : PGWT_COV_RESTART_DAEMON;
}

static int fail(struct pgwt_cov_result *out, int error)
{
    out->verdict = PGWT_COV_ERROR;
    out->error = error;
    out->n_reasons = 0;
    out->n_uncovered = 0;
    out->covered_to = PGWT_COV_NONE;
    out->restart_cause = PGWT_COV_RESTART_NONE;
    out->excludes_in_flight = 0;
    return out->verdict;
}

static int model_ok(const struct pgwt_cov_model *m)
{
    if ((m->n_boots && !m->boots) || (m->n_legacy && !m->legacy))
        return 0;
    for (size_t i = 0; i < m->n_boots; i++) {
        const struct pgwt_cov_boot *b = &m->boots[i];
        if (b->n_recs && (!b->recs || b->recs[0].kind != PGWT_COV_REC_FILE))
            return 0;
    }
    return 1;
}

int pgwt_coverage_verdict(const struct pgwt_cov_model *m, int64_t from,
                          int64_t to, int64_t now, int kind,
                          struct pgwt_cov_result *out)
{
    struct sum s;
    long chosen = -1, live_b = -1;
    if (!out)
        return PGWT_COV_ERROR;
    fail(out, PGWT_COV_E_NONE);
    if (!m || !model_ok(m) || (out->uncovered_cap && !out->uncovered))
        return fail(out, PGWT_COV_E_BAD_MODEL);
    if (from >= to)
        return fail(out, PGWT_COV_E_INVALID_WINDOW);

    for (size_t i = 0; i < m->n_boots; i++) {
        const struct pgwt_cov_boot *b = &m->boots[i];
        summarise(b, kind, now, &s);
        if (s.live)
            live_b = (long)i;
        if (chosen < 0 && s.have_e && s.S <= from && to <= s.E &&
            !bad_hit(b, kind, COVER_EXCLUDES, from, to)) {
            chosen = (long)i;
            out->verdict = PGWT_COV_COMPLETE;
        }
    }
    for (size_t i = 0; chosen < 0 && i < m->n_boots; i++) {
        const struct pgwt_cov_boot *b = &m->boots[i];
        summarise(b, kind, now, &s);
        if (s.live && s.have_w && s.S <= from && from < s.W && to > s.W &&
            to - s.W <= PGWT_COV_LIVE_L_NS &&
            !bad_hit(b, kind, COVER_EXCLUDES, from, s.W)) {
            chosen = (long)i;
            out->verdict = PGWT_COV_LIVE_EDGE;
            out->covered_to = s.W;
        }
    }

    if (chosen < 0) {
        unsigned mask = 0;
        size_t pa = 0, pb = 0, met = 0, met_a = 0, met_b = 0;
        int have_pair = 0;
        out->verdict = PGWT_COV_INCOMPLETE;
        for (int64_t x = from; x < to;) {
            int64_t nx = to;
            size_t pair = 0;
            for (size_t i = 0; i < m->n_boots; i++) {
                summarise(&m->boots[i], kind, now, &s);
                edge(&nx, x, s.start);
                edge(&nx, x, s.S);
                if (s.have_e)
                    edge(&nx, x, s.E);
                bad_edges(&m->boots[i], kind, x, &nx);
            }
            for (size_t i = 0; i < m->n_legacy; i++) {
                edge(&nx, x, m->legacy[i].from);
                edge(&nx, x, m->legacy[i].to);
            }
            int r = classify(m, kind, now, x, nx, &pair);
            if (r >= 0) {
                struct pgwt_cov_span *last =
                    out->n_uncovered ? &out->uncovered[out->n_uncovered - 1] : 0;
                mask |= 1u << r;
                if (r == PGWT_COV_R_RESTART && !have_pair) {
                    have_pair = 1;
                    pa = pair;
                    pb = pair + 1;
                }
                if (last && last->to == x && last->reason == r) {
                    last->to = nx;
                } else if (out->n_uncovered == out->uncovered_cap) {
                    return fail(out, PGWT_COV_E_BUFFER_FULL);
                } else {
                    out->uncovered[out->n_uncovered++] =
                        (struct pgwt_cov_span){x, nx, r};
                }
            }
            x = nx;
        }
        /* Window level: meeting two or more boots is a restart even with no
         * uncovered point (row 3). Legacy data is not a boot (A1 item 3). */
        for (size_t i = 0; i < m->n_boots; i++) {
            summarise(&m->boots[i], kind, now, &s);
            if (s.have_e && meets(s.S, s.E, from, to)) {
                if (met == 0)
                    met_a = i;
                else if (met == 1)
                    met_b = i;
                met++;
            }
        }
        if (met >= 2) {
            mask |= 1u << PGWT_COV_R_RESTART;
            if (!have_pair) {
                have_pair = 1;
                pa = met_a;
                pb = met_b;
            }
        }
        if (have_pair)
            out->restart_cause = restart_cause(&m->boots[pa], &m->boots[pb]);
        /* Rule precedence, not time order (A1 item 4). */
        for (int r = 0; r < PGWT_COV_N_REASONS; r++)
            if (mask & (1u << r))
                out->reasons[out->n_reasons++] = r;
    }

    /* C3 narrowed by A1 item 1: only complete and live_edge windows of a
     * live boot carry the mark. */
    long xb = chosen >= 0 ? chosen : live_b;
    if ((out->verdict == PGWT_COV_COMPLETE || out->verdict == PGWT_COV_LIVE_EDGE) && xb >= 0) {
        summarise(&m->boots[xb], kind, now, &s);
        out->excludes_in_flight = s.live && s.fd < to;
    }
    return out->verdict;
}

const char *pgwt_cov_verdict_name(int v)
{
    static const char *const names[] = {"complete", "live_edge", "incomplete", "error"};
    return v >= 0 && v <= PGWT_COV_ERROR ? names[v] : "?";
}

const char *pgwt_cov_reason_name(int r)
{
    static const char *const names[PGWT_COV_N_REASONS] = {
        "capture_loss", "missing_file", "unclean_end", "retention", "restart",
        "clock_step", "not_yet_covered", "not_recorded", "not_captured",
    };
    return r >= 0 && r < PGWT_COV_N_REASONS ? names[r] : "?";
}

const char *pgwt_cov_error_name(int e)
{
    static const char *const names[] = {"none", "invalid_window", "bad_model", "buffer_full"};
    return e >= 0 && e <= PGWT_COV_E_BUFFER_FULL ? names[e] : "?";
}

const char *pgwt_cov_restart_name(int c)
{
    static const char *const names[] = {"none", "daemon", "postgres", "unknown"};
    return c >= 0 && c <= PGWT_COV_RESTART_UNKNOWN ? names[c] : "?";
}
