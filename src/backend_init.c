#include "backend_init.h"

int pgwt_sweep_stored_inits(struct pgwt_backend_table *bt, void *ctx,
                            pgwt_init_state_reader read_state,
                            pgwt_init_attacher attach)
{
    int attached = 0;
    for (int i = 0; i < bt->count; ++i) {
        struct pgwt_backend *be = &bt->entries[i];
        if (!be->is_alive || be->pid <= 0 || be->bootstrap_fd < 0 ||
            be->wp_fd >= 0)
            continue;
        struct pgwt_pid_state st;
        if (read_state(ctx, be->pid, &st) != 0 || !st.init_stored)
            continue;
        /* A map lookup copies the whole value, not an atomic snapshot. The
         * first copy may have read the facts before publication and the flag
         * afterwards. Once that copy observes the flag, a second copy reads
         * the immutable facts published by this backend before the flag. */
        if (read_state(ctx, be->pid, &st) != 0 || !st.init_stored ||
            st.init_wait_addr == 0 || st.init_ts == 0)
            continue;
        if (attach(ctx, be->pid, st.init_wait_addr, st.init_ts) == 0 &&
            be->init_attached)
            ++attached;
    }
    return attached;
}

bool pgwt_stored_init_lost(const struct pgwt_backend *be,
                           const struct pgwt_pid_state *st)
{
    return st->init_stored && (!be || !be->init_attached);
}
