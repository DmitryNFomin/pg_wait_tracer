/* event_reader.c — Trace file reader: block decode, time-range seek, replay */
#include "event_reader.h"
#ifndef PGWT_SERVER
#include "map_reader.h"
#endif
#include "wait_event.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <lz4.h>

/* ── Single-file reader ────────────────────────────────── */

int pgwt_reader_open(struct pgwt_event_reader *r, const char *path)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->path, sizeof(r->path), "%s", path);

    r->fp = fopen(path, "rb");
    if (!r->fp) {
        fprintf(stderr, "WARN: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Read file header */
    if (fread(&r->header, sizeof(r->header), 1, r->fp) != 1) {
        fprintf(stderr, "WARN: cannot read header from %s\n", path);
        fclose(r->fp); r->fp = NULL;
        return -1;
    }

    if (r->header.magic != PGWT_TRACE_MAGIC) {
        fprintf(stderr, "WARN: bad magic in %s (0x%08x)\n", path, r->header.magic);
        fclose(r->fp); r->fp = NULL;
        return -1;
    }
    if (r->header.version < 2 || r->header.version > PGWT_TRACE_VERSION) {
        /* v2 and v3 are read; v1 stays rejected (no deployed installs existed).
         * A v2 file's TRANSITIONS records lack the cpu_ns column — the decoder
         * surfaces PGWT_CPU_NS_UNKNOWN for them and compute falls back to
         * gap-inference (T8 §5.3). A newer-than-known version is skipped — its
         * layout is unknown, and garbage must never surface as data. Non-fatal:
         * the file is skipped, never a crash on a stale/future layout. */
        fprintf(stderr,
                "ERROR: unsupported trace version %u in %s "
                "(this build reads versions 2-%u; regenerate with this build)\n",
                r->header.version, path, PGWT_TRACE_VERSION);
        fclose(r->fp); r->fp = NULL;
        return -1;
    }

    r->mono_to_wall = (int64_t)r->header.start_time_ns
                    - (int64_t)r->header.clock_offset_ns;

    /*
     * Determine number of blocks to read. Three strategies, tried in order:
     *
     * 1. Meta file (current.trace.meta) — written atomically by the daemon
     *    after each block flush. Contains the committed block count. This
     *    is the ONLY reliable way to read current.trace while the daemon
     *    is writing to it. No scanning, no guessing, no race conditions.
     *
     * 2. Footer — for completed .trace.lz4 files. Last 4 bytes = num_blocks,
     *    preceding bytes = block index array. Written on rotation.
     *
     * 3. Sequential scan — backward compat for old files without meta.
     *    Only used when no meta file and no footer. Simple validation.
     */
    int have_index = 0;

    /* Strategy 1: Meta file (Kafka-style high watermark) */
    {
        char meta_path[600];
        snprintf(meta_path, sizeof(meta_path), "%s.meta", path);
        FILE *mf = fopen(meta_path, "r");
        if (mf) {
            int committed = 0;
            /* DUR-7: the committed count sizes an allocation and drives the
             * scan — cap it like the footer count, and give the block scan
             * the same per-block sanity caps as the fallback scan (a corrupt
             * meta or torn block header must never seek us into garbage). */
            if (fscanf(mf, "%d", &committed) == 1 && committed > 0 &&
                committed < PGWT_MAX_READ_BLOCKS) {
                /* Scan exactly 'committed' blocks sequentially */
                fseek(r->fp, (long)sizeof(struct pgwt_trace_file_header),
                      SEEK_SET);
                r->block_index = malloc(committed *
                                        sizeof(struct pgwt_block_index_entry));
                if (r->block_index) {
                    r->num_blocks = 0;
                    struct pgwt_trace_block_header bh;
                    while (r->num_blocks < committed &&
                           fread(&bh, sizeof(bh), 1, r->fp) == 1 &&
                           bh.compressed_size > 0 && bh.num_events > 0 &&
                           bh.num_events <= PGWT_BLOCK_EVENTS &&
                           bh.compressed_size <= PGWT_MAX_BLOCK_COMPRESSED) {
                        long off = ftell(r->fp) - (long)sizeof(bh);
                        r->block_index[r->num_blocks++] =
                            (struct pgwt_block_index_entry){
                                .timestamp_ns = bh.first_timestamp_ns,
                                .file_offset = (uint64_t)off,
                            };
                        if (fseek(r->fp, (long)bh.compressed_size,
                                  SEEK_CUR) != 0)
                            break;
                    }
                    if (r->num_blocks > 0)
                        have_index = 1;
                    else {
                        free(r->block_index);
                        r->block_index = NULL;
                    }
                }
            }
            fclose(mf);
        }
    }

    /* Strategy 2: Footer (for completed .trace.lz4 files) */
    if (!have_index) {
        uint32_t nb = 0;
        if (fseek(r->fp, -4, SEEK_END) == 0 &&
            fread(&nb, sizeof(nb), 1, r->fp) == 1 &&
            nb > 0 && nb < PGWT_MAX_READ_BLOCKS) {  /* sanity cap */
            long index_start = ftell(r->fp) - 4
                             - (long)nb * (long)sizeof(struct pgwt_block_index_entry);
            if (index_start >= (long)sizeof(struct pgwt_trace_file_header)) {
                fseek(r->fp, index_start, SEEK_SET);
                r->block_index = malloc(nb * sizeof(struct pgwt_block_index_entry));
                if (r->block_index &&
                    fread(r->block_index, sizeof(struct pgwt_block_index_entry),
                          nb, r->fp) == nb) {
                    /* DUR-7: a footer-less file's trailing bytes can decode
                     * as a plausible count. A REAL index has its first block
                     * right after the header and strictly increasing,
                     * in-bounds offsets — verify before trusting it. */
                    int valid =
                        r->block_index[0].file_offset ==
                        sizeof(struct pgwt_trace_file_header);
                    for (uint32_t i = 1; valid && i < nb; i++)
                        if (r->block_index[i].file_offset <=
                                r->block_index[i - 1].file_offset ||
                            r->block_index[i].file_offset >=
                                (uint64_t)index_start)
                            valid = 0;
                    if (valid) {
                        r->num_blocks = (int)nb;
                        have_index = 1;
                    } else {
                        free(r->block_index);
                        r->block_index = NULL;
                    }
                } else {
                    free(r->block_index);
                    r->block_index = NULL;
                }
            }
        }
    }

    /* Strategy 3: Sequential scan (backward compat, no meta or footer) */
    if (!have_index) {
        fseek(r->fp, (long)sizeof(struct pgwt_trace_file_header), SEEK_SET);
        int cap = 128;
        r->block_index = malloc(cap * sizeof(struct pgwt_block_index_entry));
        if (!r->block_index) {
            fclose(r->fp); r->fp = NULL;
            return -1;
        }
        r->num_blocks = 0;
        struct pgwt_trace_block_header bh;
        while (fread(&bh, sizeof(bh), 1, r->fp) == 1 &&
               bh.compressed_size > 0 && bh.num_events > 0 &&
               bh.num_events <= PGWT_BLOCK_EVENTS &&
               bh.compressed_size <= PGWT_MAX_BLOCK_COMPRESSED) {
            if (r->num_blocks >= cap) {
                cap *= 2;
                struct pgwt_block_index_entry *tmp =
                    realloc(r->block_index, cap * sizeof(*tmp));
                if (!tmp) break;
                r->block_index = tmp;
            }
            long block_offset = ftell(r->fp) - (long)sizeof(bh);
            r->block_index[r->num_blocks++] = (struct pgwt_block_index_entry){
                .timestamp_ns = bh.first_timestamp_ns,
                .file_offset = (uint64_t)block_offset,
            };
            if (fseek(r->fp, (long)bh.compressed_size, SEEK_CUR) != 0)
                break;
        }
        if (r->num_blocks == 0) {
            free(r->block_index); r->block_index = NULL;
            fclose(r->fp); r->fp = NULL;
            return -1;
        }
    }

    /* Allocate scratch buffers */
    r->decode_buf_size = PGWT_BLOCK_EVENTS * 36 + PGWT_BLOCK_EVENTS * 10;
    r->decode_buf = malloc(r->decode_buf_size);
    r->compress_buf_size = r->decode_buf_size;
    r->compress_buf = malloc(r->compress_buf_size);

    if (!r->decode_buf || !r->compress_buf) {
        pgwt_reader_close(r);
        return -1;
    }

    return 0;
}

void pgwt_reader_close(struct pgwt_event_reader *r)
{
    if (r->fp) { fclose(r->fp); r->fp = NULL; }
    free(r->block_index); r->block_index = NULL;
    free(r->compress_buf); r->compress_buf = NULL;
    free(r->decode_buf); r->decode_buf = NULL;
}

int pgwt_reader_block_info(struct pgwt_event_reader *r, int block_idx,
                           struct pgwt_block_info *info)
{
    if (block_idx < 0 || block_idx >= r->num_blocks || !r->fp || !info)
        return -1;

    fseek(r->fp, (long)r->block_index[block_idx].file_offset, SEEK_SET);
    struct pgwt_trace_block_header bh;
    if (fread(&bh, sizeof(bh), 1, r->fp) != 1)
        return -1;

    info->block_type        = (enum pgwt_block_type)bh.block_type;
    info->sample_period_ns  = bh.sample_period_ns;
    info->first_timestamp_ns = bh.first_timestamp_ns;
    info->last_timestamp_ns  = bh.last_timestamp_ns;
    return 0;
}

int pgwt_reader_decode_block(struct pgwt_event_reader *r, int block_idx,
                              struct pgwt_trace_event *out, int max_events)
{
    return pgwt_reader_decode_block_info(r, block_idx, out, max_events, NULL);
}

int pgwt_reader_decode_block_info(struct pgwt_event_reader *r, int block_idx,
                                   struct pgwt_trace_event *out, int max_events,
                                   struct pgwt_block_info *info)
{
    if (block_idx < 0 || block_idx >= r->num_blocks || !r->fp)
        return -1;

    fseek(r->fp, (long)r->block_index[block_idx].file_offset, SEEK_SET);

    /* Read block header */
    struct pgwt_trace_block_header bh;
    if (fread(&bh, sizeof(bh), 1, r->fp) != 1)
        return -1;

    enum pgwt_block_type block_type = (enum pgwt_block_type)bh.block_type;
    if (info) {
        info->block_type        = block_type;
        info->sample_period_ns  = bh.sample_period_ns;
        info->first_timestamp_ns = bh.first_timestamp_ns;
        info->last_timestamp_ns  = bh.last_timestamp_ns;
    }

    /* Unknown block type (written by a NEWER build): skip the block rather
     * than decode it with the wrong columnar layout — garbage must never be
     * surfaced as data. */
    if (bh.block_type > PGWT_BLOCK_SAMPLES)
        return -1;

    int count = (int)bh.num_events;
    if (count > max_events) count = max_events;

    /* Grow compress buffer if needed */
    if (bh.compressed_size > r->compress_buf_size) {
        uint8_t *tmp = realloc(r->compress_buf, bh.compressed_size);
        if (!tmp) return -1;
        r->compress_buf = tmp;
        r->compress_buf_size = bh.compressed_size;
    }
    if (fread(r->compress_buf, 1, bh.compressed_size, r->fp) != bh.compressed_size)
        return -1;

    /* Grow decode buffer if needed */
    if (bh.uncompressed_size > r->decode_buf_size) {
        uint8_t *tmp = realloc(r->decode_buf, bh.uncompressed_size);
        if (!tmp) return -1;
        r->decode_buf = tmp;
        r->decode_buf_size = bh.uncompressed_size;
    }

    /* LZ4 decompress */
    int decompressed = LZ4_decompress_safe(
        (const char *)r->compress_buf, (char *)r->decode_buf,
        (int)bh.compressed_size, (int)r->decode_buf_size);
    if (decompressed < 0)
        return -1;

    /* Columnar decode */
    const uint8_t *p = r->decode_buf;
    size_t avail = (size_t)decompressed;

    /* Column 1: timestamps (delta varint) — common to both block types */
    uint64_t prev_ts = 0;
    for (int i = 0; i < count; i++) {
        uint64_t delta;
        int n = pgwt_decode_varint(p, avail, &delta);
        if (n < 0) return -1;
        p += n; avail -= n;
        prev_ts += delta;
        out[i].timestamp_ns = prev_ts;
    }

    /* Column 2: PIDs (raw uint32) — common to both block types */
    for (int i = 0; i < count; i++) {
        if (avail < 4) return -1;
        memcpy(&out[i].pid, p, 4); p += 4; avail -= 4;
    }

    if (block_type == PGWT_BLOCK_SAMPLES) {
        /* SAMPLES layout: event (u32), query_id (u64). No old_event,
         * no duration. Surface as a point observation: sampled event in
         * new_event, old_event/duration cleared, FLAG_SAMPLE set. */
        for (int i = 0; i < count; i++) {
            if (avail < 4) return -1;
            memcpy(&out[i].new_event, p, 4); p += 4; avail -= 4;
        }
        for (int i = 0; i < count; i++) {
            if (avail < 8) return -1;
            memcpy(&out[i].query_id, p, 8); p += 8; avail -= 8;
        }
        for (int i = 0; i < count; i++) {
            out[i].old_event = 0;
            out[i].duration_ns = 0;
            out[i].flags = PGWT_EVENT_FLAG_SAMPLE;
            /* Samples carry no measured CPU — the sampled tier's CPU signal is
             * the we==0 sample itself, weighted by sample_period. */
            out[i].cpu_ns = PGWT_CPU_NS_UNKNOWN;
        }
        return count;
    }

    /* TRANSITIONS layout — inverse of pgwt_encode_block() */

    /* Column 3: old_events (raw uint32) */
    for (int i = 0; i < count; i++) {
        if (avail < 4) return -1;
        memcpy(&out[i].old_event, p, 4); p += 4; avail -= 4;
    }

    /* Column 4: new_events (raw uint32) */
    for (int i = 0; i < count; i++) {
        if (avail < 4) return -1;
        memcpy(&out[i].new_event, p, 4); p += 4; avail -= 4;
    }

    /* Column 5: durations (varint) */
    for (int i = 0; i < count; i++) {
        int n = pgwt_decode_varint(p, avail, &out[i].duration_ns);
        if (n < 0) return -1;
        p += n; avail -= n;
    }

    /* Column 6: query_ids (raw uint64) */
    for (int i = 0; i < count; i++) {
        if (avail < 8) return -1;
        memcpy(&out[i].query_id, p, 8); p += 8; avail -= 8;
    }

    /* Column 7 (v3, T8): cpu_ns (varint). v2 files have no such column — their
     * events surface the UNKNOWN sentinel so the compute layer falls back to
     * gap-inference (an established, unchanged behavior for old traces). */
    if (r->header.version >= 3) {
        for (int i = 0; i < count; i++) {
            int n = pgwt_decode_varint(p, avail, &out[i].cpu_ns);
            if (n < 0) return -1;
            p += n; avail -= n;
        }
    } else {
        for (int i = 0; i < count; i++)
            out[i].cpu_ns = PGWT_CPU_NS_UNKNOWN;
    }

    /* Clear flags (transition records carry no per-record flag). Synthetic
     * attribution quality is durable in the sourced query-text sidecar. */
    for (int i = 0; i < count; i++)
        out[i].flags = 0;

    return count;
}

int pgwt_reader_find_block(const struct pgwt_event_reader *r, uint64_t mono_ns)
{
    int lo = 0, hi = r->num_blocks;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (r->block_index[mid].timestamp_ns <= mono_ns)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo > 0) ? lo - 1 : 0;
}

uint64_t pgwt_reader_wall_to_mono(const struct pgwt_event_reader *r, uint64_t wall_ns)
{
    return (uint64_t)((int64_t)wall_ns - r->mono_to_wall);
}

uint64_t pgwt_reader_mono_to_wall(const struct pgwt_event_reader *r, uint64_t mono_ns)
{
    return (uint64_t)((int64_t)mono_ns + r->mono_to_wall);
}

/* ── Multi-file scanning ───────────────────────────────── */

int pgwt_scan_trace_files(const char *trace_dir,
                           struct pgwt_trace_file_entry *entries,
                           int max_entries)
{
    DIR *dir = opendir(trace_dir);
    if (!dir) return -1;

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max_entries) {
        struct pgwt_trace_file_entry *e = &entries[n];

        /* Check suffix explicitly — sscanf returns 4 for ANY .*.lz4 file */
        const char *suffix = strstr(ent->d_name, ".trace.lz4");
        if (suffix && suffix[10] == '\0' &&
            sscanf(ent->d_name, "%4d-%2d-%2d_%2d.trace.lz4",
                   &e->year, &e->month, &e->day, &e->hour) == 4) {
            snprintf(e->path, sizeof(e->path), "%s/%s", trace_dir, ent->d_name);
            struct tm tm = {0};
            tm.tm_year = e->year - 1900;
            tm.tm_mon  = e->month - 1;
            tm.tm_mday = e->day;
            tm.tm_hour = e->hour;
            tm.tm_isdst = -1;
            time_t t = mktime(&tm);
            e->start_wall_ns = (uint64_t)t * 1000000000ULL;
            n++;
        } else if (strcmp(ent->d_name, "current.trace") == 0) {
            snprintf(e->path, sizeof(e->path), "%s/%s", trace_dir, ent->d_name);
            e->year = e->month = e->day = e->hour = 0;
            e->start_wall_ns = 0;
            FILE *fp = fopen(e->path, "rb");
            if (fp) {
                struct pgwt_trace_file_header hdr;
                if (fread(&hdr, sizeof(hdr), 1, fp) == 1 &&
                    hdr.magic == PGWT_TRACE_MAGIC) {
                    e->start_wall_ns = hdr.start_time_ns;
                }
                fclose(fp);
            }
            if (e->start_wall_ns > 0)
                n++;
        }
    }
    closedir(dir);

    /* Sort by start_wall_ns ascending (insertion sort, small N) */
    for (int i = 1; i < n; i++) {
        struct pgwt_trace_file_entry tmp = entries[i];
        int j = i - 1;
        while (j >= 0 && entries[j].start_wall_ns > tmp.start_wall_ns) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = tmp;
    }

    return n;
}

/* ── Time parser ───────────────────────────────────────── */

int pgwt_parse_time(const char *str, uint64_t *wall_ns)
{
    if (!str || !str[0]) return -1;

    if (strcmp(str, "now") == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        *wall_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        return 0;
    }

    /* ISO 8601: "2025-02-25T14:30:00" or "2025-02-25 14:30:00" */
    struct tm tm = {0};
    tm.tm_isdst = -1;
    if (sscanf(str, "%4d-%2d-%2dT%2d:%2d:%2d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6 ||
        sscanf(str, "%4d-%2d-%2d %2d:%2d:%2d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900;
        tm.tm_mon  -= 1;
        time_t t = mktime(&tm);
        if (t == (time_t)-1) return -1;
        *wall_ns = (uint64_t)t * 1000000000ULL;
        return 0;
    }

    /* Relative: "1h", "30m", "2h30m", "90s" (= time ago from now) */
    const char *p = str;
    uint64_t total_seconds = 0;
    int found = 0;
    while (*p) {
        char *end;
        long val = strtol(p, &end, 10);
        if (end == p || val < 0) break;
        switch (*end) {
        case 'h': total_seconds += val * 3600; p = end + 1; found++; break;
        case 'm': total_seconds += val * 60;   p = end + 1; found++; break;
        case 's': total_seconds += val;         p = end + 1; found++; break;
        default: goto done_relative;
        }
    }
done_relative:
    if (found > 0 && *p == '\0') {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        *wall_ns = now - total_seconds * 1000000000ULL;
        return 0;
    }

    return -1;
}

/* ── Replay accumulation (fidelity-aware — T1/FID-5) ────── */

#ifndef PGWT_SERVER
void pgwt_replay_events(struct pgwt_accumulator *acc,
                         const struct pgwt_trace_event *events, int count,
                         uint64_t from_mono_ns, uint64_t to_mono_ns,
                         const struct pgwt_block_info *bi,
                         struct pgwt_replay_stats *st)
{
    int is_samples = bi && bi->block_type == PGWT_BLOCK_SAMPLES;
    uint64_t period = is_samples ? bi->sample_period_ns : 0;

    if (st && is_samples && period)
        st->sample_period_ns = period;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *evt = &events[i];

        if (from_mono_ns > 0 && evt->timestamp_ns < from_mono_ns)
            continue;
        if (to_mono_ns > 0 && evt->timestamp_ns > to_mono_ns)
            break;  /* events are sorted by timestamp */

        uint32_t we;
        uint64_t dur;
        int exact;

        if (is_samples || (evt->flags & PGWT_EVENT_FLAG_SAMPLE)) {
            /* A sample is a point observation of the CURRENT wait
             * (new_event); it is worth one sample period of estimated
             * time, and carries no real duration. */
            we = evt->new_event;
            dur = period;
            exact = 0;
        } else {
            we = evt->old_event;
            dur = evt->duration_ns;
            exact = 1;

            /* Markers (exec/plan/escalation) are structural records —
             * never wait data (FID-4/5). */
            if (PGWT_IS_MARKER(we)) {
                if (st) st->markers_skipped++;
                continue;
            }
        }

        if (st) {
            if (exact) st->transitions++;
            else       st->samples++;
        }

        /* Per-PID accumulation */
        struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(acc, evt->pid);
        if (pa) {
            struct pgwt_event_stats *es = pgwt_get_or_create_event(pa, we);
            if (es) {
                es->count++;
                es->total_ns += dur;
                if (exact) {
                    if (dur < es->min_ns) es->min_ns = dur;
                    if (dur > es->max_ns) es->max_ns = dur;
                    uint32_t bucket = pgwt_duration_to_bucket(dur);
                    if (bucket < HISTOGRAM_BUCKETS)
                        es->histogram[bucket]++;
                }
            }
            if (we == 0)
                pa->cpu_time_ns += dur;
            else if (!pgwt_is_idle_event(we))
                pa->wait_time_ns += dur;
            if (!pgwt_is_idle_event(we))
                pa->db_time_ns += dur;
        }

        /* System-wide accumulation */
        struct pgwt_event_stats *se = pgwt_get_or_create_system_event(acc, we);
        if (se) {
            se->count++;
            se->total_ns += dur;
            if (exact) {
                if (dur < se->min_ns) se->min_ns = dur;
                if (dur > se->max_ns) se->max_ns = dur;
                uint32_t bucket = pgwt_duration_to_bucket(dur);
                if (bucket < HISTOGRAM_BUCKETS)
                    se->histogram[bucket]++;
            }
        }

        /* Time model */
        pgwt_update_time_model(&acc->tm, we, dur);

        /* Query events */
        if (evt->query_id != 0) {
            struct pgwt_query_event_stats *qe =
                pgwt_get_or_create_query_event(acc, evt->query_id, we);
            if (qe) {
                qe->count++;
                qe->total_ns += dur;
                if (exact) {
                    if (dur < qe->min_ns) qe->min_ns = dur;
                    if (dur > qe->max_ns) qe->max_ns = dur;
                }
            }
        }
    }
}
#endif /* !PGWT_SERVER */
