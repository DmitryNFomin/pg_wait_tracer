/* event_writer.c — Raw event file writer: columnar encoding + LZ4
 *
 * Buffers events in blocks of 4096, encodes column-wise, LZ4-compresses,
 * writes to hourly-rotating trace files with a block index footer. */
#include "event_writer.h"
#include "crash_point.h"

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
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

int pgwt_fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    close(fd);
    return rc == 0 ? 0 : -1;
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

/* ── Closure status (#424 deliverable 1) ──────────────────── */

/* Rewrite the header's `flags` field in place and make the new value durable.
 *
 * Called only AFTER the file's data is already on stable storage, so the
 * ordering is: data fsync → flag write → flag fsync. A reader can therefore
 * never see a clean/rotated bit for data that is not there; the reverse
 * (data durable, bit missing) is the safe direction and reads as "not
 * finalized".
 *
 * pwrite, not fseek+fwrite: it leaves the stream's append position alone, so
 * a caller that keeps writing (nobody does today, but a future one might)
 * cannot be silently corrupted by this. Returns 0 only if both the write and
 * the fsync succeeded. */
static int write_header_flags(FILE *fp, uint16_t flags)
{
    if (fflush(fp) != 0)
        return -1;
    int fd = fileno(fp);
    off_t off = (off_t)offsetof(struct pgwt_trace_file_header, flags);
    if (pwrite(fd, &flags, sizeof(flags), off) != (ssize_t)sizeof(flags))
        return -1;
    if (fsync(fd) != 0)
        return -1;
    return 0;
}

/* ── Open-interval checkpoints (#424 deliverable 2) ───────── */

static void ckpt_paths(const char *dir, char *live, size_t live_sz,
                       char *tmp, size_t tmp_sz)
{
    if (live) snprintf(live, live_sz, "%s/%s", dir, PGWT_OPEN_CKPT_NAME);
    if (tmp)  snprintf(tmp,  tmp_sz,  "%s/%s", dir, PGWT_OPEN_CKPT_TMP);
}

/* Drop the live sidecar (and any torn tmp). Called whenever the checkpoint it
 * holds has become redundant or unattributable:
 *
 *   - a CLEAN close already wrote the real closing records for every open
 *     interval, so keeping the checkpoint would double count observed time;
 *   - a ROTATION continues the capture in the next file, where those same
 *     intervals get real closing records — same double count;
 *   - a new file has a new identity, so the old sidecar can describe nothing.
 *
 * This is the other half of suppression, applied at the only moment when it
 * can be decided locally and with certainty. */
static void ckpt_discard(const char *dir, const char *why, bool verbose)
{
    char live[700], tmp[700];
    ckpt_paths(dir, live, sizeof(live), tmp, sizeof(tmp));
    unlink(tmp);
    if (unlink(live) == 0 && verbose)
        fprintf(stderr, "INFO: dropped the open-interval checkpoint (%s)\n",
                why);
}

enum pgwt_open_ckpt_rc
pgwt_open_ckpt_load(const char *path,
                    const struct pgwt_open_ckpt_identity *expect,
                    struct pgwt_open_ckpt_view *out, void **buf_out)
{
    if (buf_out)
        *buf_out = NULL;
    if (out) {
        memset(&out->header, 0, sizeof(out->header));
        out->records = NULL;
        out->count = 0;
    }
    if (!path)
        return PGWT_OPEN_CKPT_ABSENT;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return PGWT_OPEN_CKPT_ABSENT;   /* no sidecar is not a bad sidecar */

    /* Bound the read by the format's own cap before allocating anything: a
     * sidecar claiming a huge size must not be able to spend our memory. */
    struct stat st;
    if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(fp);
        return PGWT_OPEN_CKPT_ABSENT;   /* a directory is not a checkpoint */
    }
    size_t max = sizeof(struct pgwt_open_ckpt_header) +
        (size_t)PGWT_OPEN_CKPT_MAX_RECORDS *
        sizeof(struct pgwt_open_ckpt_record);
    if ((size_t)st.st_size > max || (size_t)st.st_size <
        sizeof(struct pgwt_open_ckpt_header)) {
        fclose(fp);
        return (size_t)st.st_size > max ? PGWT_OPEN_CKPT_COUNT_INSANE
                                        : PGWT_OPEN_CKPT_TOO_SHORT;
    }

    size_t len = (size_t)st.st_size;
    void *buf = malloc(len);
    if (!buf) {
        fclose(fp);
        return PGWT_OPEN_CKPT_IO;
    }
    size_t got = fread(buf, 1, len, fp);
    fclose(fp);

    struct pgwt_open_ckpt_view view;
    enum pgwt_open_ckpt_rc rc = pgwt_open_ckpt_parse(buf, got, expect, &view);
    if (rc != PGWT_OPEN_CKPT_OK) {
        free(buf);
        return rc;
    }
    if (out)
        *out = view;
    if (buf_out)
        *buf_out = buf;
    else
        free(buf);          /* caller only wanted the verdict */
    return PGWT_OPEN_CKPT_OK;
}

/* Hand the live checkpoint over to the archive the trace is ABOUT to become
 * (#424 review finding: both handovers must be restartable).
 *
 * The rule the review asked for: the sidecar's fate is decided by the
 * TRACE's fate, not alongside it. So the sidecar is renamed FIRST and the
 * caller renames the trace afterwards. Every state a crash can leave is then
 * resolvable without discarding evidence:
 *
 *   (current.trace, current.trace.open)   nothing done yet — the next start
 *                                         recovers both, arriving here again
 *   (current.trace, <dest>.open)          crashed inside the window: the next
 *                                         start recovers the SAME file, picks
 *                                         the SAME dest (nothing claimed the
 *                                         name), and ADOPTS the stranded
 *                                         sidecar after re-checking identity
 *   (<dest>, <dest>.open)                 done
 *
 * The one state that must never exist is (<dest>, current.trace.open): a
 * sidecar whose trace has already been archived, which the next start sees as
 * a sidecar with no current.trace and deletes. That was the bug — renaming
 * the trace first made it the normal intermediate state.
 *
 * Adoption is NOT "whatever is lying there": the stranded sidecar is parsed
 * and checked against the same identity as a live one. A foreign file at that
 * name is deleted, because a wrong sidecar beside a right archive is worse
 * than none.
 *
 * Returns 1 if a validated checkpoint now sits at <dest>.open, 0 if there is
 * none (absent, or refused and removed). */
static int ckpt_handover(const char *dir, const char *dest,
                         const struct pgwt_open_ckpt_identity *id,
                         bool verbose)
{
    char live[700], tmp[700], sidecar[800];
    ckpt_paths(dir, live, sizeof(live), tmp, sizeof(tmp));
    snprintf(sidecar, sizeof(sidecar), "%s%s", dest, PGWT_OPEN_CKPT_SUFFIX);
    unlink(tmp);          /* a torn checkpoint write is always stale */

    struct pgwt_open_ckpt_view view;
    enum pgwt_open_ckpt_rc rc = pgwt_open_ckpt_load(live, id, &view, NULL);

    if (rc == PGWT_OPEN_CKPT_OK) {
        if (rename(live, sidecar) != 0) {
            fprintf(stderr, "WARN: cannot keep the open-interval checkpoint "
                    "as %s: %s — dropping it\n", sidecar, strerror(errno));
            unlink(live);
            return 0;
        }
        if (pgwt_fsync_dir(dir) != 0)
            fprintf(stderr, "WARN: could not fsync %s after installing %s — "
                    "the rename is visible now but may not survive a power "
                    "cut\n", dir, sidecar);
        if (verbose)
            fprintf(stderr, "INFO: carried open-interval checkpoint %llu "
                    "(%d open intervals) -> %s\n",
                    (unsigned long long)view.header.seq, view.count, sidecar);
        return 1;
    }

    if (rc != PGWT_OPEN_CKPT_ABSENT) {
        /* Something IS there and cannot be trusted. "absent" and "damaged"
         * are different facts; name which, and remove it so the next start
         * does not retry the same unusable file. */
        fprintf(stderr, "WARN: refused the open-interval checkpoint for %s "
                "(%s) — dropping it\n", dest, pgwt_open_ckpt_rc_name(rc));
        unlink(live);
        return 0;
    }

    /* No live sidecar. One may already sit at the destination from a handover
     * that was interrupted after this rename and before the trace's — the
     * state the ordering above is designed to make recoverable. */
    if (access(sidecar, F_OK) != 0)
        return 0;
    rc = pgwt_open_ckpt_load(sidecar, id, &view, NULL);
    if (rc == PGWT_OPEN_CKPT_OK) {
        if (verbose)
            fprintf(stderr, "INFO: adopted the open-interval checkpoint "
                    "already at %s (seq %llu, %d open intervals) — a previous "
                    "handover was interrupted\n", sidecar,
                    (unsigned long long)view.header.seq, view.count);
        return 1;
    }
    fprintf(stderr, "WARN: %s does not describe this trace (%s) — removing "
            "it rather than leaving it beside an archive it is not about\n",
            sidecar, pgwt_open_ckpt_rc_name(rc));
    unlink(sidecar);
    return 0;
}

int pgwt_open_ckpt_write(const char *dir,
                         const struct pgwt_open_ckpt_identity *id,
                         uint64_t seq, uint64_t now_mono_ns,
                         uint64_t committed_through_ns,
                         const struct pgwt_open_ckpt_record *recs, int count,
                         gid_t trace_gid)
{
    if (!dir || !id || count < 0 || (count > 0 && !recs))
        return -1;
    if (count > PGWT_OPEN_CKPT_MAX_RECORDS) {
        fprintf(stderr, "WARN: refusing an open-interval checkpoint of %d "
                "records (cap %d)\n", count, PGWT_OPEN_CKPT_MAX_RECORDS);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        /* An interval cannot have started before the clock did. A record
         * failing this would decode to a start in the far future via
         * unsigned wraparound, and be counted. */
        if (recs[i].duration_ns > recs[i].timestamp_ns) {
            fprintf(stderr, "WARN: refusing an open-interval checkpoint: "
                    "record %d has duration > timestamp\n", i);
            return -1;
        }
    }

    char live[700], tmp[700];
    ckpt_paths(dir, live, sizeof(live), tmp, sizeof(tmp));

    struct pgwt_open_ckpt_header h = {
        .magic = PGWT_OPEN_CKPT_MAGIC,
        .version = PGWT_OPEN_CKPT_VERSION,
        .flags = 0,
        .trace_start_wall_ns = id->trace_start_wall_ns,
        .trace_start_mono_ns = id->trace_start_mono_ns,
        .seq = seq,
        .checkpoint_mono_ns = now_mono_ns,
        .committed_through_ns = committed_through_ns,
        .num_records = (uint32_t)count,
        .checksum = pgwt_open_ckpt_crc32(recs, (size_t)count * sizeof(*recs)),
    };

    FILE *f = fopen(tmp, "wbe");
    if (!f) {
        fprintf(stderr, "WARN: cannot write %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    if (trace_gid != (gid_t)-1) {
        if (fchown(fileno(f), (uid_t)-1, trace_gid) != 0 ||
            fchmod(fileno(f), 0640) != 0)
            fprintf(stderr, "WARN: cannot set ownership on %s: %s\n",
                    tmp, strerror(errno));
    }
    int ok = fwrite(&h, sizeof(h), 1, f) == 1;
    if (ok && count > 0)
        ok = fwrite(recs, sizeof(*recs), (size_t)count, f) == (size_t)count;
    if (ok && fflush(f) != 0)
        ok = 0;
    if (ok && fsync(fileno(f)) != 0)
        ok = 0;
    fclose(f);
    if (!ok) {
        unlink(tmp);
        fprintf(stderr, "WARN: open-interval checkpoint %llu not written "
                "durably (%s) — the previous one stands\n",
                (unsigned long long)seq, strerror(errno));
        return -1;
    }
    /* Atomic swap: a crash leaves either the previous checkpoint or this
     * one, never a half-written mixture of the two. */
    if (rename(tmp, live) != 0) {
        unlink(tmp);
        fprintf(stderr, "WARN: cannot install %s: %s\n", live,
                strerror(errno));
        return -1;
    }
    if (pgwt_fsync_dir(dir) != 0)
        fprintf(stderr, "WARN: could not fsync %s after installing the "
                "open-interval checkpoint — it is in place for this kernel "
                "but the name may not survive a power cut\n", dir);
    return 0;
}

int pgwt_writer_checkpoint_open(struct pgwt_event_writer *w,
                                uint64_t now_mono_ns,
                                const struct pgwt_open_ckpt_record *recs,
                                int count)
{
    if (!w || count < 0 || (count > 0 && !recs))
        return -1;
    /* No file, no identity. A checkpoint that cannot name the trace it
     * describes is exactly the stale-sidecar hazard, so it is never written. */
    if (!w->enabled || !w->fp)
        return -1;
    if (count > PGWT_OPEN_CKPT_MAX_RECORDS) {
        fprintf(stderr, "WARN: refusing an open-interval checkpoint of %d "
                "records (cap %d)\n", count, PGWT_OPEN_CKPT_MAX_RECORDS);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        /* An interval cannot have started before the clock did. A record that
         * fails this would decode to a start in the far future via unsigned
         * wraparound, and be counted. */
        if (recs[i].duration_ns > recs[i].timestamp_ns) {
            fprintf(stderr, "WARN: refusing an open-interval checkpoint: "
                    "record %d has duration > timestamp\n", i);
            return -1;
        }
    }

    /* DURABLE PUBLICATION — and deliberately NOT a flush.
     *
     * This used to call pgwt_writer_publish() first, so the watermark would
     * be as fresh as possible. A reviewer costed that: publish runs
     * flush_block, so a checkpoint on the daemon's one-second tick would cut
     * a block boundary every second, changing block layout, footer index
     * size and the block-aggregate's cost. That is a real price for an
     * unmeasured benefit, and a library must not impose it on its caller by
     * accident.
     *
     * The truthfulness the contract needs does not require the flush: the
     * value recorded below is the watermark the writer has ALREADY reached,
     * so it is by construction a point the trace really contains, never a
     * promise. Records still pending in the writer's buffers are simply not
     * covered by it — which is exactly right, since a crash loses them and
     * the checkpoint is what speaks for the intervals they would have
     * closed.
     *
     * A caller that wants the closed-record prefix on disk calls
     * pgwt_writer_publish() itself, at whatever cadence it has measured. */

    return pgwt_open_ckpt_write(w->trace_dir,
                                &(struct pgwt_open_ckpt_identity){
                                    .trace_start_wall_ns = w->file_start_wall_ns,
                                    .trace_start_mono_ns = w->file_start_mono_ns,
                                },
                                ++w->ckpt_seq, now_mono_ns,
                                w->committed_through_ns,
                                recs, count, w->trace_gid);
}

int pgwt_trace_file_closure(const char *path, enum pgwt_closure_status *out)
{
    if (out)
        *out = PGWT_CLOSURE_UNKNOWN;   /* a file we cannot read is never clean */
    if (!path)
        return -1;
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;
    struct pgwt_trace_file_header hdr;
    int ok = (fread(&hdr, sizeof(hdr), 1, fp) == 1) &&
             hdr.magic == PGWT_TRACE_MAGIC;
    fclose(fp);
    if (!ok)
        return -1;
    if (out)
        *out = pgwt_closure_from_flags(hdr.flags);
    return 0;
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
        /* A checkpoint whose trace file is gone can never be attributed to
         * anything — keeping it only risks a later misattribution. */
        ckpt_discard(w->trace_dir, "no current.trace to attribute it to",
                     w->verbose);
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
        /* Unreadable header = no identity to check a checkpoint against. */
        ckpt_discard(w->trace_dir, "its trace file has an unreadable header",
                     w->verbose);
        if (pgwt_fsync_dir(w->trace_dir) != 0)
            fprintf(stderr, "WARN: could not fsync %s\n", w->trace_dir);
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
        /* No complete block means no archive a reader would ever open, so
         * there is nothing for a checkpoint to be attached to. */
        ckpt_discard(w->trace_dir, "its trace file has no complete block",
                     w->verbose);
        if (pgwt_fsync_dir(w->trace_dir) != 0)
            fprintf(stderr, "WARN: could not fsync %s\n", w->trace_dir);
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
    /* The recovered file's data must be on stable storage BEFORE the header
     * says anything about how it ended. Both calls are checked: a failure
     * here means the footer we just wrote may not be there, so the file must
     * not then be labelled (#424 review: no unchecked flush/fsync). */
    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        finalize_failed = 1;
        fprintf(stderr, "WARN: could not flush recovered %s to stable "
                "storage (%s) — it will read as an unfinished capture\n",
                cur, strerror(errno));
    }

    /* #424: record how this file ENDED, now that its data is durable.
     *
     *   - legacy file (no CLOSURE_TRACKED): left exactly as it is. A
     *     pre-#424 daemon left no trace of whether it shut down or died, so
     *     the file reads as UNKNOWN. Stamping RECOVERED on it would be
     *     inventing evidence in the other direction — just as wrong as
     *     calling it clean.
     *   - already CLEAN or ROTATED: preserved. Finding such a file here is
     *     normal: a clean close leaves current.trace in place with its
     *     footer, and it is THIS path that archives it on the next start.
     *     Recovery must not relabel that as a crash.
     *   - tracked but never finalized: the previous daemon died without
     *     closing. RECOVERED — intervals open at the end are missing.
     *   - tracked, says CLEAN, but the scan found FEWER blocks than the meta
     *     high-watermark says were committed: the close bit contradicts the
     *     file. RECOVERED wins (trace_closure.h rule 2a) — a "clean" label
     *     over missing committed data is exactly the false clean this whole
     *     deliverable exists to prevent. */
    int tracked = (hdr.flags & PGWT_FLAG_CLOSURE_TRACKED) != 0;
    int finalized = (hdr.flags & (PGWT_FLAG_CLOSED_CLEAN |
                                  PGWT_FLAG_ROTATED)) != 0;
    int lost_committed = (committed > nidx);
    if (!finalize_failed && tracked && (!finalized || lost_committed) &&
        !(hdr.flags & PGWT_FLAG_RECOVERED)) {
        uint16_t newf = (uint16_t)(hdr.flags | PGWT_FLAG_RECOVERED);
        if (write_header_flags(fp, newf) != 0)
            fprintf(stderr, "WARN: could not mark recovered %s as uncleanly "
                    "ended (%s) — it will read as an unfinished capture\n",
                    cur, strerror(errno));
    }

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
                                     dest, sizeof(dest)) != 0) {
        fprintf(stderr, "WARN: no free archive name for recovered %s "
                "(leaving it in place; it will NOT be truncated)\n", cur);
        return;
    }

    /* #424 deliverable 2, RECOVERY — the checkpoint moves FIRST.
     *
     * The crashed daemon may have left an open-interval checkpoint. It is
     * validated against the identity of the file we are about to archive and
     * carried next to it; see ckpt_handover for why the order matters (a
     * crash between the two renames must not strand evidence that the next
     * start then deletes). It is deliberately NOT merged into the trace
     * stream: the block layout has no field that could mark a record as
     * reconstructed, and a lower bound that looks like an observation is
     * worse than no record at all. */
    struct pgwt_open_ckpt_identity id = {
        .trace_start_wall_ns = hdr.start_time_ns,
        .trace_start_mono_ns = hdr.clock_offset_ns,
    };
    ckpt_handover(w->trace_dir, dest, &id, true);
    pgwt_crash_point(PGWT_CRASH_RECOVER_AFTER_CKPT);

    if (rename(cur, dest) != 0) {
        fprintf(stderr, "WARN: cannot archive recovered %s: %s "
                "(leaving it in place; it will NOT be truncated — the next "
                "start will adopt the checkpoint already at %s%s)\n",
                cur, strerror(errno), dest, PGWT_OPEN_CKPT_SUFFIX);
        return;
    }
    pgwt_crash_point(PGWT_CRASH_RECOVER_AFTER_TRACE);
    unlink(meta);

    if (pgwt_fsync_dir(w->trace_dir) != 0)
        fprintf(stderr, "WARN: could not fsync %s after archiving %s — the "
                "archive is visible now but the name may not survive a power "
                "cut\n", w->trace_dir, dest);
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
    w->file_flags = (uint16_t)(PGWT_FLAG_LZ4 | PGWT_FLAG_CLOSURE_TRACKED);
    struct pgwt_trace_file_header hdr = {
        .magic = PGWT_TRACE_MAGIC,
        .version = PGWT_TRACE_VERSION,
        /* CLOSURE_TRACKED from byte one: it is the capability bit that makes
         * the absence of a terminal bit MEAN something later (#424). A file
         * without it is a legacy file and reads as "unknown", never clean. */
        .flags = w->file_flags,
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

    /* #424: a new file has a new identity, so any checkpoint still sitting
     * under the live name describes a file that no longer exists here.
     * Recovery has already carried the valid ones next to their archives, so
     * whatever is left is unattributable. Sequence numbers restart with the
     * file for the same reason. */
    w->ckpt_seq = 0;
    ckpt_discard(w->trace_dir, "a new trace file started", w->verbose);

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

    /* #424: the publication high-watermark. Every record in this block is now
     * in the file (fflush + the meta count below), so the LAST TRANSITIONS
     * timestamp committed is a floor the live snapshot can report as
     * committed_through_ns — the point below which a gap means "still open",
     * not "closed but not yet visible". TRANSITIONS only: a SAMPLES block
     * running ahead says nothing about whether the closed-record prefix is
     * published, and taking the max over both would OVERSTATE it. */
    if (block_type == PGWT_BLOCK_TRANSITIONS) {
        uint64_t last = events[count - 1].timestamp_ns;
        if (last > w->committed_through_ns)
            w->committed_through_ns = last;
    }

    /* Flush to OS page cache so concurrent readers can see this block. A
     * failure here means the block is NOT visible, so the committed-block
     * watermark below must not advertise it: say so and stop advertising
     * (#424 review: no unchecked flush). */
    if (fflush(w->fp) != 0) {
        fprintf(stderr, "WARN: trace flush failed (%s) — block %d is not "
                "visible to readers and is not being advertised\n",
                strerror(errno), w->num_blocks);
        return -1;
    }

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

/* Finalize the current file and record WHY it ended (#424 deliverable 1).
 *
 * `terminal_flag` is PGWT_FLAG_CLOSED_CLEAN (shutdown) or PGWT_FLAG_ROTATED
 * (hour / block-cap rotation). The bit is written only when the whole chain
 * succeeded: both pending buffers flushed, the footer written, the data
 * fsynced. Every one of those is CHECKED — before #424 all of those return
 * values were discarded, which is exactly how a file with a truncated tail
 * ended up indistinguishable from a complete one.
 *
 * Returns 0 if the file now carries that bit, -1 if anything in the chain
 * failed. On -1 the file keeps NO terminal bit and reads back as
 * IN_PROGRESS, which for an archived file means "open intervals may be
 * missing" — the safe direction. The caller still rotates/closes: refusing
 * to would only stall the capture and lose more. */
static int finalize_current_file(struct pgwt_event_writer *w,
                                 uint16_t terminal_flag)
{
    if (!w->fp)
        return -1;

    int ok = 1;
    if (flush_block(w) != 0)
        ok = 0;
    if (flush_samples(w) != 0)
        ok = 0;
    if (write_footer(w) != 0)
        ok = 0;
    if (fflush(w->fp) != 0)
        ok = 0;
    if (fsync(fileno(w->fp)) != 0)
        ok = 0;

    if (!ok) {
        fprintf(stderr, "WARN: could not finalize %s durably — it will read "
                "as an unfinished capture (open intervals may be missing)\n",
                w->current_path);
        return -1;
    }

    /* OR into what this file's header actually holds, so the terminal bit is
     * added rather than the field rebuilt — a creation-time bit added later
     * must not be erased here. */
    w->file_flags |= terminal_flag;
    if (write_header_flags(w->fp, w->file_flags) != 0) {
        fprintf(stderr, "WARN: could not record the closure status of %s "
                "(%s) — it will read as an unfinished capture\n",
                w->current_path, strerror(errno));
        return -1;
    }
    return 0;
}

/* ── Public API ───────────────────────────────────────────── */

/* Give one already-created file in the trace directory the same owner group
 * and mode as every trace file (0640, group-readable). No-op when no trace
 * group resolved. Returns 0 when nothing needed doing or it succeeded. */
static int apply_trace_file_perms(const char *path, gid_t gid)
{
    if (gid == (gid_t)-1) return 0;
    if (chown(path, (uid_t)-1, gid) != 0 || chmod(path, 0640) != 0) {
        if (errno == ENOENT) return 0;   /* nothing there to fix */
        fprintf(stderr, "WARN: cannot give %s to gid %u mode 0640 (%s) — "
                "a non-root reader may not be able to open it\n",
                path, (unsigned)gid, strerror(errno));
        return -1;
    }
    return 0;
}

/* See event_writer.h for why this is separate from pgwt_writer_init (#499). */
int pgwt_trace_dir_prepare(const char *trace_dir, const char *group_name,
                           gid_t *out_gid)
{
    gid_t gid = (gid_t)-1;

    /* Resolve trace group name → GID. An explicitly named group that does
     * not exist is never substituted (#500): handing the trace to some other
     * group than the operator asked for is worse than refusing. */
    if (group_name) {
        struct group *gr = getgrnam(group_name);
        if (gr) {
            gid = gr->gr_gid;
        } else {
            fprintf(stderr, "WARN: trace group '%s' does not exist on this "
                    "host — trace files stay root-owned, so a non-root reader "
                    "(pgwt-server replay, demo service) cannot open them; "
                    "create the group or pass --trace-group <existing group>\n",
                    group_name);
        }
    }

    if (out_gid) *out_gid = gid;

    /* Create trace directory with group-readable permissions */
    if (mkdir(trace_dir, 0750) != 0 && errno != EEXIST) {
        fprintf(stderr, "ERROR: cannot create trace directory %s: %s\n",
                trace_dir, strerror(errno));
        return -1;
    }

    /* EEXIST is not enough: the path could be a regular file, in which case
     * nothing below can be written and the caller must hear about it now. */
    struct stat st;
    if (stat(trace_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "ERROR: trace path %s is not a directory\n", trace_dir);
        return -1;
    }

    if (gid != (gid_t)-1) {
        if (chown(trace_dir, (uid_t)-1, gid) != 0 || chmod(trace_dir, 0750) != 0)
            fprintf(stderr, "WARN: cannot give trace directory %s to gid %u "
                    "(%s) — a non-root reader may not be able to enter it\n",
                    trace_dir, (unsigned)gid, strerror(errno));
    }
    return 0;
}

const char *pgwt_default_trace_group(void)
{
    static char chosen[64];

    /* "dba" first: it is what every released version documented and defaulted
     * to, so a site that created it keeps exactly the behaviour it has. */
    static const char *const literal_candidates[] = { "dba", "postgres", NULL };

    if (getgrnam(literal_candidates[0])) {
        snprintf(chosen, sizeof(chosen), "%s", literal_candidates[0]);
        return chosen;
    }

    /* Then the group PostgreSQL itself runs under — the account that a
     * non-root pgwt-server replay or demo service reads the trace as. Resolved
     * through the postgres user's primary gid rather than assuming the group
     * is also called "postgres". */
    struct passwd *pw = getpwnam("postgres");
    if (pw) {
        struct group *gr = getgrgid(pw->pw_gid);
        if (gr) {
            snprintf(chosen, sizeof(chosen), "%s", gr->gr_name);
            return chosen;
        }
    }

    for (int i = 1; literal_candidates[i]; i++) {
        if (getgrnam(literal_candidates[i])) {
            snprintf(chosen, sizeof(chosen), "%s", literal_candidates[i]);
            return chosen;
        }
    }
    return NULL;
}

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

    if (pgwt_trace_dir_prepare(w->trace_dir, group_name, &w->trace_gid) != 0)
        return -1;

    /* The name sidecar is written by pgwt_discover(), BEFORE this runs (#499),
     * with fopen()'s umask-derived mode and no group. Give it the trace-file
     * treatment so the reader that can open the trace can also read its name
     * provenance (#500). */
    char names_path[sizeof(w->trace_dir) + 32];
    snprintf(names_path, sizeof(names_path), "%s/wait_event_names.json",
             w->trace_dir);
    apply_trace_file_perms(names_path, w->trace_gid);

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

/* #424: flush both pending buffers, in the order that keeps the block index
 * sorted (flush_block writes an older sample batch first). After this returns
 * 0 every record the writer has been handed is in the file and counted in the
 * meta high-watermark, so a reader scanning the trace can see it. */
int pgwt_writer_publish(struct pgwt_event_writer *w)
{
    if (!w->enabled || !w->fp)
        return 0;                 /* nothing opened yet: nothing unpublished */
    int rc = 0;
    if (flush_block(w) != 0)
        rc = -1;
    if (flush_sample_buf(w) != 0)
        rc = -1;
    return rc;
}

uint64_t pgwt_writer_committed_through_ns(const struct pgwt_event_writer *w)
{
    return w->committed_through_ns;
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

    /* Hour changed (or the block cap forced it) — rotate. #424: ROTATED, not
     * CLOSED_CLEAN. The capture did NOT end here, so an interval open across
     * the boundary is not lost — it closes in the next file, and a reader
     * must not be warned about it. That distinction is the whole reason the
     * footer alone could not certify anything. */
    finalize_current_file(w, PGWT_FLAG_ROTATED);

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
     * storage before it gets its final name. finalize_current_file already
     * did this as a CHECKED chain; repeat it only to catch anything written
     * since, and report a failure rather than discarding it (#424 review:
     * no unchecked flush/fsync anywhere on this path). */
    if (fflush(w->fp) != 0 || fsync(fileno(w->fp)) != 0)
        fprintf(stderr, "WARN: final flush of %s before rotation failed "
                "(%s) — it is archived uncertified\n",
                w->current_path, strerror(errno));
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
                                     new_path, sizeof(new_path)) != 0) {
        fprintf(stderr, "WARN: no free archive name for %s — leaving it as "
                "current.trace (it will be recovered on next start)\n",
                w->current_path);
    } else {
        /* #424 review finding: the checkpoint is CARRIED to the rotated
         * archive, and it moves BEFORE the trace does (see ckpt_handover).
         *
         * It used to be deleted here, on the argument that the open
         * intervals continue into the next file and get real closing records
         * there. That argument holds only if the daemon survives to write
         * them. Deleting the evidence before any successor exists means a
         * SIGKILL in this window loses the open intervals completely — the
         * one outcome this work exists to prevent — so the deletion was
         * trading a certainty for a convenience.
         *
         * The overlap with the next file's real records is not a reason to
         * destroy evidence; it is what pgwt_open_ckpt_suppressed_by() is
         * for, and the carried records are a lower bound presented next to
         * the archive, never added to totals. */
        struct pgwt_open_ckpt_identity id = {
            .trace_start_wall_ns = w->file_start_wall_ns,
            .trace_start_mono_ns = w->file_start_mono_ns,
        };
        ckpt_handover(w->trace_dir, new_path, &id, w->verbose);
        pgwt_crash_point(PGWT_CRASH_ROTATE_AFTER_CKPT);

        if (rename(w->current_path, new_path) != 0)
            fprintf(stderr, "WARN: cannot rename %s to %s: %s (it will be "
                    "recovered on the next start, which adopts the "
                    "checkpoint already at %s%s)\n", w->current_path,
                    new_path, strerror(errno), new_path,
                    PGWT_OPEN_CKPT_SUFFIX);
        pgwt_crash_point(PGWT_CRASH_ROTATE_AFTER_TRACE);
    }

    /* Remove meta file — the rotated .trace.lz4 has a real footer */
    {
        char meta_path[512];
        snprintf(meta_path, sizeof(meta_path), "%s/current.trace.meta",
                 w->trace_dir);
        unlink(meta_path);
    }
    if (pgwt_fsync_dir(w->trace_dir) != 0)
        fprintf(stderr, "WARN: could not fsync %s after rotation — the "
                "rename is visible now but may not survive a power cut\n",
                w->trace_dir);

    /* Reset stats for next file */
    w->total_events_written = 0;
    w->total_bytes_written = 0;
    w->force_rotate = false;

    return 0;
}

int pgwt_writer_close(struct pgwt_event_writer *w)
{
    if (!w->fp) return 0;

    /* #424: the ONLY place a file is certified clean. finalize_current_file
     * checks every step and fsyncs the data BEFORE the bit, so the bit is
     * never set over data that is not on disk. If it fails the file simply
     * stays uncertified — a reader then says "open intervals may be
     * missing", which is the truth. */
    int certified = finalize_current_file(w, PGWT_FLAG_CLOSED_CLEAN) == 0;

    /* #424 SUPPRESSION at shutdown. A clean close means the daemon's
     * open-interval flush (pgwt_flush_open_intervals, called before this —
     * src/daemon.c) already wrote a REAL closing record for every open
     * interval, so the checkpoint describes the same nanoseconds and must go.
     *
     * If the close could NOT be certified, the opposite holds: those real
     * records may not have reached the disk, so the checkpoint is the only
     * surviving evidence and is left for the next start's recovery to
     * validate and carry. */
    if (certified)
        ckpt_discard(w->trace_dir, "a clean close flushed those intervals as "
                     "real records", w->verbose);

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
     * daemon start by recovery, footer intact). finalize_current_file ran
     * this as a checked chain; repeat it for anything written since, and
     * report rather than discard a failure (#424 review). */
    if (fflush(w->fp) != 0 || fsync(fileno(w->fp)) != 0)
        fprintf(stderr, "WARN: final flush of %s failed (%s) — the file is "
                "closed but its tail may not be on disk\n",
                w->current_path, strerror(errno));
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
    /* #424 open-interval checkpoint sidecars. A torn tmp is always stale. A
     * sidecar is live state while the file it describes exists (the live
     * current.trace, or the archive it was carried next to) and an orphan
     * once that file is gone — otherwise every deleted archive would leave
     * one behind for good. */
    if (has_suffix(name, PGWT_OPEN_CKPT_SUFFIX ".tmp"))
        return RETN_ORPHAN_META;
    if (has_suffix(name, PGWT_OPEN_CKPT_SUFFIX)) {
        char base[600];
        snprintf(base, sizeof(base), "%s/%.*s", dir,
                 (int)(strlen(name) - strlen(PGWT_OPEN_CKPT_SUFFIX)), name);
        return access(base, F_OK) == 0 ? RETN_SKIP : RETN_ORPHAN_META;
    }
    return RETN_SKIP;
}

/* Delete an archive's checkpoint sidecar along with the archive: the sidecar
 * is evidence ABOUT that file and means nothing without it. */
static void retn_unlink_sidecar(const char *archive_path)
{
    char s[700];
    snprintf(s, sizeof(s), "%s%s", archive_path, PGWT_OPEN_CKPT_SUFFIX);
    unlink(s);
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
                retn_unlink_sidecar(path);
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
                retn_unlink_sidecar(path);
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
