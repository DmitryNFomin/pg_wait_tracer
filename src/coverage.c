/* Coverage label verdict: coverage spec r2 §2 plus Amendments A1 and A2.
 * Refs #947, #871.
 *
 * Pure: every derived fact (holes, taint, E_b, liveness, clock-step
 * overlaps) is recomputed from the caller's records on each call; the only
 * working storage is the caller's scratch area. Windows are half-open
 * [from, to); a boot's covered piece [S_b, E_b] is closed, and uncovered
 * ranges are reported as half-open [from, to) wall intervals (a measure-zero
 * endpoint is not represented).
 *
 * Cost (A2 item 2): two passes over each boot's records, one sort of its
 * gaps, then one sort and sweep of every interval clipped to the window:
 * O(n log n) in boots + records + legacy ranges. Nothing compares every
 * segment with every gap.
 *
 * Choices where §2 is silent and no example row decides (each fails
 * closed): a hole or taint with no earlier WATERMARK starts at boot_start;
 * a boot with no WATERMARK and no SEAL has no covered piece and adds no
 * unclean_end, restart or not_yet_covered range (§5 C7: "BOOT but no
 * WATERMARK => not_captured"); "meets a boot" uses the covered piece
 * [S_b, E_b]; T_b and holes apply to a point whichever boot it lies in; the
 * first restart in time order names restart_cause; a GAP_OPEN pairs with
 * the first GAP_CLOSE of its gap_id after it in record order (§1: the close
 * is written at recovery), else it never closes. clock_step (A2 item 1)
 * keeps §2's place in the reason order, right after restart. */
#include "coverage.h"

#include <stdlib.h>

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
 * retention are not a hole (A1 item 5). Valid W never decreases along the
 * walk. */
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

/* Per-boot facts of §2 Model, Live and Covered piece, in wall time; bad_lo
 * and bad_hi delimit the boot's taint and hole intervals. */
struct sum {
    int64_t start, S, W, fd, fk, E;
    int have_w, have_e, sealed, live;
    size_t bad_lo, bad_hi;
};

struct gap_open {
    int64_t start, key, lo, hi; /* mono */
    size_t pos;                 /* record index */
};

struct gap_close {
    int64_t key, end;
    size_t pos;
};

struct iv {
    int64_t lo, hi;
    int cls;
};

/* Sweep event: `what` is a reason index, or one of EV_* below. */
struct ev {
    int64_t t;
    int what, delta;
    size_t boot;
};

enum { EV_PIECE = PGWT_COV_N_REASONS, EV_TAINT, EV_HOLE };

struct bootcnt {
    int piece, bad;
};

/* Caller scratch, carved per call. Every count below is bounded by the
 * model: a hole or a gap is opened by a distinct record of its boot, so a
 * boot has at most n_recs of them; at most n_boots overlap regions; and at
 * most n_recs + 6 n_boots + n_legacy sweep intervals (piece, retention,
 * unclean_end, restart, not_yet_covered per boot, plus overlaps). */
struct ws {
    struct sum *sums;
    struct gap_open *go;
    struct gap_close *gc;
    struct iv *bad, *ov;
    struct bootcnt *cnt;
    struct ev *ev;
};

#define WS_ALIGN (_Alignof(max_align_t))

static int take(size_t *off, size_t n, size_t each)
{
    size_t bytes;
    if (each && n > SIZE_MAX / each)
        return 0;
    bytes = n * each;
    if (bytes > SIZE_MAX - WS_ALIGN)
        return 0;
    bytes = (bytes + WS_ALIGN - 1) / WS_ALIGN * WS_ALIGN;
    if (*off > SIZE_MAX - bytes)
        return 0;
    *off += bytes;
    return 1;
}

/* Region sizes in carve order; returns 0 on size_t overflow. */
static int layout(const struct pgwt_cov_model *m, size_t off[8])
{
    size_t r = 0, maxr = 0, nb = m->n_boots, n_iv, at = 0;
    for (size_t i = 0; i < nb; i++) {
        size_t n = m->boots[i].n_recs;
        if (n > SIZE_MAX - r)
            return 0;
        r += n;
        maxr = n > maxr ? n : maxr;
    }
    if (nb > (SIZE_MAX - r) / 7 || m->n_legacy > SIZE_MAX - r - 6 * nb ||
        r + 6 * nb + m->n_legacy > SIZE_MAX / 2)
        return 0;
    n_iv = r + 6 * nb + m->n_legacy;
    off[0] = at;
    if (!take(&at, nb, sizeof(struct sum)))
        return 0;
    off[1] = at;
    if (!take(&at, maxr, sizeof(struct gap_open)))
        return 0;
    off[2] = at;
    if (!take(&at, maxr, sizeof(struct gap_close)))
        return 0;
    off[3] = at;
    if (!take(&at, r, sizeof(struct iv)))
        return 0;
    off[4] = at;
    if (!take(&at, nb, sizeof(struct iv)))
        return 0;
    off[5] = at;
    if (!take(&at, nb, sizeof(struct bootcnt)))
        return 0;
    off[6] = at;
    if (!take(&at, 2 * n_iv, sizeof(struct ev)))
        return 0;
    off[7] = at;
    return 1;
}

size_t pgwt_cov_scratch_bytes(const struct pgwt_cov_model *m)
{
    size_t off[8];
    if (!m || (m->n_boots && !m->boots) || !layout(m, off))
        return SIZE_MAX;
    return off[7];
}

/* The caller's area must be aligned as malloc's result is. */
static int carve(const struct pgwt_cov_model *m, void *p, size_t size, struct ws *ws)
{
    size_t off[8];
    /* An empty model needs no bytes; its zero-length regions still get a
     * valid (never dereferenced) base, never NULL + 0. */
    char *base = p ? (char *)p : (char *)ws;
    if (!layout(m, off) || size < off[7] ||
        (off[7] && (!p || (uintptr_t)p % WS_ALIGN)))
        return 0;
    ws->sums = (struct sum *)(void *)(base + off[0]);
    ws->go = (struct gap_open *)(void *)(base + off[1]);
    ws->gc = (struct gap_close *)(void *)(base + off[2]);
    ws->bad = (struct iv *)(void *)(base + off[3]);
    ws->ov = (struct iv *)(void *)(base + off[4]);
    ws->cnt = (struct bootcnt *)(void *)(base + off[5]);
    ws->ev = (struct ev *)(void *)(base + off[6]);
    return 1;
}

static int cmp64(int64_t a, int64_t b) { return (a > b) - (a < b); }
static int cmpsz(size_t a, size_t b) { return (a > b) - (a < b); }

static int cmp_go_start(const void *x, const void *y)
{
    const struct gap_open *a = x, *b = y;
    int c = cmp64(a->start, b->start);
    return c ? c : cmpsz(a->pos, b->pos);
}

static int cmp_go_pair(const void *x, const void *y)
{
    const struct gap_open *a = x, *b = y;
    int c = cmp64(a->key, b->key);
    return c ? c : cmpsz(a->pos, b->pos);
}

static int cmp_gc_pair(const void *x, const void *y)
{
    const struct gap_close *a = x, *b = y;
    int c = cmp64(a->key, b->key);
    return c ? c : cmpsz(a->pos, b->pos);
}

static int cmp_ev(const void *x, const void *y)
{
    const struct ev *a = x, *b = y;
    return cmp64(a->t, b->t);
}

/* §1: GAP_OPEN and GAP_CLOSE carry gap_id in c. */
static int64_t pair_key(int64_t id) { return id; }

/* §1: a GAP_CLOSE is written at recovery, after its GAP_OPEN. */
static int closes_after(size_t open_pos, size_t close_pos) { return close_pos > open_pos; }

/* §2 Taint (C6) for every gap of one boot, in mono time: taint_from = the
 * floor of the last valid WATERMARK with W <= the gap start, whatever was
 * appended in between, else boot_start; the end is the paired GAP_CLOSE,
 * else INF. W never decreases along the walk, so one merge of the walk's
 * WATERMARKs with the gaps sorted by start resolves every taint_from. */
static void resolve_gaps(const struct pgwt_cov_boot *b, int kind,
                         struct gap_open *go, size_t ngo,
                         struct gap_close *gc, size_t ngc)
{
    struct walk w;
    const struct pgwt_cov_rec *r = 0;
    size_t k = 0, j = 0, c = 0;
    int64_t lo = 0, hi = 0, cur = b->boot_start;
    int st;
    if (!ngo)
        return;
    qsort(go, ngo, sizeof(*go), cmp_go_start);
    walk_init(&w, b, kind);
    while ((st = walk_next(&w, &r, &k, &lo, &hi)) != W_END) {
        if (st != W_REC || r->kind != PGWT_COV_REC_WATERMARK)
            continue;
        while (j < ngo && go[j].start < r->a)
            go[j++].lo = cur;
        cur = floor_of(kind, r->b, r->c);
    }
    while (j < ngo)
        go[j++].lo = cur;

    if (ngc) {
        qsort(go, ngo, sizeof(*go), cmp_go_pair);
        qsort(gc, ngc, sizeof(*gc), cmp_gc_pair);
    }
    for (j = 0; j < ngo; j++) {
        while (c < ngc && (gc[c].key < go[j].key ||
                           (gc[c].key == go[j].key && !closes_after(go[j].pos, gc[c].pos))))
            c++;
        go[j].hi = c < ngc && gc[c].key == go[j].key ? gc[c].end : PGWT_COV_INF;
    }
}

/* One boot: summary, holes and taint intervals (appended to bad[]). */
static void prepare_boot(const struct pgwt_cov_boot *b, int kind, int64_t now,
                         struct sum *s, struct ws *ws, size_t *n_bad)
{
    struct walk w;
    const struct pgwt_cov_rec *r = 0;
    size_t k = 0, ngo = 0, ngc = 0;
    int64_t lo = 0, hi = 0, w_final = 0;
    int st, seal_seen = 0;
    *s = (struct sum){0};
    s->bad_lo = *n_bad;
    walk_init(&w, b, kind);
    while ((st = walk_next(&w, &r, &k, &lo, &hi)) != W_END) {
        if (st == W_HOLE) {
            ws->bad[(*n_bad)++] = (struct iv){wall(b, lo), wall(b, hi), BAD_HOLE};
        } else if (r->kind == PGWT_COV_REC_SEAL) {
            seal_seen = 1;
            w_final = r->a;
        } else if (r->kind == PGWT_COV_REC_GAP_OPEN) {
            ws->go[ngo++] = (struct gap_open){.start = r->a, .key = pair_key(r->c), .pos = k};
        } else if (r->kind == PGWT_COV_REC_GAP_CLOSE) {
            ws->gc[ngc++] = (struct gap_close){.key = pair_key(r->c), .end = r->a, .pos = k};
        }
    }
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

    resolve_gaps(b, kind, ws->go, ngo, ws->gc, ngc);
    for (size_t j = 0; j < ngo; j++)
        ws->bad[(*n_bad)++] =
            (struct iv){wall(b, ws->go[j].lo), wall(b, ws->go[j].hi), BAD_TAINT};
    s->bad_hi = *n_bad;
}

/* A2 item 1 (clock_step): the wall ranges [S_b, E_b] of two boots overlap
 * only after a backward clock step, and every point inside the overlap is
 * uncovered. One sweep over the sorted piece endpoints finds every point
 * that two or more pieces share (touching pieces share none, A1 item 7). */
static size_t overlaps(const struct sum *s, size_t nb, struct ev *ev, struct iv *ov)
{
    size_t n = 0, n_ov = 0;
    int depth = 0;
    for (size_t i = 0; i < nb; i++)
        if (s[i].have_e && s[i].S < s[i].E) {
            ev[n++] = (struct ev){s[i].S, 0, 1, i};
            ev[n++] = (struct ev){s[i].E, 0, -1, i};
        }
    qsort(ev, n, sizeof(*ev), cmp_ev);
    for (size_t i = 0; i < n;) {
        int64_t t = ev[i].t;
        while (i < n && ev[i].t == t)
            depth += ev[i++].delta;
        if (depth >= 2 && i < n) {
            if (n_ov && ov[n_ov - 1].hi == t)
                ov[n_ov - 1].hi = ev[i].t;
            else
                ov[n_ov++] = (struct iv){t, ev[i].t, 0};
        }
    }
    return n_ov;
}

/* overlaps() emits disjoint regions in time order: binary search for the
 * first one ending after lo. */
static int ov_meets(const struct iv *ov, size_t n_ov, int64_t lo, int64_t hi)
{
    size_t a = 0, b = n_ov;
    while (a < b) {
        size_t mid = a + (b - a) / 2;
        if (ov[mid].hi <= lo)
            a = mid + 1;
        else
            b = mid;
    }
    return a < n_ov && meets(ov[a].lo, ov[a].hi, lo, hi);
}

/* [lo, hi) meets neither the boot's own taint/holes nor a clock-step
 * overlap (§2 complete and live_edge; A2 item 1). */
static int clean(const struct ws *ws, const struct sum *s, size_t n_ov,
                 int64_t lo, int64_t hi)
{
    for (size_t j = s->bad_lo; j < s->bad_hi; j++)
        if ((ws->bad[j].cls & COVER_EXCLUDES) && meets(ws->bad[j].lo, ws->bad[j].hi, lo, hi))
            return 0;
    return !ov_meets(ws->ov, n_ov, lo, hi);
}

static void add_iv(struct ev *ev, size_t *n, int64_t lo, int64_t hi,
                   int64_t from, int64_t to, int what, size_t boot)
{
    lo = max64(lo, from);
    hi = min64(hi, to);
    if (lo >= hi)
        return;
    ev[(*n)++] = (struct ev){lo, what, 1, boot};
    ev[(*n)++] = (struct ev){hi, what, -1, boot};
}

static void apply(const struct ev *e, int *cnt, struct bootcnt *bc, size_t *n_cover)
{
    struct bootcnt *c;
    int before, after;
    if (e->what < PGWT_COV_N_REASONS) {
        cnt[e->what] += e->delta;
        return;
    }
    c = &bc[e->boot];
    before = c->piece > 0 && c->bad == 0;
    if (e->what == EV_PIECE) {
        c->piece += e->delta;
    } else {
        int cls = e->what == EV_TAINT ? BAD_TAINT : BAD_HOLE;
        cnt[cls == BAD_TAINT ? PGWT_COV_R_CAPTURE_LOSS : PGWT_COV_R_MISSING_FILE] += e->delta;
        if (cls & COVER_EXCLUDES)
            c->bad += e->delta;
    }
    after = c->piece > 0 && c->bad == 0;
    *n_cover = *n_cover + (size_t)after - (size_t)before;
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

/* Every point of [from, to) that no boot covers takes the first reason of
 * §2 States that applies (reason index order). One sorted sweep. */
static int sweep(const struct pgwt_cov_model *m, const struct ws *ws, size_t n_ov,
                 int64_t from, int64_t to, struct pgwt_cov_result *out,
                 unsigned *mask, int *have_pair, size_t *pa, size_t *pb)
{
    size_t nb = m->n_boots, n = 0, i = 0, n_cover = 0;
    int cnt[PGWT_COV_N_REASONS] = {0};
    int64_t x = from;
    for (size_t b = 0; b < nb; b++) {
        const struct sum *s = &ws->sums[b];
        int64_t next_s = b + 1 < nb ? ws->sums[b + 1].S : PGWT_COV_INF;
        ws->cnt[b] = (struct bootcnt){0, 0};
        if (s->have_e)
            add_iv(ws->ev, &n, s->S, s->E, from, to, EV_PIECE, b);
        for (size_t j = s->bad_lo; j < s->bad_hi; j++)
            add_iv(ws->ev, &n, ws->bad[j].lo, ws->bad[j].hi, from, to,
                   ws->bad[j].cls == BAD_TAINT ? EV_TAINT : EV_HOLE, b);
        if (!s->sealed && !s->live && s->have_e)
            add_iv(ws->ev, &n, s->E, next_s, from, to, PGWT_COV_R_UNCLEAN_END, b);
        add_iv(ws->ev, &n, s->start, s->S, from, to, PGWT_COV_R_RETENTION, b);
        if (b + 1 < nb && s->have_e)
            add_iv(ws->ev, &n, s->E, next_s, from, to, PGWT_COV_R_RESTART, b);
        if (s->live && s->have_e)
            add_iv(ws->ev, &n, s->E, PGWT_COV_INF, from, to, PGWT_COV_R_NOT_YET_COVERED, b);
    }
    for (size_t j = 0; j < n_ov; j++)
        add_iv(ws->ev, &n, ws->ov[j].lo, ws->ov[j].hi, from, to, PGWT_COV_R_CLOCK_STEP, 0);
    for (size_t j = 0; j < m->n_legacy; j++)
        add_iv(ws->ev, &n, m->legacy[j].from, m->legacy[j].to, from, to,
               PGWT_COV_R_NOT_RECORDED, 0);
    qsort(ws->ev, n, sizeof(*ws->ev), cmp_ev);

    while (x < to) {
        int r = PGWT_COV_R_NOT_CAPTURED;
        int64_t nx;
        while (i < n && ws->ev[i].t == x)
            apply(&ws->ev[i++], cnt, ws->cnt, &n_cover);
        nx = i < n ? ws->ev[i].t : to;
        if (n_cover && !cnt[PGWT_COV_R_CLOCK_STEP]) {
            x = nx;
            continue;
        }
        for (int k = 0; k < PGWT_COV_N_REASONS; k++)
            if (cnt[k] > 0) {
                r = k;
                break;
            }
        *mask |= 1u << r;
        if (r == PGWT_COV_R_RESTART && !*have_pair) {
            for (size_t b = 0; b + 1 < nb; b++)
                if (ws->sums[b].have_e && meets(ws->sums[b].E, ws->sums[b + 1].S, x, nx)) {
                    *have_pair = 1;
                    *pa = b;
                    *pb = b + 1;
                    break;
                }
        }
        struct pgwt_cov_span *last =
            out->n_uncovered ? &out->uncovered[out->n_uncovered - 1] : 0;
        if (last && last->to == x && last->reason == r) {
            last->to = nx;
        } else if (out->n_uncovered == out->uncovered_cap) {
            return 0;
        } else {
            out->uncovered[out->n_uncovered++] = (struct pgwt_cov_span){x, nx, r};
        }
        x = nx;
    }
    return 1;
}

int pgwt_coverage_verdict(const struct pgwt_cov_model *m, int64_t from,
                          int64_t to, int64_t now, int kind,
                          struct pgwt_cov_result *out)
{
    struct ws ws;
    struct sum s;
    size_t n_bad = 0, n_ov;
    long chosen = -1, live_b = -1;
    if (!out)
        return PGWT_COV_ERROR;
    fail(out, PGWT_COV_E_NONE);
    if (!m || !model_ok(m) || (out->uncovered_cap && !out->uncovered))
        return fail(out, PGWT_COV_E_BAD_MODEL);
    if (from >= to)
        return fail(out, PGWT_COV_E_INVALID_WINDOW);
    if (!carve(m, out->scratch, out->scratch_size, &ws))
        return fail(out, PGWT_COV_E_SCRATCH_SMALL);

    for (size_t i = 0; i < m->n_boots; i++)
        prepare_boot(&m->boots[i], kind, now, &ws.sums[i], &ws, &n_bad);
    n_ov = overlaps(ws.sums, m->n_boots, ws.ev, ws.ov);

    for (size_t i = 0; i < m->n_boots; i++) {
        const struct sum *p = &ws.sums[i];
        if (p->live)
            live_b = (long)i;
        if (chosen < 0 && p->have_e && p->S <= from && to <= p->E &&
            clean(&ws, p, n_ov, from, to)) {
            chosen = (long)i;
            out->verdict = PGWT_COV_COMPLETE;
        }
    }
    for (size_t i = 0; chosen < 0 && i < m->n_boots; i++) {
        const struct sum *p = &ws.sums[i];
        if (p->live && p->have_w && p->S <= from && from < p->W && to > p->W &&
            to - p->W <= PGWT_COV_LIVE_L_NS && clean(&ws, p, n_ov, from, p->W)) {
            chosen = (long)i;
            out->verdict = PGWT_COV_LIVE_EDGE;
            out->covered_to = p->W;
        }
    }

    if (chosen < 0) {
        unsigned mask = 0;
        size_t pa = 0, pb = 0, met = 0, met_a = 0, met_b = 0;
        int have_pair = 0;
        out->verdict = PGWT_COV_INCOMPLETE;
        if (!sweep(m, &ws, n_ov, from, to, out, &mask, &have_pair, &pa, &pb))
            return fail(out, PGWT_COV_E_BUFFER_FULL);
        /* Window level: meeting two or more boots is a restart even with no
         * uncovered point (row 3). Legacy data is not a boot (A1 item 3). */
        for (size_t i = 0; i < m->n_boots; i++) {
            s = ws.sums[i];
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
        s = ws.sums[xb];
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
    static const char *const names[] = {"none", "invalid_window", "bad_model", "buffer_full",
                                        "scratch_small"};
    return e >= 0 && e <= PGWT_COV_E_SCRATCH_SMALL ? names[e] : "?";
}

const char *pgwt_cov_restart_name(int c)
{
    static const char *const names[] = {"none", "daemon", "postgres", "unknown"};
    return c >= 0 && c <= PGWT_COV_RESTART_UNKNOWN ? names[c] : "?";
}
