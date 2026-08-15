/* event_writer.c — Raw event file writer: columnar encoding + LZ4
 *
 * Buffers events in blocks of 4096, encodes column-wise, LZ4-compresses,
 * writes to hourly-rotating trace files with a block index footer. */
#include "event_writer.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <lz4.h>

/* ── Varint (unsigned LEB128) ─────────────────────────────── */

int pgwt_encode_varint(uint64_t val, uint8_t *out)
{
    int n = 0;
    do {
        uint8_t byte = val & 0x7F;
        val >>= 7;
        if (val) byte |= 0x80;
        out[n++] = byte;
    } while (val);
    return n;
}

int pgwt_decode_varint(const uint8_t *in, size_t avail, uint64_t *val)
{
    uint64_t result = 0;
    int shift = 0, n = 0;
    do {
        if (n >= (int)avail || shift > 63) return -1;
        uint8_t byte = in[n];
        result |= (uint64_t)(byte & 0x7F) << shift;
        shift += 7;
        n++;
        if (!(byte & 0x80)) break;
    } while (1);
    *val = result;
    return n;
}

/* ── Columnar block encoder ───────────────────────────────── */

size_t pgwt_encode_block(const struct pgwt_trace_event *events, int count,
                         uint8_t *out, size_t out_size)
{
    uint8_t *p = out;
    (void)out_size;  /* caller guarantees sufficient space */

    /* Column 1: timestamps, delta-encoded as varint */
    uint64_t prev_ts = 0;
    for (int i = 0; i < count; i++) {
        uint64_t delta = events[i].timestamp_ns - prev_ts;
        p += pgwt_encode_varint(delta, p);
        prev_ts = events[i].timestamp_ns;
    }

    /* Column 2: PIDs as raw uint32 */
    for (int i = 0; i < count; i++) {
        uint32_t pid = events[i].pid;
        memcpy(p, &pid, 4); p += 4;
    }

    /* Column 3: old_events as raw uint32 */
    for (int i = 0; i < count; i++) {
        uint32_t ev = events[i].old_event;
        memcpy(p, &ev, 4); p += 4;
    }

    /* Column 4: new_events as raw uint32 */
    for (int i = 0; i < count; i++) {
        uint32_t ev = events[i].new_event;
        memcpy(p, &ev, 4); p += 4;
    }

    /* Column 5: durations as varint */
    for (int i = 0; i < count; i++) {
        p += pgwt_encode_varint(events[i].duration_ns, p);
    }

    /* Column 6: query_ids as raw uint64 */
    for (int i = 0; i < count; i++) {
        uint64_t qid = events[i].query_id;
        memcpy(p, &qid, 8); p += 8;
    }

    /* Column 7 (v3, T8): cpu_ns as varint. Measured on-CPU nanoseconds for the
     * interval, or PGWT_CPU_NS_UNKNOWN (encodes to a 10-byte varint, only on
     * legacy-capability traces). The daemon's push funnel stamps UNKNOWN when
     * CPU accounting is off; measured values (incl. genuine 0 for waits) are
     * written as-is. */
    for (int i = 0; i < count; i++) {
        p += pgwt_encode_varint(events[i].cpu_ns, p);
    }

    return (size_t)(p - out);
}

/* ── Columnar SAMPLE block encoder ────────────────────────── */
/* Samples are point observations: no old_event, no duration. The sampled
 * event id lives in new_event (the field the daemon's sampler populates).
 * Layout: ts (delta varint), pid (u32), event (u32), query_id (u64). */
size_t pgwt_encode_sample_block(const struct pgwt_trace_event *events, int count,
                                uint8_t *out, size_t out_size)
{
    uint8_t *p = out;
    (void)out_size;  /* caller guarantees sufficient space */

    /* Column 1: timestamps, delta-encoded as varint */
    uint64_t prev_ts = 0;
    for (int i = 0; i < count; i++) {
        uint64_t delta = events[i].timestamp_ns - prev_ts;
        p += pgwt_encode_varint(delta, p);
        prev_ts = events[i].timestamp_ns;
    }

    /* Column 2: PIDs as raw uint32 */
    for (int i = 0; i < count; i++) {
        uint32_t pid = events[i].pid;
        memcpy(p, &pid, 4); p += 4;
    }

    /* Column 3: events as raw uint32 (the sampled event id, from new_event) */
    for (int i = 0; i < count; i++) {
        uint32_t ev = events[i].new_event;
        memcpy(p, &ev, 4); p += 4;
    }

    /* Column 4: query_ids as raw uint64 */
    for (int i = 0; i < count; i++) {
        uint64_t qid = events[i].query_id;
        memcpy(p, &qid, 8); p += 8;
    }

    return (size_t)(p - out);
}

/* ── Shared file-lifecycle helpers (DUR-1/2/5) ────────────── */

void pgwt_fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

int pgwt_archive_path_nonclobber(const char *dir, const struct tm *tm,
                                 const char *ext, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%04d-%02d-%02d_%02d%s",
             dir, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, ext);
    if (access(out, F_OK) != 0)
        return 0;
    /* Target exists (restart mid-hour, or a DST fold repeating the hour):
     * pick a numeric-suffix name instead of clobbering. Readers accept the
     * suffixed form — pgwt_scan_trace_files / retention parse the leading
     * YYYY-MM-DD_HH and match the .trace.lz4/.summary.lz4 suffix, which the
     * extra ".N." does not disturb. */
    for (int n = 1; n < 10000; n++) {
        snprintf(out, out_size, "%s/%04d-%02d-%02d_%02d.%d%s",
                 dir, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, n, ext);
        if (access(out, F_OK) != 0)
            return 0;
    }
    return -1;
}

/* ── Startup recovery (DUR-1) ─────────────────────────────── */

/* A previous daemon left a current.trace behind (crash, kill -9, or plain
 * shutdown). NEVER truncate it — the old code's fopen("wb") erased up to an
 * hour of committed data on every restart. Instead: scan its committed
 * blocks (same sanity rules as the reader's fallback scan), drop any torn
 * tail, append a real footer, fsync, and rename it aside to a collision-safe
 * archive name so it stays readable like any rotated file. A file with a
 * valid header but no complete block carries no events; if it has any
 * payload beyond the header it is preserved as *.corrupt.<ts> for forensics,
 * otherwise removed. */
static void recover_current_trace(struct pgwt_event_writer *w)
{
    char cur[512], meta[600], meta_tmp[600];
    snprintf(cur, sizeof(cur), "%s/current.trace", w->trace_dir);
    snprintf(meta, sizeof(meta), "%s/current.trace.meta", w->trace_dir);
    snprintf(meta_tmp, sizeof(meta_tmp), "%s/current.trace.meta.tmp",
             w->trace_dir);

    /* A half-written meta tmp from a crash mid-meta-write is always stale. */
    unlink(meta_tmp);

    struct stat st;
    if (stat(cur, &st) != 0) {
        unlink(meta);   /* meta without a current file is an orphan */
        return;
    }

    /* Read the meta high-watermark BEFORE touching the file: it is the
     * committed-block floor the recovery scan must reach. */
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
    /* Accept any reader-supported version (v2..current): recovery only scans
     * block headers to rebuild the footer index — it never re-encodes the
     * columnar payload — so a v2 current.trace left by a pre-T8 daemon is
     * archived intact and stays readable as v2. */
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1 ||
        hdr.magic != PGWT_TRACE_MAGIC ||
        hdr.version < 2 || hdr.version > PGWT_TRACE_VERSION) {
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

    /* Scan committed blocks (strict: same caps as the reader, plus the block
     * payload must lie fully inside the file — a torn tail ends the scan). */
    long fsize = (long)st.st_size;
    struct pgwt_block_index_entry *idx = NULL;
    int nidx = 0, cap = 0;
    uint64_t nevents = 0;
    long pos = (long)sizeof(hdr);
    struct pgwt_trace_block_header bh;
    while (nidx < PGWT_MAX_READ_BLOCKS) {
        if (fseek(fp, pos, SEEK_SET) != 0 ||
            fread(&bh, sizeof(bh), 1, fp) != 1)
            break;
        if (bh.num_events == 0 || bh.num_events > PGWT_BLOCK_EVENTS ||
            bh.compressed_size == 0 ||
            bh.compressed_size > PGWT_MAX_BLOCK_COMPRESSED ||
            bh.block_type > 15)
            break;
        long end = pos + (long)sizeof(bh) + (long)bh.compressed_size;
        if (end > fsize)
            break;   /* torn tail: block header landed, payload didn't */
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
            .timestamp_ns = bh.first_timestamp_ns,
            .file_offset = (uint64_t)pos,
        };
        nevents += bh.num_events;
        pos = end;
    }

    if (committed > nidx)
        fprintf(stderr, "ERROR: recovery of %s found %d complete blocks but "
                "the meta high-watermark says %d were committed — data below "
                "the watermark is missing or corrupt\n", cur, nidx, committed);

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
            unlink(cur);   /* header-only file: nothing to preserve */
        }
        unlink(meta);
        pgwt_fsync_dir(w->trace_dir);
        return;
    }

    /* Finalize: drop the torn tail (or the previous footer, which re-scans
     * as an invalid block and is rewritten identically), append the footer,
     * make it durable. */
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
                "(%s) — archiving anyway; readers will fall back to a "
                "sequential scan\n", cur, strerror(errno));

    /* Archive under the hour of the file's own start time, never clobbering
     * an existing archive. */
    time_t start_sec = (time_t)(hdr.start_time_ns / 1000000000ULL);
    struct tm tm;
    localtime_r(&start_sec, &tm);
    char dest[512];
    if (pgwt_archive_path_nonclobber(w->trace_dir, &tm, ".trace.lz4",
                                     dest, sizeof(dest)) != 0 ||
        rename(cur, dest) != 0) {
        fprintf(stderr, "WARN: cannot archive recovered %s: %s "
                "(leaving it in place; it will NOT be truncated)\n",
                cur, strerror(errno));
        return;
    }
    unlink(meta);
    pgwt_fsync_dir(w->trace_dir);
    fprintf(stderr, "INFO: recovered previous current.trace: %d blocks, "
            "%llu events -> %s\n", nidx, (unsigned long long)nevents, dest);
}

/* ── File operations (static) ─────────────────────────────── */

static int write_footer(struct pgwt_event_writer *w)
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

static int open_trace_file(struct pgwt_event_writer *w, uint64_t first_mono_ns)
{
    (void)first_mono_ns;

    snprintf(w->current_path, sizeof(w->current_path),
             "%s/current.trace", w->trace_dir);

    /* DUR-1: NEVER truncate an existing current.trace ("wbx", not "wb").
     * Init already recovered any leftover file; if one (re)appeared since —
     * e.g. lazy first-open long after init — recover it now and retry. */
    w->fp = fopen(w->current_path, "wbxe");
    if (!w->fp && errno == EEXIST) {
        recover_current_trace(w);
        w->fp = fopen(w->current_path, "wbxe");
    }
    if (!w->fp) {
        fprintf(stderr, "WARN: cannot create %s: %s\n",
                w->current_path, strerror(errno));
        return -1;
    }

    /* Set group ownership and permissions for non-root access */
    if (w->trace_gid != (gid_t)-1) {
        fchown(fileno(w->fp), (uid_t)-1, w->trace_gid);
        fchmod(fileno(w->fp), 0640);
    }

    /* Capture wall-clock and monotonic timestamps */
    struct timespec wall, mono;
    clock_gettime(CLOCK_REALTIME, &wall);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    w->file_start_wall_ns = (uint64_t)wall.tv_sec * 1000000000ULL + wall.tv_nsec;
    w->file_start_mono_ns = (uint64_t)mono.tv_sec * 1000000000ULL + mono.tv_nsec;

    /* Record current hour for rotation */
    time_t now = wall.tv_sec;
    struct tm tm;
    localtime_r(&now, &tm);
    w->current_hour = tm.tm_yday * 24 + tm.tm_hour;

    /* Write file header */
    struct pgwt_trace_file_header hdr = {
        .magic = PGWT_TRACE_MAGIC,
        .version = PGWT_TRACE_VERSION,
        .flags = PGWT_FLAG_LZ4,
        .pg_version = (uint32_t)w->pg_version,
        .start_time_ns = w->file_start_wall_ns,
        .clock_offset_ns = w->file_start_mono_ns,
    };
    if (fwrite(&hdr, sizeof(hdr), 1, w->fp) != 1) {
        fprintf(stderr, "WARN: cannot write trace file header: %s\n", strerror(errno));
        fclose(w->fp);
        w->fp = NULL;
        return -1;
    }

    w->num_blocks = 0;
    return 0;
}

/* Encode, compress, and write one typed block (TRANSITIONS or SAMPLES).
 * `events`/`count` are the records; block_type selects the columnar layout;
 * sample_period_ns is recorded in the header (0 for TRANSITIONS). Updates
 * the block index, stats, and the committed-block meta file. */
static int write_typed_block(struct pgwt_event_writer *w,
                             const struct pgwt_trace_event *events, int count,
                             enum pgwt_block_type block_type,
                             uint64_t sample_period_ns)
{
    if (count <= 0 || !w->fp) return 0;

    /* Columnar encode (layout depends on block type) */
    size_t encoded_size = (block_type == PGWT_BLOCK_SAMPLES)
        ? pgwt_encode_sample_block(events, count, w->encode_buf, w->encode_buf_size)
        : pgwt_encode_block(events, count, w->encode_buf, w->encode_buf_size);

    /* LZ4 compress */
    int compressed_size = LZ4_compress_default(
        (const char *)w->encode_buf, (char *)w->compress_buf,
        (int)encoded_size, (int)w->compress_buf_size);
    if (compressed_size <= 0) {
        fprintf(stderr, "WARN: LZ4 compression failed\n");
        return -1;
    }

    /* DUR-8: a file must never outgrow what the readers' footer/meta sanity
     * caps accept. Ask for an early rotation at the writer cap; the readers
     * accept up to 4× it, which covers the few blocks written between the
     * cap being hit and the next rotation check (only reachable at
     * pathological block rates — see PGWT_MAX_FILE_BLOCKS). */
    if (w->num_blocks >= PGWT_MAX_FILE_BLOCKS && !w->force_rotate) {
        w->force_rotate = true;
        fprintf(stderr, "WARN: trace file reached %d blocks — forcing an "
                "early rotation\n", PGWT_MAX_FILE_BLOCKS);
    }

    /* Grow block index if needed */
    if (w->num_blocks >= w->block_index_cap) {
        int new_cap = w->block_index_cap * 2;
        struct pgwt_block_index_entry *new_idx =
            realloc(w->block_index, new_cap * sizeof(*new_idx));
        if (!new_idx) {
            fprintf(stderr, "WARN: cannot grow block index\n");
            w->enabled = false;
            return -1;
        }
        w->block_index = new_idx;
        w->block_index_cap = new_cap;
    }

    /* Record block index entry */
    long file_offset = ftell(w->fp);
    w->block_index[w->num_blocks++] = (struct pgwt_block_index_entry){
        .timestamp_ns = events[0].timestamp_ns,
        .file_offset = (uint64_t)file_offset,
    };

    /* Write block header + compressed data */
    struct pgwt_trace_block_header bh = {
        .first_timestamp_ns = events[0].timestamp_ns,
        .last_timestamp_ns = events[count - 1].timestamp_ns,
        .num_events = (uint32_t)count,
        .compressed_size = (uint32_t)compressed_size,
        .uncompressed_size = (uint32_t)encoded_size,
        .block_type = (uint16_t)block_type,
        .reserved = 0,
        .sample_period_ns = sample_period_ns,
    };
    if (fwrite(&bh, sizeof(bh), 1, w->fp) != 1 ||
        fwrite(w->compress_buf, 1, compressed_size, w->fp) != (size_t)compressed_size) {
        fprintf(stderr, "WARN: trace file write failed: %s\n", strerror(errno));
        w->enabled = false;
        return -1;
    }

    w->total_events_written += count;
    w->total_bytes_written += sizeof(bh) + compressed_size;

    /* Flush to OS page cache so concurrent readers can see this block */
    fflush(w->fp);

    /* Write committed block count atomically (Kafka-style high watermark).
     * Readers use this instead of scanning/guessing block count. */
    {
        char meta_path[512], tmp_path[512];
        snprintf(meta_path, sizeof(meta_path), "%s/current.trace.meta",
                 w->trace_dir);
        snprintf(tmp_path, sizeof(tmp_path), "%s/current.trace.meta.tmp",
                 w->trace_dir);
        FILE *mf = fopen(tmp_path, "w");
        if (mf) {
            fprintf(mf, "%d\n", w->num_blocks);
            fclose(mf);
            rename(tmp_path, meta_path);  /* atomic on POSIX */
        }
    }

    return 0;
}

/* Write the buffered TRANSITIONS block (no ordering logic — see flush_block). */
static int flush_block_core(struct pgwt_event_writer *w)
{
    if (w->num_events == 0 || !w->fp) return 0;

    int rc = write_typed_block(w, w->events, w->num_events,
                               PGWT_BLOCK_TRANSITIONS, 0);
    w->num_events = 0;
    return rc;
}

/* Write the pending batched sample-type block (DUR-8). The block period is
 * the count-weighted mean of the batched pushes' periods, so the block's
 * TOTAL estimated time equals the exact per-tick sum (SMP-3 weights are
 * preserved in aggregate; the compatibility gate in push bounds the per-tick
 * skew to ~1.6%). */
static int flush_sample_buf(struct pgwt_event_writer *w)
{
    if (w->num_sample_buf == 0 || !w->fp) return 0;

    uint64_t period = w->sample_buf_weight_ns / (uint64_t)w->num_sample_buf;
    int rc = write_typed_block(w, w->sample_buf, w->num_sample_buf,
                               (enum pgwt_block_type)w->sample_buf_type,
                               period);
    w->num_sample_buf = 0;
    w->sample_buf_weight_ns = 0;
    w->sample_buf_period_ns = 0;
    return rc;
}

/* Ordered flushes: whichever pending buffer holds the OLDER first timestamp
 * is written first, so the file's block index stays sorted (readers
 * binary-search it). Each buffer is internally time-sorted already. */
static int flush_block(struct pgwt_event_writer *w)
{
    if (w->num_sample_buf > 0 && w->num_events > 0 &&
        w->sample_buf[0].timestamp_ns < w->events[0].timestamp_ns) {
        if (flush_sample_buf(w) != 0)
            return -1;
    }
    return flush_block_core(w);
}

static int flush_samples(struct pgwt_event_writer *w)
{
    if (w->num_events > 0 && w->num_sample_buf > 0 &&
        w->events[0].timestamp_ns <= w->sample_buf[0].timestamp_ns) {
        if (flush_block_core(w) != 0)
            return -1;
    }
    return flush_sample_buf(w);
}

/* ── Public API ───────────────────────────────────────────── */

int pgwt_writer_init(struct pgwt_event_writer *w, const char *trace_dir,
                     int pg_version, int retention_hours,
                     const char *group_name)
{
    memset(w, 0, sizeof(*w));
    snprintf(w->trace_dir, sizeof(w->trace_dir), "%s", trace_dir);
    w->pg_version = pg_version;
    w->retention_hours = retention_hours;
    w->enabled = true;
    w->current_hour = -1;
    w->trace_gid = (gid_t)-1;

    /* Resolve trace group name → GID */
    if (group_name) {
        struct group *gr = getgrnam(group_name);
        if (gr) {
            w->trace_gid = gr->gr_gid;
        } else {
            fprintf(stderr, "WARN: group '%s' not found — "
                    "trace files will be readable by root only\n", group_name);
        }
    }

    /* Create trace directory with group-readable permissions */
    mkdir(w->trace_dir, 0750);  /* ignore EEXIST */
    if (w->trace_gid != (gid_t)-1) {
        chown(w->trace_dir, (uid_t)-1, w->trace_gid);
        chmod(w->trace_dir, 0750);
    }

    /* DUR-1: recover (finalize + archive) any current.trace a previous
     * daemon left behind — never truncate it. */
    recover_current_trace(w);

    /* Allocate scratch buffers. Per-event worst case (v3): ts<=10 + pid4 +
     * old4 + new4 + dur<=10 + qid8 + cpu_ns<=10 = 50 bytes. */
    w->encode_buf_size = PGWT_BLOCK_EVENTS * 56;
    w->encode_buf = malloc(w->encode_buf_size);

    w->compress_buf_size = LZ4_compressBound((int)w->encode_buf_size);
    w->compress_buf = malloc(w->compress_buf_size);

    w->block_index_cap = 256;
    w->block_index = malloc(w->block_index_cap * sizeof(*w->block_index));

    if (!w->encode_buf || !w->compress_buf || !w->block_index) {
        pgwt_writer_destroy(w);
        return -1;
    }

    return 0;
}

int pgwt_writer_push_event(struct pgwt_event_writer *w,
                           const struct pgwt_trace_event *evt)
{
    if (!w->enabled) return 0;

    /* Lazy file open on first event */
    if (!w->fp) {
        if (open_trace_file(w, evt->timestamp_ns) != 0) {
            w->enabled = false;
            return -1;
        }
    }

    struct pgwt_trace_event *dst = &w->events[w->num_events++];
    *dst = *evt;
    /* T8: on a legacy-capability daemon (no measured CPU), stamp every record
     * UNKNOWN so the reader/compute fall back to gap-inference rather than
     * trusting the BPF's 0. When accounting is on, a per-record UNKNOWN set by
     * a userspace producer (e.g. a flush that could not read schedstat) is
     * preserved. */
    if (!w->cpu_measured)
        dst->cpu_ns = PGWT_CPU_NS_UNKNOWN;

    if (w->num_events >= PGWT_BLOCK_EVENTS)
        return flush_block(w);

    return 0;
}

int pgwt_writer_push_samples(struct pgwt_event_writer *w,
                             const struct pgwt_trace_event *samples,
                             int count, uint64_t sample_period_ns)
{
    if (!w->enabled) return 0;
    if (count <= 0) return 0;

    /* Lazy file open on first write */
    if (!w->fp) {
        if (open_trace_file(w, samples[0].timestamp_ns) != 0) {
            w->enabled = false;
            return -1;
        }
    }

    /* DUR-8: buffer sample-type records and cut a block roughly once per
     * second instead of once per sampler tick (~10× fewer blocks at 10 Hz).
     * The buffering is type-agnostic: a pending batch of a DIFFERENT block
     * type is flushed before this one starts. A push whose period deviates
     * from the pending batch by more than ~1.6% (period/64) also flushes
     * first — timer jitter batches, an SMP-3 stall tick (measured elapsed ≫
     * nominal) gets its own block so its compensation weight stays exact. */
    if (w->num_sample_buf > 0) {
        uint64_t p0 = w->sample_buf_period_ns;
        uint64_t diff = sample_period_ns > p0 ? sample_period_ns - p0
                                              : p0 - sample_period_ns;
        if (w->sample_buf_type != (uint16_t)PGWT_BLOCK_SAMPLES ||
            diff > p0 / 64) {
            if (flush_samples(w) != 0)
                return -1;
        }
    }

    int off = 0;
    while (off < count) {
        if (w->num_sample_buf == 0) {
            w->sample_buf_type = (uint16_t)PGWT_BLOCK_SAMPLES;
            w->sample_buf_period_ns = sample_period_ns;
        }
        int room = PGWT_BLOCK_EVENTS - w->num_sample_buf;
        int chunk = count - off;
        if (chunk > room) chunk = room;
        memcpy(&w->sample_buf[w->num_sample_buf], samples + off,
               (size_t)chunk * sizeof(*samples));
        w->num_sample_buf += chunk;
        w->sample_buf_weight_ns += (uint64_t)chunk * sample_period_ns;
        off += chunk;
        if (w->num_sample_buf >= PGWT_BLOCK_EVENTS) {
            if (flush_samples(w) != 0)
                return -1;
        }
    }

    /* Time-based cut: the pending batch spans ~1 s of ticks. */
    if (w->num_sample_buf > 0 &&
        w->sample_buf[w->num_sample_buf - 1].timestamp_ns -
        w->sample_buf[0].timestamp_ns >= PGWT_SAMPLE_BATCH_NS)
        return flush_samples(w);

    return 0;
}

int pgwt_writer_check_rotation(struct pgwt_event_writer *w)
{
    if (!w->enabled || !w->fp) return 0;

    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    struct tm tm;
    time_t now = wall.tv_sec;
    localtime_r(&now, &tm);
    int current_hour = tm.tm_yday * 24 + tm.tm_hour;

    if (current_hour == w->current_hour && !w->force_rotate)
        return 0;

    /* Hour changed (or the block cap forced it) — rotate */
    flush_block(w);
    flush_samples(w);
    write_footer(w);

    if (w->verbose) {
        double ratio = w->total_events_written > 0
            ? (double)(w->total_events_written * 36) / w->total_bytes_written
            : 0;
        fprintf(stderr, "INFO: trace file closed: %lu events, %lu bytes "
                "(%.1fx compression)\n",
                (unsigned long)w->total_events_written,
                (unsigned long)w->total_bytes_written, ratio);
    }

    /* DUR-5: an archive is durable once rotated — flush it to stable
     * storage before it gets its final name. */
    fflush(w->fp);
    fsync(fileno(w->fp));
    fclose(w->fp);
    w->fp = NULL;

    /* Rename current.trace → YYYY-MM-DD_HH[.N].trace.lz4. DUR-2: the target
     * is derived from the file's START hour; a restart mid-hour (the
     * recovered archive already claimed the name) or a DST fold (localtime_r
     * repeats an hour) must never clobber an existing archive — the helper
     * picks a numeric-suffix name instead. Naming stays in LOCAL time
     * (operators read these against server logs); collision safety, not
     * unique naming, is what prevents data loss. */
    time_t file_start_sec = (time_t)(w->file_start_wall_ns / 1000000000ULL);
    struct tm file_tm;
    localtime_r(&file_start_sec, &file_tm);
    char new_path[512];
    if (pgwt_archive_path_nonclobber(w->trace_dir, &file_tm, ".trace.lz4",
                                     new_path, sizeof(new_path)) == 0)
        rename(w->current_path, new_path);
    else
        fprintf(stderr, "WARN: no free archive name for %s — leaving it as "
                "current.trace (it will be recovered on next start)\n",
                w->current_path);

    /* Remove meta file — the rotated .trace.lz4 has a real footer */
    {
        char meta_path[512];
        snprintf(meta_path, sizeof(meta_path), "%s/current.trace.meta",
                 w->trace_dir);
        unlink(meta_path);
    }
    pgwt_fsync_dir(w->trace_dir);

    /* Reset stats for next file */
    w->total_events_written = 0;
    w->total_bytes_written = 0;
    w->force_rotate = false;

    return 0;
}

int pgwt_writer_close(struct pgwt_event_writer *w)
{
    if (!w->fp) return 0;

    flush_block(w);
    flush_samples(w);
    write_footer(w);

    if (w->verbose) {
        double ratio = w->total_events_written > 0
            ? (double)(w->total_events_written * 36) / w->total_bytes_written
            : 0;
        fprintf(stderr, "INFO: trace file closed: %lu events, %lu bytes "
                "(%.1fx compression)\n",
                (unsigned long)w->total_events_written,
                (unsigned long)w->total_bytes_written, ratio);
    }

    /* DUR-5: a cleanly closed file is durable (it is archived on the next
     * daemon start by recovery, footer intact). */
    fflush(w->fp);
    fsync(fileno(w->fp));
    fclose(w->fp);
    w->fp = NULL;
    return 0;
}

/* Classify a trace-dir entry for retention purposes. */
enum retn_kind {
    RETN_SKIP = 0,        /* live files, sidecars, unknown names */
    RETN_TRACE_ARCHIVE,   /* *.trace.lz4 (incl. numeric-suffix names) */
    RETN_SUMMARY_ARCHIVE, /* *.summary.lz4 */
    RETN_CORRUPT,         /* preserved *.corrupt.<ts> files */
    RETN_ORPHAN_META,     /* current.*.meta[.tmp] with no live writer file */
};

static int has_suffix(const char *name, const char *suffix)
{
    size_t nl = strlen(name), sl = strlen(suffix);
    return nl >= sl && strcmp(name + nl - sl, suffix) == 0;
}

static enum retn_kind retn_classify(const char *dir, const char *name)
{
    if (strcmp(name, "current.trace") == 0 ||
        strcmp(name, "current.summary") == 0)
        return RETN_SKIP;
    if (has_suffix(name, ".trace.lz4"))
        return RETN_TRACE_ARCHIVE;
    if (has_suffix(name, ".summary.lz4"))
        return RETN_SUMMARY_ARCHIVE;
    if (strstr(name, ".corrupt."))
        return RETN_CORRUPT;
    if (has_suffix(name, ".meta.tmp") || has_suffix(name, ".jsonl.tmp"))
        return RETN_ORPHAN_META;   /* torn meta/compaction write — stale */
    if (strcmp(name, "current.trace.meta") == 0 ||
        strcmp(name, "current.summary.meta") == 0) {
        /* Orphan only if the file it describes is gone. */
        char live[600];
        snprintf(live, sizeof(live), "%s/%.*s", dir,
                 (int)(strlen(name) - 5), name);
        return access(live, F_OK) == 0 ? RETN_SKIP : RETN_ORPHAN_META;
    }
    return RETN_SKIP;
}

/* Archive age: from the YYYY-MM-DD_HH name when parseable (recovered
 * archives are CREATED late but hold old data), else the file mtime. */
static time_t retn_file_time(const char *name, time_t mtime)
{
    struct tm ftm = {0};
    int hour;
    if (sscanf(name, "%4d-%2d-%2d_%2d",
               &ftm.tm_year, &ftm.tm_mon, &ftm.tm_mday, &hour) == 4) {
        ftm.tm_year -= 1900;
        ftm.tm_mon -= 1;
        ftm.tm_hour = hour;
        ftm.tm_isdst = -1;
        time_t t = mktime(&ftm);
        if (t > 0)
            return t;
    }
    return mtime;
}

struct retn_entry {
    char     name[300];
    time_t   ftime;
    uint64_t size;
};

static int retn_cmp_oldest(const void *a, const void *b)
{
    time_t ta = ((const struct retn_entry *)a)->ftime;
    time_t tb = ((const struct retn_entry *)b)->ftime;
    return (ta > tb) - (ta < tb);
}

int pgwt_writer_cleanup_old_files(struct pgwt_event_writer *w)
{
    DIR *dir = opendir(w->trace_dir);
    if (!dir) return -1;

    time_t now = time(NULL);
    time_t cutoff = w->retention_hours > 0
                  ? now - (time_t)w->retention_hours * 3600 : 0;

    struct retn_entry *kept = NULL;
    int n_kept = 0, cap_kept = 0;
    uint64_t total_bytes = 0;   /* everything in the dir, live files included */

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", w->trace_dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        enum retn_kind kind = retn_classify(w->trace_dir, ent->d_name);
        if (kind == RETN_ORPHAN_META) {
            /* DUR-3: leftovers of a dead writer — but only when provably
             * stale (an hour old) so we never race a live daemon's
             * meta-tmp-rename cycle. */
            if (now - st.st_mtime > 3600 && unlink(path) == 0 && w->verbose)
                fprintf(stderr, "INFO: deleted orphaned %s\n", ent->d_name);
            continue;
        }
        if (kind == RETN_SKIP) {
            total_bytes += (uint64_t)st.st_size;
            continue;
        }

        /* Hours-based retention (archives + preserved corrupt files). */
        time_t ftime = retn_file_time(ent->d_name, st.st_mtime);
        if (cutoff && ftime < cutoff) {
            if (unlink(path) == 0) {
                if (w->verbose)
                    fprintf(stderr, "INFO: deleted old file %s\n",
                            ent->d_name);
                continue;
            }
        }

        total_bytes += (uint64_t)st.st_size;
        if (w->retention_bytes > 0) {
            if (n_kept >= cap_kept) {
                int newcap = cap_kept ? cap_kept * 2 : 64;
                struct retn_entry *tmp =
                    realloc(kept, (size_t)newcap * sizeof(*tmp));
                if (!tmp)
                    continue;
                kept = tmp;
                cap_kept = newcap;
            }
            snprintf(kept[n_kept].name, sizeof(kept[n_kept].name), "%s",
                     ent->d_name);
            kept[n_kept].ftime = ftime;
            kept[n_kept].size = (uint64_t)st.st_size;
            n_kept++;
        }
    }
    closedir(dir);

    /* DUR-3: size cap. Total dir usage (live files included) must fit under
     * retention_bytes; delete the OLDEST archives first, never the live
     * current files or sidecars. */
    if (w->retention_bytes > 0 && total_bytes > w->retention_bytes) {
        if (n_kept > 0)
            qsort(kept, (size_t)n_kept, sizeof(*kept), retn_cmp_oldest);
        for (int i = 0; i < n_kept && total_bytes > w->retention_bytes; i++) {
            char path[600];
            snprintf(path, sizeof(path), "%s/%s", w->trace_dir, kept[i].name);
            if (unlink(path) == 0) {
                total_bytes -= kept[i].size;
                if (w->verbose)
                    fprintf(stderr, "INFO: size cap: deleted %s (%llu bytes)\n",
                            kept[i].name,
                            (unsigned long long)kept[i].size);
            }
        }
        if (total_bytes > w->retention_bytes)
            fprintf(stderr, "WARN: trace dir still %llu bytes over the "
                    "--retention-gb cap after deleting every archive — the "
                    "live current files alone exceed the cap\n",
                    (unsigned long long)(total_bytes - w->retention_bytes));
    }
    free(kept);
    return 0;
}

void pgwt_writer_destroy(struct pgwt_event_writer *w)
{
    free(w->encode_buf);
    w->encode_buf = NULL;
    free(w->compress_buf);
    w->compress_buf = NULL;
    free(w->block_index);
    w->block_index = NULL;
}
