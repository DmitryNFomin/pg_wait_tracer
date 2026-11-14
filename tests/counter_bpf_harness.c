/* Host C shims for selected production failure paths. Not a BPF loader or
 * verifier substitute. The Python driver inserts unchanged function bodies. */
#include "pg_wait_tracer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#undef __always_inline
#define __always_inline inline
#define BPF_ANY 0
#define BPF_NOEXIST 1
#define PT_REGS_PARM1(ctx) ((ctx)->arg1)
#define CHECK(label, condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s\n", label); exit(1); } } while (0)
struct pt_regs { u64 arg1; };
struct pgwt_seen_query_key { u32 pid; u32 reserved; u64 query_id; };
struct bpf_perf_event_data { int unused; };
struct trace_event_raw_sched_process_fork { u32 parent_pid, child_pid; };
static int fail_counters, ringbuf_site_drops, exact_attr_map, accum_map;
static int state_map, seen_query_ids, lifecycle_rb, test_failure_claims;
static u64 failures[PGWT_BPF_FAIL_MAX], sites[PGWT_RING_FAIL_SITE_MAX];
static struct pgwt_exact_attr attr;
static struct pgwt_exact_config config;
static struct pgwt_pid_state state;
static struct pgwt_accum_val accum;
static unsigned char ring_record[2048];
static bool attr_present, accum_present, state_present, seen, claimed;
static bool lose_relookup, reserve_fail;
static int update_error, read_error_at, read_calls, text_result;
static int submits, discards;
static u32 test_counter_failure;
static u64 my_wait_ptr_addr, debug_query_string_addr;
static u32 target_postmaster_pid = 1, pgproc_wait_offset, lightweight_mode, skip_query_id;
static bool test_skip_init_store, test_drop_init_hint;
static u32 pg13_query_attr = 1, pg13_qd_plannedstmt_off, pg13_ps_queryid_off;
static u32 pg13_qd_sourcetext_off;

static void *bpf_map_lookup_elem(void *map, const void *key)
{
    if (map == &fail_counters) return &failures[*(const u32 *)key];
    if (map == &ringbuf_site_drops) return &sites[*(const u32 *)key];
    if (map == &exact_attr_map) return attr_present && !lose_relookup ? &attr : NULL;
    if (map == &accum_map) return accum_present ? &accum : NULL;
    if (map == &state_map) return state_present ? &state : NULL;
    if (map == &seen_query_ids) return seen ? &seen : NULL;
    abort();
}
static int bpf_map_update_elem(void *map, const void *key, const void *value, int flags)
{
    (void)key;
    if (map == &test_failure_claims) {
        CHECK("claim uses NOEXIST", flags == BPF_NOEXIST);
        if (claimed) return -EEXIST;
        claimed = true;
        return 0;
    }
    if (update_error) return update_error;
    if (map == &exact_attr_map) { attr = *(const struct pgwt_exact_attr *)value; attr_present = true; }
    else if (map == &accum_map) { accum = *(const struct pgwt_accum_val *)value; accum_present = true; }
    else if (map == &state_map) { state = *(const struct pgwt_pid_state *)value; state_present = true; }
    else if (map == &seen_query_ids) seen = true;
    else abort();
    return 0;
}
static int bpf_probe_read_user(void *out, size_t len, const void *addr)
{
    ++read_calls;
    if (read_calls == read_error_at) { memset(out, 0, len); return -EFAULT; }
    memcpy(out, addr, len);
    return 0;
}
static long bpf_probe_read_user_str(void *out, size_t len, const void *addr)
{
    (void)len; (void)addr;
    memcpy(out, "SELECT 1", 9);
    return text_result;
}
static void *bpf_ringbuf_reserve(void *map, size_t len, int flags)
{
    (void)map; (void)flags;
    CHECK("ring record fits", len <= sizeof(ring_record));
    return reserve_fail ? NULL : ring_record;
}
static void bpf_ringbuf_submit(void *record, int flags) { (void)record; (void)flags; ++submits; }
static void bpf_ringbuf_discard(void *record, int flags) { (void)record; (void)flags; ++discards; }
static u64 bpf_get_current_pid_tgid(void) { return (u64)42 << 32; }
static u64 bpf_ktime_get_ns(void) { return 1000; }
static struct pgwt_exact_config *exact_config(void) { return &config; }
static bool exact_admission_open(void) { return true; }
static void count_uprobe_fire(u32 slot) { (void)slot; }

/* PRODUCTION_FUNCTIONS */

static void reset(void)
{
    memset(failures, 0, sizeof(failures)); memset(sites, 0, sizeof(sites));
    memset(&attr, 0, sizeof(attr)); memset(&state, 0, sizeof(state));
    memset(&accum, 0, sizeof(accum));
    attr_present = accum_present = seen = claimed = false;
    state_present = true;
    lose_relookup = reserve_fail = false;
    test_counter_failure = 0;
    update_error = read_error_at = read_calls = submits = discards = 0;
    text_result = 9;
}
static void test_reads(void)
{
    u32 wait = 77; u64 pointer = (u64)(uintptr_t)&wait, addr;
    my_wait_ptr_addr = (u64)(uintptr_t)&pointer;
    reset();
    CHECK("wait direct success", read_wait_event_direct(pointer) == 77 && failures[3] == 0);
    wait = 0;
    CHECK("wait valid zero", read_wait_event_direct(pointer) == 0 && failures[3] == 0);
    CHECK("wait zero address", read_wait_event_direct(0) == 0 && failures[3] == 1);
    reset(); read_error_at = 1;
    CHECK("wait direct read error", read_wait_event_direct(pointer) == 0 && failures[3] == 1);
    reset(); read_error_at = 1;
    CHECK("wait pointer read error", read_wait_event(&addr) == 0 && addr == 0 && failures[3] == 1);
    reset(); read_error_at = 2;
    CHECK("wait indirect read error", read_wait_event(&addr) == 0 && addr == pointer && failures[3] == 1);
    reset(); wait = 77;
    CHECK("wait indirect success", read_wait_event(&addr) == 77 && addr == pointer && failures[3] == 0);
    reset(); read_error_at = 1;
    on_bootstrap(NULL);
    CHECK("wait bootstrap pointer error", failures[3] == 1 && submits == 0);
    reset(); on_bootstrap(NULL);
    CHECK("bootstrap success", failures[3] == 0 && submits == 1 && state.init_stored);
    reset(); test_counter_failure = PGWT_BPF_FAIL_WAIT_READ;
    CHECK("wait knob once", read_wait_event_direct(pointer) == 0 && failures[3] == 1);
    CHECK("wait knob subsequent success", read_wait_event_direct(pointer) == 77 && failures[3] == 1);
}
static void test_maps(void)
{
    reset(); update_error = -ENOSPC; note_query_edge(42, 10, 100);
    CHECK("exact query insert error", failures[4] == 1 && !attr_present);
    reset(); update_error = -ENOSPC; note_cmd_edge(42, 1, 100);
    CHECK("exact cmd insert error", failures[5] == 1 && !attr_present);
    reset(); update_error = -ENOSPC; note_phase_edge(42, PGWT_MARKER_PLAN_START, 100);
    CHECK("exact phase insert error", failures[6] == 1 && !attr_present);
    reset(); lose_relookup = true; note_phase_edge(42, PGWT_MARKER_PLAN_START, 100);
    CHECK("exact phase relookup miss", failures[6] == 1 && attr_present);
    reset(); update_error = -ENOSPC; accum_add(1, 10);
    CHECK("accum insert error", failures[7] == 1 && !accum_present);
    struct trace_event_raw_sched_process_fork fork = {.parent_pid = 1, .child_pid = 42};
    reset(); update_error = -ENOSPC; on_fork(&fork);
    CHECK("fork state insert error", failures[8] == 1 && failures[0] == 0);
    reset(); on_fork(&fork);
    CHECK("fork state success", failures[8] == 0 && submits == 1);
    reset(); note_query_edge(42, 10, 100); note_cmd_edge(42, 1, 110);
    note_phase_edge(42, PGWT_MARKER_PLAN_START, 120); accum_add(1, 10); accum_add(1, 15);
    CHECK("exact and accum success", attr.query_id == 10 && attr.cmd_open == 1 &&
          attr.plan_start_ts == 120 && accum.total_ns == 25 && accum.count == 2);
    for (int slot = 4; slot <= 8; ++slot) {
        reset(); test_counter_failure = slot;
        for (int repeat = 0; repeat < 2; ++repeat) {
            switch (slot) {
            case 4: note_query_edge(42, 10, 100); break;
            case 5: note_cmd_edge(42, 1, 100); break;
            case 6: note_phase_edge(42, PGWT_MARKER_PLAN_START, 100); break;
            case 7: accum_add(1, 10); break;
            case 8: on_fork(&fork); break;
            }
        }
        CHECK("map knob once", failures[slot] == 1);
    }
}
static void test_text(void)
{
    u64 text_pointer = (u64)(uintptr_t)"SELECT 1";
    debug_query_string_addr = (u64)(uintptr_t)&text_pointer;
    struct { u64 planned; u64 source; } desc;
    u64 query_id = 99;
    desc.planned = (u64)(uintptr_t)&query_id; desc.source = text_pointer;
    pg13_qd_sourcetext_off = 8;
    struct pt_regs ctx = {.arg1 = 99};
    for (int pg13 = 0; pg13 < 2; ++pg13) {
        for (int result = -1; result <= 9; result += result == -1 ? 2 : 8) {
            reset(); text_result = result;
            ctx.arg1 = pg13 ? (u64)(uintptr_t)&desc : 99;
            if (pg13) on_executor_start(&ctx); else on_report_query_id(&ctx);
            if (pg13)
                CHECK("pg13 text read outcome", sites[8] == (result < 0));
            else
                CHECK("query text read outcome", sites[7] == (result < 0));
            CHECK("text empty vs unreadable", discards == (result <= 1) && submits == (result > 1));
            CHECK("text first seen only on success", seen == (result > 1));
        }
        reset(); reserve_fail = true;
        if (pg13) on_executor_start(&ctx); else on_report_query_id(&ctx);
        CHECK("text reserve error", sites[pg13 ? 8 : 7] == 1 && !seen);
        reset(); test_counter_failure = pg13 ? 10 : 9;
        if (pg13) on_executor_start(&ctx); else on_report_query_id(&ctx);
        CHECK("text knob error", sites[pg13 ? 8 : 7] == 1 && discards == 1 && !seen);
        if (pg13) on_executor_start(&ctx); else on_report_query_id(&ctx);
        CHECK("text knob once", sites[pg13 ? 8 : 7] == 1 && submits == 1 && seen);
    }
}
int main(void)
{
    test_reads(); test_maps(); test_text();
    puts("BPF failure paths (host C): PASS (natural errors, silent paths, success, once-only knobs)");
    return 0;
}
