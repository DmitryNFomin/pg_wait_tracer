/* U1b-0 seams only. B supplies production admission and the flat sink. */
#include "merge_capture.h"
#include <errno.h>

int pgwt_merge_capture_open(struct pgwt_daemon *d)
{
    (void)d;
    return -ENOSYS;
}

int pgwt_merge_capture_poll(struct pgwt_daemon *d)
{
    (void)d;
    return -ENOSYS;
}

int pgwt_merge_capture_tick(struct pgwt_daemon *d)
{
    (void)d;
    return -ENOSYS;
}

int pgwt_merge_capture_finish(struct pgwt_daemon *d)
{
    (void)d;
    return -ENOSYS;
}

int pgwt_merge_capture_record(struct pgwt_daemon *d, size_t source,
                             const void *bytes, size_t size)
{
    (void)d; (void)source; (void)bytes; (void)size;
    return -ENOSYS;
}

int pgwt_merge_capture_attach(struct pgwt_daemon *d,
                             const struct pgwt_attach_report *report)
{
    (void)d; (void)report;
    return -ENOSYS;
}

void pgwt_merge_capture_close(struct pgwt_daemon *d)
{
    (void)d;
}
