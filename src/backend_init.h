/* Portable bootstrap recovery policy; production supplies map/attach calls. */
#ifndef PGWT_BACKEND_INIT_H
#define PGWT_BACKEND_INIT_H

#include "backend.h"

typedef int (*pgwt_init_state_reader)(void *, pid_t, struct pgwt_pid_state *);
typedef int (*pgwt_init_attacher)(void *, pid_t, uint64_t, uint64_t);

void pgwt_pending_init_add(struct pgwt_backend_table *, struct pgwt_backend *);
void pgwt_pending_init_remove(struct pgwt_backend_table *, struct pgwt_backend *);
int pgwt_sweep_stored_inits(struct pgwt_backend_table *bt, void *ctx,
                            pgwt_init_state_reader read_state,
                            pgwt_init_attacher attach);
bool pgwt_stored_init_lost(const struct pgwt_backend *be,
                           const struct pgwt_pid_state *st);
/* Final single-pid sweep after the bootstrap producer is quiesced. EXIT
 * cannot recover coverage: this only recognises evidence and decides loss.
 * Readers return -ENOENT for an absent state entry, another error otherwise.
 * before_attach is true for retained state/registry evidence with no INIT facts,
 * no completed real attachment and no already-counted real attach failure.
 * An exit-race reason does not exclude opportunity classification.
 * init_ts receives the retained timestamp, or 0 if timing is unavailable. */
bool pgwt_exit_init_lost(const struct pgwt_backend *be, pid_t pid,
                         void *ctx, pgwt_init_state_reader read_state,
                         uint64_t *init_ts, bool *before_attach);

/* Same decision, with the branch that supplied it; no extra map reads. */
bool pgwt_exit_init_lost_diagnostic(const struct pgwt_backend *be, pid_t pid,
    void *ctx, pgwt_init_state_reader read_state, uint64_t *init_ts,
    bool *before_attach, const char **branch);

#endif
