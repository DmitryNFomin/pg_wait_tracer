/* backend_status_layout.h -- PgBackendStatus discovery and validation. */
#ifndef PGWT_BACKEND_STATUS_LAYOUT_H
#define PGWT_BACKEND_STATUS_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

enum pgwt_pgbs_arch {
    PGWT_PGBS_ARCH_UNKNOWN = 0,
    PGWT_PGBS_ARCH_X86_64,
    PGWT_PGBS_ARCH_ARM64,
};

enum pgwt_pgbs_abi {
    PGWT_PGBS_ABI_UNKNOWN = 0,
    PGWT_PGBS_ABI_SYSV_LP64,
    PGWT_PGBS_ABI_AAPCS64_LP64,
};

enum pgwt_pgbs_source {
    PGWT_PGBS_SOURCE_NONE = 0,
    PGWT_PGBS_SOURCE_DWARF,
    PGWT_PGBS_SOURCE_HARD_TABLE,
};

enum pgwt_pgbs_validation {
    PGWT_PGBS_VALIDATION_NOT_RUN = 0,
    PGWT_PGBS_VALIDATION_VALIDATED,
    PGWT_PGBS_VALIDATION_DEGRADED,
    PGWT_PGBS_VALIDATION_REJECTED,
};

enum pgwt_pgbs_field_validation {
    PGWT_PGBS_FIELD_NOT_RUN = 0,
    PGWT_PGBS_FIELD_VALIDATED,
    PGWT_PGBS_FIELD_ABSENT,
    PGWT_PGBS_FIELD_INVALID,
};

struct pgwt_pgbs_field {
    uint32_t offset;
    uint8_t  width;
    bool     present;
    enum pgwt_pgbs_field_validation validation;
};

enum pgwt_pgbs_fallback {
    PGWT_PGBS_FALLBACK_STATE        = 1u << 0,
    PGWT_PGBS_FALLBACK_ACTIVITY_RAW = 1u << 1,
    PGWT_PGBS_FALLBACK_QUERY_ID     = 1u << 2,
};

struct PgBackendStatusLayout {
    int major;
    enum pgwt_pgbs_arch arch;
    enum pgwt_pgbs_abi abi;
    uint8_t pointer_width;
    uint32_t struct_size;
    uint32_t activity_buffer_size; /* validated track_activity_query_size */
    uint64_t status_anchor; /* validated row in the shared status array */

    struct pgwt_pgbs_field st_changecount;
    struct pgwt_pgbs_field st_procpid;
    struct pgwt_pgbs_field st_databaseid;
    struct pgwt_pgbs_field st_userid;
    struct pgwt_pgbs_field st_state;
    struct pgwt_pgbs_field st_activity_raw;
    struct pgwt_pgbs_field st_query_id;
    bool st_query_id_signed;

    int state_running;
    int state_fastpath;
    int state_max;

    enum pgwt_pgbs_source source;
    enum pgwt_pgbs_validation validation;
    uint32_t fallback_mask;
    pid_t validated_pid;
    char detail[192];
};

/* A coherent copy of the fields used by the runtime validator.  read_mask
 * uses the internal field order exposed by PGWT_PGBS_READ_* below so tests
 * can inject short reads without touching another process. */
enum pgwt_pgbs_read_mask {
    PGWT_PGBS_READ_CHANGECOUNT = 1u << 0,
    PGWT_PGBS_READ_PROCPID     = 1u << 1,
    PGWT_PGBS_READ_DATABASEID  = 1u << 2,
    PGWT_PGBS_READ_USERID      = 1u << 3,
    PGWT_PGBS_READ_STATE       = 1u << 4,
    PGWT_PGBS_READ_ACTIVITY    = 1u << 5,
    PGWT_PGBS_READ_QUERY_ID    = 1u << 6,
};

struct pgwt_pgbs_snapshot {
    uint32_t changecount_before;
    uint32_t changecount_after;
    uint32_t procpid;
    uint32_t databaseid;
    uint32_t userid;
    uint32_t state;
    uint64_t activity_raw;
    uint64_t query_id;
    uint32_t read_mask;
    bool activity_readable;
    bool activity_truncated;
    bool activity_marker_matched;
};

#define PGWT_PGBS_ACTIVITY_MAX 4096

/* Sampled-tier fields derived from one coherent PgBackendStatus snapshot.
 * PG14+ fills query_id directly. PG13 instead fills activity so the sampler
 * can normalize/hash it into a versioned synthetic grouping key. */
struct pgwt_pgbs_sampled_attr {
    uint64_t query_id;
    uint32_t databaseid;
    uint32_t userid;
    uint32_t state;
    bool cmd_open;
    bool activity_truncated;
    char activity[PGWT_PGBS_ACTIVITY_MAX];
};

struct pgwt_pgbs_expected {
    pid_t pid;
    uint32_t databaseid; /* 0 means plausibility check only */
    uint32_t userid;     /* 0 means plausibility check only */
    bool require_running;
    bool state_shadow_available;
    bool state_active;
    bool activity_marker_required;
    bool query_id_available;
    uint64_t query_id;
};

/* Pure aggregation state for the auth-free bounded warmup.  A field needs
 * three independent positive matches; state matches are active-only, and a
 * PG14+ query-id proof additionally needs two distinct nonzero IDs. */
struct pgwt_pgbs_warmup_evidence {
    unsigned observations;
    unsigned state_matches;
    unsigned activity_matches;
    unsigned query_matches;
    bool state_seen;
    bool state_varied;
    uint32_t first_state;
    bool query_id_seen;
    bool query_id_varied;
    uint64_t first_query_id;
};

/* Validation clients run before capture enrollment starts.  Keep their
 * PostgreSQL server-process identities out of both the startup scan and the
 * later fork/init paths.  start_time is field 22 of /proc/<pid>/stat, so a
 * recycled numeric PID does not exclude an unrelated future backend. */
#define PGWT_PGBS_MAX_EXCLUDED_PIDS 256

struct pgwt_pgbs_excluded_pid {
    pid_t pid;
    uint64_t start_time;
};

struct pgwt_pgbs_exclusion_set {
    struct pgwt_pgbs_excluded_pid entries[PGWT_PGBS_MAX_EXCLUDED_PIDS];
    unsigned count;
};

enum pgwt_pgbs_arch pgwt_pgbs_arch_from_name(const char *name);
enum pgwt_pgbs_abi pgwt_pgbs_abi_for_arch(enum pgwt_pgbs_arch arch,
                                           unsigned pointer_width);
const char *pgwt_pgbs_arch_name(enum pgwt_pgbs_arch arch);
const char *pgwt_pgbs_abi_name(enum pgwt_pgbs_abi abi);
const char *pgwt_pgbs_source_name(enum pgwt_pgbs_source source);
const char *pgwt_pgbs_validation_name(enum pgwt_pgbs_validation validation);

/* Parse readelf --debug-dump=info --wide text.  Exposed for deterministic
 * fixture tests; production reaches it through pgwt_pgbs_discover(). */
int pgwt_pgbs_parse_dwarf(FILE *fp, int pg_major, unsigned pointer_width,
                          struct PgBackendStatusLayout *out,
                          char *why, size_t why_sz);

/* Exact hard-table lookup.  No architecture, pointer-width or ABI default is
 * applied: an unknown key returns -1 and leaves out zeroed. */
int pgwt_pgbs_hard_table_lookup(int pg_major, enum pgwt_pgbs_arch arch,
                                unsigned pointer_width,
                                enum pgwt_pgbs_abi abi,
                                struct PgBackendStatusLayout *out);

/* DWARF first, exact hard table second. */
int pgwt_pgbs_discover(const char *pg_binary, int pg_major,
                       struct PgBackendStatusLayout *out);

/* Pure validation core and live controlled-backend wrapper. */
int pgwt_pgbs_validate_snapshot(struct PgBackendStatusLayout *layout,
                                const struct pgwt_pgbs_snapshot *snapshot,
                                const struct pgwt_pgbs_expected *expected);

/* Stage 2/4 sampled attribution. PG14+ uses st_query_id; validated PG13 uses
 * st_activity_raw. Both paths include db/user context and fail closed on an
 * incoherent/short/identity-mismatched read. */
bool pgwt_pgbs_sampled_attr_enabled(
    const struct PgBackendStatusLayout *layout);
bool pgwt_pgbs_sampled_query_id_enabled(
    const struct PgBackendStatusLayout *layout);
bool pgwt_pgbs_sampled_activity_enabled(
    const struct PgBackendStatusLayout *layout);
int pgwt_pgbs_derive_sampled_attr(
    const struct PgBackendStatusLayout *layout,
    const struct pgwt_pgbs_snapshot *snapshot, pid_t expected_pid,
    struct pgwt_pgbs_sampled_attr *out);
int pgwt_pgbs_derive_sampled_activity(
    const struct PgBackendStatusLayout *layout,
    const struct pgwt_pgbs_snapshot *snapshot, pid_t expected_pid,
    const char *activity, bool activity_truncated,
    struct pgwt_pgbs_sampled_attr *out);
int pgwt_pgbs_read_sampled_attr(
    pid_t backend_pid, uint64_t my_be_entry_addr,
    const struct PgBackendStatusLayout *layout,
    struct pgwt_pgbs_sampled_attr *out);
int pgwt_pgbs_read_sampled_attr_at(
    pid_t backend_pid, uint64_t backend_status_addr,
    const struct PgBackendStatusLayout *layout,
    struct pgwt_pgbs_sampled_attr *out);
uint64_t pgwt_pgbs_resolve_entry(
    pid_t backend_pid, const struct PgBackendStatusLayout *layout);

void pgwt_pgbs_validate_runtime(struct PgBackendStatusLayout *layout,
                                pid_t postmaster_pid,
                                uint64_t my_be_entry_addr,
                                const char *pg_binary,
                                struct pgwt_pgbs_exclusion_set *exclusions);
int pgwt_pgbs_pid_start_time(pid_t pid, uint64_t *start_time);
bool pgwt_pgbs_pid_is_original(pid_t pid, uint64_t start_time);
int pgwt_pgbs_exclusion_add(struct pgwt_pgbs_exclusion_set *set, pid_t pid);
/* Add a short-lived helper only while it is still live. If /proc identity
 * capture loses the exit race, already_retired is true and no entry is kept. */
int pgwt_pgbs_exclusion_add_live(struct pgwt_pgbs_exclusion_set *set,
                                 pid_t pid, uint64_t *start_time,
                                 bool *already_retired);
bool pgwt_pgbs_exclusion_contains(const struct pgwt_pgbs_exclusion_set *set,
                                  pid_t pid);
/* Auth-free fallback used only when the controlled backend could not be
 * created.  It compares a coherent memory snapshot with the already-running
 * activity/query uprobes; callers repeat it during a bounded warmup. */
int pgwt_pgbs_validate_uprobe_shadow(struct PgBackendStatusLayout *layout,
                                     pid_t backend_pid,
                                     uint64_t my_be_entry_addr,
                                     uint64_t debug_query_string_addr,
                                     bool cmd_open,
                                     bool query_id_available,
                                     uint64_t query_id,
                                     uint32_t *observed_state);

void pgwt_pgbs_warmup_note(struct pgwt_pgbs_warmup_evidence *evidence,
                           const struct PgBackendStatusLayout *candidate,
                           bool state_active,
                           bool state_value_available,
                           uint32_t state_value,
                           bool query_id_available,
                           uint64_t query_id);
bool pgwt_pgbs_warmup_complete(
    const struct pgwt_pgbs_warmup_evidence *evidence, int pg_major);
void pgwt_pgbs_warmup_apply(struct PgBackendStatusLayout *layout,
                            const struct pgwt_pgbs_warmup_evidence *evidence);

unsigned pgwt_pgbs_fallback_count(const struct PgBackendStatusLayout *layout);
void pgwt_pgbs_log(const struct PgBackendStatusLayout *layout);

#endif /* PGWT_BACKEND_STATUS_LAYOUT_H */
