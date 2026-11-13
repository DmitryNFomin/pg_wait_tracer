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

static bool equal_counters(const pgwt_evidence_counters *a, const pgwt_evidence_counters *b)
{
    /* Unknown vectors cannot certify, even against another unknown. */
    if (!a->known || !b->known) return false;
    for (size_t i = 0; i < 15; ++i) if (a->value[i] != b->value[i]) return false;
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
    return a->pid == b->pid && a->has_fork == b->has_fork && (!a->has_fork || a->fork == b->fork);
}

/* A logical capture is replayed in physical append order across its validated
 * file prefix. Stop at a gap: later files cannot repair lost dependencies.
 * Copies own record arrays only; event bytes continue to borrow the input. */
typedef struct {
    pgwt_evidence_input input;
    bool **descriptors; /* Private boundary markers, never taken from record fields. */
} joined_input;

static void free_chains(joined_input *joined_data)
{
    pgwt_evidence_input *joined = &joined_data->input;
    for (size_t i = 0; i < joined->file_count; ++i) {
        free((void *)joined->files[i].records);
        free(joined_data->descriptors[i]);
    }
    free(joined_data->descriptors);
    free((void *)joined->files);
}

static pgwt_replay_status join_chains(const pgwt_evidence_input *in,
                                      joined_input *joined_data, const char **why)
{
    *joined_data = (joined_input){0};
    pgwt_evidence_input *joined = &joined_data->input;
    *joined = (pgwt_evidence_input){0};
    if (!in || in->unsupported_shape || (in->file_count && !in->files) || !in->pid || !nonempty(in->capture))
        return PGWT_REPLAY_ERROR;
    if (in->file_count > SIZE_MAX / sizeof(pgwt_evidence_file))
        return PGWT_REPLAY_ERROR;
    pgwt_evidence_file *files = calloc(in->file_count + 1, sizeof(*files));
    if (!files) return PGWT_REPLAY_ERROR;
    joined_data->descriptors = calloc(in->file_count + 1, sizeof(bool *));
    if (!joined_data->descriptors) { free(files); return PGWT_REPLAY_ERROR; }
    *joined = *in;
    joined->files = files;
    joined->file_count = 0;
    bool bound = false;
    for (size_t i = 0; i < in->file_count; ++i) {
        const pgwt_evidence_file *f = &in->files[i];
        if (!nonempty(f->capture)) return PGWT_REPLAY_ERROR;
        bool seen = false;
        for (size_t j = 0; j < joined->file_count; ++j)
            if (same(files[j].capture, f->capture)) seen = true;
        if (seen) continue;
        pgwt_evidence_file *chain = &files[joined->file_count++];
        *chain = (pgwt_evidence_file){.version = 5, .boot = f->boot,
            .capture = f->capture, .present = true};
        bool started = false;
        uint64_t last = 0;
        for (size_t j = i; j < in->file_count; ++j) {
            const pgwt_evidence_file *part = &in->files[j];
            if (!same(part->capture, f->capture)) continue;
            if (started && !same(part->boot, chain->boot)) break;
            bool linked = !started ? (!part->file_no && !part->has_previous) :
                (last != UINT64_MAX && part->file_no == last + 1 &&
                 part->has_previous && part->previous == last);
            if (part->unsupported_shape)
                break;
            if (!nonempty(part->boot) || !part->present || !linked ||
                part->version < 2 || part->version > 5) break;
            /* Legacy has no evidence continuity into a v5 successor. */
            if (started && chain->version != 5) break;
            if (!started) {
                chain->boot = part->boot;
                if (same(part->capture, in->capture)) bound = true;
                chain->version = part->version;
                chain->qualified = part->qualified;
                chain->baseline = part->baseline;
            } else if (part->version != 5) break;
            if (part->record_count && !part->records)
                break;
            size_t extra = started && part->version == 5;
            if (part->record_count >= SIZE_MAX / sizeof(pgwt_evidence_record) - chain->record_count)
                return PGWT_REPLAY_ERROR;
            size_t total = chain->record_count + extra + part->record_count;
            if (part->record_count || extra) {
                pgwt_evidence_record *records = realloc((void *)chain->records, total * sizeof(*records));
                if (!records) return PGWT_REPLAY_ERROR;
                chain->records = records;
                bool **markers = &joined_data->descriptors[joined->file_count - 1];
                bool *resized = realloc(*markers, total * sizeof(*resized));
                if (!resized) return PGWT_REPLAY_ERROR;
                *markers = resized;
                memset(resized + chain->record_count, 0, (total - chain->record_count) * sizeof(*resized));
                if (extra) {
                    resized[chain->record_count] = true;
                    records[chain->record_count] = (pgwt_evidence_record){.counters = part->baseline};
                }
                if (part->record_count)
                    memcpy(records + chain->record_count + extra, part->records, part->record_count * sizeof(*records));
            }
            chain->record_count = total;
            last = part->file_no;
            started = true;
        }
    }
    bool boot_seen = false, unique_boot = true;
    pgwt_core_event boot = {0};
    if (!bound) {
        for (size_t i = 0; i < in->file_count; ++i) {
            if (!same(in->files[i].capture, in->capture)) continue;
            if (boot_seen && !same(boot, in->files[i].boot)) unique_boot = false;
            boot = in->files[i].boot;
            boot_seen = true;
        }
        if (!boot_seen || !unique_boot)
            return unsupported(why, "evidence-v1 §7: target capture has no unique boot descriptor");
    }
    return PGWT_REPLAY_OK;
}

/* Schema failure cuts at this physical record, before touching replay state. */
static bool valid_record(const pgwt_evidence_record *r)
{
    if (r->unsupported_shape)
        return false;
    if (r->kind != PGWT_EVIDENCE_LIFE && r->kind != PGWT_EVIDENCE_ATTACH_OK &&
        r->kind != PGWT_EVIDENCE_TRANS && r->kind != PGWT_EVIDENCE_FRONTIER &&
        r->kind != PGWT_EVIDENCE_END && r->kind != PGWT_EVIDENCE_VOID)
        return false;
    if (r->kind != PGWT_EVIDENCE_FRONTIER && r->kind != PGWT_EVIDENCE_VOID && !r->pid)
        return false;
    if (r->kind == PGWT_EVIDENCE_LIFE) {
        if ((r->start_known ? (!r->has_fork || r->start != r->fork) : r->has_fork) ||
            r->start > r->t || (r->has_exit && (r->exit < r->start || r->exit > r->t)))
            return false;
    } else if (r->kind == PGWT_EVIDENCE_ATTACH_OK) {
        if (!state(r->seed) || !state(r->state))
            return false;

    } else if (r->kind == PGWT_EVIDENCE_END) {
        if (!same(r->end_reason, (pgwt_core_event){"detach", 6}) &&
            !same(r->end_reason, (pgwt_core_event){"deescalate", 10}) &&
            !same(r->end_reason, (pgwt_core_event){"stop", 4}))
            return false;
    } else if (r->kind == PGWT_EVIDENCE_FRONTIER) {

    } else if (r->kind == PGWT_EVIDENCE_VOID) {
        if (!nonempty(r->void_reason))
            return false;
    } else if (!state(r->old_event) || (!state(r->new_event) && !exit_event(r->new_event)) ||
               same(r->old_event, r->new_event) || r->duration > r->t) {
        return false;
    }
    return true;
}

/* A generation belongs to exactly one capture. LIFE updates address the key,
 * never the most recently observed PID. Discovery bounds assignment only;
 * the conservative LIFE.start remains the core lifetime's lower bound. */
typedef struct {
    const pgwt_evidence_record *identity, *life;
    uint64_t discovery;
    size_t life_order;
    bool invalid, tainted, blocked;
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

typedef struct snapshot {
    struct snapshot *previous;
    uint64_t frontier;
    pgwt_core_span coverage;
    pgwt_core_open_wait open;
    pgwt_core_wait *waits;
    size_t wait_count;
    bool representable, has_open, terminal;
} snapshot;

typedef struct {
    const pgwt_evidence_record *attach, *end;
    size_t generation;
    pgwt_core_span coverage;
    pgwt_core_open_wait open;
    pgwt_core_wait *waits;
    size_t wait_count;
    bool has_snapshot, has_open, terminal, frozen;
    snapshot *snapshots;
} attachment_run;

/* Snapshots own their waits. The run fields are a borrowed view of a selected
 * snapshot, used by the existing query preference and projection code. */
static void restore(attachment_run *run, const snapshot *s)
{
    run->has_snapshot = s && s->representable;
    if (!s) {
        run->coverage = (pgwt_core_span){0};
        run->open = (pgwt_core_open_wait){0};
        run->waits = NULL;
        run->wait_count = 0;
        run->has_open = run->terminal = false;
        return;
    }
    run->coverage = s->coverage;
    run->open = s->open;
    run->waits = s->waits;
    run->wait_count = s->wait_count;
    run->has_open = s->has_open;
    run->terminal = s->terminal;
}

static void discard_snapshot(attachment_run *run)
{
    snapshot *s = run->snapshots;
    run->snapshots = s->previous;
    free(s->waits);
    free(s);
}

/* VOID is global, including for a mismatch in a certificate on another PID.
 * Snapshot publication times, not coverage endpoints or detection time, decide
 * revocation. The publication high-water is deliberately not rolled back. */
static bool lifecycle_loss(const pgwt_evidence_counters *baseline,
                           const pgwt_evidence_record *r)
{
    if (r->has_invalid_from || !r->counters.known || !baseline->known) return true;
    const size_t slots[] = {0, 1, 2, 10, 12, 14};
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i)
        if (r->counters.value[slots[i]] != baseline->value[slots[i]]) return true;
    return false;
}

static void freeze(attachment_run *runs, size_t run_count,
                   const pgwt_evidence_record *r)
{
    for (size_t h = 0; h < run_count; ++h) {
        attachment_run *run = &runs[h];
        if (r->has_invalid_from)
            while (run->snapshots && run->snapshots->frontier >= r->invalid_from)
                discard_snapshot(run);
        restore(run, run->snapshots);
        run->frozen = true;
        /* Frozen bounds live entirely in snapshots. No pending END can affect
         * them, or compete with an END in a later attachment epoch. */
        run->end = NULL;
    }
}

/* Compare one run at a time. An empty required interval still needs a
 * certified exit when the query includes exit. Malformed queries use time order. */
static bool prefer(const attachment_run *a, const attachment_run *b,
                   const pgwt_core_evidence *life, const pgwt_core_query *q)
{
    if (q && !q->invalid_fields && nonempty(q->event) && q->from < q->to) {
        uint64_t left = q->from > life->life_start ? q->from : life->life_start;
        uint64_t right = life->has_life_end && life->life_end < q->to ? life->life_end : q->to;
        if (right < left) right = left;
        uint64_t al = a->coverage.start > left ? a->coverage.start : left;
        uint64_t ar = a->coverage.end < right ? a->coverage.end : right;
        uint64_t bl = b->coverage.start > left ? b->coverage.start : left;
        uint64_t br = b->coverage.end < right ? b->coverage.end : right;
        uint64_t ao = ar > al ? ar - al : 0, bo = br > bl ? br - bl : 0;
        bool exit_needed = life->has_life_end && q->from <= life->life_end && life->life_end < q->to;
        bool af = ao == right - left && (!exit_needed || (a->coverage.end == life->life_end && a->terminal));
        bool bf = bo == right - left && (!exit_needed || (b->coverage.end == life->life_end && b->terminal));
        if (af != bf) return af;
        if (ao != bo) return ao > bo;
    }
    if (a->coverage.start != b->coverage.start) return a->coverage.start < b->coverage.start;
    if (a->coverage.end != b->coverage.end) return a->coverage.end < b->coverage.end;
    return false; /* Iteration retains attachment append order at exact ties. */
}

/* Replay one independent capture. Every frontier resolves its physical prefix
 * against the lifetime facts available there. Later LIFE cannot certify an
 * earlier prefix. Even non-target generations must pass run consistency. */
static pgwt_replay_status replay_file(const pgwt_evidence_file *file,
                                      const bool *descriptors, const pgwt_evidence_input *in, const pgwt_core_query *query,
                                      pgwt_evidence_output *out, size_t *fault, const char **reason)
{
    pgwt_replay_status status = PGWT_REPLAY_OK;
    *fault = SIZE_MAX;
#define FAULT(pos) do { size_t at = (pos); if (at < *fault) *fault = at; } while (0)
    size_t n = file->record_count, count = 0, generations = 0, run_count = 0;
    attachment_run *runs = NULL;
    bool *processed = NULL;
    transition *history = NULL;
    generation *g = NULL;
    pgwt_core_wait *candidate = NULL;
    size_t *assigned = NULL;
    pgwt_core_evidence *e = &out->projection;
    const bool target_capture = same(file->capture, in->capture);
    if (n > SIZE_MAX / sizeof(*history) || n > SIZE_MAX / sizeof(*g) ||
        n > SIZE_MAX / sizeof(*runs) || n > SIZE_MAX / sizeof(*candidate) || n > SIZE_MAX / sizeof(*assigned))
        goto allocation_error;
    if (n) {
        history = calloc(n, sizeof(*history));
        g = calloc(n, sizeof(*g));
        candidate = calloc(n, sizeof(*candidate));
        assigned = calloc(n, sizeof(*assigned));
        runs = calloc(n, sizeof(*runs));
        processed = calloc(n, sizeof(*processed));
        if (!history || !g || !candidate || !assigned || !runs || !processed) goto allocation_error;
    }
    for (size_t i = 0; i < n; ++i)
        if (file->records[i].kind == PGWT_EVIDENCE_TRANS)
            history[count++] = (transition){&file->records[i], i};
    if (count) qsort(history, count, sizeof(*history), transition_order);
    bool has_frontier = false, identity_uncertain = false;
    uint64_t frontier = 0, latest_void = 0, identity_void = 0;
    size_t epoch_start = 0;
    pgwt_evidence_counters baseline = file->baseline;
    for (size_t i = 0; i <= n; ++i) {
        bool eof = i == n;
        const pgwt_evidence_record *r = eof ? NULL : &file->records[i];
        if (file->version != 5) continue;
        if (!eof && descriptors[i]) {
            bool restated = baseline.known == r->counters.known &&
                (!baseline.known || equal_counters(&baseline, &r->counters));
            if (!restated) { FAULT(i); goto done; }
            continue;
        }
        if (!eof && !valid_record(r)) { FAULT(i); goto done; }
        if (!eof && file->qualified && r->kind == PGWT_EVIDENCE_FRONTIER &&
            has_frontier && r->t <= frontier) { FAULT(i); goto done; }
        if (!eof) {
            if (r->kind == PGWT_EVIDENCE_LIFE) {
                generation *life = &g[generation_for(g, &generations, r)];
                if (life->tainted) continue;
                if (identity_uncertain &&
                    (!r->start_known || r->fork < identity_void || !baseline.known)) {
                    life->tainted = true;
                    continue;
                }
                if (!life->life) life->life_order = i;
                lifetime(life, r);
            }
            bool counter_mismatch = file->qualified &&
                (r->kind == PGWT_EVIDENCE_ATTACH_OK || r->kind == PGWT_EVIDENCE_FRONTIER) &&
                !equal_counters(&baseline, &r->counters);
            if (r->kind == PGWT_EVIDENCE_VOID || counter_mismatch) {
                /* Certificate mismatches have no explicit invalid_from. Ignore
                 * unrelated storage in native callers' unused record fields. */
                pgwt_evidence_record loss = *r;
                if (counter_mismatch) {
                    loss.has_invalid_from = false;
                    if (baseline.known && loss.counters.known)
                        for (size_t k = 0; k < 15; ++k)
                            if (loss.counters.value[k] < baseline.value[k]) loss.counters.known = false;
                }
                bool uncertain = lifecycle_loss(&baseline, &loss);
                if (uncertain) {
                    for (size_t k = 0; k < generations; ++k) g[k].tainted = true;
                    identity_uncertain = true;
                    identity_void = r->t;
                }
                freeze(runs, run_count, &loss);
                for (size_t j = epoch_start; j <= i; ++j) processed[j] = true;
                epoch_start = i + 1;
                baseline = loss.counters;
                latest_void = r->t;
                continue;
            }
            if (!file->qualified) continue;
            if (r->kind == PGWT_EVIDENCE_ATTACH_OK || r->kind == PGWT_EVIDENCE_END) {
                if (r->kind == PGWT_EVIDENCE_ATTACH_OK && r->t < latest_void) {
                    FAULT(i); goto done;
                }
                (void)generation_for(g, &generations, r);
            }
            if (r->kind != PGWT_EVIDENCE_FRONTIER) continue;
            has_frontier = true;
            frontier = r->t;
        } else if (!file->qualified) { continue; }
        uint64_t cutoff = eof ? UINT64_MAX : frontier;
        /* Resolve all delayed attachments before addressing ENDs. Then check
         * overlaps using their resolved endpoints, attributing each fault to
         * its source record rather than this certifying FRONTIER/EOF. */
        for (size_t pass = 0; pass < 2; ++pass)
        for (size_t j = 0; j < i; ++j) {
            const pgwt_evidence_record *p = &file->records[j];
            if (processed[j] || descriptors[j] || (p->kind != PGWT_EVIDENCE_ATTACH_OK && p->kind != PGWT_EVIDENCE_END)) continue;
            if ((p->kind == PGWT_EVIDENCE_END) != (pass == 1)) continue;
            size_t k = generation_for(g, &generations, p);
            if (g[k].invalid || g[k].tainted) { processed[j] = true; continue; }
            if (p->kind == PGWT_EVIDENCE_ATTACH_OK) {
                if (!g[k].life) continue;
                if (identity_uncertain && j <= g[k].life_order) { processed[j] = true; continue; }
                runs[run_count++] = (attachment_run){.attach = p, .generation = k};
            } else {
                size_t choices = 0, chosen = 0;
                bool preceding = false;
                for (size_t h = 0; h < run_count; ++h)
                    if (runs[h].generation == k && !runs[h].frozen && runs[h].attach->t <= p->t && !runs[h].end &&
                        g[k].life && (!g[k].life->has_exit || p->t <= g[k].life->exit)) {
                        bool before = runs[h].attach < p;
                        if (preceding && !before) continue;
                        if (!preceding && before) { preceding = true; choices = 0; }
                        chosen = h; ++choices;
                    }
                if (choices != 1) { FAULT(j); }
                else runs[chosen].end = p;
            }
            processed[j] = true;
        }
        for (size_t h = 0; h < run_count; ++h) {
            attachment_run *run = &runs[h];
            size_t k = run->generation;
            if (g[k].invalid || g[k].tainted || !g[k].life) continue;
            const pgwt_evidence_record *attach = run->attach, *life = g[k].life;
            if (attach->t < life->start || (life->has_exit && attach->t >= life->exit)) {
                FAULT((size_t)(attach - file->records));
            }
            if (run->frozen) continue;
            for (size_t x = h + 1; x < run_count; ++x) {
                if (runs[x].generation != k || runs[x].frozen) continue;
                attachment_run *earlier = run, *later = &runs[x];
                if (earlier->attach->t > later->attach->t) { earlier = &runs[x]; later = run; }
                if (!earlier->end || earlier->end->t > later->attach->t)
                    FAULT((size_t)(runs[x].attach - file->records));
            }
        }
        /* SIZE_MAX marks zero/multiple possible identities. Such a dependency
         * withholds this PID's certification, preserving prior snapshots. */
        for (size_t k = 0; k < generations; ++k) g[k].blocked = false;
        for (size_t j = 0; j < count; ++j) {
            assigned[j] = SIZE_MAX;
            const pgwt_evidence_record *t = history[j].record;
            if (history[j].order >= i || history[j].order < epoch_start || t->t > cutoff) continue;
            bool in_run = false;
            for (size_t h = 0; h < run_count; ++h) {
                attachment_run *run = &runs[h];
                generation *life = &g[run->generation];
                if (life->identity->pid == t->pid && !life->invalid && !life->tainted && !run->frozen &&
                    run->attach->t <= t->t && (!run->end || t->t <= run->end->t) &&
                    (!life->life || !life->life->has_exit || t->t <= life->life->exit)) in_run = true;
            }
            for (size_t x = epoch_start; x < i; ++x)
                if (!processed[x] && file->records[x].kind == PGWT_EVIDENCE_ATTACH_OK &&
                    file->records[x].pid == t->pid && file->records[x].t <= t->t) in_run = true;
            if (!in_run) continue;
            size_t choices = 0;
            for (size_t k = 0; k < generations; ++k) {
                const pgwt_evidence_record *life = g[k].life;
                if (!life || g[k].invalid || g[k].tainted || life->pid != t->pid) continue;
                uint64_t start = life->has_fork ? life->fork : g[k].discovery;
                bool matches = exit_event(t->new_event) ? (life->has_exit && life->exit == t->t) :
                    (start <= t->t && (!life->has_exit || t->t < life->exit));
                if (matches) { assigned[j] = k; ++choices; }
            }
            if (choices != 1)
                for (size_t k = 0; k < generations; ++k)
                    if (g[k].identity->pid == t->pid) g[k].blocked = true;
        }
        for (size_t h = 0; h < run_count; ++h) {
            attachment_run *run = &runs[h];
            size_t k = run->generation;
            const pgwt_evidence_record *life = g[k].life, *attach = run->attach;
            if (!life || g[k].invalid || g[k].tainted || run->frozen || !attach) continue;
            bool touching = false;
            /* A tie at any touching boundary withholds the whole generation,
             * including its other, separate attachment windows. */
            for (size_t x = 0; x < run_count; ++x)
                for (size_t y = 0; y < run_count; ++y)
                    if (x != y && !runs[y].frozen && runs[x].generation == k && runs[y].generation == k &&
                        runs[x].end && runs[x].end->t == runs[y].attach->t)
                        for (size_t j = 0; j < count; ++j)
                            if (assigned[j] == k && history[j].record->t == runs[y].attach->t) touching = true;
            if (touching) continue;
            if (g[k].blocked || attach->t > cutoff) continue;
            uint64_t endpoint = life->has_exit && life->exit < cutoff ? life->exit : cutoff;
            if (run->end && run->end->t < endpoint) endpoint = run->end->t;
            pgwt_core_event current = attach->state;
            uint64_t entry = 0;
            uint64_t trimmed = attach->t;
            bool entry_known = false, portion = null_event(current);
            bool anchored = attach->reread;
            if (attach->reread && !same(attach->seed, current)) {
                FAULT((size_t)(attach - file->records)); continue;
            }
            size_t waits = 0;
            bool terminal = false;
            for (size_t j = 0; j < count; ++j) {
                const pgwt_evidence_record *t = history[j].record;
                if (assigned[j] != k) continue;
                if (t->t > endpoint) continue;
                /* Timestamp order, not append position relative to ATTACH_OK,
                 * selects the first transition at the anchor, including ties. */
                if (t->t < attach->t) continue;
                if (!anchored) {
                    if (t->t != attach->t) break;
                    if (!same(t->new_event, current)) {
                        FAULT((size_t)(attach - file->records)); break;
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
                    FAULT(history[j].order); break;
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
                .pid = in->pid, .has_fork = in->has_fork, .fork = in->fork});
            if (!target || eof) continue;
            if (!anchored) continue;
            /* Keep every accepted bound, even a nonrepresentable portion:
             * later END can retract a formerly representable snapshot. */
            snapshot *snap = calloc(1, sizeof(*snap));
            if (!snap) goto allocation_error;
            snap->previous = run->snapshots;
            snap->frontier = frontier;
            run->snapshots = snap;
            if (!portion || trimmed > endpoint) {
                snap->coverage.end = endpoint;
                continue;
            }
            pgwt_core_wait *saved = waits ? malloc(waits * sizeof(*saved)) : NULL;
            if (waits && !saved) goto allocation_error;
            if (waits) memcpy(saved, candidate, waits * sizeof(*candidate));
            run->waits = saved;
            run->wait_count = waits;
            run->coverage = (pgwt_core_span){trimmed, endpoint};
            run->has_open = entry_known;
            run->open = (pgwt_core_open_wait){current, entry, endpoint};
            run->terminal = terminal;
            run->has_snapshot = true;
            *snap = (snapshot){.previous = snap->previous, .frontier = frontier,
                .coverage = run->coverage, .open = run->open, .waits = saved,
                .wait_count = waits, .representable = true,
                .has_open = run->has_open, .terminal = terminal};
        }
        if (*fault != SIZE_MAX) goto done;
    }
    /* LIFE updates after a loss cannot refine a tainted generation. Select
     * from the retained facts, then fall back to a surviving pre-exit bound. */
    generation *final = NULL;
    for (size_t k = 0; k < generations; ++k)
        if (target_capture && key(g[k].identity, &(pgwt_evidence_record){
            .pid = in->pid, .has_fork = in->has_fork, .fork = in->fork})) final = &g[k];
    if (final && final->life && !final->invalid) {
        e->invalid_fields = 0;
        e->life_start = final->life->start;
        e->has_life_end = final->life->has_exit;
        e->life_end = final->life->exit;
        for (size_t h = 0; h < run_count; ++h) {
            attachment_run *run = &runs[h];
            const snapshot *s = run->snapshots;
            if (s && e->has_life_end && s->coverage.end >= e->life_end && !s->terminal) {
                while (s && !(s->frontier < e->life_end && s->coverage.end < e->life_end))
                    s = s->previous;
            }
            restore(run, s);
        }
    } else {
        for (size_t h = 0; h < run_count; ++h) runs[h].has_snapshot = false;
    }
    attachment_run *selected = NULL;
    for (size_t h = 0; h < run_count; ++h)
        if (runs[h].has_snapshot && (!selected || prefer(&runs[h], selected, e, query))) selected = &runs[h];
    if (selected) {
        if (selected->wait_count) memcpy(out->waits, selected->waits, selected->wait_count * sizeof(*out->waits));
        e->wait_count = selected->wait_count;
        out->coverage = selected->coverage;
        e->coverage = &out->coverage;
        e->coverage_count = 1;
        e->has_open = selected->has_open;
        e->open = selected->open;
    }
    goto done;
allocation_error:
    status = PGWT_REPLAY_ERROR;
    if (reason) *reason = "replay allocation failed";
done:
    if (runs) for (size_t h = 0; h < run_count; ++h)
        while (runs[h].snapshots) discard_snapshot(&runs[h]);
    free(runs); free(processed);
    free(history); free(g); free(candidate); free(assigned);
    return status;
#undef FAULT
}

pgwt_replay_status pgwt_evidence_replay(const pgwt_evidence_input *input,
                                       const pgwt_core_query *query,
                                       pgwt_evidence_output *out,
                                       const char **reason)
{
    if (reason) *reason = NULL;
    if (!out) { if (reason) *reason = "null output"; return PGWT_REPLAY_ERROR; }
    memset(out, 0, sizeof(*out));
    joined_input joined;
    pgwt_replay_status status = join_chains(input, &joined, reason);
    if (status != PGWT_REPLAY_OK) { free_chains(&joined); return status; }
    const pgwt_evidence_input *in = &joined.input;
    size_t capacity = 0;
    for (size_t i = 0; i < in->file_count; ++i)
        if (same(in->files[i].capture, in->capture))
            capacity = in->files[i].record_count > capacity ? in->files[i].record_count : capacity;
    pgwt_core_wait *scratch = NULL;
    if (capacity > SIZE_MAX / sizeof(*out->waits)) goto allocation_error;
    if (capacity) {
        out->waits = calloc(capacity, sizeof(*out->waits));
        scratch = calloc(capacity, sizeof(*scratch));
        if (!out->waits || !scratch) goto allocation_error;
    }
    out->projection.invalid_fields = PGWT_CORE_LIFE_INVALID;
    for (size_t i = 0; i < in->file_count; ++i) {
        pgwt_evidence_file prefix = in->files[i];
        size_t fault;
        do {
            status = replay_file(&prefix, joined.descriptors[i], in, query, out, &fault, reason);
            if (status != PGWT_REPLAY_OK) goto done;
            if (fault != SIZE_MAX) prefix.record_count = fault;
        } while (fault != SIZE_MAX);
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
    free_chains(&joined);
    if (status != PGWT_REPLAY_OK) pgwt_evidence_output_free(out);
    return status;
}
