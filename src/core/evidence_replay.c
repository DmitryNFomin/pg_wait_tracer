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

/* Validate the ENTIRE supplied file before computing any metric, including
 * unqualified/legacy files and records beyond the last frontier. U1 must not
 * silently turn future storage semantics into an apparently usable result. */
static pgwt_replay_status scope(const pgwt_evidence_input *in, const char **why)
{
    if (!in || in->unsupported_shape || in->file_count != 1 || !in->files)
        return unsupported(why, "unsupported file/run structure");
    const pgwt_evidence_file *f = in->files;
    if (f->unsupported_shape || !f->present || f->file_no || f->has_previous ||
        !nonempty(f->boot) || !nonempty(f->capture) || !same(f->capture, in->capture) ||
        !in->pid || (f->record_count && !f->records) ||
        !zero_counters(&f->baseline) || f->version < 2 || f->version > 5)
        return unsupported(why, "unsupported descriptor or counters");
    size_t attachments = 0;
    bool life_seen = false;
    uint64_t life_start = 0;
    for (size_t i = 0; i < f->record_count; ++i) {
        const pgwt_evidence_record *r = &f->records[i];
        if (r->unsupported_shape)
            return unsupported(why, "unsupported record shape");
        if (r->kind != PGWT_EVIDENCE_LIFE && r->kind != PGWT_EVIDENCE_ATTACH_OK &&
            r->kind != PGWT_EVIDENCE_TRANS && r->kind != PGWT_EVIDENCE_FRONTIER)
            return unsupported(why, "unsupported record kind");
        if (r->kind != PGWT_EVIDENCE_FRONTIER && r->pid != in->pid)
            return unsupported(why, "unsupported generation");
        if (r->kind == PGWT_EVIDENCE_LIFE || r->kind == PGWT_EVIDENCE_ATTACH_OK) {
            if (r->has_fork != in->has_fork || (r->has_fork && r->fork != in->fork))
                return unsupported(why, "unsupported generation");
        }
        if (r->kind == PGWT_EVIDENCE_LIFE) {
            if (!r->start_known || !r->has_fork || r->start != r->fork ||
                r->start > r->t || r->has_exit || (life_seen && r->start != life_start))
                return unsupported(why, "unsupported lifetime");
            life_seen = true;
            life_start = r->start;
        } else if (r->kind == PGWT_EVIDENCE_ATTACH_OK) {
            if (++attachments > 1 || !r->reread || !null_event(r->seed) || !null_event(r->state))
                return unsupported(why, "unsupported attachment");
            if (!zero_counters(&r->counters))
                return unsupported(why, "unsupported attachment counters");
        } else if (r->kind == PGWT_EVIDENCE_FRONTIER) {
            if (!zero_counters(&r->counters))
                return unsupported(why, "unsupported frontier counters");
        } else if (!state(r->old_event) || !state(r->new_event) ||
                   same(r->old_event, r->new_event) || r->duration > r->t) {
            return unsupported(why, "unsupported transition");
        }
    }
    return PGWT_REPLAY_OK;
}

typedef struct { const pgwt_evidence_record *record; size_t order; } transition;
static int transition_order(const void *av, const void *bv)
{
    const transition *a = av, *b = bv;
    if (a->record->t != b->record->t) return a->record->t < b->record->t ? -1 : 1;
    return a->order < b->order ? -1 : a->order != b->order;
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
    const pgwt_evidence_file *file = in->files;
    pgwt_core_evidence *e = &out->projection;
    e->invalid_fields = PGWT_CORE_LIFE_INVALID;
    transition *history = NULL;
    pgwt_core_wait *scratch = NULL;
    size_t count = 0;
    if (file->record_count > SIZE_MAX / sizeof(*history) ||
        file->record_count > SIZE_MAX / sizeof(*out->waits)) goto allocation_error;
    if (file->record_count) {
        history = calloc(file->record_count, sizeof(*history));
        out->waits = calloc(file->record_count, sizeof(*out->waits));
        scratch = calloc(file->record_count, sizeof(*scratch));
        if (!history || !out->waits || !scratch) goto allocation_error;
    }
    for (size_t i = 0; i < file->record_count; ++i)
        if (file->records[i].kind == PGWT_EVIDENCE_TRANS)
            history[count++] = (transition){&file->records[i], i};
    if (count) qsort(history, count, sizeof(*history), transition_order);
    const pgwt_evidence_record *attach = NULL;
    bool has_frontier = false;
    uint64_t frontier = 0;
    /* Each FRONTIER replays only its physical prefix, timestamp-sorted with
     * append order at ties. O(records * transitions), with one initial sort.
     * Later LIFE/TRANS cannot retroactively certify an earlier prefix. */
    for (size_t i = 0; i < file->record_count; ++i) {
        const pgwt_evidence_record *r = &file->records[i];
        if (file->version != 5) continue;
        if (r->kind == PGWT_EVIDENCE_LIFE) {
            e->life_start = r->start;
            e->invalid_fields = 0;
        }
        if (!file->qualified) continue;
        if (r->kind == PGWT_EVIDENCE_ATTACH_OK) attach = r;
        if (r->kind != PGWT_EVIDENCE_FRONTIER) continue;
        if (has_frontier && r->t <= frontier) {
            status = unsupported(reason, "unsupported nonadvancing frontier"); goto done;
        }
        has_frontier = true;
        frontier = r->t;
        if (!attach || attach->t > frontier || e->invalid_fields) continue;
        if (attach->t < e->life_start) {
            status = unsupported(reason, "unsupported attachment lifetime"); goto done;
        }
        pgwt_core_event current = {0};
        uint64_t entry = 0;
        e->wait_count = 0;
        for (size_t j = 0; j < count; ++j) {
            const pgwt_evidence_record *t = history[j].record;
            if (history[j].order >= i || t->t > frontier) continue;
            if (t->t < e->life_start) {
                status = unsupported(reason, "unsupported transition identity"); goto done;
            }
            /* A reread already reconciles transitions THROUGH its anchor. */
            if (t->t <= attach->t) continue;
            if (!same(t->old_event, current) ||
                (!null_event(current) && t->t - t->duration != entry)) {
                status = unsupported(reason, "unsupported inconsistent run"); goto done;
            }
            if (!null_event(current))
                out->waits[e->wait_count++] = (pgwt_core_wait){current, entry, t->t};
            current = t->new_event;
            entry = t->t;
        }
        out->coverage = (pgwt_core_span){attach->t, frontier};
        e->coverage = &out->coverage;
        e->coverage_count = 1;
        e->has_open = !null_event(current);
        e->open = (pgwt_core_open_wait){current, entry, frontier};
    }
    e->waits = out->waits;
    pgwt_core_prepared prepared;
    if (pgwt_core_prepare(e, scratch, file->record_count, &prepared)) goto allocation_error;
    pgwt_core_wait_metrics(&prepared, query, &out->wait_time, &out->completed_waits);
    status = PGWT_REPLAY_OK;
    goto done;
allocation_error:
    status = PGWT_REPLAY_ERROR;
    if (reason) *reason = "replay allocation/preparation failed";
done:
    free(history);
    free(scratch);
    if (status != PGWT_REPLAY_OK) pgwt_evidence_output_free(out);
    return status;
}
