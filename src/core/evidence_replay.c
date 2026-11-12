#include "evidence_replay.h"

#include <stdlib.h>
#include <string.h>

static bool same(pgwt_core_event a, pgwt_core_event b)
{
    return a.length == b.length &&
           (!a.length || (a.data && b.data && !memcmp(a.data, b.data, a.length)));
}

static bool null_event(pgwt_core_event e) { return !e.data && !e.length; }
static bool nonempty(pgwt_core_event e) { return e.data && e.length; }
static bool name(pgwt_core_event e)
{
    const pgwt_core_event exit_name = {"$EXIT", 5};
    return nonempty(e) && !same(e, exit_name);
}
static bool state(pgwt_core_event e) { return null_event(e) || name(e); }

static bool zero_counters(const pgwt_evidence_counters *c)
{
    if (!c->known) return false;
    for (size_t i = 0; i < 15; ++i) if (c->value[i]) return false;
    return true;
}

void pgwt_evidence_output_free(pgwt_evidence_output *out)
{
    if (out) { free(out->waits); memset(out, 0, sizeof(*out)); }
}

static pgwt_replay_status unsupported(const char **reason, const char *why)
{
    if (reason) *reason = why;
    return PGWT_REPLAY_UNSUPPORTED;
}

static bool exit_event(pgwt_core_event e)
{
    return same(e, (pgwt_core_event){"$EXIT", 5});
}

static bool key(const pgwt_evidence_record *a, const pgwt_evidence_record *b)
{
    return a->has_fork == b->has_fork && (!a->has_fork || a->fork == b->fork);
}

/* Validate every capture, including uncertified suffixes. Rotation, loss,
 * multiple PIDs and multiple attachments per generation remain outside U3. */
static pgwt_replay_status scope(const pgwt_evidence_input *in, const char **why)
{
    if (!in || in->unsupported_shape || !in->files)
        return unsupported(why, "unsupported file/run structure");
    size_t targets = 0;
    for (size_t fno = 0; fno < in->file_count; ++fno) {
        const pgwt_evidence_file *f = &in->files[fno];
        if (same(f->capture, in->capture)) ++targets;
        if (f->unsupported_shape || !f->present || f->file_no || f->has_previous ||
            !nonempty(f->boot) || !nonempty(f->capture) || !in->pid ||
            (f->record_count && !f->records) || !zero_counters(&f->baseline) ||
            f->version < 2 || f->version > 5)
            return unsupported(why, "unsupported descriptor or counters");
        for (size_t j = 0; j < fno; ++j)
            if (same(in->files[j].capture, f->capture) || !same(in->files[j].boot, f->boot))
                return unsupported(why, "unsupported rotation or clock domain");
        for (size_t i = 0; i < f->record_count; ++i) {
            const pgwt_evidence_record *r = &f->records[i];
            if (r->unsupported_shape)
                return unsupported(why, "unsupported record shape");
            if (r->kind != PGWT_EVIDENCE_LIFE && r->kind != PGWT_EVIDENCE_ATTACH_OK &&
                r->kind != PGWT_EVIDENCE_TRANS && r->kind != PGWT_EVIDENCE_FRONTIER)
                return unsupported(why, "unsupported record kind");
            if (r->kind != PGWT_EVIDENCE_FRONTIER && r->pid != in->pid)
                return unsupported(why, "unsupported generation");
            if (r->kind == PGWT_EVIDENCE_LIFE) {
                if ((r->start_known ? (!r->has_fork || r->start != r->fork) : r->has_fork) ||
                    r->start > r->t || (r->has_exit && (r->exit < r->start || r->exit > r->t)))
                    return unsupported(why, "unsupported lifetime");
            } else if (r->kind == PGWT_EVIDENCE_ATTACH_OK) {
                if (!state(r->seed) || !state(r->state))
                    return unsupported(why, "unsupported attachment");
                for (size_t j = 0; j < i; ++j)
                    if (f->records[j].kind == PGWT_EVIDENCE_ATTACH_OK && key(r, &f->records[j]))
                        return unsupported(why, "unsupported overlapping attachment");
                if (!zero_counters(&r->counters))
                    return unsupported(why, "unsupported attachment counters");
            } else if (r->kind == PGWT_EVIDENCE_FRONTIER) {
                if (!zero_counters(&r->counters))
                    return unsupported(why, "unsupported frontier counters");
            } else if (!state(r->old_event) || (!state(r->new_event) && !exit_event(r->new_event)) ||
                       same(r->old_event, r->new_event) || r->duration > r->t) {
                return unsupported(why, "unsupported transition");
            }
        }
    }
    if (targets != 1) return unsupported(why, "unsupported target capture");
    return PGWT_REPLAY_OK;
}

/* A generation belongs to exactly one capture. LIFE updates address the key,
 * never the most recently observed PID. Discovery bounds assignment only;
 * the conservative LIFE.start remains the core lifetime's lower bound. */
typedef struct {
    const pgwt_evidence_record *identity, *life, *attach;
    uint64_t discovery;
    bool invalid;
} generation;

static size_t generation_for(generation *g, size_t *count, const pgwt_evidence_record *r)
{
    for (size_t i = 0; i < *count; ++i) if (key(g[i].identity, r)) return i;
    size_t i = (*count)++;
    g[i].identity = r;
    return i;
}

static void lifetime(generation *g, const pgwt_evidence_record *r)
{
    if (g->life && (g->life->start != r->start ||
                   (g->life->has_exit && (!r->has_exit || g->life->exit != r->exit)))) {
        g->invalid = true;
        return;
    }
    if (!g->life) g->discovery = r->t;
    g->life = r;
}

typedef struct { const pgwt_evidence_record *record; size_t order; } transition;
static int transition_order(const void *av, const void *bv)
{
    const transition *a = av, *b = bv;
    if (a->record->t != b->record->t) return a->record->t < b->record->t ? -1 : 1;
    return a->order < b->order ? -1 : a->order != b->order;
}

/* Replay one independent capture. Every frontier resolves its physical prefix
 * against the lifetime facts available there. Later LIFE cannot certify an
 * earlier prefix. Even non-target generations must pass run consistency. */
static pgwt_replay_status replay_file(const pgwt_evidence_file *file,
                                      const pgwt_evidence_input *in,
                                      pgwt_evidence_output *out, const char **reason)
{
    pgwt_replay_status status = PGWT_REPLAY_OK;
    size_t n = file->record_count, count = 0, generations = 0;
    transition *history = NULL;
    generation *g = NULL;
    pgwt_core_wait *candidate = NULL;
    size_t *assigned = NULL;
    pgwt_core_evidence *e = &out->projection;
    const bool target_capture = same(file->capture, in->capture);
    if (n > SIZE_MAX / sizeof(*history) || n > SIZE_MAX / sizeof(*g) ||
        n > SIZE_MAX / sizeof(*candidate) || n > SIZE_MAX / sizeof(*assigned))
        goto allocation_error;
    if (n) {
        history = calloc(n, sizeof(*history));
        g = calloc(n, sizeof(*g));
        candidate = calloc(n, sizeof(*candidate));
        assigned = calloc(n, sizeof(*assigned));
        if (!history || !g || !candidate || !assigned) goto allocation_error;
    }
    for (size_t i = 0; i < n; ++i)
        if (file->records[i].kind == PGWT_EVIDENCE_TRANS)
            history[count++] = (transition){&file->records[i], i};
    if (count) qsort(history, count, sizeof(*history), transition_order);
    /* Final LIFE is used only to filter snapshots whose missing terminal close
     * becomes apparent later. Assignment still uses each frontier's prefix. */
    generation final = {0};
    for (size_t i = 0; i < n; ++i) {
        const pgwt_evidence_record *r = &file->records[i];
        if (file->version == 5 && r->kind == PGWT_EVIDENCE_LIFE &&
            r->has_fork == in->has_fork && (!r->has_fork || r->fork == in->fork))
            lifetime(&final, r);
    }
    if (target_capture && final.life && !final.invalid) {
        e->invalid_fields = 0;
        e->life_start = final.life->start;
        e->has_life_end = final.life->has_exit;
        e->life_end = final.life->exit;
    }
    bool has_frontier = false;
    uint64_t frontier = 0;
    for (size_t i = 0; i < n; ++i) {
        const pgwt_evidence_record *r = &file->records[i];
        if (file->version != 5) continue;
        if (r->kind == PGWT_EVIDENCE_LIFE)
            lifetime(&g[generation_for(g, &generations, r)], r);
        if (!file->qualified) continue;
        if (r->kind == PGWT_EVIDENCE_ATTACH_OK)
            g[generation_for(g, &generations, r)].attach = r;
        if (r->kind != PGWT_EVIDENCE_FRONTIER) continue;
        if (has_frontier && r->t <= frontier) {
            status = unsupported(reason, "unsupported nonadvancing frontier"); goto done;
        }
        has_frontier = true;
        frontier = r->t;
        /* SIZE_MAX marks zero/multiple possible identities. Such a dependency
         * withholds this PID's certification, preserving prior snapshots. */
        bool blocked = false;
        for (size_t j = 0; j < count; ++j) {
            assigned[j] = SIZE_MAX;
            const pgwt_evidence_record *t = history[j].record;
            if (history[j].order >= i || t->t > frontier) continue;
            size_t choices = 0;
            for (size_t k = 0; k < generations; ++k) {
                const pgwt_evidence_record *life = g[k].life;
                if (!life || g[k].invalid) continue;
                uint64_t start = life->has_fork ? life->fork : g[k].discovery;
                bool matches = exit_event(t->new_event) ? (life->has_exit && life->exit == t->t) :
                    (start <= t->t && (!life->has_exit || t->t < life->exit));
                if (matches) { assigned[j] = k; ++choices; }
            }
            if (choices != 1) blocked = true;
        }
        for (size_t k = 0; k < generations; ++k) {
            const pgwt_evidence_record *life = g[k].life, *attach = g[k].attach;
            if (!life || g[k].invalid || !attach) continue;
            if (attach->t < life->start || (life->has_exit && attach->t >= life->exit)) {
                status = unsupported(reason, "unsupported attachment lifetime"); goto done;
            }
            if (blocked || attach->t > frontier) continue;
            uint64_t endpoint = life->has_exit && life->exit < frontier ? life->exit : frontier;
            pgwt_core_event current = attach->state;
            uint64_t entry = 0;
            uint64_t trimmed = attach->t;
            bool entry_known = false, portion = null_event(current);
            bool anchored = attach->reread;
            if (attach->reread && !same(attach->seed, current)) {
                status = unsupported(reason, "unsupported reread seed mismatch"); goto done;
            }
            size_t waits = 0;
            bool terminal = false;
            for (size_t j = 0; j < count; ++j) {
                const pgwt_evidence_record *t = history[j].record;
                if (assigned[j] != k) continue;
                /* Timestamp order, not append position relative to ATTACH_OK,
                 * selects the first transition at the anchor, including ties. */
                if (t->t < attach->t) continue;
                if (!anchored) {
                    if (t->t != attach->t) break;
                    if (!same(t->new_event, current)) {
                        status = unsupported(reason, "unsupported anchor state mismatch"); goto done;
                    }
                    anchored = true;
                    entry = t->t;
                    entry_known = !null_event(current);
                    portion = true;
                    if (!null_event(t->old_event)) {
                        portion = t->t != UINT64_MAX;
                        trimmed = t->t + 1; /* Wrapped value is never projected. */
                    }
                    continue; /* The anchor's old side is untrusted. */
                }
                /* A reread reconciles transitions THROUGH its anchor. */
                if (attach->reread && t->t == attach->t) continue;
                if (terminal || !same(t->old_event, current) ||
                    (entry_known && t->t - t->duration != entry)) {
                    status = unsupported(reason, "unsupported inconsistent run"); goto done;
                }
                bool unknown_close = !null_event(current) && !entry_known;
                if (entry_known)
                    candidate[waits++] = (pgwt_core_wait){current, entry, t->t};
                if (unknown_close) {
                    portion = t->t != UINT64_MAX;
                    trimmed = t->t + 1; /* Wrapped value is never projected. */
                }
                terminal = exit_event(t->new_event);
                current = terminal ? (pgwt_core_event){0} : t->new_event;
                entry = t->t;
                entry_known = !null_event(current);
            }
            bool target = target_capture && key(g[k].identity, &(pgwt_evidence_record){
                .has_fork = in->has_fork, .fork = in->fork});
            if (!target || final.invalid) continue;
            if (e->has_life_end && endpoint >= e->life_end && !terminal) continue;
            if (!anchored || !portion || trimmed > endpoint) continue;
            memcpy(out->waits, candidate, waits * sizeof(*candidate));
            e->wait_count = waits;
            out->coverage = (pgwt_core_span){trimmed, endpoint};
            e->coverage = &out->coverage;
            e->coverage_count = 1;
            e->has_open = entry_known;
            e->open = (pgwt_core_open_wait){current, entry, endpoint};
        }
    }
    goto done;
allocation_error:
    status = PGWT_REPLAY_ERROR;
    if (reason) *reason = "replay allocation failed";
done:
    free(history); free(g); free(candidate); free(assigned);
    return status;
}

pgwt_replay_status pgwt_evidence_replay(const pgwt_evidence_input *in,
                                       const pgwt_core_query *query,
                                       pgwt_evidence_output *out,
                                       const char **reason)
{
    if (reason) *reason = NULL;
    if (!out) { if (reason) *reason = "null output"; return PGWT_REPLAY_ERROR; }
    memset(out, 0, sizeof(*out));
    pgwt_replay_status status = scope(in, reason);
    if (status != PGWT_REPLAY_OK) return status;
    size_t capacity = 0;
    for (size_t i = 0; i < in->file_count; ++i)
        if (same(in->files[i].capture, in->capture)) capacity = in->files[i].record_count;
    pgwt_core_wait *scratch = NULL;
    if (capacity > SIZE_MAX / sizeof(*out->waits)) goto allocation_error;
    if (capacity) {
        out->waits = calloc(capacity, sizeof(*out->waits));
        scratch = calloc(capacity, sizeof(*scratch));
        if (!out->waits || !scratch) goto allocation_error;
    }
    out->projection.invalid_fields = PGWT_CORE_LIFE_INVALID;
    for (size_t i = 0; i < in->file_count; ++i) {
        status = replay_file(&in->files[i], in, out, reason);
        if (status != PGWT_REPLAY_OK) goto done;
    }
    out->projection.waits = out->waits;
    pgwt_core_prepared prepared;
    if (pgwt_core_prepare(&out->projection, scratch, capacity, &prepared)) goto allocation_error;
    pgwt_core_wait_metrics(&prepared, query, &out->wait_time, &out->completed_waits);
    status = PGWT_REPLAY_OK;
    goto done;
allocation_error:
    status = PGWT_REPLAY_ERROR;
    if (reason) *reason = "replay allocation/preparation failed";
done:
    free(scratch);
    if (status != PGWT_REPLAY_OK) pgwt_evidence_output_free(out);
    return status;
}
