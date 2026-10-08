/* summary_reader.c — Summary file reader: block decode, time-range seek, bulk load */
#include "summary_reader.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <lz4.h>

/* ── Single-file reader ────────────────────────────────── */

int pgwt_summary_reader_open(struct pgwt_summary_reader *r, const char *path)
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

    if (r->header.magic != PGWT_SUMMARY_MAGIC) {
        fprintf(stderr, "WARN: bad magic in %s (0x%08x, expected PGWS)\n",
                path, r->header.magic);
        fclose(r->fp); r->fp = NULL;
        return -1;
    }
    /* COMPUTATION requires the CURRENT accounting version.
     *
     * v1/v2 precomputed their per-second class_ns and per-query totals under
     * the pre-2026-10-06 rule (idle events lumped into the class totals), so
     * mixing them with v3 seconds inside one window would BLEND two accounting
     * rules and report a DB Time that is neither. Refusing here is necessary
     * but NOT sufficient on its own: pgwt_visit_summaries SKIPS a file it
     * cannot open, which would silently return a plausible PARTIAL answer --
     * a window that looks fine and is quietly missing seconds. That is why
     * pgwt_summaries_window_current() exists and why pgwt-server preflights
     * the window before choosing this path at all (src/server.c
     * should_use_summaries). Startup recovery still accepts older intact
     * headers so they are archived, not called corrupt
     * (src/summary_writer.c). */
    if (r->header.version != PGWT_SUMMARY_VERSION) {
        fprintf(stderr, "WARN: summary %s is version %d, this build computes "
                "only from version %d (older files use the pre-2026-10-06 "
                "idle accounting) -- recomputing from raw events instead\n",
                path, r->header.version, PGWT_SUMMARY_VERSION);
        fclose(r->fp); r->fp = NULL;
        return -1;
    }

    r->mono_to_wall = (int64_t)r->header.start_time_ns
                    - (int64_t)r->header.clock_offset_ns;

    /* Same 3-strategy approach as event_reader:
     * 1. Meta file (committed block count from daemon)
     * 2. Footer (completed .summary.lz4 files)
     * 3. Sequential scan (backward compat) */
    int have_index = 0;

    /* Strategy 1: Meta file */
    {
        char meta_path[600];
        snprintf(meta_path, sizeof(meta_path), "%s.meta", path);
        FILE *mf = fopen(meta_path, "r");
        if (mf) {
            int committed = 0;
            /* DUR-7: cap the committed count (it sizes an allocation) and
             * sanity-check each block header, exactly like event_reader. */
            if (fscanf(mf, "%d", &committed) == 1 && committed > 0 &&
                committed < PGWT_MAX_READ_BLOCKS) {
                fseek(r->fp, (long)sizeof(struct pgwt_trace_file_header),
                      SEEK_SET);
                r->block_index = malloc(committed *
                                        sizeof(struct pgwt_block_index_entry));
                if (r->block_index) {
                    r->num_blocks = 0;
                    struct pgwt_summary_block_header bh;
                    while (r->num_blocks < committed &&
                           fread(&bh, sizeof(bh), 1, r->fp) == 1 &&
                           bh.compressed_size > 0 &&
                           bh.compressed_size <= PGWT_MAX_BLOCK_COMPRESSED) {
                        long off = ftell(r->fp) - (long)sizeof(bh);
                        r->block_index[r->num_blocks].file_offset = (uint64_t)off;
                        r->block_index[r->num_blocks].timestamp_ns = bh.wall_ns;
                        r->num_blocks++;
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

    /* Strategy 2: Footer */
    if (!have_index) {
        uint32_t nb = 0;
        if (fseek(r->fp, -4, SEEK_END) == 0 &&
            fread(&nb, sizeof(nb), 1, r->fp) == 1 &&
            nb > 0 && nb < PGWT_MAX_READ_BLOCKS) {
            long index_start = ftell(r->fp) - 4
                             - (long)nb * (long)sizeof(struct pgwt_block_index_entry);
            if (index_start >= (long)sizeof(struct pgwt_trace_file_header)) {
                fseek(r->fp, index_start, SEEK_SET);
                r->block_index = malloc(nb * sizeof(struct pgwt_block_index_entry));
                if (r->block_index &&
                    fread(r->block_index, sizeof(struct pgwt_block_index_entry),
                          nb, r->fp) == nb) {
                    /* DUR-7: verify the index shape before trusting it
                     * (see event_reader.c Strategy 2). */
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

    /* Strategy 3: Sequential scan */
    if (!have_index) {
        fseek(r->fp, (long)sizeof(struct pgwt_trace_file_header), SEEK_SET);
        int cap = 128;
        r->block_index = malloc(cap * sizeof(struct pgwt_block_index_entry));
        if (!r->block_index) {
            fclose(r->fp); r->fp = NULL;
            return -1;
        }
        r->num_blocks = 0;
        struct pgwt_summary_block_header bh;
        while (fread(&bh, sizeof(bh), 1, r->fp) == 1 &&
               bh.compressed_size > 0 && bh.uncompressed_size > 0 &&
               bh.compressed_size <= PGWT_MAX_BLOCK_COMPRESSED &&
               bh.uncompressed_size <= PGWT_MAX_BLOCK_COMPRESSED) {
            if (r->num_blocks >= cap) {
                cap *= 2;
                struct pgwt_block_index_entry *tmp =
                    realloc(r->block_index, cap * sizeof(*tmp));
                if (!tmp) break;
                r->block_index = tmp;
            }
            long block_start = ftell(r->fp) - (long)sizeof(bh);
            r->block_index[r->num_blocks].file_offset = (uint64_t)block_start;
            r->block_index[r->num_blocks].timestamp_ns = bh.wall_ns;
            r->num_blocks++;
            if (fseek(r->fp, (long)bh.compressed_size, SEEK_CUR) != 0)
                break;
        }
    }

    /* Allocate scratch buffers (summary records are up to ~260 KB uncompressed) */
    r->decode_buf_size = 300 * 1024;
    r->decode_buf = malloc(r->decode_buf_size);
    r->compress_buf_size = r->decode_buf_size;
    r->compress_buf = malloc(r->compress_buf_size);

    if (!r->decode_buf || !r->compress_buf) {
        pgwt_summary_reader_close(r);
        return -1;
    }

    return 0;
}

void pgwt_summary_reader_close(struct pgwt_summary_reader *r)
{
    if (r->fp) { fclose(r->fp); r->fp = NULL; }
    free(r->block_index); r->block_index = NULL;
    free(r->compress_buf); r->compress_buf = NULL;
    free(r->decode_buf); r->decode_buf = NULL;
}

int pgwt_summary_reader_decode_block(struct pgwt_summary_reader *r,
                                      int block_idx,
                                      struct pgwt_summary_accum *out)
{
    if (block_idx < 0 || block_idx >= r->num_blocks || !r->fp)
        return -1;

    fseek(r->fp, (long)r->block_index[block_idx].file_offset, SEEK_SET);

    /* Read block header */
    struct pgwt_summary_block_header bh;
    if (fread(&bh, sizeof(bh), 1, r->fp) != 1)
        return -1;

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

    /* Initialize output accumulator from block header */
    memset(out, 0, sizeof(*out));
    out->second_wall_ns = bh.wall_ns;
    out->num_events = bh.num_events;
    out->num_sessions = bh.num_sessions;
    out->num_queries = bh.num_queries;

    /* Deserialize payload */
    if (pgwt_summary_deserialize(r->decode_buf, (size_t)decompressed, out,
                                  r->header.version) != 0)
        return -1;

    return 0;
}

int pgwt_summary_reader_find_block(const struct pgwt_summary_reader *r,
                                    uint64_t wall_ns)
{
    /* Block index timestamps are wall-clock for summary files */
    int lo = 0, hi = r->num_blocks;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (r->block_index[mid].timestamp_ns <= wall_ns)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo > 0) ? lo - 1 : 0;
}

/* ── Multi-file scanning ───────────────────────────────── */

int pgwt_scan_summary_files(const char *trace_dir,
                             struct pgwt_summary_file_entry *entries,
                             int max_entries)
{
    DIR *dir = opendir(trace_dir);
    if (!dir) return -1;

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max_entries) {
        struct pgwt_summary_file_entry *e = &entries[n];

        /* Check suffix explicitly — sscanf returns 4 for ANY .*.lz4 file */
        const char *suffix = strstr(ent->d_name, ".summary.lz4");
        if (suffix && suffix[12] == '\0' &&
            sscanf(ent->d_name, "%4d-%2d-%2d_%2d.summary.lz4",
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
        } else if (strcmp(ent->d_name, "current.summary") == 0) {
            snprintf(e->path, sizeof(e->path), "%s/%s", trace_dir, ent->d_name);
            e->year = e->month = e->day = e->hour = 0;
            e->start_wall_ns = 0;
            FILE *fp = fopen(e->path, "rb");
            if (fp) {
                struct pgwt_trace_file_header hdr;
                if (fread(&hdr, sizeof(hdr), 1, fp) == 1 &&
                    hdr.magic == PGWT_SUMMARY_MAGIC) {
                    e->start_wall_ns = hdr.start_time_ns;
                }
                fclose(fp);
            }
            if (e->start_wall_ns > 0)
                n++;
        }
    }
    closedir(dir);

    /* Sort by start_wall_ns ascending */
    for (int i = 1; i < n; i++) {
        struct pgwt_summary_file_entry tmp = entries[i];
        int j = i - 1;
        while (j >= 0 && entries[j].start_wall_ns > tmp.start_wall_ns) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = tmp;
    }

    return n;
}

/* ── Streaming visitor ─────────────────────────────────── */

/* Does this summary file's hour overlap [from, to]? The visitor and the
 * preflight MUST use the same test: if the preflight checked a narrower set of
 * files than the visitor then reads, it could approve a window containing a
 * file the visitor skips -- a gate that cannot see what it is gating. Sharing
 * the one function makes that divergence impossible rather than unlikely. */
static int summary_file_in_window(const struct pgwt_summary_file_entry *e,
                                  uint64_t from_wall_ns, uint64_t to_wall_ns)
{
    uint64_t file_end_ns = e->start_wall_ns + 3600ULL * 1000000000ULL;
    if (to_wall_ns > 0 && e->start_wall_ns > to_wall_ns)
        return 0;
    if (from_wall_ns > 0 && file_end_ns < from_wall_ns)
        return 0;
    return 1;
}

int pgwt_summaries_window_current(const char *trace_dir,
                                  uint64_t from_wall_ns, uint64_t to_wall_ns,
                                  int *out_considered, int *out_unusable)
{
    if (out_considered) *out_considered = 0;
    if (out_unusable)   *out_unusable = 0;

    struct pgwt_summary_file_entry files[256];
    int nfiles = pgwt_scan_summary_files(trace_dir, files, 256);
    /* A scan error (-1) is NOT "nothing to worry about": it means this
     * function could not see the directory it was asked to vouch for, so it
     * REFUSES. nfiles == 0 is different and genuinely fine -- there are no
     * summaries, the caller will read raw, and there is nothing to blend. */
    if (nfiles < 0)
        return 0;

    int considered = 0, unusable = 0;
    for (int f = 0; f < nfiles; f++) {
        if (!summary_file_in_window(&files[f], from_wall_ns, to_wall_ns))
            continue;
        considered++;

        FILE *fp = fopen(files[f].path, "rb");
        if (!fp) { unusable++; continue; }
        struct pgwt_trace_file_header hdr;
        /* An unreadable or short header counts as UNUSABLE, not as absent:
         * the visitor would skip this file and answer from the rest, which is
         * exactly the silent-partial-window failure this guard exists for. */
        if (fread(&hdr, sizeof(hdr), 1, fp) != 1 ||
            hdr.magic != PGWT_SUMMARY_MAGIC ||
            hdr.version != PGWT_SUMMARY_VERSION)
            unusable++;
        fclose(fp);
    }

    /* THE SCANNER'S BLIND SPOT, which is why this is not just a loop over
     * pgwt_scan_summary_files.
     *
     * A rotated YYYY-MM-DD_HH.summary.lz4 file derives its start from its NAME,
     * so the scanner lists it even when its contents are unreadable and the
     * loop above sees it. `current.summary` is different: the scanner reads its
     * START FROM ITS HEADER and DROPS the entry when that read fails
     * (`if (e->start_wall_ns > 0) n++`). So a current.summary with a short,
     * corrupt or older-version header is invisible to the scanner -- and
     * therefore invisible to pgwt_visit_summaries, which silently answers the
     * window from the remaining files and loses the current hour's seconds.
     * That is precisely the silent-partial-window failure this function exists
     * to prevent, so it has to look at the file directly.
     *
     * Its coverage is UNKNOWN when the header cannot be read, and unknown
     * coverage is never treated as "does not overlap": it counts against every
     * window. The cost of being wrong is one request recomputed from raw. */
    {
        char cur[600];
        snprintf(cur, sizeof(cur), "%s/current.summary", trace_dir);
        FILE *fp = fopen(cur, "rb");
        if (fp) {
            struct pgwt_trace_file_header hdr;
            /* EXACTLY the scanner's own listing condition. If it holds, the
             * loop above already examined this file (including its version) and
             * counting it again here would double-report it; if it does not, the
             * scanner dropped the entry and this is the only place that can see
             * the file at all. */
            int scanner_can_see = (fread(&hdr, sizeof(hdr), 1, fp) == 1 &&
                                   hdr.magic == PGWT_SUMMARY_MAGIC &&
                                   hdr.start_time_ns > 0);
            fclose(fp);
            if (!scanner_can_see) {
                considered++;
                unusable++;
            }
        }
    }

    if (out_considered) *out_considered = considered;
    if (out_unusable)   *out_unusable = unusable;
    return unusable == 0;
}

int pgwt_visit_summaries(const char *trace_dir,
                          uint64_t from_wall_ns, uint64_t to_wall_ns,
                          pgwt_summary_visitor visitor, void *ctx)
{
    /* Scan available summary files */
    struct pgwt_summary_file_entry files[256];
    int nfiles = pgwt_scan_summary_files(trace_dir, files, 256);
    if (nfiles <= 0)
        return nfiles;

    /* Single reusable decode buffer (~280KB) */
    struct pgwt_summary_accum *record = malloc(sizeof(*record));
    if (!record) return -1;

    int total = 0;

    for (int f = 0; f < nfiles; f++) {
        /* Quick range check: skip files obviously outside range */
        if (!summary_file_in_window(&files[f], from_wall_ns, to_wall_ns))
            continue;

        struct pgwt_summary_reader reader;
        if (pgwt_summary_reader_open(&reader, files[f].path) != 0)
            continue;

        /* Find starting block */
        int start_blk = 0;
        if (from_wall_ns > 0)
            start_blk = pgwt_summary_reader_find_block(&reader, from_wall_ns);

        int stop = 0;
        for (int b = start_blk; b < reader.num_blocks && !stop; b++) {
            /* Block-level PRUNE, deliberately still `>`: it is a conservative
             * over-fetch (one extra block decoded at the boundary, which the
             * per-record filter below then drops) and it does not depend on
             * the block index being sorted at the exact boundary. The record
             * filter is the authority on the window; this is an optimisation.
             * Keeping it loose means a bug in one cannot be masked by the
             * other -- #316's test asserts a window ending ON the last
             * record's second, which is the case that reaches here. */
            if (to_wall_ns > 0 &&
                reader.block_index[b].timestamp_ns > to_wall_ns)
                break;

            if (pgwt_summary_reader_decode_block(&reader, b, record) == 0) {
                /* Filter by exact time range.
                 *
                 * #316: HALF-OPEN, [from, to) -- `>=`, not `>`. Every other
                 * window consumer in the codebase is half-open (the raw
                 * path's event_window_ns clips at to_ns;
                 * pgwt_compute_heatmap drops `ev_ts >= to_ns`), and a record
                 * carries a WHOLE second, so including the one whose second
                 * STARTS at to_ns made every summary answer over [T, T+W)
                 * cover W+1 seconds. Measured on an interior 1 s window of
                 * the cross-check fixture: raw 58.872 ms vs summary 137.368
                 * ms, +133.3%. It was invisible on a live "last 15 minutes"
                 * (the second at to_ns is not flushed yet) and never
                 * invisible on a historical window -- and because
                 * aas_summary_visitor clamps an out-of-range bucket index to
                 * the last bucket, the extra second landed entirely on the
                 * final bar of every chart.
                 *
                 * This is READER-bound: it changes which records an answer
                 * is built from, not what any record contains, so it needs
                 * no summary version bump and it applies to files already on
                 * disk. The start bound stays INCLUSIVE. */
                uint64_t rec_ns = record->second_wall_ns;
                if (from_wall_ns > 0 && rec_ns < from_wall_ns)
                    continue;
                if (to_wall_ns > 0 && rec_ns >= to_wall_ns)
                    continue;

                if (visitor(record, ctx) != 0)
                    stop = 1;
                total++;
            }
        }

        pgwt_summary_reader_close(&reader);
        if (stop) break;
    }

    free(record);
    return total;
}
