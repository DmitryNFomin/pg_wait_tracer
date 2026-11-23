/* timeline_writer.c — S0 stub of the timeline writer and reader (#1006).
 *
 * The prototypes in timeline_writer.h are frozen; S2 replaces these bodies.
 * Every entry point refuses with -ENOSYS so nothing can mistake the stub for
 * a writer that wrote nothing.
 */
#include "timeline_writer.h"

#include <errno.h>
#include <string.h>

int pgwt_tl_writer_open(struct pgwt_tl_writer **out,
                        const struct pgwt_tl_writer_config *cfg)
{
    (void)cfg;
    if (out)
        *out = NULL;
    return -ENOSYS;
}

int pgwt_tl_writer_append(struct pgwt_tl_writer *w,
                          const struct pgwt_tl_record *rec)
{
    (void)w; (void)rec;
    return -ENOSYS;
}

int pgwt_tl_writer_flush(struct pgwt_tl_writer *w)
{
    (void)w;
    return -ENOSYS;
}

int pgwt_tl_writer_close(struct pgwt_tl_writer *w, uint64_t end_ns,
                         uint64_t lost_raw_total)
{
    (void)w; (void)end_ns; (void)lost_raw_total;
    return -ENOSYS;
}

void pgwt_tl_header_init(struct pgwt_tl_header *h)
{
    if (h)
        memset(h, 0, sizeof(*h));
}

int pgwt_tl_header_check(const struct pgwt_tl_header *h)
{
    (void)h;
    return -ENOSYS;
}

int pgwt_tl_read_boot_id(uint8_t out[16])
{
    if (out)
        memset(out, 0, 16);
    return -ENOSYS;
}

int pgwt_tl_scan_segments(const char *dir, uint32_t *indexes, int max)
{
    (void)dir; (void)indexes; (void)max;
    return -ENOSYS;
}

int pgwt_tl_segment_path(const char *dir, uint32_t index, char *buf, size_t bufsz)
{
    (void)dir; (void)index;
    if (buf && bufsz)
        buf[0] = '\0';
    return -ENOSYS;
}

int pgwt_tl_reader_open(struct pgwt_tl_reader **out, const char *path)
{
    (void)path;
    if (out)
        *out = NULL;
    return -ENOSYS;
}

const struct pgwt_tl_header *pgwt_tl_reader_header(const struct pgwt_tl_reader *r)
{
    (void)r;
    return NULL;
}

int pgwt_tl_reader_next(struct pgwt_tl_reader *r, struct pgwt_tl_record *out)
{
    (void)r; (void)out;
    return -ENOSYS;
}

int pgwt_tl_reader_truncated(const struct pgwt_tl_reader *r)
{
    (void)r;
    return -ENOSYS;
}

int pgwt_tl_reader_stats(const struct pgwt_tl_reader *r, struct pgwt_tl_stats *out)
{
    (void)r; (void)out;
    return -ENOSYS;
}

uint64_t pgwt_tl_reader_skipped(const struct pgwt_tl_reader *r)
{
    (void)r;
    return 0;
}

void pgwt_tl_reader_close(struct pgwt_tl_reader *r)
{
    (void)r;
}
