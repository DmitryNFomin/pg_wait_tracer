#ifndef PGWT_DISCOVERY_H
#define PGWT_DISCOVERY_H

#include <stdbool.h>
#include <stdint.h>

/* Producer -> merge interface, not a wire/storage format. This consumer does
 * not establish any physical fact. The producer retains the attempt history
 * and completed producer fences, including silent mutable-state updates.
 * Every required bit must be both known and true. Unknown never means pass. */
#define PGWT_DISCOVERY_COOKIE_MIN (UINT64_C(1) << 63)
enum pgwt_discovery_fact {
    PGWT_DISC_LIFECYCLE_ACCOUNTING = UINT64_C(1) << 0,
    PGWT_DISC_PREEXISTING          = UINT64_C(1) << 1,
    PGWT_DISC_NO_FORK_PROVENANCE   = UINT64_C(1) << 2,
    PGWT_DISC_COOKIE_AUTHORITY     = UINT64_C(1) << 3,
    PGWT_DISC_IDENTITY_VISIBLE     = UINT64_C(1) << 4,
    PGWT_DISC_INSTALL_BEFORE_ENABLE= UINT64_C(1) << 5,
    PGWT_DISC_PINNED_BINDING       = UINT64_C(1) << 6,
    PGWT_DISC_ADDRESS_CONTINUOUS   = UINT64_C(1) << 7,
    PGWT_DISC_PRESERVED_STATE      = UINT64_C(1) << 8,
    PGWT_DISC_ENABLED              = UINT64_C(1) << 9,
    PGWT_DISC_EMISSION_IDENTITY    = UINT64_C(1) << 10,
    PGWT_DISC_FAILURE_ACCOUNTING   = UINT64_C(1) << 11,
    PGWT_DISC_BRACKET_ORDER        = UINT64_C(1) << 12,
    PGWT_DISC_THREAD_HISTORY       = UINT64_C(1) << 13,
    PGWT_DISC_OWNERSHIP            = UINT64_C(1) << 14,
    PGWT_DISC_CPU_ENROLLED         = UINT64_C(1) << 15,
    PGWT_DISC_FRESH_COHERENT_SEED   = UINT64_C(1) << 16,
    PGWT_DISC_RECONCILED           = UINT64_C(1) << 17,
    PGWT_DISC_COUNTER_NO_WRAP      = UINT64_C(1) << 18,
    PGWT_DISC_REQUIRED_READS       = UINT64_C(1) << 19,
    /* Capture-owned facts, resolved from retained inventory and gap state. */
    PGWT_DISC_INVENTORIED          = UINT64_C(1) << 20,
    PGWT_DISC_STARTUP_OPEN         = UINT64_C(1) << 21
};
#define PGWT_DISC_ADMISSION_FACTS ((UINT64_C(1) << 12) - 1)
#define PGWT_DISC_QUIET_FACTS ((((UINT64_C(1) << 20) - 1) & \
    ~(uint64_t)PGWT_DISC_ADMISSION_FACTS) | PGWT_DISC_PINNED_BINDING)
/* Only INACTIVE_SYSCALL is qualified wait_task_inactive evidence. These
 * separate values prevent state letters, pcount or schedstat from passing. */
typedef enum {
    PGWT_DISC_INACTIVITY_UNKNOWN, PGWT_DISC_INACTIVE_SYSCALL,
    PGWT_DISC_RUNNING, PGWT_DISC_DENIED, PGWT_DISC_STATE_COUNTER_ONLY,
    PGWT_DISC_SCHEDSTAT_ONLY, PGWT_DISC_MALFORMED
} pgwt_discovery_inactivity;
typedef struct {
    uint64_t known, truth;
} pgwt_discovery_facts;
typedef struct {
    uint64_t t, e, wait_word, v0, v1, handle0, handle1;
    uint64_t armed_address, resolved_address, accumulator, cpu_base, on_cpu_ts;
    uint32_t threads0, threads1;
    pgwt_discovery_inactivity S0, S1;
    bool terminal0, terminal1, cpu_base_valid;
    pgwt_discovery_facts facts;
    /* Failed optional brackets are ordinary rejection. Claiming completed
     * coherence/identity/continuity despite contradictory facts is a fault. */
    bool claims_completed_coherence;
} pgwt_discovery_witness;
typedef enum {
    PGWT_DISC_ADMIT, PGWT_DISC_CONFIRM, PGWT_DISC_TERMINAL, PGWT_DISC_ABANDON
} pgwt_discovery_control_kind;
typedef struct {
    pgwt_discovery_control_kind kind;
    uint64_t pid, cookie, attachment_generation, e;
    pgwt_discovery_facts proof;
    pgwt_discovery_witness witness;
    /* TERMINAL: retained matching kernel/lifecycle proof (not ESRCH). The
     * subsequent matching EXIT must release successfully at terminal_time. */
    uint64_t terminal_time;
    bool terminal_binding_proved;
} pgwt_discovery_attempt;
typedef struct {
    uint64_t attachment_generation;
    bool membership_proved, marker, baselines_refreshed;
    /* Retained producer attribution: metadata belongs only to (pid,cookie).
     * Shared metadata never sets this flag. */
    bool exclusive_text;
} pgwt_discovery_origin;
typedef struct {
    bool admitted, confirmed, has_lower_bound, terminal_released, abandoned;
    bool has_uncertified;
    uint64_t uncertified_through; /* greatest validated consumed numerical stamp */
    uint64_t attachment_generation, lower_bound, terminal_time;
    uint64_t source, position;
    pgwt_discovery_witness baseline;
} pgwt_discovery_state;

static inline bool pgwt_discovery_has(pgwt_discovery_facts f, uint64_t required)
{ return (f.known & f.truth & required) == required; }

#endif
