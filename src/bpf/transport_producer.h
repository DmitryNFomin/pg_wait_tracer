/* Pure producer operations shared with the portable A1 test. Little-endian
 * x86/arm64 targets only, as for the frozen payloads. No q allocation. */
#ifndef PGWT_TRANSPORT_PRODUCER_H
#define PGWT_TRANSPORT_PRODUCER_H
#include "../pgwt_transport_wire.h"
#include "../pg_wait_tracer.h"

struct pgwt_wire_trace { struct pgwt_wire_hdr hdr; struct pgwt_trace_event payload; };
struct pgwt_wire_lifecycle { struct pgwt_wire_hdr hdr; struct pgwt_lifecycle_event payload; };
struct pgwt_wire_text { struct pgwt_wire_hdr hdr; struct pgwt_query_text_event payload; };
struct pgwt_wire_cut { struct pgwt_wire_hdr hdr; struct pgwt_wire_boundary payload; };
_Static_assert(sizeof(struct pgwt_wire_trace) == 80, "wire TRACE size");
_Static_assert(sizeof(struct pgwt_wire_lifecycle) == 56, "wire lifecycle size");
_Static_assert(sizeof(struct pgwt_wire_text) == 304, "wire TEXT size");
_Static_assert(sizeof(struct pgwt_wire_cut) == 48, "wire boundary size");
/* Test-only out-of-band evidence. 'completed' is published after accounting. */
struct pgwt_fail_spin { u64 started, ended, completed; };
struct pgwt_helper_progress { u32 el_request; };

static __inline u64 pgwt_producer_cookie(const struct pgwt_pid_state *state)
{
    return state ? state->cookie : 0;
}
/* Only the selected postmaster's synchronous fork path writes this authority.
 * Saturate rather than reuse a cookie on wrap; failure disqualifies admission. */
static __inline u64 pgwt_producer_allocate(u64 *authority)
{
    if (!authority || *authority == ~(u64)0) return 0;
    return ++*authority;
}
static __inline u64 pgwt_producer_attempt(u64 *attempts)
{
    if (!attempts) return 0;
    /* XADD, not fetch-XADD (the latter needs a newer kernel). Qualified
     * producers of one destination cannot nest on one CPU; see the argument. */
    __sync_fetch_and_add(attempts, 1);
    return *(volatile u64 *)attempts;
}
static __inline void pgwt_producer_header(struct pgwt_wire_hdr *h, u8 kind,
                                         u16 length, u16 cpu, u64 seq, u64 cookie)
{
    *h = (struct pgwt_wire_hdr){ .version = PGWT_WIRE_VERSION, .kind = kind,
        .payload_len = length, .cpu = cpu, .seq = seq, .cookie = cookie };
}
static __inline bool pgwt_producer_helper(u64 id, u32 tgid, const u32 *expected, u32 cpu)
{
    return tgid && (u32)(id >> 32) == tgid && expected && *expected == cpu;
}
#endif
