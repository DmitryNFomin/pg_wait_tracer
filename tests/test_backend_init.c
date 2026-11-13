/* The daemon's bootstrap sweep with fake map reads and hardware attachment. */
#include "backend_init.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct fixture {
    struct pgwt_backend_table bt;
    struct pgwt_pid_state state;
    int reads, attaches, read_error, attach_error, torn_first_read, fail_read_at;
    int unpublished_first_read;
    int retire_first_read;
    uint64_t addr, ts, exit_init_ts;
};

static int read_state(void *ctx, pid_t pid, struct pgwt_pid_state *out)
{
    struct fixture *f = ctx;
    assert(pid >= 42 && pid <= 44);
    ++f->reads;
    if (f->retire_first_read && f->reads == 1) {
        pgwt_pending_init_remove(&f->bt, &f->bt.entries[0]);
        f->bt.entries[0].is_alive = false;
        return -ENOENT;
    }
    if (f->reads == f->fail_read_at)
        return -1;
    *out = f->state;
    if (f->unpublished_first_read && f->reads == 1)
        out->init_stored = 0;
    if (f->torn_first_read && f->reads == 1) {
        out->init_wait_addr = 0;
        out->init_ts = 0;
    }
    return f->read_error;
}

static int attach(void *ctx, pid_t pid, uint64_t addr, uint64_t ts)
{
    struct fixture *f = ctx;
    assert(pid >= 42 && pid <= 44);
    ++f->attaches;
    f->addr = addr;
    f->ts = ts;
    if (f->attach_error)
        return -1;
    struct pgwt_backend *be = NULL;
    for (int i = 0; i < f->bt.count; ++i)
        if (f->bt.entries[i].pid == pid)
            be = &f->bt.entries[i];
    assert(be);
    pgwt_pending_init_remove(&f->bt, be);
    be->bootstrap_fd = -1;
    be->wp_fd = 99;
    be->init_ts = ts;
    be->init_attached = true;
    return 0;
}

static void reset(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->bt.count = 1;
    f->bt.entries[0] = (struct pgwt_backend){
        .pid = 42, .is_alive = true, .bootstrap_fd = 88, .wp_fd = -1,
    };
    pgwt_pending_init_add(&f->bt, &f->bt.entries[0]);
    f->state = (struct pgwt_pid_state){
        .init_stored = 1, .init_wait_addr = 0x12345678, .init_ts = 123456789,
    };
}

static int sweep(struct fixture *f)
{
    return pgwt_sweep_stored_inits(&f->bt, f, read_state, attach);
}

int main(void)
{
    struct fixture f;
    reset(&f);
    assert(sweep(&f) == 1 && f.attaches == 1);
    assert(f.addr == 0x12345678 && f.ts == 123456789);
    assert(f.bt.entries[0].init_ts == 123456789); /* captured time, not now */
    assert(!pgwt_stored_init_lost(&f.bt.entries[0], &f.state));
    assert(sweep(&f) == 0 && f.attaches == 1); /* second tick */

    reset(&f);
    f.torn_first_read = 1;
    assert(sweep(&f) == 1 && f.attaches == 1);
    assert(f.addr == 0x12345678 && f.ts == 123456789); /* publication race */

    reset(&f);
    f.fail_read_at = 2;
    assert(sweep(&f) == 0 && f.attaches == 0); /* publication re-read failed */

    reset(&f);
    f.state.init_wait_addr = 0;
    assert(sweep(&f) == 0 && f.attaches == 0); /* incomplete facts */
    f.state.init_wait_addr = 0x12345678;
    f.state.init_ts = 0;
    assert(sweep(&f) == 0 && f.attaches == 0);

    reset(&f);
    f.state.init_stored = 0;
    assert(sweep(&f) == 0 && f.attaches == 0); /* no evidence, keep pending */
    assert(f.bt.entries[0].bootstrap_fd == 88);
    assert(!pgwt_stored_init_lost(&f.bt.entries[0], &f.state));
    f.state.init_stored = 1;
    assert(sweep(&f) == 1); /* a later tick observes publication */

    reset(&f);
    f.read_error = -1;
    assert(sweep(&f) == 0 && f.attaches == 0); /* missing/failed map read */
    f.read_error = 0;
    f.attach_error = 1;
    assert(sweep(&f) == 0 && f.attaches == 1);
    assert(pgwt_stored_init_lost(&f.bt.entries[0], &f.state));

    reset(&f);
    assert(attach(&f, 42, f.state.init_wait_addr, f.state.init_ts) == 0);
    assert(sweep(&f) == 0 && f.reads == 0 && f.attaches == 1); /* hint first */

    reset(&f);
    assert(pgwt_stored_init_lost(&f.bt.entries[0], &f.state)); /* exit first */
    assert(pgwt_stored_init_lost(NULL, &f.state)); /* no registry entry */
    f.bt.entries[0].is_alive = false;
    assert(sweep(&f) == 0 && f.attaches == 0);
    f.bt.entries[0].pid = 0;
    f.bt.entries[0].bootstrap_fd = -1;
    assert(sweep(&f) == 0 && f.reads == 0); /* cleanup then late tick */

    reset(&f);
    f.bt.entries[0].bootstrap_fd = -1;
    assert(sweep(&f) == 0 && f.reads == 0); /* sampled/direct backend */
    f.bt.entries[0].init_attached = true;
    assert(!pgwt_stored_init_lost(&f.bt.entries[0], &f.state)); /* later tier */

    /* Every lifecycle batch can use the same sweep before any timer fires.
     * A large attached population adds no map reads to the pending sweep. */
    reset(&f);
    f.bt.count = MAX_BACKENDS;
    assert(sweep(&f) == 1 && f.reads == 2 && f.attaches == 1);
    assert(f.bt.pending_init_count == 0);

    /* Dense-set removal cannot skip the backend moved into the removed slot. */
    reset(&f);
    f.bt.count = 3;
    for (int i = 1; i < 3; ++i) {
        f.bt.entries[i] = (struct pgwt_backend){
            .pid = 42 + i, .is_alive = true, .bootstrap_fd = 88, .wp_fd = -1,
        };
        pgwt_pending_init_add(&f.bt, &f.bt.entries[i]);
    }
    pgwt_pending_init_remove(&f.bt, &f.bt.entries[1]);
    assert(f.bt.pending_init_count == 2);
    assert(f.bt.pending_init[1] == 2 && f.bt.entries[2].pending_init_slot == 2);
    pgwt_pending_init_remove(&f.bt, &f.bt.entries[2]);
    pgwt_pending_init_remove(&f.bt, &f.bt.entries[2]); /* idempotent */
    pgwt_pending_init_add(&f.bt, &f.bt.entries[0]); /* duplicate enrollment */
    assert(f.bt.pending_init_count == 1);
    assert(sweep(&f) == 1 && f.bt.pending_init_count == 0);

    reset(&f);
    f.bt.count = 3;
    for (int i = 1; i < 3; ++i) {
        f.bt.entries[i] = (struct pgwt_backend){
            .pid = 42 + i, .is_alive = true, .bootstrap_fd = 88, .wp_fd = -1,
        };
        pgwt_pending_init_add(&f.bt, &f.bt.entries[i]);
    }
    f.retire_first_read = 1;
    assert(sweep(&f) == 2 && f.attaches == 2 && f.reads == 5);
    assert(f.bt.pending_init_count == 0); /* removing one cannot skip another */

    reset(&f);
    f.unpublished_first_read = 1;
    assert(pgwt_exit_init_lost(&f.bt.entries[0], 42, &f, read_state, &f.exit_init_ts));
    assert(f.exit_init_ts == 123456789);
    assert(f.reads == 2 && f.attaches == 0); /* raced EXIT is recognised, not recovered */
    reset(&f);
    f.state.init_stored = 0;
    assert(!pgwt_exit_init_lost(&f.bt.entries[0], 42, &f, read_state, &f.exit_init_ts));
    f.state.init_store_failed = 1;
    assert(pgwt_exit_init_lost(&f.bt.entries[0], 42, &f, read_state, &f.exit_init_ts));
    assert(attach(&f, 42, 0x12345678, 123) == 0); /* another attach path succeeds */
    assert(!pgwt_exit_init_lost(&f.bt.entries[0], 42, &f, read_state, &f.exit_init_ts));

    reset(&f);
    f.read_error = -ENOENT;
    assert(sweep(&f) == 0 && f.attaches == 0);
    assert(pgwt_exit_init_lost(&f.bt.entries[0], 42, &f, read_state, &f.exit_init_ts));
    /* Missing fork-seeded state plus a delivered hint is not an INIT loss. */
    assert(attach(&f, 42, 0x12345678, 123) == 0);
    assert(!pgwt_exit_init_lost(&f.bt.entries[0], 42, &f, read_state, &f.exit_init_ts));
    assert(!pgwt_exit_init_lost(NULL, 42, &f, read_state, &f.exit_init_ts)); /* repeated cleanup */
    reset(&f);
    f.read_error = -ENOENT;
    f.bt.entries[0].bootstrap_fd = -1;
    f.bt.entries[0].init_ts = 123; /* delivered hint, failed real attach */
    assert(pgwt_exit_init_lost(&f.bt.entries[0], 42, &f, read_state, &f.exit_init_ts));
    f.read_error = 0;
    f.state = (struct pgwt_pid_state){0}; /* failed attach preseed made an entry */
    assert(pgwt_exit_init_lost(&f.bt.entries[0], 42, &f, read_state, &f.exit_init_ts));
    reset(&f);
    f.read_error = -EIO;
    assert(!pgwt_exit_init_lost(&f.bt.entries[0], 42, &f, read_state, &f.exit_init_ts));

    puts("backend INIT sweep: PASS (pending batches, timestamp, duplicate, exit races, missing state)");
    return 0;
}
