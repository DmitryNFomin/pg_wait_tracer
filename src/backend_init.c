#include "backend_init.h"
#include <errno.h>

void pgwt_pending_init_add(struct pgwt_backend_table *bt, struct pgwt_backend *be)
{
    if (be->pending_init_slot)
        return;
    bt->pending_init[bt->pending_init_count++] = (int)(be - bt->entries);
    be->pending_init_slot = bt->pending_init_count;
}

void pgwt_pending_init_remove(struct pgwt_backend_table *bt, struct pgwt_backend *be)
{
    if (!be->pending_init_slot)
        return;
    int slot = be->pending_init_slot - 1;
    int moved = bt->pending_init[--bt->pending_init_count];
    bt->pending_init[slot] = moved;
    bt->entries[moved].pending_init_slot = slot + 1;
    be->pending_init_slot = 0;
}

int pgwt_sweep_stored_inits(struct pgwt_backend_table *bt, void *ctx,
                            pgwt_init_state_reader read_state,
                            pgwt_init_attacher attach)
{
    int attached = 0;
    int i = 0;
    while (i < bt->pending_init_count) {
        struct pgwt_backend *be = &bt->entries[bt->pending_init[i]];
        if (!be->is_alive || be->pid <= 0 || be->bootstrap_fd < 0 ||
            be->wp_fd >= 0) {
            pgwt_pending_init_remove(bt, be);
            continue;
        }
        struct pgwt_pid_state st;
        if (read_state(ctx, be->pid, &st) != 0 || !st.init_stored) {
            if (be->pending_init_slot == i + 1)
                ++i;
            continue;
        }
        /* A map lookup copies the whole value, not an atomic snapshot. The
         * first copy may have read the facts before publication and the flag
         * afterwards. Once that copy observes the flag, a second copy reads
         * the immutable facts published by this backend before the flag. */
        if (read_state(ctx, be->pid, &st) != 0 || !st.init_stored ||
            st.init_wait_addr == 0 || st.init_ts == 0) {
            if (be->pending_init_slot == i + 1)
                ++i;
            continue;
        }
        if (attach(ctx, be->pid, st.init_wait_addr, st.init_ts) == 0 &&
            be->init_attached)
            ++attached;
        /* Production removes the pending entry when closing bootstrap.
         * A fake/failed attacher may leave it pending for the next batch. */
        if (be->pending_init_slot == i + 1)
            ++i;
    }
    return attached;
}

bool pgwt_stored_init_lost(const struct pgwt_backend *be,
                           const struct pgwt_pid_state *st)
{
    return (st->init_stored || st->init_store_failed || (be && be->init_ts)) &&
           (!be || !be->init_attached);
}

bool pgwt_exit_init_lost(const struct pgwt_backend *be, pid_t pid,
                         void *ctx, pgwt_init_state_reader read_state,
                         uint64_t *init_ts)
{
    *init_ts = be ? be->init_ts : 0;
    struct pgwt_pid_state st = {0};
    int rc = read_state(ctx, pid, &st);
    if (rc == -ENOENT)
        return be && !be->init_attached &&
               (be->bootstrap_fd >= 0 || be->init_ts != 0);
    if (rc != 0)
        return false;
    /* A second read recognises an INIT published during the first copy.
     * The caller closed bootstrap first, so the facts are now immutable. */
    if (read_state(ctx, pid, &st) != 0)
        return false;
    if (st.init_ts)
        *init_ts = st.init_ts;
    return pgwt_stored_init_lost(be, &st);
}
