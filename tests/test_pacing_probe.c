/* Pure contracts for the pre-14.3 syscall pacing bridge. Refs #804. */
#include "pacing_probe.h"
#include "sampler.h"
#include "idle_rule.h"
#include "wait_event.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int lookups;
static int lookup(void *ctx, uint32_t pid)
{
    assert(pid == 123);
    lookups++;
    return *(int *)ctx;
}

int main(void)
{
    assert(pgwt_idle_rule_timeout_mask() == 0);
    assert(pgwt_is_idle_event(PGWT_WEI_CKPT_PACING));
    assert(pgwt_timeout_id_is_classifiable(30));
    assert(strcmp(pgwt_event_name(PGWT_WEI_CKPT_PACING), "CheckpointWriteDelay") == 0);
    const struct { const char *version; int major, minor, needed; } cases[] = {
        {"12.22", 12, 22, 0}, {"13.0", 13, 0, 1}, {"13.23", 13, 23, 1},
        {"13beta1", 13, -1, 1}, {"13rc2", 13, -1, 1},
        {"14.0", 14, 0, 1}, {"14.2", 14, 2, 1}, {"14.3", 14, 3, 0},
        {"14.12", 14, 12, 0}, {"15.1", 15, 1, 0}, {"16.4", 16, 4, 0},
        {"17.0", 17, 0, 0}, {"18.1", 18, 1, 0}, {"14beta1", 14, -1, 0},
        {"14rc1", 14, -1, 0}, {"14", 14, -1, 0}, {"bad", 0, -1, 0},
        {"14devel", 14, -1, 0}, {"15beta1", 15, -1, 0},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        char line[160]; int major, minor;
        snprintf(line, sizeof(line), "postgres (PostgreSQL) %s (Ubuntu)", cases[i].version);
        pgwt_parse_pg_version(line, &major, &minor);
        assert(major == cases[i].major && minor == cases[i].minor);
        assert(pgwt_needs_ckpt_pacing_probe(major, minor) == cases[i].needed);
    }
    assert(!pgwt_needs_ckpt_pacing_probe(14, -1));
    assert(pgwt_needs_ckpt_pacing_probe(14, 0));
    assert(pgwt_needs_ckpt_pacing_probe(14, 1));
    assert(pgwt_needs_ckpt_pacing_probe(14, 2));
    assert(!pgwt_needs_ckpt_pacing_probe(14, 3));
    assert(!pgwt_needs_ckpt_pacing_probe(0, -1));
    const struct { uint64_t fires, obs; unsigned threshold; int inert; } verdicts[] = {
        {0, 0, 5, 0}, {0, 4, 5, 0}, {0, 5, 5, 1}, {0, 6, 5, 1},
        {1, 5, 5, 0}, {1, UINT64_MAX, 5, 0}, {0, 0, 0, 0},
        {0, 1, 1, 1}, {0, 2, 3, 0},
    };
    for (unsigned i = 0; i < sizeof(verdicts)/sizeof(verdicts[0]); i++)
        assert((pgwt_pacing_probe_verdict(verdicts[i].fires, verdicts[i].obs,
                verdicts[i].threshold) == PGWT_PACING_INERT) == verdicts[i].inert);

    for (int major = 13; major <= 18; major++) {
        pgwt_init_event_names(major);
        uint32_t mask = pgwt_idle_rule_timeout_mask();
        assert(pgwt_is_idle_event(PGWT_WEI_CKPT_PACING));
        assert(!pgwt_is_hidden_event(PGWT_WEI_CKPT_PACING));
        assert(!pgwt_is_session_idle_event(PGWT_WEI_CKPT_PACING));
        assert(pgwt_timeout_id_is_classifiable(30));
        assert(strcmp(pgwt_event_name(PGWT_WEI_CKPT_PACING), "CheckpointWriteDelay") == 0);
        assert(pgwt_idle_rule_timeout_mask() == mask);
        assert(!(mask & (1U << 30))); /* tracer id NEVER added to PG's table mask */
        if (major == 14) assert(mask == 0);
        if (major == 18) assert(mask == PGWT_IDLE_TIMEOUT_MASK_PG18);
    }
    pgwt_init_event_names(13);
    unsigned rows = 1; /* ClientRead */
    uint32_t mask = pgwt_idle_rule_timeout_mask();
    for (unsigned id = 0; id < 30; id++) {
        rows += (mask >> id) & 1U;
        const char *name = pgwt_event_name(WEI(PG_WAIT_TIMEOUT, id));
        assert(!name || strcmp(name, "CheckpointWriteDelay") != 0);
    }
    rows++; /* synthetic replaces the native event absent on PG13 */
    assert(rows <= PGWT_MAX_VISIBLE_IDLE_EVENTS);
    _Static_assert(PGWT_PACING_TIMEOUT_COUNT == 6 && PGWT_MAX_VISIBLE_IDLE_EVENTS == 7,
                   "synthetic replaces an absent name, not an additional Idle row");

    int present = 1;
    assert(pgwt_sampler_pacing_event(PGWT_BT_CHECKPOINTER, 123, 0, lookup, &present)
            == PGWT_WEI_CKPT_PACING);
    assert(lookups == 1);
    const enum pgwt_backend_type others[] = { PGWT_BT_CLIENT, PGWT_BT_BG_WRITER,
        PGWT_BT_AUTOVAC_WORKER, PGWT_BT_UNKNOWN };
    for (unsigned i = 0; i < sizeof(others)/sizeof(others[0]); i++)
        assert(pgwt_sampler_pacing_event(others[i], 123, 0, lookup, &present) == 0);
    uint32_t resource = WEI(PG_WAIT_IO, 1);
    assert(pgwt_sampler_pacing_event(PGWT_BT_CHECKPOINTER, 123, resource, lookup, &present) == resource);
    assert(lookups == 1); /* no map read on an ineligible target or real wait */
    present = 0;
    assert(pgwt_sampler_pacing_event(PGWT_BT_CHECKPOINTER, 123, 0, lookup, &present) == 0);
    present = -1;
    assert(pgwt_sampler_pacing_event(PGWT_BT_CHECKPOINTER, 123, 0, lookup, &present) == 0);
    assert(pgwt_sampler_pacing_event(PGWT_BT_CHECKPOINTER, 123, 0, NULL, NULL) == 0);

    struct pgwt_sample_target t = {.pid=123, .backend_type=PGWT_BT_CHECKPOINTER};
    struct pgwt_trace_event out;
    uint8_t valid = 1;
    uint32_t wei = PGWT_WEI_CKPT_PACING;
    assert(pgwt_sampler_build_batch(&t, &wei, &valid, 1, 100, &out, NULL, NULL) == 1);
    assert(out.new_event == PGWT_WEI_CKPT_PACING);
    assert(pgwt_is_idle_event(out.new_event));
    valid = 0;
    assert(pgwt_sampler_build_batch(&t, &wei, &valid, 1, 100, &out, NULL, NULL) == 0);
    puts("PASS pacing version, verdict, idle, budget and sampled substitution contracts");
    return 0;
}
