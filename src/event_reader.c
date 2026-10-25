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

    /* #429: does block_index[] actually ascend? It is built in COMMIT order
     * (by the writer's footer, by the meta-file read, or by the fallback
     * sequential scan — all three append in file order), and nothing makes
     * that timestamp order. Check it once here instead of assuming it in
     * every binary search; see the field's comment in event_reader.h for the
     * lagging-drain case that breaks it in a real `--mode full` capture. */
    r->block_index_sorted = 1;
    for (int i = 1; i < r->num_blocks; i++) {
        if (r->block_index[i].timestamp_ns <
            r->block_index[i - 1].timestamp_ns) {
            r->block_index_sorted = 0;
            break;
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
    info->num_events         = bh.num_events;
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
        info->num_events         = bh.num_events;
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
    /* #429. A binary search needs a sorted array, and block_index[] is in
     * COMMIT order. On a lagging `--mode full` drain the two disagree, and
     * then this search does not return a slightly wrong block — it returns
     * an ARBITRARY one. Measured on a trace written through the public
     * writer API with first timestamps [1000 ms, 6000 ms, 1100 ms]: the
     * search for 6000 ms returns block 2 (TRANSITIONS, 1100-1119 ms) and
     * skips block 1 (SAMPLES, 6000-7100 ms) — the only block covering that
     * window. Every record in the window is lost and the scan reports
     * itself complete.
     *
     * So when the precondition does not hold, refuse to narrow: 0 says
     * "start at the beginning", which is slower and correct, instead of a
     * confident wrong answer. A caller that wants the fast path can test
     * r->block_index_sorted itself. */
    if (!r->block_index_sorted)
        return 0;

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

int pgwt_reader_block_candidate(struct pgwt_event_reader *r, int bi,
                                uint64_t from_mono_ns, uint64_t to_mono_ns,
                                struct pgwt_block_info *info, int *right_open)
{
    struct pgwt_block_info hdr;
    if (right_open) *right_open = 0;
    if (pgwt_reader_block_info(r, bi, &hdr) != 0)
        return -1;
    if (info) *info = hdr;

    /* A record is written in the block it CLOSES in, so a block whose last
     * timestamp is before `from` holds nothing that reaches the window —
     * whatever its first timestamp or its position in the index says. This
     * is the only exclusion that is sound under commit order. */
    if (from_mono_ns != 0 && hdr.last_timestamp_ns < from_mono_ns)
        return 0;

    if (to_mono_ns != 0 && hdr.first_timestamp_ns > to_mono_ns) {
        /* Everything in this block closes after `to`. An interval record can
         * still have STARTED inside the window (a wait still running at
         * `to`), so the block is a straddler candidate and the caller's
         * budget decides whether to pay for it. A SAMPLES block cannot: its
         * records are point observations, so a sample past `to` is simply
         * outside the window. */
        if (hdr.block_type == PGWT_BLOCK_SAMPLES)
            return 0;
        if (right_open) *right_open = 1;
    }
    return 1;
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
            e->placement_unknown = false;

            /* #405: this USED to derive the archive's placement from its
             * NAME via mktime() — but the name is formatted by the WRITER's
             * localtime_r at rotation (event_writer.c), so a reader in a
             * different timezone parses the identical digits as a different
             * instant. A replay bounded by --from/--to then silently drops
             * whatever file that shift displaces past the window edge: no
             * error, just missing data (src/replay.c's quick filter trusts
             * this value to decide a file need not even be opened).
             *
             * The header's start_time_ns is CLOCK_REALTIME at file creation
             * (event_writer.h) — the same absolute instant everywhere, and
             * it is written uncompressed at the front of the file
             * (event_writer.c), so reading it here costs one small fread,
             * no LZ4 decompression. Trust it, not the name.
             *
             * If the header cannot be read or validated, do NOT fall back
             * to the name: that fallback IS the TZ bug this replaces, just
             * on a smaller set of files. Disable replay for this one file
             * instead — placement_unknown stays visible on the entry (still
             * listed, never silently dropped) and src/replay.c refuses to
             * use it rather than guess. */
            uint64_t hdr_start_ns = 0;
            FILE *fp = fopen(e->path, "rb");
            if (fp) {
                struct pgwt_trace_file_header hdr;
                if (fread(&hdr, sizeof(hdr), 1, fp) == 1 &&
                    hdr.magic == PGWT_TRACE_MAGIC &&
                    hdr.version >= 2 && hdr.version <= PGWT_TRACE_VERSION)
                    hdr_start_ns = hdr.start_time_ns;
                fclose(fp);
            }
            if (hdr_start_ns > 0) {
                e->start_wall_ns = hdr_start_ns;
            } else {
                fprintf(stderr,
                        "WARN: cannot establish time placement for %s from "
                        "its header — replay for this file is disabled "
                        "rather than guessed from its name (#405)\n",
                        e->path);
                e->start_wall_ns = 0;
                e->placement_unknown = true;
            }
            n++;
        } else if (strcmp(ent->d_name, "current.trace") == 0) {
            snprintf(e->path, sizeof(e->path), "%s/%s", trace_dir, ent->d_name);
            e->year = e->month = e->day = e->hour = 0;
            e->start_wall_ns = 0;
            e->placement_unknown = false;
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

bool pgwt_trace_file_quick_skip(const struct pgwt_trace_file_entry *entries,
                                int n, int idx, uint64_t from_wall_ns)
{
    if (from_wall_ns == 0)
        return false;
    if (idx < 0 || idx + 1 >= n)
        return false;
    /* The ENTRY ITSELF, not just its successor: the scanner's insertion
     * sort puts a start_wall_ns==0 (placement_unknown) entry FIRST, so an
     * unplaceable file lands at a low idx -- exactly where this predicate
     * is asked "can I skip this one?" most often. Its own coverage being
     * unknown is reason enough to refuse, regardless of what comes after. */
    if (entries[idx].placement_unknown)
        return false;
    const struct pgwt_trace_file_entry *next = &entries[idx + 1];
    if (next->placement_unknown || next->start_wall_ns == 0)
        return false;
    return next->start_wall_ns < from_wall_ns;
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

/* Window clipping for replay — the SAME rule as the server's raw path
 * (event_window_ns, src/compute.c).
 *
 * A trace record's timestamp_ns is when the wait ENDED and duration_ns is how
 * long it ran, so the record covers the interval [ts - dur, ts]. A record
 * that straddles a window edge contributes only the part inside the window.
 *
 * Deliberately a verbatim copy of compute.c's helper rather than a shared
 * symbol: compute.c belongs to the pgwt-server build and this replay half is
 * #ifdef'd out of it, so no translation unit can hold both. Keep the two in
 * step — tests/test_replay_window.c pins this one on the same straddler cases
 * tests/test_window_clip.c pins the other.
 *
 * Returns 0 for a record wholly outside the window (never a wrapped uint64
 * difference), and 0 for the impossible record (duration longer than its own
 * absolute monotonic timestamp) that compute.c refuses the same way.
 * from_ns == 0 / to_ns == 0 mean "unbounded on that side", so a whole-capture
 * replay still credits exactly duration_ns. */
static inline uint64_t replay_window_ns(const struct pgwt_trace_event *ev,
                                        uint64_t from_ns, uint64_t to_ns)
{
    if (ev->duration_ns > ev->timestamp_ns)
        return 0;
    uint64_t start = ev->timestamp_ns - ev->duration_ns;
    uint64_t end   = ev->timestamp_ns;
    if (from_ns != 0 && start < from_ns) start = from_ns;
    if (to_ns   != 0 && end   > to_ns)   end   = to_ns;
    return end > start ? end - start : 0;
}

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
        int sampled = is_samples || (evt->flags & PGWT_EVENT_FLAG_SAMPLE);

        uint32_t we;
        uint64_t add_ns;   /* time credited to THIS window (clipped) */
        uint64_t occ_ns;   /* this occurrence's whole duration */
        int exact;
        int closed_in_window;

        if (sampled) {
            /* A sample is a POINT observation of the CURRENT wait
             * (new_event): worth one sample period of estimated time, no real
             * duration, and no interval to clip. So it keeps its own
             * timestamp-selected rule — the overlap rule below is for
             * intervals only. */
            if (from_mono_ns > 0 && evt->timestamp_ns < from_mono_ns)
                continue;
            if (to_mono_ns > 0 && evt->timestamp_ns > to_mono_ns) {
                if (is_samples)
                    break;  /* a SAMPLES block is sorted and holds only points */
                continue;
            }
            we = evt->new_event;
            occ_ns = add_ns = period;
            exact = 0;
            closed_in_window = 1;
        } else {
            /* INTERVAL record (#424). Admission is by OVERLAP
             * (close >= from && start <= to) and the contribution is CLIPPED
             * to the window. The old rule selected on the close timestamp
             * alone and added duration_ns whole, which was wrong twice:
             *   - a LEFT straddler (started before `from`) over-reported by
             *     everything that ran before the window opened;
             *   - a RIGHT straddler (still running at `to`) was dropped
             *     entirely, contributing 0 of its in-window part.
             *
             * There is no `break` on ts > to any more: the records past the
             * right edge are exactly where the right straddlers are. The
             * block loop (pgwt_replay_file) has to scan past `to` for the
             * same reason — fixing only this loop leaves them unreachable.
             *
             * What this canNOT fix, and no admission rule can: a wait that is
             * still open has no closing record in the trace at all, so a
             * window at the very end of a still-growing capture still
             * under-reports it until the close is written. That needs
             * open-interval markers in the trace format. */
            add_ns = replay_window_ns(evt, from_mono_ns, to_mono_ns);
            closed_in_window =
                (from_mono_ns == 0 || evt->timestamp_ns >= from_mono_ns) &&
                (to_mono_ns   == 0 || evt->timestamp_ns <= to_mono_ns);
            if (add_ns == 0 && !closed_in_window)
                continue;   /* no overlap with the window at all */

            we = evt->old_event;
            occ_ns = evt->duration_ns;

            /* Markers (exec/plan/escalation) are structural records —
             * never wait data (FID-4/5). Counted as skipped only when they
             * fall in the window, exactly as before this change: a marker
             * carries no duration, so overlap and close-in-window agree. */
            if (PGWT_IS_MARKER(we)) {
                if (st && closed_in_window) st->markers_skipped++;
                continue;
            }
            exact = 1;
        }

        /* INSTANT vs INTERVAL, the product-wide rule (src/summary_writer.h's
         * v8 note, src/compute.c's raw half): TIME is overlap-selected and
         * clipped; per-OCCURRENCE statistics (count, histogram, min/max) are
         * selected by the record's own close timestamp and use the whole
         * duration. Widening admission without this split would turn a known
         * undercount of time into a new OVERCOUNT of counts and latencies —
         * two adjacent windows would each count the same close. */
        if (st) {
            if (!exact)                 st->samples++;
            else if (closed_in_window)  st->transitions++;
            else                        st->straddlers_admitted++;
        }

        uint64_t cpu_add = add_ns, offcpu_add = 0;
        /* Per-event-table row value (#531). `row_add` is what the TIME
         * column of every per-event table gets (window-clipped), `row_occ`
         * what the per-OCCURRENCE columns get (min/max/histogram, the whole
         * occurrence). Both are wall for every wait and idle label; for the
         * CPU* row they are the MEASURED on-CPU ns, exactly as
         * map_reader.c's add_event_stats does on the live path. Before this,
         * one replay invocation printed the same "CPU*" label twice with two
         * different numbers -- wall in system_event/session_event/
         * query_event, measured in time_model -- and src/output.c's
         * print_offcpu_row_wide (#498) then added the off-CPU remainder on
         * top of a row that already held the whole wall, so the view's %DB
         * column summed past 100%. */
        uint64_t row_add = add_ns, row_occ = occ_ns;
        if (we == 0 && !sampled && evt->cpu_ns != PGWT_CPU_NS_UNKNOWN) {
            /* #495: match map_reader.c's measured CPU split and compute.c's
             * window scaling. Clamp skew before scaling; measured zero is
             * real zero, while UNKNOWN keeps legacy whole-gap CPU. Samples
             * keep their whole sample-period weight as CPU, with no
             * off-CPU. */
            uint64_t m = evt->cpu_ns < occ_ns ? evt->cpu_ns : occ_ns;
            cpu_add = occ_ns
                ? (uint64_t)(((unsigned __int128)m * add_ns) / occ_ns) : 0;
            if (cpu_add > add_ns) cpu_add = add_ns;
            offcpu_add = add_ns - cpu_add;
            row_add = cpu_add;
            row_occ = m;
        }

        /* Per-PID accumulation */
        struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(acc, evt->pid);
        if (pa) {
            struct pgwt_event_stats *es = pgwt_get_or_create_event(pa, we);
            if (es) {
                if (closed_in_window) es->count++;
                es->total_ns += row_add;
                if (exact && closed_in_window) {
                    if (row_occ < es->min_ns) es->min_ns = row_occ;
                    if (row_occ > es->max_ns) es->max_ns = row_occ;
                    uint32_t bucket = pgwt_duration_to_bucket(row_occ);
                    if (bucket < HISTOGRAM_BUCKETS)
                        es->histogram[bucket]++;
                }
            }
            if (we == 0)
                pa->cpu_time_ns += cpu_add;
            else if (!pgwt_is_idle_event(we))
                pa->wait_time_ns += add_ns;
            if (!pgwt_is_idle_event(we))
                pa->db_time_ns += add_ns;
        }

        /* System-wide accumulation */
        struct pgwt_event_stats *se = pgwt_get_or_create_system_event(acc, we);
        if (se) {
            if (closed_in_window) se->count++;
            se->total_ns += row_add;
            if (exact && closed_in_window) {
                if (row_occ < se->min_ns) se->min_ns = row_occ;
                if (row_occ > se->max_ns) se->max_ns = row_occ;
                uint32_t bucket = pgwt_duration_to_bucket(row_occ);
                if (bucket < HISTOGRAM_BUCKETS)
                    se->histogram[bucket]++;
            }
        }

        /* #495: split inline as in map_reader.c; pgwt_update_time_model
         * accepts only wall time and cannot express measured off-CPU.
         * Wait-labelled spin stays observability only (compute.c): every
         * other label keeps its full wall contribution. */
        if (we == 0) {
            acc->tm.cpu_time_ns += cpu_add;
            acc->tm.offcpu_time_ns += offcpu_add;
            acc->tm.db_time_ns += add_ns;
        } else {
            pgwt_update_time_model(&acc->tm, we, add_ns);
        }

        /* Query events */
        if (evt->query_id != 0) {
            struct pgwt_query_event_stats *qe =
                pgwt_get_or_create_query_event(acc, evt->query_id, we);
            if (qe) {
                if (closed_in_window) qe->count++;
                qe->total_ns += row_add;
                if (exact && closed_in_window) {
                    if (row_occ < qe->min_ns) qe->min_ns = row_occ;
                    if (row_occ > qe->max_ns) qe->max_ns = row_occ;
                }
            }
        }
    }
}

int pgwt_replay_file(struct pgwt_event_reader *r,
                     struct pgwt_accumulator *acc,
                     uint64_t from_mono_ns, uint64_t to_mono_ns,
                     struct pgwt_trace_event *buf, int max_events,
                     int *lookahead_budget,
                     struct pgwt_replay_stats *st,
                     uint64_t *out_decoded)
{
    if (out_decoded) *out_decoded = 0;
    if (!r || !acc || !buf || max_events <= 0)
        return -1;

    uint64_t decoded = 0;

    /* EVERY block is examined, by its HEADER (#424 both edges, #429).
     *
     * The pattern this replaces — binary-search block_index[] for a start,
     * iterate forward, stop at the first block past `to` — is wrong three
     * times over, and each way is silent:
     *   - block_index[] is in COMMIT order, so the binary search has no
     *     precondition and returns an arbitrary index on a lagging
     *     `--mode full` drain (#429);
     *   - one start index assumes only the immediately preceding block can
     *     reach into the window, but blocks OVERLAP in time, so a block two
     *     or more positions back can still be inside it (#429; this cost
     *     400 ms of 1200 ms of sampled time in one window of the test);
     *   - stopping at `to` discards the straddlers still running there
     *     (#424).
     * A header read costs no decompression. Measured warm over a
     * 1024-block trace (tests/results/issue424-replay/bench-lookahead.log):
     * 0.12 us per block to ask, against 58 us to decode one. So the sound
     * scan is simply to ask every block whether it can matter, and the
     * binary search was never buying much to begin with.
     * There is NO `break` in this loop on purpose:
     * under commit order, a block that cannot matter says nothing about the
     * blocks after it. */
    for (int bi = 0; bi < r->num_blocks; bi++) {
        struct pgwt_block_info hdr;
        int right_open = 0;
        int cand = pgwt_reader_block_candidate(r, bi, from_mono_ns,
                                               to_mono_ns, &hdr, &right_open);
        if (cand == 0)
            continue;   /* provably holds nothing for this window */
        /* cand < 0: the header could not be read. NOT treated as "nothing
         * here" — fall through to the decode and let the record rule
         * decide, so a bad header can only cost work, never silently remove
         * time. If the decode fails too it is counted below. */

        if (cand > 0 && right_open) {
            /* Still-running straddlers only (#424). Nothing in the trace
             * bounds how far right the close can be — an io_worker can hold
             * one wait for a whole capture, and the index carries timestamps
             * but no maximum duration — so the scan past `to` is budgeted.
             * When the budget is gone the answer is MARKED
             * (right_edge_truncated) and the scan CONTINUES: skipping this
             * block must not skip the in-window blocks that follow it in
             * commit order. */
            if (lookahead_budget) {
                if (*lookahead_budget <= 0) {
                    if (st) st->right_edge_truncated = 1;
                    continue;
                }
                (*lookahead_budget)--;
            }
        }

        struct pgwt_block_info binfo;
        int n = pgwt_reader_decode_block_info(r, bi, buf, max_events, &binfo);
        if (n <= 0) {
            /* A block that cannot be decoded is time this window will never
             * see. Skipping it quietly (what the old loop did) makes a
             * truncated or corrupt file look like a complete, smaller
             * answer; count it so the caller can say so. */
            if (n < 0 && st) st->blocks_unreadable++;
            continue;
        }

        pgwt_replay_events(acc, buf, n, from_mono_ns, to_mono_ns, &binfo, st);
        decoded += (uint64_t)n;
    }

    if (out_decoded) *out_decoded = decoded;
    return 0;
}
#endif /* !PGWT_SERVER */
