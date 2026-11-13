/* Portable bootstrap recovery policy; production supplies map/attach calls. */
#ifndef PGWT_BACKEND_INIT_H
#define PGWT_BACKEND_INIT_H

#include "backend.h"

typedef int (*pgwt_init_state_reader)(void *, pid_t, struct pgwt_pid_state *);
typedef int (*pgwt_init_attacher)(void *, pid_t, uint64_t, uint64_t);

int pgwt_sweep_stored_inits(struct pgwt_backend_table *bt, void *ctx,
                            pgwt_init_state_reader read_state,
                            pgwt_init_attacher attach);
bool pgwt_stored_init_lost(const struct pgwt_backend *be,
                           const struct pgwt_pid_state *st);

#endif
