/* summary_writer.c — Per-second summary file writer
 *
 * Accumulates trace events into 1-second snapshots. Each snapshot contains
 * time model, per-event stats (with histograms), per-session stats, and
 * per-query stats. Snapshots are serialized and LZ4-compressed to
 * .summary.lz4 files with the same hourly rotation as trace files. */
#include "summary_writer.h"
#include "event_writer.h"   /* block index entry, varint, trace file header reuse */
/* pgwt_duration_to_bucket: defined in map_reader.c (daemon) but not available
 * in the server build. Use a local copy for server compilation. */
#ifdef PGWT_SERVER
static uint32_t pgwt_duration_to_bucket(uint64_t ns)
{
    /* Hardcoded log2 buckets matching daemon (map_reader.c) exactly.
     * 0: <1us, 1: 1-2us, ..., 15: >=16ms */
    uint64_t us = ns / 1000;
    if (us < 1)     return 0;
    if (us < 2)     return 1;
    if (us < 4)     return 2;
    if (us < 8)     return 3;
    if (us < 16)    return 4;
    if (us < 32)    return 5;
    if (us < 64)    return 6;
    if (us < 128)   return 7;
    if (us < 256)   return 8;
    if (us < 512)   return 9;
    if (us < 1024)  return 10;
    if (us < 2048)  return 11;
    if (us < 4096)  return 12;
    if (us < 8192)  return 13;
    if (us < 16384) return 14;
    return 15;
}
#else
#include "map_reader.h"     /* pgwt_duration_to_bucket */
#endif
#include "wait_event.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <grp.h>
#include <lz4.h>

/* ── Wait class index (local copy to avoid compute.c dependency) ── */

static int summary_wait_class_index(uint32_t event_id)
{
    if (event_id == 0)
        return PGWT_CLASS_CPU;
    uint8_t cls = (event_id >> 24) & 0xFF;
    switch (cls) {
    case 0x0A: return PGWT_CLASS_IO;
    case 0x03: return PGWT_CLASS_LOCK;
    case 0x01: return PGWT_CLASS_LWLOCK;
    case 0x08: return PGWT_CLASS_IPC;
    case 0x06: return PGWT_CLASS_CLIENT;
    case 0x09: return PGWT_CLASS_TIMEOUT;
    case 0x04: return PGWT_CLASS_BUFFERPIN;
    case 0x05: return PGWT_CLASS_ACTIVITY;
    case 0x07: return PGWT_CLASS_EXTENSION;
    default:   return PGWT_CLASS_UNKNOWN;
    }
}

/* ── Accumulator helpers ──────────────────────────────────── */

static void accum_reset(struct pgwt_summary_accum *acc)
{
    memset(acc, 0, sizeof(*acc));
}

static uint32_t hash32(uint32_t x)
{
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x;
}

static uint64_t hash64(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

/* Find or insert event in open-addressing hash table.
 * Returns pointer to slot, or NULL if table full. */
static struct pgwt_summary_event *find_or_insert_event(
    struct pgwt_summary_accum *acc, uint32_t event_id)
{
    uint32_t idx = hash32(event_id) & (SUMMARY_MAX_EVENTS - 1);
    for (int i = 0; i < SUMMARY_MAX_EVENTS; i++) {
        struct pgwt_summary_event *e = &acc->events[idx];
        /* For event_id=0 (CPU), an empty slot (count==0) looks identical
         * to "found" since both have event_id==0. Distinguish by count. */
        if (e->event_id == event_id && (event_id != 0 || e->count > 0))
            return e;
        if (e->event_id == 0 && e->count == 0) {
            /* Empty slot — insert */
            e->event_id = event_id;
            acc->num_events++;
            return e;
        }
        idx = (idx + 1) & (SUMMARY_MAX_EVENTS - 1);
    }
    return NULL; /* table full */
}

static struct pgwt_summary_session *find_or_insert_session(
    struct pgwt_summary_accum *acc, uint32_t pid)
{
    uint32_t idx = hash32(pid) & (SUMMARY_MAX_SESSIONS - 1);
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++) {
        struct pgwt_summary_session *s = &acc->sessions[idx];
        if (s->pid == pid)
            return s;
        if (s->pid == 0 && s->db_time_ns == 0) {
            s->pid = pid;
            acc->num_sessions++;
            return s;
        }
        idx = (idx + 1) & (SUMMARY_MAX_SESSIONS - 1);
    }
    return NULL;
}

static struct pgwt_summary_query *find_or_insert_query(
    struct pgwt_summary_accum *acc, uint64_t query_id)
{
    if (query_id == 0) return NULL;  /* skip unknown queries */

    uint32_t idx = (uint32_t)(hash64(query_id) & (SUMMARY_MAX_QUERIES - 1));
    for (int i = 0; i < SUMMARY_MAX_QUERIES; i++) {
        struct pgwt_summary_query *q = &acc->queries[idx];
        if (q->query_id == query_id)
            return q;
        if (q->query_id == 0 && q->count == 0) {
            q->query_id = query_id;
            acc->num_queries++;
            return q;
        }
        idx = (idx + 1) & (SUMMARY_MAX_QUERIES - 1);
    }
    return NULL;
}

/* Accumulate a single trace event into the per-second snapshot. */
static void accum_event(struct pgwt_summary_accum *acc,
                         const struct pgwt_trace_event *evt)
{
    uint32_t old_ev = evt->old_event;
    uint64_t dur = evt->duration_ns;

    if (old_ev == PGWT_EVENT_EXIT)
        return;

    /* Markers must never be accumulated (FID-4): exec/plan markers inflate
     * per-query counts (skewing avg low) and escalation markers insert
     * bogus PGWT_ESC_PACK query_ids. This is the write-side chokepoint —
     * event_stream pushes every record (markers included, so they land in
     * the trace) before its own marker check. */
    if (PGWT_IS_MARKER(old_ev))
        return;

    acc->total_events++;

    /* Time model: classify old_event into wait class */
    int cls = summary_wait_class_index(old_ev);
    if (cls >= 0 && cls < PGWT_NUM_CLASSES)
        acc->class_ns[cls] += dur;

    /* Per-event stats */
    struct pgwt_summary_event *se = find_or_insert_event(acc, old_ev);
    if (se) {
        se->count++;
        se->total_ns += dur;
        if (dur > se->max_ns) se->max_ns = dur;
        uint32_t bucket = pgwt_duration_to_bucket(dur);
        if (bucket < HISTOGRAM_BUCKETS)
            se->histogram[bucket]++;
    }

    /* Per-session stats */
    struct pgwt_summary_session *ss = find_or_insert_session(acc, evt->pid);
    if (ss) {
        if (!pgwt_is_idle_event(old_ev))
            ss->db_time_ns += dur;
        if (old_ev == 0)
            ss->cpu_ns += dur;
        /* Track top wait per session */
        if (old_ev != 0 && dur > ss->top_wait_ns) {
            ss->top_wait_id = old_ev;
            ss->top_wait_ns = dur;
        }
    }

    /* Per-query stats */
    struct pgwt_summary_query *sq = find_or_insert_query(acc, evt->query_id);
    if (sq) {
        sq->count++;
        sq->total_ns += dur;
        /* Per-class breakdown */
        if (cls >= 0 && cls < PGWT_NUM_CLASSES)
            sq->class_ns[cls] += dur;
        /* Per-event top-8 tracking */
        if (old_ev != 0) {
            int found = -1;
            for (int j = 0; j < sq->num_top_events; j++) {
                if (sq->top_events[j].event_id == old_ev) { found = j; break; }
            }
            if (found >= 0) {
                sq->top_events[found].count++;
                sq->top_events[found].total_ns += dur;
            } else if (sq->num_top_events < SUMMARY_QUERY_TOP_EVENTS) {
                sq->top_events[sq->num_top_events].event_id = old_ev;
                sq->top_events[sq->num_top_events].count = 1;
                sq->top_events[sq->num_top_events].total_ns = dur;
                sq->num_top_events++;
            }
        }
        if (old_ev != 0 && dur > sq->top_wait_ns) {
            sq->top_wait_id = old_ev;
            sq->top_wait_ns = dur;
        }
    }
}

/* ── Serialization ────────────────────────────────────────── */

size_t pgwt_summary_serialize(const struct pgwt_summary_accum *acc,
                               uint8_t *out, size_t out_size)
{
    uint8_t *p = out;
    (void)out_size;

    /* Time model: 11 × 8 = 88 bytes */
    memcpy(p, acc->class_ns, sizeof(acc->class_ns));
    p += sizeof(acc->class_ns);

    /* Per-event entries: only non-empty */
    for (int i = 0; i < SUMMARY_MAX_EVENTS; i++) {
        const struct pgwt_summary_event *e = &acc->events[i];
        if (e->event_id == 0 && e->count == 0) continue;

        memcpy(p, &e->event_id, 4);  p += 4;
        memcpy(p, &e->count, 8);     p += 8;
        memcpy(p, &e->total_ns, 8);  p += 8;
        memcpy(p, &e->max_ns, 8);    p += 8;
        memcpy(p, e->histogram, HISTOGRAM_BUCKETS * 8);
        p += HISTOGRAM_BUCKETS * 8;
        /* 4 + 8 + 8 + 8 + 128 = 156 bytes per event */
    }

    /* Per-session entries: only non-empty */
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++) {
        const struct pgwt_summary_session *s = &acc->sessions[i];
        if (s->pid == 0 && s->db_time_ns == 0) continue;

        memcpy(p, &s->pid, 4);           p += 4;
        memcpy(p, &s->db_time_ns, 8);    p += 8;
        memcpy(p, &s->cpu_ns, 8);        p += 8;
        memcpy(p, &s->top_wait_id, 4);   p += 4;
        memcpy(p, &s->top_wait_ns, 8);   p += 8;
        /* 4 + 8 + 8 + 4 + 8 = 32 bytes per session */
    }

    /* Per-query entries (v2): base 36 + class_ns 88 + 1 + top_events n×20 */
    for (int i = 0; i < SUMMARY_MAX_QUERIES; i++) {
        const struct pgwt_summary_query *q = &acc->queries[i];
        if (q->query_id == 0 && q->count == 0) continue;

        memcpy(p, &q->query_id, 8);      p += 8;
        memcpy(p, &q->count, 8);         p += 8;
        memcpy(p, &q->total_ns, 8);      p += 8;
        memcpy(p, &q->top_wait_id, 4);   p += 4;
        memcpy(p, &q->top_wait_ns, 8);   p += 8;
        /* v2: class_ns */
        memcpy(p, q->class_ns, sizeof(q->class_ns));
        p += sizeof(q->class_ns);
        /* v2: top_events (variable length) */
        uint8_t nte = (uint8_t)q->num_top_events;
        *p = nte;  p += 1;
        for (int j = 0; j < nte; j++) {
            memcpy(p, &q->top_events[j].event_id, 4);   p += 4;
            memcpy(p, &q->top_events[j].count, 8);       p += 8;
            memcpy(p, &q->top_events[j].total_ns, 8);    p += 8;
        }
    }

    return (size_t)(p - out);
}

int pgwt_summary_deserialize(const uint8_t *in, size_t in_size,
                              struct pgwt_summary_accum *acc,
                              int version)
{
    const uint8_t *p = in;
    const uint8_t *end = in + in_size;

    /* Caller sets: acc->second_wall_ns, acc->num_events, acc->num_sessions,
     * acc->num_queries from the block header. Save counts before we iterate,
     * since find_or_insert modifies num_events/sessions/queries. */
    int n_events   = acc->num_events;
    int n_sessions = acc->num_sessions;
    int n_queries  = acc->num_queries;

    /* Reset counters — find_or_insert will re-count */
    acc->num_events   = 0;
    acc->num_sessions = 0;
    acc->num_queries  = 0;

    /* Time model */
    if (p + sizeof(acc->class_ns) > end) return -1;
    memcpy(acc->class_ns, p, sizeof(acc->class_ns));
    p += sizeof(acc->class_ns);

    /* Per-event entries */
    for (int i = 0; i < n_events; i++) {
        if (p + 156 > end) return -1;

        uint32_t event_id;
        memcpy(&event_id, p, 4);  p += 4;

        struct pgwt_summary_event *e = find_or_insert_event(acc, event_id);
        if (!e) { p += 152; continue; }

        memcpy(&e->count, p, 8);     p += 8;
        memcpy(&e->total_ns, p, 8);  p += 8;
        memcpy(&e->max_ns, p, 8);    p += 8;
        memcpy(e->histogram, p, HISTOGRAM_BUCKETS * 8);
        p += HISTOGRAM_BUCKETS * 8;
    }

    /* Per-session entries */
    for (int i = 0; i < n_sessions; i++) {
        if (p + 32 > end) return -1;

        uint32_t pid;
        memcpy(&pid, p, 4);  p += 4;

        struct pgwt_summary_session *s = find_or_insert_session(acc, pid);
        if (!s) { p += 28; continue; }

        memcpy(&s->db_time_ns, p, 8);    p += 8;
        memcpy(&s->cpu_ns, p, 8);        p += 8;
        memcpy(&s->top_wait_id, p, 4);   p += 4;
        memcpy(&s->top_wait_ns, p, 8);   p += 8;
    }

    /* Per-query entries */
    for (int i = 0; i < n_queries; i++) {
        if (p + 36 > end) return -1;

        uint64_t query_id;
        memcpy(&query_id, p, 8);  p += 8;

        struct pgwt_summary_query *q = find_or_insert_query(acc, query_id);
        if (!q) {
            p += 28;  /* skip base fields */
            if (version >= 2) {
                p += sizeof(uint64_t) * PGWT_NUM_CLASSES;  /* class_ns */
                if (p + 1 <= end) {
                    uint8_t nte = *p;  p += 1;
                    p += nte * 20;     /* top_events */
                }
            }
            continue;
        }

        memcpy(&q->count, p, 8);         p += 8;
        memcpy(&q->total_ns, p, 8);      p += 8;
        memcpy(&q->top_wait_id, p, 4);   p += 4;
        memcpy(&q->top_wait_ns, p, 8);   p += 8;

        if (version >= 2) {
            if (p + sizeof(q->class_ns) + 1 > end) return -1;
            memcpy(q->class_ns, p, sizeof(q->class_ns));
            p += sizeof(q->class_ns);
            q->num_top_events = *p;  p += 1;
            if (q->num_top_events > SUMMARY_QUERY_TOP_EVENTS)
                q->num_top_events = SUMMARY_QUERY_TOP_EVENTS;
            for (int j = 0; j < q->num_top_events; j++) {
                if (p + 20 > end) return -1;
                memcpy(&q->top_events[j].event_id, p, 4);   p += 4;
                memcpy(&q->top_events[j].count, p, 8);       p += 8;
                memcpy(&q->top_events[j].total_ns, p, 8);    p += 8;
            }
        }
    }

    return 0;
}

/* ── Startup recovery (DUR-1) ─────────────────────────────── */

/* Same contract as the trace writer's recovery: a leftover current.summary
 * is finalized (torn tail dropped, footer appended) and renamed aside to a
 * collision-safe archive — NEVER truncated. */
static void recover_current_summary(struct pgwt_summary_writer *w)
{
    char cur[512], meta[600], meta_tmp[600];
    snprintf(cur, sizeof(cur), "%s/current.summary", w->trace_dir);
    snprintf(meta, sizeof(meta), "%s/current.summary.meta", w->trace_dir);
    snprintf(meta_tmp, sizeof(meta_tmp), "%s/current.summary.meta.tmp",
             w->trace_dir);

    unlink(meta_tmp);   /* torn meta write — always stale */

    struct stat st;
    if (stat(cur, &st) != 0) {
        unlink(meta);
        return;
    }

    int committed = 0;
    {
        FILE *mf = fopen(meta, "r");
        if (mf) {
            if (fscanf(mf, "%d", &committed) != 1)
                committed = 0;
            fclose(mf);
        }
    }

    FILE *fp = fopen(cur, "r+b");
    struct pgwt_trace_file_header hdr;
    if (!fp) {
        fprintf(stderr, "WARN: cannot open %s for recovery: %s "
                "(leaving it in place)\n", cur, strerror(errno));
        return;
    }
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1 ||
        hdr.magic != PGWT_SUMMARY_MAGIC ||
        (hdr.version != 1 && hdr.version != PGWT_SUMMARY_VERSION)) {
        fclose(fp);
        char aside[600];
        snprintf(aside, sizeof(aside), "%s.corrupt.%lld", cur,
                 (long long)time(NULL));
        if (rename(cur, aside) == 0)
            fprintf(stderr, "WARN: %s has an unreadable header — preserved "
                    "as %s\n", cur, aside);
        unlink(meta);
        pgwt_fsync_dir(w->trace_dir);
        return;
    }

    long fsize = (long)st.st_size;
    struct pgwt_block_index_entry *idx = NULL;
    int nidx = 0, cap = 0;
    long pos = (long)sizeof(hdr);
    struct pgwt_summary_block_header bh;
    while (nidx < PGWT_MAX_READ_BLOCKS) {
        if (fseek(fp, pos, SEEK_SET) != 0 ||
            fread(&bh, sizeof(bh), 1, fp) != 1)
            break;
        if (bh.compressed_size == 0 || bh.uncompressed_size == 0 ||
            bh.compressed_size > PGWT_MAX_BLOCK_COMPRESSED ||
            bh.uncompressed_size > PGWT_MAX_BLOCK_COMPRESSED)
            break;
        long end = pos + (long)sizeof(bh) + (long)bh.compressed_size;
        if (end > fsize)
            break;   /* torn tail */
        if (nidx >= cap) {
            int newcap = cap ? cap * 2 : 256;
            struct pgwt_block_index_entry *tmp =
                realloc(idx, newcap * sizeof(*tmp));
            if (!tmp)
                break;
            idx = tmp;
            cap = newcap;
        }
        idx[nidx++] = (struct pgwt_block_index_entry){
            .timestamp_ns = bh.wall_ns,
            .file_offset = (uint64_t)pos,
        };
        pos = end;
    }

    if (committed > nidx)
        fprintf(stderr, "ERROR: recovery of %s found %d complete blocks but "
                "the meta high-watermark says %d were committed\n",
                cur, nidx, committed);

    if (nidx == 0) {
        free(idx);
        fclose(fp);
        if (fsize > (long)sizeof(hdr)) {
            char aside[600];
            snprintf(aside, sizeof(aside), "%s.corrupt.%lld", cur,
                     (long long)time(NULL));
            if (rename(cur, aside) == 0)
                fprintf(stderr, "WARN: %s had no complete block — preserved "
                        "as %s\n", cur, aside);
        } else {
            unlink(cur);
        }
        unlink(meta);
        pgwt_fsync_dir(w->trace_dir);
        return;
    }

    int finalize_failed = 0;
    if (ftruncate(fileno(fp), (off_t)pos) != 0)
        finalize_failed = 1;
    if (!finalize_failed && fseek(fp, pos, SEEK_SET) == 0) {
        for (int i = 0; i < nidx && !finalize_failed; i++)
            if (fwrite(&idx[i], sizeof(idx[i]), 1, fp) != 1)
                finalize_failed = 1;
        uint32_t nb = (uint32_t)nidx;
        if (!finalize_failed && fwrite(&nb, sizeof(nb), 1, fp) != 1)
            finalize_failed = 1;
    } else {
        finalize_failed = 1;
    }
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    free(idx);
    if (finalize_failed)
        fprintf(stderr, "WARN: could not append a footer to recovered %s "
                "(%s) — archiving anyway\n", cur, strerror(errno));

    time_t start_sec = (time_t)(hdr.start_time_ns / 1000000000ULL);
    struct tm tm;
    localtime_r(&start_sec, &tm);
    char dest[512];
    if (pgwt_archive_path_nonclobber(w->trace_dir, &tm, ".summary.lz4",
                                     dest, sizeof(dest)) != 0 ||
        rename(cur, dest) != 0) {
        fprintf(stderr, "WARN: cannot archive recovered %s: %s "
                "(leaving it in place; it will NOT be truncated)\n",
                cur, strerror(errno));
        return;
    }
    unlink(meta);
    pgwt_fsync_dir(w->trace_dir);
    fprintf(stderr, "INFO: recovered previous current.summary: %d blocks "
            "-> %s\n", nidx, dest);
}

/* ── File operations ──────────────────────────────────────── */

static int write_footer(struct pgwt_summary_writer *w)
{
    for (int i = 0; i < w->num_blocks; i++) {
        if (fwrite(&w->block_index[i], sizeof(w->block_index[i]), 1, w->fp) != 1)
            return -1;
    }
    uint32_t nb = (uint32_t)w->num_blocks;
    if (fwrite(&nb, sizeof(nb), 1, w->fp) != 1)
        return -1;
    return 0;
}

static int open_summary_file(struct pgwt_summary_writer *w)
{
    snprintf(w->current_path, sizeof(w->current_path),
             "%s/current.summary", w->trace_dir);

    /* DUR-1: never truncate an existing current.summary ("wbx"); recover
     * and retry if one (re)appeared after init. */
    w->fp = fopen(w->current_path, "wbxe");
    if (!w->fp && errno == EEXIST) {
        recover_current_summary(w);
        w->fp = fopen(w->current_path, "wbxe");
    }
    if (!w->fp) {
        fprintf(stderr, "WARN: cannot create %s: %s\n",
                w->current_path, strerror(errno));
        return -1;
    }

    /* Set group ownership and permissions */
    if (w->trace_gid != (gid_t)-1) {
        fchown(fileno(w->fp), (uid_t)-1, w->trace_gid);
        fchmod(fileno(w->fp), 0640);
    }

    /* Capture wall-clock and monotonic timestamps */
    struct timespec wall, mono;
    clock_gettime(CLOCK_REALTIME, &wall);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    w->clock_offset_wall_ns = (uint64_t)wall.tv_sec * 1000000000ULL + wall.tv_nsec;
    w->clock_offset_mono_ns = (uint64_t)mono.tv_sec * 1000000000ULL + mono.tv_nsec;

    /* Record current hour for rotation */
    time_t now = wall.tv_sec;
    struct tm tm;
    localtime_r(&now, &tm);
    w->current_hour = tm.tm_yday * 24 + tm.tm_hour;

    /* Write file header (same struct as trace files, different magic) */
    struct pgwt_trace_file_header hdr = {
        .magic = PGWT_SUMMARY_MAGIC,
        .version = PGWT_SUMMARY_VERSION,
        .flags = PGWT_FLAG_LZ4,
        .pg_version = 0,
        .start_time_ns = w->clock_offset_wall_ns,
        .clock_offset_ns = w->clock_offset_mono_ns,
    };
    if (fwrite(&hdr, sizeof(hdr), 1, w->fp) != 1) {
        fprintf(stderr, "WARN: cannot write summary file header: %s\n",
                strerror(errno));
        fclose(w->fp);
        w->fp = NULL;
        return -1;
    }

    w->num_blocks = 0;
    return 0;
}

/* Convert monotonic timestamp to wall-clock using clock offsets. */
static uint64_t mono_to_wall(const struct pgwt_summary_writer *w,
                              uint64_t mono_ns)
{
    return w->clock_offset_wall_ns + (mono_ns - w->clock_offset_mono_ns);
}

/* Flush current accumulator as one LZ4-compressed block. */
static int flush_accum(struct pgwt_summary_writer *w)
{
    if (!w->accum_active || !w->fp)
        return 0;

    struct pgwt_summary_accum *acc = &w->accum;

    /* Serialize */
    size_t encoded_size = pgwt_summary_serialize(acc, w->encode_buf,
                                                  w->encode_buf_size);

    /* LZ4 compress */
    int compressed_size = LZ4_compress_default(
        (const char *)w->encode_buf, (char *)w->compress_buf,
        (int)encoded_size, (int)w->compress_buf_size);
    if (compressed_size <= 0) {
        fprintf(stderr, "WARN: summary LZ4 compression failed\n");
        w->accum_active = false;
        return -1;
    }

    /* Grow block index if needed */
    if (w->num_blocks >= w->block_index_cap) {
        int new_cap = w->block_index_cap * 2;
        struct pgwt_block_index_entry *new_idx =
            realloc(w->block_index, new_cap * sizeof(*new_idx));
        if (!new_idx) {
            fprintf(stderr, "WARN: cannot grow summary block index\n");
            w->enabled = false;
            return -1;
        }
        w->block_index = new_idx;
        w->block_index_cap = new_cap;
    }

    /* Record block index entry */
    long file_offset = ftell(w->fp);
    uint64_t wall_ns = mono_to_wall(w, acc->second_mono_ns);
    w->block_index[w->num_blocks++] = (struct pgwt_block_index_entry){
        .timestamp_ns = wall_ns,
        .file_offset = (uint64_t)file_offset,
    };

    /* Write block header */
    struct pgwt_summary_block_header bh = {
        .wall_ns = wall_ns,
        .num_events = (uint32_t)acc->num_events,
        .num_sessions = (uint16_t)acc->num_sessions,
        .num_queries = (uint16_t)acc->num_queries,
        .compressed_size = (uint32_t)compressed_size,
        .uncompressed_size = (uint32_t)encoded_size,
    };
    if (fwrite(&bh, sizeof(bh), 1, w->fp) != 1 ||
        fwrite(w->compress_buf, 1, compressed_size, w->fp) != (size_t)compressed_size) {
        fprintf(stderr, "WARN: summary file write failed: %s\n", strerror(errno));
        w->enabled = false;
        return -1;
    }

    w->total_records_written++;
    w->total_bytes_written += sizeof(bh) + compressed_size;
    w->accum_active = false;

    /* Flush + committed block count (same pattern as event_writer) */
    fflush(w->fp);
    {
        char meta_path[512], tmp_path[512];
        snprintf(meta_path, sizeof(meta_path), "%s/current.summary.meta",
                 w->trace_dir);
        snprintf(tmp_path, sizeof(tmp_path), "%s/current.summary.meta.tmp",
                 w->trace_dir);
        FILE *mf = fopen(tmp_path, "w");
        if (mf) {
            fprintf(mf, "%d\n", w->num_blocks);
            fclose(mf);
            rename(tmp_path, meta_path);
        }
    }

    return 0;
}

/* ── Public API ───────────────────────────────────────────── */

int pgwt_summary_writer_init(struct pgwt_summary_writer *w,
                               const char *trace_dir,
                               int retention_hours,
                               const char *group_name)
{
    memset(w, 0, sizeof(*w));
    snprintf(w->trace_dir, sizeof(w->trace_dir), "%s", trace_dir);
    w->retention_hours = retention_hours;
    w->enabled = true;
    w->current_hour = -1;
    w->trace_gid = (gid_t)-1;

    /* Resolve trace group → GID */
    if (group_name) {
        struct group *gr = getgrnam(group_name);
        if (gr)
            w->trace_gid = gr->gr_gid;
        /* Don't warn here — event_writer already warned */
    }

    /* Directory already created by event_writer */

    /* DUR-1: recover (finalize + archive) any current.summary a previous
     * daemon left behind — never truncate it. */
    recover_current_summary(w);

    /* Allocate scratch buffers.
     * Worst case: 1024 events × 156 + 1024 sessions × 32 + 2048 queries × 36
     *           = 159744 + 32768 + 73728 = ~260 KB uncompressed */
    w->encode_buf_size = 800 * 1024;
    w->encode_buf = malloc(w->encode_buf_size);

    w->compress_buf_size = LZ4_compressBound((int)w->encode_buf_size);
    w->compress_buf = malloc(w->compress_buf_size);

    w->block_index_cap = 4096;  /* one per second, ~1 hour */
    w->block_index = malloc(w->block_index_cap * sizeof(*w->block_index));

    if (!w->encode_buf || !w->compress_buf || !w->block_index) {
        pgwt_summary_destroy(w);
        return -1;
    }

    return 0;
}

int pgwt_summary_push_event(struct pgwt_summary_writer *w,
                              const struct pgwt_trace_event *evt)
{
    if (!w->enabled) return 0;

    /* Lazy file open on first event */
    if (!w->fp) {
        if (open_summary_file(w) != 0) {
            w->enabled = false;
            return -1;
        }
    }

    /* Determine which second this event belongs to (monotonic, floored) */
    uint64_t evt_second = (evt->timestamp_ns / 1000000000ULL) * 1000000000ULL;

    /* Second boundary detection */
    if (w->accum_active && evt_second > w->accum.second_mono_ns) {
        /* New second — flush old accumulator */
        flush_accum(w);
        accum_reset(&w->accum);
    }

    /* Start new accumulator if needed */
    if (!w->accum_active) {
        w->accum.second_mono_ns = evt_second;
        w->accum.second_wall_ns = mono_to_wall(w, evt_second);
        w->accum_active = true;
    }

    accum_event(&w->accum, evt);
    return 0;
}

int pgwt_summary_flush(struct pgwt_summary_writer *w)
{
    return flush_accum(w);
}

int pgwt_summary_check_rotation(struct pgwt_summary_writer *w)
{
    if (!w->enabled || !w->fp) return 0;

    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    struct tm tm;
    time_t now = wall.tv_sec;
    localtime_r(&now, &tm);
    int current_hour = tm.tm_yday * 24 + tm.tm_hour;

    if (current_hour == w->current_hour)
        return 0;

    /* Hour changed — rotate */
    flush_accum(w);
    write_footer(w);

    if (w->verbose) {
        fprintf(stderr, "INFO: summary file closed: %lu records, %lu bytes\n",
                (unsigned long)w->total_records_written,
                (unsigned long)w->total_bytes_written);
    }

    /* DUR-5: archives are durable once rotated. */
    fflush(w->fp);
    fsync(fileno(w->fp));
    fclose(w->fp);
    w->fp = NULL;

    /* Rename current.summary → YYYY-MM-DD_HH[.N].summary.lz4 (DUR-2:
     * collision-safe — see the trace writer's rotation comment). */
    time_t file_start_sec = (time_t)(w->clock_offset_wall_ns / 1000000000ULL);
    struct tm file_tm;
    localtime_r(&file_start_sec, &file_tm);
    char new_path[512];
    if (pgwt_archive_path_nonclobber(w->trace_dir, &file_tm, ".summary.lz4",
                                     new_path, sizeof(new_path)) == 0)
        rename(w->current_path, new_path);
    else
        fprintf(stderr, "WARN: no free archive name for %s — leaving it as "
                "current.summary (it will be recovered on next start)\n",
                w->current_path);

    /* Remove meta file */
    {
        char meta_path[512];
        snprintf(meta_path, sizeof(meta_path), "%s/current.summary.meta",
                 w->trace_dir);
        unlink(meta_path);
    }
    pgwt_fsync_dir(w->trace_dir);

    /* Reset stats */
    w->total_records_written = 0;
    w->total_bytes_written = 0;

    return 0;
}

int pgwt_summary_close(struct pgwt_summary_writer *w)
{
    if (!w->fp) return 0;

    flush_accum(w);
    write_footer(w);

    if (w->verbose) {
        fprintf(stderr, "INFO: summary file closed: %lu records, %lu bytes\n",
                (unsigned long)w->total_records_written,
                (unsigned long)w->total_bytes_written);
    }

    fflush(w->fp);
    fsync(fileno(w->fp));
    fclose(w->fp);
    w->fp = NULL;
    return 0;
}

int pgwt_summary_cleanup_old_files(struct pgwt_summary_writer *w)
{
    if (w->retention_hours <= 0) return 0;

    DIR *dir = opendir(w->trace_dir);
    if (!dir) return -1;

    time_t cutoff = time(NULL) - (time_t)w->retention_hours * 3600;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strstr(ent->d_name, ".summary.lz4"))
            continue;

        struct tm ftm = {0};
        int hour;
        if (sscanf(ent->d_name, "%4d-%2d-%2d_%2d.summary.lz4",
                   &ftm.tm_year, &ftm.tm_mon, &ftm.tm_mday, &hour) != 4)
            continue;

        ftm.tm_year -= 1900;
        ftm.tm_mon -= 1;
        ftm.tm_hour = hour;
        ftm.tm_isdst = -1;
        time_t file_time = mktime(&ftm);

        if (file_time > 0 && file_time < cutoff) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", w->trace_dir, ent->d_name);
            if (unlink(path) == 0 && w->verbose)
                fprintf(stderr, "INFO: deleted old summary file %s\n", ent->d_name);
        }
    }
    closedir(dir);
    return 0;
}

void pgwt_summary_destroy(struct pgwt_summary_writer *w)
{
    free(w->encode_buf);
    w->encode_buf = NULL;
    free(w->compress_buf);
    w->compress_buf = NULL;
    free(w->block_index);
    w->block_index = NULL;
}
