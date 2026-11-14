/* Host-only kernel shims. The driver inserts unchanged production bodies.
 * These tests exercise ordering, identity and accounting, never the verifier. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bpf/transport_producer.h"
/* glibc's <stdlib.h> declares its own on_exit(); the production program of
 * that name is compiled here under another one. (Found on the EL8 box: the
 * Mac's libc has no such declaration, so only a Linux build fails.) */
#define on_exit pgwt_program_on_exit
#define CHECK(label, condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s\n", label); exit(1); } } while (0)
#undef __always_inline
#define __always_inline inline
#define BPF_ANY 0
#define BPF_NOEXIST 1
#define BPF_RB_NO_WAKEUP 1
#define BPF_F_CURRENT_CPU 0xffffffffULL
#define PT_REGS_PARM1(ctx) ((ctx)->arg1)
#define PGWT_TRANSPORT_FALSIFIER 1
struct pt_regs { u64 arg1; };
struct bpf_perf_event_data { int unused; };
struct trace_event_raw_sched_process_fork { u32 parent_pid, child_pid; };
struct trace_event_raw_sched_process_template { u32 pid; };
struct pgwt_seen_query_key { u32 pid, reserved; u64 query_id; };
static int state_map, transport_ctl, transport_attempts, transport_failures, transport_discards;
static int lifetime_allocator, helper_cpus, helper_progress, test_spin;
static int event_ringbuf, lifecycle_rb, ringbuf_drops, ringbuf_site_drops;
static int fail_counters, test_failure_claims, perf_events, exact_attr_map, exact_seed_map;
static int init_wake, wakes; /* the INIT wake-up hint: counted, never a record */
static int seen_query_ids;
static struct pgwt_pid_state state;
static struct pgwt_transport_ctl ctl;
static struct pgwt_helper_progress progress;
static struct pgwt_fail_spin spin;
/* withdrawn[] is the transport_discards map; `discards` counts release calls. */
static u64 attempts[3], failures[3], withdrawn[3], drops, sites[PGWT_RING_FAIL_SITE_MAX];
static u64 map_failures[PGWT_BPF_FAIL_MAX], allocator, tick;
static u32 cpu, expected_cpu, exact_perf_mode = 1, helper_tgid = 100, helper_el_tid;
static u64 current_id = (u64)42 << 32 | 42, test_fail_spin_ns;
static u32 target_postmaster_pid = 1, test_counter_failure;
static bool state_present, registered, claimed, admission = true;
static int rewrite_wp_live = -1;
static bool close_on_edge, remove_on_clock;
static bool test_skip_init_store, test_drop_init_hint, test_no_sched_oncpu;
static u32 lightweight_mode, skip_query_id, pgproc_wait_offset;
static u64 my_wait_ptr_addr, debug_query_string_addr;
static u32 pg13_query_attr = 1, pg13_synthetic_attr, pg13_qd_plannedstmt_off;
static u32 pg13_ps_queryid_off, pg13_qd_sourcetext_off = 8;
static u32 bs_state_running = 2, bs_state_fastpath = 4, new_wait = 7;
static u64 edge_stamp;
static int fail_dest = -1, update_error, outputs, reservations, discards, text_result = 9;
struct record { unsigned char bytes[512]; int dest, done; u64 reserved_at; size_t length; };
static struct record records[64];
static void *bpf_map_lookup_elem(void *map, const void *key)
{
    u32 k = *(const u32 *)key;
    if (map == &state_map) return state_present && k == 42 ? &state : NULL;
    if (map == &transport_ctl) return &ctl;
    if (map == &transport_attempts) return &attempts[k];
    if (map == &transport_failures) return &failures[k];
    if (map == &transport_discards) return &withdrawn[k];
    if (map == &lifetime_allocator) return &allocator;
    if (map == &helper_cpus) return registered ? &expected_cpu : NULL;
    if (map == &helper_progress) return &progress;
    if (map == &test_spin) return &spin;
    if (map == &ringbuf_drops) return &drops;
    if (map == &ringbuf_site_drops) return &sites[k];
    if (map == &fail_counters) return &map_failures[k];
    if (map == &seen_query_ids) return NULL;
    abort();
}
static int bpf_map_update_elem(void *map, const void *key, const void *value, int flags)
{
    (void)key; (void)flags;
    if (map == &test_failure_claims) { if (claimed) return -1; claimed = true; return 0; }
    if (map == &state_map) { if (update_error) return update_error;
        state = *(const struct pgwt_pid_state *)value; state_present = true; return 0; }
    if (map == &seen_query_ids) return 0;
    abort();
}
static int bpf_map_delete_elem(void *map, const void *key)
{
    (void)key;
    if (map == &state_map) state_present = false;
    return 0;
}
static u64 bpf_ktime_get_ns(void)
{ tick += 1000000; if (remove_on_clock) state_present = false; return tick; }
static u64 bpf_get_current_pid_tgid(void) { return current_id; }
static u32 bpf_get_smp_processor_id(void) { return cpu; }
static void *bpf_ringbuf_reserve(void *map, size_t len, int flags)
{
    (void)flags;
    int dest = map == &event_ringbuf ? PGWT_WIRE_DEST_E : PGWT_WIRE_DEST_L;
    tick += 1000000;
    if (fail_dest == dest) return NULL;
    CHECK("reservation capacity", reservations < 64 && len <= 512);
    struct record *r = &records[reservations++];
    r->dest = dest; r->reserved_at = tick; r->length = len;
    if (rewrite_wp_live >= 0) {
        /* Reproduce the review: userspace replaces the whole map entry
         * between the reservation condition and the publication condition. */
        struct pgwt_pid_state replacement = state;
        replacement.wp_live = (u32)rewrite_wp_live;
        state = replacement;
    }
    return r->bytes;
}
static struct record *find_record(void *ptr)
{
    for (int i = 0; i < reservations; ++i) if (ptr == records[i].bytes) return &records[i];
    abort();
}
static void bpf_ringbuf_submit(void *ptr, int flags)
{ (void)flags; CHECK("submit owns live reservation", !find_record(ptr)->done); find_record(ptr)->done = 1; ++outputs; }
static void bpf_ringbuf_discard(void *ptr, int flags)
{ (void)flags; CHECK("discard owns live reservation", !find_record(ptr)->done); find_record(ptr)->done = -1; ++discards; }
static long bpf_ringbuf_output(void *map, void *ptr, size_t len, int flags)
{
    void *p = bpf_ringbuf_reserve(map, len, flags);
    if (!p) return -1;
    memcpy(p, ptr, len); bpf_ringbuf_submit(p, flags); return 0;
}
static long bpf_perf_event_output(void *ctx, void *map, u64 flags, void *ptr, size_t len)
{
    (void)ctx;
    CHECK("perf always current CPU", flags == BPF_F_CURRENT_CPU);
    if (map == &init_wake) { CHECK("hint carries only a pid", len == 4); ++wakes; return 0; }
    CHECK("transport perf destination", map == &perf_events);
    CHECK("perf capacity", reservations < 64 && len <= 512);
    if (fail_dest == PGWT_WIRE_DEST_CPU) return -1;
    struct record *r = &records[reservations++];
    memcpy(r->bytes, ptr, len); r->dest = PGWT_WIRE_DEST_CPU; r->length = len;
    r->done = 1; ++outputs; return 0;
}
static bool exact_admission_open(void) { return admission; }
static bool test_fail_once(u32 slot); /* actual body inserted below */
static u64 read_task_cpu_ns_for(struct pgwt_pid_state *s, u64 now)
{ (void)s; (void)now; return 0; }
static void resolve_exact_attr(u32 pid, u64 *qid, u16 *open, u16 *quality)
{ (void)pid; *qid = state.last_query_id; *open = state.cmd_open; *quality = 0; }
static void note_phase_edge(u32 p, u32 m, u64 now)
{ (void)p; (void)m; edge_stamp = now; if (close_on_edge) admission = false; }
static void note_cmd_edge(u32 p, u16 open, u64 now)
{
    (void)p; state.cmd_open = open; edge_stamp = now;
    if (rewrite_wp_live >= 0) {
        struct pgwt_pid_state replacement = state;
        replacement.wp_live = (u32)rewrite_wp_live;
        state = replacement;
    }
    if (close_on_edge) admission = false;
}
static void note_query_edge(u32 p, u64 qid, u64 now) { (void)p; state.last_query_id = qid; edge_stamp = now; }
static void count_uprobe_fire(u32 slot) { (void)slot; }
static u32 read_wait_event_direct(u64 addr) { (void)addr; return new_wait; }
static u32 read_wait_event(u64 *addr) { *addr = 100; return new_wait; }
static u64 resolve_be_entry(void) { return 0; }
static void accum_add(u32 e, u64 n) { (void)e; (void)n; }
static int bpf_probe_read_user(void *out, size_t len, const void *addr)
{ memcpy(out, addr, len); return 0; }
static long bpf_probe_read_user_str(void *out, size_t len, const void *addr)
{ (void)len; strcpy(out, addr); return text_result; }
/* PRODUCTION_FUNCTIONS */
static void reset(void)
{
    for (int i = 0; i < reservations; ++i)
        CHECK("every reservation released exactly once", records[i].done == 1 || records[i].done == -1);
    memset(&state, 0, sizeof(state)); state.cookie = 9; state.wp_live = 1;
    memset(attempts, 0, sizeof(attempts)); memset(failures, 0, sizeof(failures));
    memset(withdrawn, 0, sizeof(withdrawn)); helper_el_tid = 0;
    memset(sites, 0, sizeof(sites)); memset(map_failures, 0, sizeof(map_failures));
    memset(records, 0, sizeof(records)); memset(&spin, 0, sizeof(spin));
    memset(&progress, 0, sizeof(progress)); ctl = (struct pgwt_transport_ctl){.round=1};
    state_present = true; registered = true; claimed = false; admission = true;
    fail_dest = -1; outputs = reservations = discards = update_error = wakes = 0; text_result = 9;
    drops = tick = edge_stamp = allocator = 0; cpu = expected_cpu = 0;
    exact_perf_mode = 1; test_fail_spin_ns = 0; current_id = (u64)42 << 32 | 42;
    rewrite_wp_live = -1; close_on_edge = remove_on_clock = false;
}
static void timestamp_order(void)
{
    for (int i = 0; i < reservations; ++i) {
        struct record *r = &records[i];
        CHECK("every reservation released exactly once", r->done == 1 || r->done == -1);
        if (r->done != 1) continue;
        struct pgwt_wire_hdr *h = (void *)r->bytes;
        u64 stamp = 0;
        if (h->kind == PGWT_WIRE_TRACE) stamp = ((struct pgwt_wire_trace *)h)->payload.timestamp_ns;
        if (h->kind == PGWT_WIRE_LIFECYCLE) stamp = ((struct pgwt_wire_lifecycle *)h)->payload.timestamp;
        if (stamp) CHECK("timestamp after reserve", stamp >= r->reserved_at);
        CHECK("header lifetime immutable", h->cookie == 9 || h->kind == PGWT_WIRE_BOUNDARY);
        CHECK("N mode envelope", h->version == 1 && !h->flags && !h->q && h->cpu == cpu);
    }
}
static void tests(void)
{
    struct trace_event_raw_sched_process_fork fork = {.parent_pid=1, .child_pid=42};
    reset(); on_fork(&fork);
    CHECK("fresh fork cookie", state.cookie == 1 && allocator == 1);
    struct pgwt_wire_lifecycle saved = *(struct pgwt_wire_lifecycle *)records[0].bytes;
    on_fork(&fork);
    CHECK("pid reuse fresh cookie", state.cookie == 2 && allocator == 2);
    CHECK("queued cookie survives pid reuse", saved.hdr.cookie == 1 &&
        ((struct pgwt_wire_lifecycle *)records[0].bytes)->hdr.cookie == saved.hdr.cookie);
    reset(); allocator=~(u64)0; on_fork(&fork);
    CHECK("cookie exhaustion counted", !outputs && !attempts[2] && map_failures[PGWT_BPF_FAIL_FORK_STATE_UPDATE]==1);
    reset(); allocator = 8; on_fork(&fork); timestamp_order();
    reset(); on_fork(&fork); state.wp_live=1; on_watchpoint(NULL);
    struct pgwt_wire_trace first_data=*(struct pgwt_wire_trace *)records[1].bytes;
    on_fork(&fork); state.wp_live=1; on_watchpoint(NULL);
    CHECK("DATA cookie across PID reuse", first_data.hdr.cookie==1 &&
        ((struct pgwt_wire_trace *)records[1].bytes)->hdr.cookie==1 &&
        ((struct pgwt_wire_trace *)records[3].bytes)->hdr.cookie==2);
    reset(); emit_marker(PGWT_MARKER_PLAN_START); timestamp_order();
    CHECK("marker destination", attempts[1] == 1 && attempts[0] == 0);
    reset(); fail_dest = 1; emit_marker(PGWT_MARKER_PLAN_START);
    CHECK("marker fail attribution coherent", edge_stamp && attempts[1] == 1 && failures[1] == 1 && drops == 1 && sites[2] == 1);
    fail_dest = -1; emit_marker(PGWT_MARKER_PLAN_START);
    CHECK("failed attempt consumes seq", attempts[1] == 2 && ((struct pgwt_wire_hdr *)records[0].bytes)->seq == 2);
    reset(); struct pt_regs cmd = {.arg1=2}; on_report_activity(&cmd); timestamp_order();
    reset(); rewrite_wp_live = 0; on_report_activity(&cmd);
    CHECK("command saved admission releases reservation", reservations == 1 && outputs == 1 &&
        records[0].done == 1 && !discards && attempts[1] == 1 && !failures[1]);
    reset(); state.wp_live = 0; rewrite_wp_live = 1; on_report_activity(&cmd);
    CHECK("command saved admission avoids unreserved output", !reservations && !attempts[1] && !outputs);
    reset(); close_on_edge = true; on_report_activity(&cmd); timestamp_order();
    CHECK("command admission discard is not a ring drop", discards == 1 && !outputs &&
        attempts[1] == 1 && withdrawn[1] == 1 && !failures[1] && !drops && !sites[PGWT_RING_FAIL_COMMAND_BOUNDARY]);
    reset(); close_on_edge = true; emit_marker(PGWT_MARKER_PLAN_START); timestamp_order();
    CHECK("marker admission discard is not a ring drop", discards == 1 && !outputs &&
        attempts[1] == 1 && withdrawn[1] == 1 && !failures[1] && !drops && !sites[PGWT_RING_FAIL_MARKER]);
    reset(); remove_on_clock = true; emit_marker(PGWT_MARKER_PLAN_START); timestamp_order();
    CHECK("marker vanished state releases reservation", discards == 1 && !outputs &&
        withdrawn[1] == 1 && !failures[1] && !drops && !sites[PGWT_RING_FAIL_MARKER]);
    reset(); fail_dest = 1; on_report_activity(&cmd);
    CHECK("command fail attribution coherent", edge_stamp && state.cmd_open == 1 && attempts[1] == 1 && failures[1] == 1);
    reset(); transport_exit(42, &state); timestamp_order();
    CHECK("exit reserves both", attempts[1] == 1 && attempts[2] == 1 && outputs == 2 && !state_present);
    CHECK("exit shared timestamp", ((struct pgwt_wire_trace *)records[0].bytes)->payload.timestamp_ns ==
        ((struct pgwt_wire_lifecycle *)records[1].bytes)->payload.timestamp);
    for (int dest = 1; dest <= 2; ++dest) {
        reset(); fail_dest = dest; transport_exit(42, &state);
        CHECK("partial exit discarded and counted", !outputs && discards == 1 && failures[1] == 1 && failures[2] == 1 && drops == 1 && !state_present &&
            !withdrawn[1] && !withdrawn[2]);
    }
    reset(); state.wp_live=0; transport_exit(42, &state);
    CHECK("unattached exit submits lifecycle only", outputs==1 && !attempts[1] && attempts[2]==1 &&
        records[0].done==1 && !state_present);
    reset(); state.wp_live=0; fail_dest=2; transport_exit(42, &state);
    CHECK("unattached failed exit has no reservation to release", !reservations && !attempts[1] &&
        attempts[2]==1 && failures[2]==1 && !state_present);
    reset(); u64 addr = 1234; my_wait_ptr_addr = (u64)(uintptr_t)&addr;
    on_bootstrap(NULL);
    u64 init_ts = state.init_ts;
    CHECK("bootstrap stores INIT facts", state.init_stored && state.init_wait_addr == addr && init_ts);
    on_bootstrap(NULL);
    CHECK("bootstrap stores INIT once", state.init_ts == init_ts);
    CHECK("zero INIT records on L under exact_perf_mode", !reservations && !outputs && !attempts[2] && !failures[2]);
    /* One wake-up per stored INIT, and no transport attempt on any key. */
    CHECK("stored INIT wakes the sweep once, with no attempt", wakes == 1 && !attempts[0] && !attempts[1] && !attempts[2] &&
        !failures[0] && !withdrawn[0]);
    reset(); fail_dest = 2; on_bootstrap(NULL);
    CHECK("INIT storage independent of ring availability", state.init_stored && state.init_ts &&
        !reservations && !attempts[2] && !failures[2] && !sites[PGWT_RING_FAIL_LIFECYCLE_INIT_HINT]);
    reset(); state_present = false; on_bootstrap(NULL);
    CHECK("INIT missing state counted without publication", map_failures[PGWT_BPF_FAIL_INIT_NO_STATE] == 1 &&
        !reservations && !attempts[2] && !failures[2] && !wakes);
    reset(); on_watchpoint(NULL);
    CHECK("watchpoint perf only", outputs == 1 && attempts[0] == 1 && !attempts[1] && records[0].dest == 0);
    reset(); fail_dest = 0; on_watchpoint(NULL);
    CHECK("perf failure accounting", attempts[0] == 1 && failures[0] == 1 && drops == 1 && sites[0] == 1);
    reset(); current_id = (u64)100 << 32 | 100; ctl.el_request = 1;
    on_transport_helper(NULL);
    CHECK("helper destinations", outputs == 3 && attempts[0] == 1 && attempts[1] == 1 && attempts[2] == 1);
    u64 w = ((struct pgwt_wire_cut *)records[0].bytes)->payload.w;
    CHECK("boundary read before reserve", w < records[0].reserved_at);
    current_id = (u64)42 << 32 | 42; emit_marker(PGWT_MARKER_PLAN_START); timestamp_order();
    CHECK("nothing older after boundary", ((struct pgwt_wire_trace *)records[3].bytes)->payload.timestamp_ns >= w);
    /* The daemon's leader is its event loop: a named helper thread issues E/L. */
    reset(); helper_el_tid = 77; ctl.el_request = 1;
    current_id = (u64)100 << 32 | 100; on_transport_helper(NULL);
    CHECK("only the configured helper thread issues E/L", outputs == 1 && attempts[0] == 1 && !attempts[1] && !attempts[2]);
    current_id = (u64)100 << 32 | 78; on_transport_helper(NULL);
    CHECK("only the configured helper thread issues E/L", outputs == 2 && attempts[0] == 2 && !attempts[1] && !attempts[2]);
    current_id = (u64)100 << 32 | 77; on_transport_helper(NULL);
    CHECK("only the configured helper thread issues E/L", outputs == 5 && attempts[1] == 1 && attempts[2] == 1);
    on_transport_helper(NULL);
    CHECK("one E/L pair per request token", outputs == 6 && attempts[1] == 1 && attempts[2] == 1);
    ctl.el_request = 2; on_transport_helper(NULL);
    CHECK("a new token issues a new E/L pair", outputs == 9 && attempts[1] == 2 && attempts[2] == 2);
    for (int dest=0; dest<3; ++dest) {
        reset(); current_id=(u64)100<<32|100; ctl.el_request=1; fail_dest=dest;
        on_transport_helper(NULL); timestamp_order();
        CHECK("boundary failure reconciles only its destination", outputs==2 && !discards &&
            failures[dest]==1 && attempts[0]==1 && attempts[1]==1 && attempts[2]==1 && !drops);
    }
    reset(); current_id = (u64)100 << 32 | 100; expected_cpu = cpu + 1;
    on_transport_helper(NULL); CHECK("helper wrong CPU refused", !outputs && !attempts[0]);
    expected_cpu = cpu; current_id = (u64)101 << 32 | 100;
    on_transport_helper(NULL); CHECK("helper foreign TGID refused", !outputs);
    u64 query = 99, string = (u64)(uintptr_t)"SELECT 1";
    struct { u64 planned, text; } desc = {(u64)(uintptr_t)&query, string};
    debug_query_string_addr = (u64)(uintptr_t)&string;
    for (int pg13=0; pg13<2; ++pg13) {
        struct pt_regs q = {.arg1=pg13 ? (u64)(uintptr_t)&desc : 99};
        reset();
        if (pg13) on_executor_start(&q); else on_report_query_id(&q);
        timestamp_order();
        CHECK("text envelope", outputs==1 && attempts[2]==1 && ((struct pgwt_wire_hdr *)records[0].bytes)->kind==PGWT_WIRE_TEXT);
        reset(); fail_dest=2;
        if (pg13) on_executor_start(&q); else on_report_query_id(&q);
        CHECK("text failed reserve", !outputs && attempts[2]==1 && failures[2]==1 && sites[pg13?8:7]==1);
        reset(); text_result=-1;
        if (pg13) on_executor_start(&q); else on_report_query_id(&q);
        CHECK("text discarded read failure", !outputs && discards==1 && attempts[2]==1 && failures[2]==1 && !withdrawn[2] && sites[pg13?8:7]==1);
        reset(); text_result=1;
        if (pg13) on_executor_start(&q); else on_report_query_id(&q);
        CHECK("empty text is a discard, not a failure", !outputs && discards==1 && attempts[2]==1 && withdrawn[2]==1 && !failures[2] && sites[pg13?8:7]==0);
    }
    reset(); test_fail_spin_ns = 1000000; on_fork(&fork);
    CHECK("falsifier counted after measured spin", spin.ended-spin.started >= 1000000 && spin.completed >= spin.ended && failures[2] == 1 && attempts[2] == 1 && !outputs);
    reset(); exact_perf_mode = 0; emit_marker(PGWT_MARKER_PLAN_START);
    CHECK("mode zero raw marker", records[0].length == sizeof(struct pgwt_trace_event) && !attempts[1]);
    reset(); exact_perf_mode = 0; on_watchpoint(NULL);
    CHECK("mode zero raw wait", records[0].length == sizeof(struct pgwt_trace_event) && !attempts[0]);
    reset(); exact_perf_mode=0; on_fork(&fork);
    CHECK("mode zero raw fork", records[0].length==sizeof(struct pgwt_lifecycle_event) && !allocator && !attempts[2]);
    reset(); exact_perf_mode=0; on_bootstrap(NULL);
    CHECK("mode zero raw init", records[0].length==sizeof(struct pgwt_lifecycle_event) && !attempts[2]);
    init_ts = state.init_ts; on_bootstrap(NULL);
    CHECK("mode zero repeated hints keep stored timestamp", outputs==2 && state.init_ts==init_ts &&
        ((struct pgwt_lifecycle_event *)records[1].bytes)->timestamp==init_ts);
    reset(); exact_perf_mode=0; fail_dest=2; on_bootstrap(NULL);
    CHECK("mode zero INIT reserve failure keeps facts and hint drop", state.init_stored && state.init_ts &&
        !outputs && !attempts[2] && !failures[2] && sites[PGWT_RING_FAIL_LIFECYCLE_INIT_HINT]==1);
    reset(); exact_perf_mode=0;
    struct trace_event_raw_sched_process_template exit_context={.pid=42}; on_exit(&exit_context);
    CHECK("mode zero raw exit and retained state", outputs==2 && state_present && !attempts[1] && !attempts[2] && records[0].length==sizeof(struct pgwt_trace_event) && records[1].length==sizeof(struct pgwt_lifecycle_event));
    reset(); exact_perf_mode=0; on_report_activity(&cmd);
    CHECK("mode zero raw command", outputs==1 && records[0].length==sizeof(struct pgwt_trace_event) && !attempts[1]);
    puts("production producer bodies: PASS (identity, reservation order, accounting, helper, mode zero)");
}
int main(void) { tests(); return 0; }
