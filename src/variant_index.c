/* variant_index.c — per-execution step-sequence index (paint-latency 3b).
 *
 * The contract, the selection rule, the carry-over argument and every
 * refusal are documented in variant_index.h. Read that first; this file is
 * the mechanism.
 *
 * Two halves:
 *   BUILD (add_block/seal) runs pgwt_compute_variants' phase 1 — the marker
 *   walk — once per committed block, with the per-pid partial sequence
 *   carried across calls, and reduces each closed execution to one row plus
 *   its pattern steps.
 *   QUERY replays pgwt_compute_variants' phase 2 — hash-table accumulation,
 *   slot-order collection, qsort by total time, truncation — over the rows
 *   the window selects, in closing-marker order.
 *
 * Every arithmetic expression that feeds an output is written the same way
 * the oracle writes it, including the double division for loop_n and the
 * `(int)(n * 0.95)` index pick, because "bit-exact" here means the same
 * bits, not the same value to within a tolerance.
 */
#include "variant_index.h"

#include "idle_rule.h"
#include "percentile.h"
#include "pid_index.h"
#include "test_alloc_fail.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── the oracle's structures, reimplemented (see NOT A SHARED
 * IMPLEMENTATION in the header: compute.c is held by other agents) ── */

struct vi_raw_exec {
    uint32_t events[PGWT_VARIANT_INDEX_MAX_RAW];
    uint64_t durations[PGWT_VARIANT_INDEX_MAX_RAW];
    int      len;
    uint64_t total_ns;
    uint64_t query_id;
};

struct vi_pattern {
    uint32_t steps[PGWT_MAX_VARIANT_STEPS];
    int      is_loop[PGWT_MAX_VARIANT_STEPS];
    int      loop_len[PGWT_MAX_VARIANT_STEPS];
    int      num_steps;
};

/* compute.c detect_loop(), verbatim. */
static int vi_detect_loop(const uint32_t *events, int len, int pos)
{
    for (int body = 1; body <= (len - pos) / 2; body++) {
        int reps = 1;
        int j = pos + body;
        while (j + body <= len) {
            int match = 1;
            for (int k = 0; k < body; k++) {
                if (events[pos + k] != events[j + k]) { match = 0; break; }
            }
            if (!match) break;
            reps++;
            j += body;
        }
        if (reps >= 2) return body;
    }
    return 0;
}

/* compute.c compress_exec(), verbatim. */
static void vi_compress_exec(const struct vi_raw_exec *raw,
                             struct vi_pattern *out)
{
    out->num_steps = 0;
    int i = 0;
    while (i < raw->len && out->num_steps < PGWT_MAX_VARIANT_STEPS) {
        int body = vi_detect_loop(raw->events, raw->len, i);
        if (body > 0 && out->num_steps + body < PGWT_MAX_VARIANT_STEPS) {
            out->steps[out->num_steps] = raw->events[i];
            out->is_loop[out->num_steps] = 1;
            out->loop_len[out->num_steps] = body;
            out->num_steps++;
            for (int k = 1; k < body && out->num_steps < PGWT_MAX_VARIANT_STEPS; k++) {
                out->steps[out->num_steps] = raw->events[i + k];
                out->is_loop[out->num_steps] = 0;
                out->loop_len[out->num_steps] = 0;
                out->num_steps++;
            }
            int reps = 0;
            int j = i;
            while (j + body <= raw->len) {
                int match = 1;
                for (int k = 0; k < body; k++) {
                    if (raw->events[i + k] != raw->events[j + k]) { match = 0; break; }
                }
                if (!match) break;
                reps++;
                j += body;
            }
            i = j;
        } else {
            out->steps[out->num_steps] = raw->events[i];
            out->is_loop[out->num_steps] = 0;
            out->loop_len[out->num_steps] = 0;
            out->num_steps++;
            i++;
        }
    }
}

/* compute.c hash_pattern(), verbatim. Step ORDER is folded in, which is why
 * [A, B] and [B, A] are two variants. */
static uint64_t vi_hash_pattern(const struct vi_pattern *p)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < p->num_steps; i++) {
        h ^= p->steps[i];
        h *= 0x100000001b3ULL;
        if (p->is_loop[i]) {
            h ^= 0xDEADBEEF;
            h *= 0x100000001b3ULL;
        }
    }
    return h;
}

/* compute.c cmp_variant_time_desc(), verbatim. It returns 0 for equal
 * total_ns, so qsort's tie order is unspecified by the standard — which is
 * fine and is why the test compares the SORTED arrays of both sides: an
 * identical pre-sort array through an identical comparator in one binary
 * yields one permutation. */
static int vi_cmp_variant_time_desc(const void *a, const void *b)
{
    const struct pgwt_variant *va = a, *vb = b;
    if (vb->total_ns > va->total_ns) return 1;
    if (vb->total_ns < va->total_ns) return -1;
    return 0;
}

/* ── per-pid carry-over state ─────────────────────────────── */

struct vi_phase_carry {
    int      active;
    uint64_t start_ns;      /* the opening marker's timestamp */
    struct vi_raw_exec exec;
};

struct vi_pid_carry {
    uint32_t pid;
    struct vi_phase_carry ph[2];    /* [PGWT_PHASE_EXEC], [PGWT_PHASE_PLAN] */
};

static const uint32_t vi_marker_start[2] = {
    PGWT_MARKER_EXEC_START, PGWT_MARKER_PLAN_START
};
static const uint32_t vi_marker_end[2] = {
    PGWT_MARKER_EXEC_END, PGWT_MARKER_PLAN_END
};

/* ── refusal plumbing ─────────────────────────────────────── */

const char *pgwt_variant_index_refusal_str(enum pgwt_variant_index_refusal r)
{
    switch (r) {
    case PGWT_VARIANT_INDEX_OK:                 return "ok";
    case PGWT_VARIANT_INDEX_REFUSE_NULL:        return "null-argument";
    case PGWT_VARIANT_INDEX_REFUSE_VERSION:     return "version-mismatch";
    case PGWT_VARIANT_INDEX_REFUSE_UNSEALED:    return "unsealed";
    case PGWT_VARIANT_INDEX_REFUSE_BUILD_FAILED:return "build-failed";
    case PGWT_VARIANT_INDEX_REFUSE_EMPTY:       return "empty-index";
    case PGWT_VARIANT_INDEX_REFUSE_RANGE:       return "window-outside-coverage";
    case PGWT_VARIANT_INDEX_REFUSE_BAD_WINDOW:  return "inverted-window";
    case PGWT_VARIANT_INDEX_REFUSE_GAP:         return "block-feed-gap";
    case PGWT_VARIANT_INDEX_REFUSE_ALLOC:       return "query-allocation-failed";
    case PGWT_VARIANT_INDEX_REFUSE_FILTER:      return "filter-not-indexable";
    case PGWT_VARIANT_INDEX_REFUSE_PHASE:       return "unknown-phase";
    }
    return "unknown";
}

static int vi_fail_build(struct pgwt_variant_index *idx,
                         enum pgwt_variant_index_refusal why)
{
    idx->failed = 1;
    if (idx->build_refusal == PGWT_VARIANT_INDEX_OK)
        idx->build_refusal = why;
    return -1;
}

/* ── init / free ──────────────────────────────────────────── */

void pgwt_variant_index_init(struct pgwt_variant_index *idx)
{
    if (!idx)
        return;
    memset(idx, 0, sizeof(*idx));
    idx->version = PGWT_VARIANT_INDEX_VERSION;
    idx->cover_from_ns = UINT64_MAX;
    idx->cover_to_ns = 0;
}

void pgwt_variant_index_free(struct pgwt_variant_index *idx)
{
    if (!idx)
        return;
    free(idx->rows);
    free(idx->steps);
    free(idx->chunks);
    free(idx->pids);
    if (idx->pid_ix) {
        pgwt_pid_index_free((struct pgwt_pid_index *)idx->pid_ix);
        free(idx->pid_ix);
    }
    memset(idx, 0, sizeof(*idx));
}

/* ── growable arrays ──────────────────────────────────────── */

static int vi_grow_rows(struct pgwt_variant_index *idx)
{
    if (idx->n_rows < idx->cap_rows)
        return 0;
    if (pgwt_test_alloc_fail("variant_index_rows"))
        return -1;
    int newcap = idx->cap_rows ? idx->cap_rows * 2 : 64;
    struct pgwt_variant_index_entry *t =
        realloc(idx->rows, (size_t)newcap * sizeof(*t));
    if (!t)
        return -1;
    idx->rows = t;
    idx->cap_rows = newcap;
    return 0;
}

static int vi_grow_steps(struct pgwt_variant_index *idx, int need)
{
    if (idx->n_steps + need <= idx->cap_steps)
        return 0;
    if (pgwt_test_alloc_fail("variant_index_steps"))
        return -1;
    int newcap = idx->cap_steps ? idx->cap_steps * 2 : 256;
    while (newcap < idx->n_steps + need)
        newcap *= 2;
    struct pgwt_variant_index_step *t =
        realloc(idx->steps, (size_t)newcap * sizeof(*t));
    if (!t)
        return -1;
    idx->steps = t;
    idx->cap_steps = newcap;
    return 0;
}

static int vi_grow_chunks(struct pgwt_variant_index *idx)
{
    if (idx->n_chunks < idx->cap_chunks)
        return 0;
    if (pgwt_test_alloc_fail("variant_index_chunks"))
        return -1;
    int newcap = idx->cap_chunks ? idx->cap_chunks * 2 : 16;
    struct pgwt_variant_index_chunk *t =
        realloc(idx->chunks, (size_t)newcap * sizeof(*t));
    if (!t)
        return -1;
    idx->chunks = t;
    idx->cap_chunks = newcap;
    return 0;
}

/* Find or create the carry state for `pid`. Created ONLY by the caller's
 * opening-marker branch, which is behaviour-identical to creating it on
 * first sight (every other branch is gated on ->active) and keeps the
 * footprint proportional to the pids that actually execute — a carry entry
 * embeds two ~1.5 KB partial sequences. */
static struct vi_pid_carry *vi_pid(struct pgwt_variant_index *idx, uint32_t pid)
{
    if (!idx->pid_ix) {
        if (pgwt_test_alloc_fail("variant_index_pidix"))
            return NULL;
        struct pgwt_pid_index *ix = calloc(1, sizeof(*ix));
        if (!ix)
            return NULL;
        pgwt_pid_index_init(ix);
        idx->pid_ix = ix;
    }
    struct pgwt_pid_index *ix = (struct pgwt_pid_index *)idx->pid_ix;
    int slot = pgwt_pid_index_find(ix, pid);
    if (slot >= 0)
        return &((struct vi_pid_carry *)idx->pids)[slot];

    if (idx->n_pids >= idx->cap_pids) {
        if (pgwt_test_alloc_fail("variant_index_pids"))
            return NULL;
        int newcap = idx->cap_pids ? idx->cap_pids * 2 : 16;
        struct vi_pid_carry *t = realloc(idx->pids,
                                         (size_t)newcap * sizeof(*t));
        if (!t)
            return NULL;
        memset(t + idx->cap_pids, 0,
               (size_t)(newcap - idx->cap_pids) * sizeof(*t));
        idx->pids = t;
        idx->cap_pids = newcap;
    }
    slot = idx->n_pids;
    if (pgwt_pid_index_put(ix, pid, slot) != 0)
        return NULL;
    idx->n_pids++;
    struct vi_pid_carry *c = &((struct vi_pid_carry *)idx->pids)[slot];
    memset(c, 0, sizeof(*c));
    c->pid = pid;
    return c;
}

/* Counters the test uses to prove the adversarial shapes really happened.
 * Kept in the index rather than in file statics so a test can build several
 * indexes without cross-talk. */
static int vi_open_at_end(const struct pgwt_variant_index *idx)
{
    int n = 0;
    for (int i = 0; i < idx->n_pids; i++) {
        const struct vi_pid_carry *c = &((struct vi_pid_carry *)idx->pids)[i];
        for (int p = 0; p < 2; p++)
            if (c->ph[p].active)
                n++;
    }
    return n;
}

int pgwt_variant_index_open_at_end(const struct pgwt_variant_index *idx)
{
    if (!idx || idx->failed || idx->version != PGWT_VARIANT_INDEX_VERSION)
        return -1;
    return vi_open_at_end(idx);
}

int pgwt_variant_index_discarded(const struct pgwt_variant_index *idx)
{
    if (!idx || idx->failed || idx->version != PGWT_VARIANT_INDEX_VERSION)
        return -1;
    return idx->n_discarded;
}

/* ── BUILD: close one execution into a row ────────────────── */

/* pgwt_compute_variants' marker_end branch, up to and including the
 * per-step walk, reduced to one row. The oracle's accumulator updates that
 * are per-VARIANT rather than per-execution (exec_count, total_ns,
 * loop_n_sum, the query_id set, the p95 sample) are applied at query time
 * instead, in the same close order, so the sums are the same sums. */
static int vi_close_row(struct pgwt_variant_index *idx,
                        struct vi_phase_carry *pc, uint32_t pid, int phase,
                        uint64_t close_ns)
{
    struct vi_raw_exec *re = &pc->exec;

    /* CPU-only execution (no waits). The oracle mutates re->len here,
     * BEFORE loop_n is computed from it, so raw_len is 1 and loop_n is 0
     * for a pure-CPU execution. */
    if (re->len == 0) {
        re->events[0] = 0;
        re->durations[0] = re->total_ns;
        re->len = 1;
    }

    struct vi_pattern cp;
    memset(&cp, 0, sizeof(cp));
    vi_compress_exec(re, &cp);

    if (vi_grow_rows(idx) != 0)
        return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_BUILD_FAILED);
    if (vi_grow_steps(idx, cp.num_steps) != 0)
        return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_BUILD_FAILED);

    struct pgwt_variant_index_entry *row = &idx->rows[idx->n_rows];
    memset(row, 0, sizeof(*row));
    row->pid = pid;
    row->phase = (uint8_t)phase;
    row->start_ns = pc->start_ns;
    row->close_ns = close_ns;
    row->query_id = re->query_id;
    row->total_ns = re->total_ns;
    row->hash = vi_hash_pattern(&cp);
    row->raw_len = re->len;
    row->num_steps = cp.num_steps;
    row->step_off = idx->n_steps;

    for (int s = 0; s < cp.num_steps; s++) {
        struct pgwt_variant_index_step *st = &idx->steps[idx->n_steps + s];
        st->event_id = cp.steps[s];
        st->is_loop = cp.is_loop[s];
        st->loop_len = cp.loop_len[s];
        st->count = 0;
        st->total_ns = 0;
    }

    /* The oracle's per-step walk, verbatim, accumulating into this
     * execution's own step slots instead of straight into the variant. */
    int si = 0;
    for (int j = 0; j < re->len && si < cp.num_steps; j++) {
        if (re->events[j] == cp.steps[si]) {
            struct pgwt_variant_index_step *st = &idx->steps[idx->n_steps + si];
            st->total_ns += re->durations[j];
            st->count++;
            if (!cp.is_loop[si] || j + 1 >= re->len ||
                re->events[j + 1] != cp.steps[si])
                si++;
            if (si >= cp.num_steps) si = cp.num_steps - 1;
        }
    }

    idx->n_steps += cp.num_steps;
    idx->n_rows++;
    pc->active = 0;
    memset(re, 0, sizeof(*re));
    return 0;
}

/* ── BUILD: one block ─────────────────────────────────────── */

int pgwt_variant_index_add_block(struct pgwt_variant_index *idx, int block_idx,
                                 uint64_t block_first_ns, uint64_t block_last_ns,
                                 const struct pgwt_trace_event *events,
                                 int count)
{
    if (!idx)
        return -1;
    if (idx->version != PGWT_VARIANT_INDEX_VERSION)
        return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_VERSION);
    if (idx->failed)
        return -1;
    if (idx->sealed)
        return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_GAP);
    if (count < 0 || (count > 0 && !events))
        return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_NULL);
    if (block_idx != idx->next_block_idx)
        return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_GAP);
    if (block_first_ns > block_last_ns)
        return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_GAP);
    if (idx->n_chunks > 0 &&
        block_first_ns < idx->chunks[idx->n_chunks - 1].block_first_ns)
        return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_GAP);

    if (vi_grow_chunks(idx) != 0)
        return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_BUILD_FAILED);

    int first_row = idx->n_rows;
    uint64_t last_ts = idx->last_event_ns;
    int have_last = idx->have_last_event;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];

        /* A record outside its own block's header bounds, or below the
         * previous record's timestamp, is an inconsistency: the oracle sees
         * ONE timestamp-ordered array, so this is a different input, not a
         * different index. Latch, never clamp. */
        if (ev->timestamp_ns < block_first_ns || ev->timestamp_ns > block_last_ns)
            return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_GAP);
        if (have_last && ev->timestamp_ns < last_ts)
            return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_GAP);
        last_ts = ev->timestamp_ns;
        have_last = 1;

        for (int phase = 0; phase < 2; phase++) {
            struct vi_pid_carry *c = NULL;
            struct vi_phase_carry *pc = NULL;

            if (ev->old_event == vi_marker_start[phase]) {
                c = vi_pid(idx, ev->pid);
                if (!c)
                    return vi_fail_build(idx,
                                         PGWT_VARIANT_INDEX_REFUSE_BUILD_FAILED);
                pc = &c->ph[phase];
                /* #222 shape: a second opening marker with no closing one
                 * between. The oracle resets the partial sequence and the
                 * execution it was building is never reported, in any
                 * window. Counted, not hidden. */
                if (pc->active)
                    idx->n_discarded++;
                memset(&pc->exec, 0, sizeof(pc->exec));
                pc->active = 1;
                pc->start_ns = ev->timestamp_ns;
                pc->exec.query_id = ev->query_id;
                continue;
            }

            /* No carry state and not an opening marker: the oracle's
             * `pidx < 0 && old_event != marker_start -> continue`. */
            if (!idx->pid_ix)
                continue;
            int slot = pgwt_pid_index_find((struct pgwt_pid_index *)idx->pid_ix,
                                           ev->pid);
            if (slot < 0)
                continue;
            c = &((struct vi_pid_carry *)idx->pids)[slot];
            pc = &c->ph[phase];

            if (ev->old_event == vi_marker_end[phase] && pc->active) {
                if (vi_close_row(idx, pc, ev->pid, phase,
                                 ev->timestamp_ns) != 0)
                    return -1;
                continue;
            }

            if (PGWT_IS_MARKER(ev->old_event) || PGWT_IS_MARKER(ev->new_event))
                continue;

            /* A regular record inside an open sequence. The filter test is
             * pgwt_filter_matches() with an ALL-ZERO filter, which is not a
             * no-op: it rejects a record whose duration exceeds its own end
             * timestamp. The marker half of that function is already
             * covered by the branch above. */
            if (pc->active && pc->exec.len < PGWT_VARIANT_INDEX_MAX_RAW) {
                if (ev->duration_ns > ev->timestamp_ns)
                    continue;
                if (pgwt_is_idle_event(ev->old_event))
                    continue;
                struct vi_raw_exec *re = &pc->exec;
                re->events[re->len] = ev->old_event;
                re->durations[re->len] = ev->duration_ns;
                re->total_ns += ev->duration_ns;
                re->len++;
                if (ev->query_id)
                    re->query_id = ev->query_id;
            }
        }
    }

    idx->last_event_ns = last_ts;
    idx->have_last_event = have_last;

    struct pgwt_variant_index_chunk *ch = &idx->chunks[idx->n_chunks++];
    memset(ch, 0, sizeof(*ch));
    ch->block_idx = block_idx;
    ch->first_row = first_row;
    ch->n_rows = idx->n_rows - first_row;
    ch->block_first_ns = block_first_ns;
    ch->block_last_ns = block_last_ns;
    ch->min_start_ns = UINT64_MAX;
    ch->max_close_ns = 0;

    idx->next_block_idx = block_idx + 1;
    if (block_first_ns < idx->cover_from_ns)
        idx->cover_from_ns = block_first_ns;
    if (block_last_ns > idx->cover_to_ns)
        idx->cover_to_ns = block_last_ns;
    return 0;
}

int pgwt_variant_index_seal(struct pgwt_variant_index *idx)
{
    if (!idx)
        return -1;
    if (idx->version != PGWT_VARIANT_INDEX_VERSION)
        return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_VERSION);
    if (idx->failed)
        return -1;
    for (int c = 0; c < idx->n_chunks; c++) {
        struct pgwt_variant_index_chunk *ch = &idx->chunks[c];
        ch->min_start_ns = UINT64_MAX;
        ch->max_close_ns = 0;
        for (int r = ch->first_row; r < ch->first_row + ch->n_rows; r++) {
            /* A row's start_ns may lie many blocks earlier than its close —
             * that IS the carry-over — so min_start is computed, never
             * assumed to be the block's own first timestamp. */
            if (idx->rows[r].start_ns < ch->min_start_ns)
                ch->min_start_ns = idx->rows[r].start_ns;
            if (idx->rows[r].close_ns > ch->max_close_ns)
                ch->max_close_ns = idx->rows[r].close_ns;
        }
    }
    idx->sealed = 1;
    return 0;
}

/* ── the selection predicate and the filter gate ──────────── */

int pgwt_variant_index_selects(uint64_t start_ns, uint64_t close_ns,
                               uint64_t from_ns, uint64_t to_ns)
{
    /* Both markers must be selected by the loader, whose window is
     * INCLUSIVE at both ends and keyed on the record's end timestamp. */
    return start_ns >= from_ns && close_ns <= to_ns;
}

int pgwt_variant_index_filter_ok(const struct pgwt_filter *f)
{
    if (!f)
        return 0;   /* NULL means "no filter at all" to the oracle, which
                     * keeps records this index drops. A different answer. */
    return f->class_name[0] == '\0' && f->event_id == 0 && f->pid == 0 &&
           f->query_id == 0;
}

/* ── QUERY ────────────────────────────────────────────────── */

struct vi_accum {
    uint64_t hash;
    int      used;
    struct vi_pattern pattern;
    int      exec_count;
    uint64_t total_ns;
    uint64_t *exec_times;
    int      exec_times_cap;
    int      exec_times_n;
    double   loop_n_sum;
    int      loop_n_count;
    uint64_t query_ids[16];
    int      num_query_ids;
    uint64_t step_total_ns[PGWT_MAX_VARIANT_STEPS];
    int      step_count[PGWT_MAX_VARIANT_STEPS];
};

static void vi_accum_add_qid(struct vi_accum *va, uint64_t qid)
{
    if (qid == 0) return;
    for (int i = 0; i < va->num_query_ids; i++)
        if (va->query_ids[i] == qid) return;
    if (va->num_query_ids < 16)
        va->query_ids[va->num_query_ids++] = qid;
}

/* Every refusal goes through here: res zeroed, counts POISONED, failed set.
 * The poison is not the guard — -1 is truthy — res.failed and
 * sequences_indexed are. */
static int vi_query_refuse(struct pgwt_variant_index_query_result *out,
                           enum pgwt_variant_index_refusal why)
{
    if (!out)
        return -1;
    free(out->res.variants);
    memset(&out->res, 0, sizeof(out->res));
    out->res.variants = NULL;
    out->res.num_variants = PGWT_VARIANT_INDEX_NOT_INDEXED;
    out->res.total_executions = PGWT_VARIANT_INDEX_NOT_INDEXED;
    out->res.failed = 1;
    out->sequences_indexed = 0;
    out->refused = why;
    return -1;
}

void pgwt_variant_index_query_free(struct pgwt_variant_index_query_result *out)
{
    if (!out)
        return;
    free(out->res.variants);
    out->res.variants = NULL;
    out->res.num_variants = 0;
}

int pgwt_variant_index_query(const struct pgwt_variant_index *idx,
                             uint64_t from_ns, uint64_t to_ns,
                             const struct pgwt_filter *f, int max_variants,
                             enum pgwt_variant_phase phase,
                             struct pgwt_variant_index_query_result *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!idx)
        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_NULL);
    if (idx->version != PGWT_VARIANT_INDEX_VERSION)
        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_VERSION);
    if (idx->failed)
        return vi_query_refuse(out, idx->build_refusal == PGWT_VARIANT_INDEX_OK
                               ? PGWT_VARIANT_INDEX_REFUSE_BUILD_FAILED
                               : idx->build_refusal);
    if (!idx->sealed)
        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_UNSEALED);
    if (phase != PGWT_PHASE_EXEC && phase != PGWT_PHASE_PLAN)
        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_PHASE);
    if (!pgwt_variant_index_filter_ok(f))
        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_FILTER);
    if (from_ns > to_ns)
        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_BAD_WINDOW);
    if (idx->n_chunks == 0)
        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_EMPTY);
    /* Containment, not intersection. A window reaching past the indexed
     * range must never be answered for its covered part only. */
    if (from_ns < idx->cover_from_ns || to_ns > idx->cover_to_ns)
        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_RANGE);

    if (pgwt_test_alloc_fail("variant_index_query_ht"))
        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_ALLOC);
    struct vi_accum *ht = calloc(PGWT_VARIANT_INDEX_HT_SIZE, sizeof(*ht));
    if (!ht)
        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_ALLOC);

    out->chunks_total = idx->n_chunks;
    int total_execs = 0;
    int alloc_failed = 0;

    for (int c = 0; c < idx->n_chunks && !alloc_failed; c++) {
        const struct pgwt_variant_index_chunk *ch = &idx->chunks[c];
        /* Prefilter. No row in this chunk can satisfy the selection
         * predicate when every close is below `from` or every start is
         * above `to`. Inclusive comparisons, matching the predicate. */
        if (ch->n_rows == 0 || ch->max_close_ns < from_ns ||
            ch->min_start_ns > to_ns)
            continue;
        out->chunks_scanned++;

        for (int r = ch->first_row; r < ch->first_row + ch->n_rows; r++) {
            const struct pgwt_variant_index_entry *e = &idx->rows[r];
            out->rows_examined++;
            if (e->phase != (uint8_t)phase)
                continue;
            if (!pgwt_variant_index_selects(e->start_ns, e->close_ns,
                                            from_ns, to_ns))
                continue;
            out->rows_selected++;
            total_execs++;

            /* Rebuild the compressed pattern from the stored steps. */
            struct vi_pattern cp;
            memset(&cp, 0, sizeof(cp));
            cp.num_steps = e->num_steps;
            for (int s = 0; s < e->num_steps; s++) {
                const struct pgwt_variant_index_step *st =
                    &idx->steps[e->step_off + s];
                cp.steps[s] = st->event_id;
                cp.is_loop[s] = st->is_loop;
                cp.loop_len[s] = st->loop_len;
            }
            out->steps_examined += e->num_steps;

            double loop_n = 0;
            if (e->raw_len > 1)
                loop_n = cp.num_steps > 0
                       ? (double)e->raw_len / cp.num_steps : 1;

            uint32_t slot = (uint32_t)(e->hash & (PGWT_VARIANT_INDEX_HT_SIZE - 1));
            while (ht[slot].used && ht[slot].hash != e->hash)
                slot = (slot + 1) & (PGWT_VARIANT_INDEX_HT_SIZE - 1);
            struct vi_accum *va = &ht[slot];
            if (!va->used) {
                va->used = 1;
                va->hash = e->hash;
                va->pattern = cp;
            }
            va->exec_count++;
            va->total_ns += e->total_ns;
            va->loop_n_sum += loop_n;
            va->loop_n_count++;
            vi_accum_add_qid(va, e->query_id);

            if (va->exec_times_n < PGWT_VARIANT_INDEX_MAX_SAMPLES) {
                if (va->exec_times_n >= va->exec_times_cap) {
                    int newcap = va->exec_times_cap ? va->exec_times_cap * 2 : 64;
                    if (newcap > PGWT_VARIANT_INDEX_MAX_SAMPLES)
                        newcap = PGWT_VARIANT_INDEX_MAX_SAMPLES;
                    uint64_t *tmp = pgwt_test_alloc_fail("variant_index_samples")
                        ? NULL
                        : realloc(va->exec_times, newcap * sizeof(uint64_t));
                    if (tmp) { va->exec_times = tmp; va->exec_times_cap = newcap; }
                    else { alloc_failed = 1; break; }
                }
                if (va->exec_times && va->exec_times_n < va->exec_times_cap)
                    va->exec_times[va->exec_times_n++] = e->total_ns;
            }

            for (int s = 0; s < e->num_steps && s < PGWT_MAX_VARIANT_STEPS; s++) {
                const struct pgwt_variant_index_step *st =
                    &idx->steps[e->step_off + s];
                va->step_total_ns[s] += st->total_ns;
                va->step_count[s] += st->count;
            }
        }
    }

    /* The oracle degrades here (it drops one p95 sample and carries on);
     * this REFUSES, because an index that cannot establish the answer must
     * never present a nearly-right one. The caller then recomputes from
     * raw and gets the oracle's own behaviour. */
    if (alloc_failed) {
        for (int i = 0; i < PGWT_VARIANT_INDEX_HT_SIZE; i++)
            free(ht[i].exec_times);
        free(ht);
        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_ALLOC);
    }

    int nv = 0;
    for (int i = 0; i < PGWT_VARIANT_INDEX_HT_SIZE; i++)
        if (ht[i].used) nv++;

    struct pgwt_variant *variants =
        pgwt_test_alloc_fail("variant_index_query_out")
        ? NULL : calloc(nv ? (size_t)nv : 1, sizeof(*variants));
    if (!variants) {
        for (int i = 0; i < PGWT_VARIANT_INDEX_HT_SIZE; i++)
            free(ht[i].exec_times);
        free(ht);
        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_ALLOC);
    }

    int vi = 0;
    for (int i = 0; i < PGWT_VARIANT_INDEX_HT_SIZE && vi < nv; i++) {
        if (!ht[i].used) continue;
        struct vi_accum *va = &ht[i];
        struct pgwt_variant *v = &variants[vi++];

        v->hash = va->hash;
        v->exec_count = va->exec_count;
        v->num_query_ids = va->num_query_ids;
        v->total_ns = va->total_ns;
        v->avg_ns = va->exec_count > 0 ? va->total_ns / va->exec_count : 0;
        v->avg_loop_n = va->loop_n_count > 0
                      ? va->loop_n_sum / va->loop_n_count : 1;
        v->num_steps = va->pattern.num_steps;
        v->top_query_id = va->num_query_ids > 0 ? va->query_ids[0] : 0;

        v->p95_sample_n = va->exec_times_n;
        if (va->exec_times && va->exec_times_n > 0) {
            int n = va->exec_times_n;
            pgwt_sort_u64_asc(va->exec_times, n);
            v->p95_ns = va->exec_times[(int)(n * 0.95)];
        }

        for (int s = 0; s < va->pattern.num_steps && s < PGWT_MAX_VARIANT_STEPS; s++) {
            v->steps[s].event_id = va->pattern.steps[s];
            v->steps[s].is_loop = va->pattern.is_loop[s];
            v->steps[s].loop_len = va->pattern.loop_len[s];
            if (va->pattern.steps[s] == 0)
                snprintf(v->steps[s].name, 64, "CPU*");
            else
                pgwt_event_full_name(va->pattern.steps[s],
                                     v->steps[s].name, sizeof(v->steps[s].name));
            v->step_avg_ns[s] = va->step_count[s] > 0
                ? va->step_total_ns[s] / va->step_count[s] : 0;
        }

        free(va->exec_times);
    }

    qsort(variants, vi, sizeof(variants[0]), vi_cmp_variant_time_desc);

    int nr = vi < max_variants ? vi : max_variants;
    out->res.variants = variants;
    out->res.num_variants = nr;
    out->res.total_executions = total_execs;
    out->res.failed = 0;
    out->sequences_indexed = 1;
    out->refused = PGWT_VARIANT_INDEX_OK;

    free(ht);
    return 0;
}
