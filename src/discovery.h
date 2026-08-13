/* discovery.h — Postmaster PID, ELF symbol offset, /proc helpers */
#ifndef PGWT_DISCOVERY_H
#define PGWT_DISCOVERY_H

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

/* Find postmaster PID from pgdata/postmaster.pid. Returns 0 on error. */
pid_t pgwt_find_postmaster_pid(const char *pgdata);

/* Resolve absolute path to postgres binary via /proc/<pid>/exe. */
int pgwt_find_pg_binary(pid_t pid, char *buf, size_t bufsz);

/* Find symbol value in ELF binary (using libelf).
 * Returns the raw st_value from the ELF symbol table. */
uint64_t pgwt_find_symbol_offset(const char *binary, const char *symbol);

/* Find the load base address of the binary in the target process.
 * `binary_path` is matched EXACTLY against the maps pathname field: full
 * pathname equality when it contains a '/', exact-basename equality
 * otherwise — never a substring match (CAP-4: extension .so paths that
 * contain "postgres" must not win). */
uint64_t pgwt_find_load_base(pid_t pid, const char *binary_path);

/* Same, but against an explicit maps file (the /proc/<pid>/maps format).
 * pgwt_find_load_base() is a thin wrapper over this; the split exists so
 * unit tests can drive the parser with committed fixture files
 * (tests/test_discovery.c — the #24 load-base regression class). */
uint64_t pgwt_find_load_base_in_maps(const char *maps_path,
                                     const char *binary_path);

/* Resolve a symbol to its runtime virtual address in the target process.
 * Handles both PIE (ET_DYN) and non-PIE (ET_EXEC) binaries correctly.
 * The load base is matched against the FULL `binary` path (CAP-4). */
uint64_t pgwt_resolve_symbol(const char *binary, const char *symbol,
                             pid_t pid);

/* Translate an ELF virtual address (a symbol's st_value) to the FILE offset
 * a uprobe attach needs, via the PT_LOAD program header containing it:
 * offset = vaddr - p_vaddr + p_offset. Correct for ET_EXEC and ET_DYN alike.
 * Replaces the non-PIE `va - 0x400000` heuristic that silently attached
 * uprobes to dead bytes on PIE builds (T2 study defect 1 / PR #24 class).
 * Returns 0 if no PT_LOAD's file-backed range contains the vaddr. */
uint64_t pgwt_vaddr_to_file_offset(const char *binary, uint64_t vaddr);

/* Is addr inside a MAP_SHARED mapping of pid? 1 = shared, 0 = private or
 * not mapped, -1 = maps unreadable. The sampler may only BATCH-read
 * addresses that are shared (SMP-2): a process-local address mapped at the
 * same VA in every forked child reads *successfully* through another pid
 * and returns the reader's value, silently misattributed. */
int pgwt_addr_is_shared(pid_t pid, uint64_t addr);

/* Read an 8-byte pointer from /proc/<pid>/mem at addr. Returns 0 on error. */
uint64_t pgwt_read_pointer(pid_t pid, uint64_t addr);

/* T8: read a task's exact on-CPU accumulator (nanoseconds) — field 1 of
 * /proc/<pid>/schedstat, the same task_struct->se.sum_exec_runtime the BPF
 * watchpoint reads at wait boundaries. The userspace seed/flush/live paths use
 * it to establish the per-interval CPU base and to close open CPU stretches.
 * Returns 0 on any failure (proc gone, or CONFIG_SCHED_INFO absent so the file
 * does not exist) — callers treat 0 as "no measurement available". */
uint64_t pgwt_read_sched_cpu_ns(pid_t pid);

/* Detect PostgreSQL major version from the postgres binary.
 * Runs "<binary> --version" and parses "postgres (PostgreSQL) XX.Y".
 * Returns major version (e.g. 14, 15, 16, 17, 18) or 0 on error. */
int pgwt_detect_pg_version(const char *pg_binary);

/* Detect st_query_id offset in PgBackendStatus.
 * Tries: 1) DWARF debug info, 2) known offset table.
 * Returns offset in bytes, or 0 if unavailable. */
int pgwt_detect_query_id_offset(const char *pg_binary, int pg_major);

/* PG13 query-attribution offsets (Route B1 via pg_stat_statements).
 * PG13 has no in-core query_id; when pg_stat_statements is loaded its
 * post_parse_analyze hook populates the core PlannedStmt.queryId field, which
 * matches pg_stat_statements.queryid. We uprobe ExecutorStart(QueryDesc*) and
 * walk QueryDesc->plannedstmt->queryId; QueryDesc->sourceText gives the text.
 * Offsets are header-derived (postgresql13-devel, x86_64) since PG13 is EOL
 * and has no debuginfo anywhere. All fields 0 => not available. */
struct pgwt_pg13_query_offsets {
    int querydesc_plannedstmt;  /* offsetof(QueryDesc, plannedstmt) */
    int plannedstmt_queryid;    /* offsetof(PlannedStmt, queryId)   */
    int querydesc_sourcetext;   /* offsetof(QueryDesc, sourceText)  */
};

/* Fill header-derived PG13 query-attribution offsets for the given major
 * version on x86_64. Returns 1 if a known layout was filled, 0 otherwise
 * (caller leaves query attribution disabled). */
int pgwt_detect_pg13_query_offsets(int pg_major,
                                   struct pgwt_pg13_query_offsets *out);

/* Plan-tree capture offsets (PlannedStmt -> Plan tree walking).
 * Plan.plan_node_id @ 40, lefttree @ 64, righttree @ 72, sizeof(Plan) = 104.
 * PlannedStmt.planTree: 32 on PG13-17, 40 on PG18 (due to planId field). */
struct pgwt_plan_tree_offsets {
    int plan_type;              /* offsetof(Node, type) = 0 */
    int plan_plannodeid;        /* offsetof(Plan, plan_node_id) = 40 */
    int plan_lefttree;          /* offsetof(Plan, lefttree) = 64 */
    int plan_righttree;         /* offsetof(Plan, righttree) = 72 */
    int plan_sizeof;            /* sizeof(Plan) = 104 */
    int plannedstmt_plantree;   /* PG13-17: 32, PG18: 40 */
    int gather_num_workers;     /* offsetof(Gather, num_workers) = 104 */
};

int pgwt_detect_plan_tree_offsets(int pg_major,
                                  struct pgwt_plan_tree_offsets *out);

/* Detect whether pg_stat_statements is loaded into the running postmaster.
 * Scans /proc/<postmaster_pid>/maps for pg_stat_statements.so — no DB
 * connection or auth needed, works regardless of port. Returns 1 if loaded,
 * 0 if not, -1 if the maps file could not be read. */
int pgwt_detect_pgss_loaded(pid_t postmaster_pid);

/* offsetof(PGPROC, wait_event_info) for a given PG major version on x86_64.
 * Used on PG<17, where the daemon resolves the MyProc (PGPROC*) global and
 * adds this offset to reach each backend's wait_event_info. Header-derived
 * (postgresql<major>-devel), since PG13 has no debuginfo anywhere. Returns 0
 * if the version is unknown (caller refuses to attach). */
int pgwt_detect_pgproc_wait_offset(int pg_major);

/* Resolve a backend's wait_event_info address from the MyProc PGPROC* global.
 * Reads *my_proc_global_addr from /proc/<pid>/mem to get the backend's PGPROC,
 * then adds pgproc_wait_offset. Returns 0 if MyProc is not yet set (backend
 * still in early init) or on read error. */
uint64_t pgwt_resolve_wait_addr_via_myproc(pid_t pid, uint64_t my_proc_global_addr,
                                           int pgproc_wait_offset);

/* Validate a resolved wait_event_info address by classifying the value it
 * currently holds (CAP-2/3/5). Returns (see pgwt_classify_wei):
 *   PGWT_WEI_VALID_NONZERO (1) — a known wait class: PROOF the address is
 *                                right (callers may set validated).
 *   PGWT_WEI_ZERO (2)          — on-CPU: consistent with a correct address
 *                                but ALSO the most likely reading from a
 *                                WRONG offset (zeroed memory). NEVER proof —
 *                                keep sampling other backends / re-check.
 *   PGWT_WEI_GARBAGE (0)       — unknown class byte: the offset/address is
 *                                wrong — the caller must refuse to attach.
 *   -1                         — transient read error (process gone). */
int pgwt_validate_wait_addr(pid_t pid, uint64_t wait_addr);

/* Detect st_activity_raw offset in PgBackendStatus.
 * Tries: 1) DWARF debug info, 2) known offset table.
 * Returns offset in bytes, or 0 if unavailable.
 * Note: st_activity_raw is a char* pointer (PG18+), not an inline array. */
int pgwt_detect_activity_offset(const char *pg_binary, int pg_major);

/* Auto-discover a running PostgreSQL postmaster.
 * Scans /proc for postgres processes, filters children.
 * Returns PID if exactly one postmaster found, 0 otherwise.
 * On multiple instances, prints a list to stderr. */
pid_t pgwt_auto_discover_postmaster(bool verbose);

/* Forward declaration */
struct pgwt_daemon;

/* Full discovery: resolve postmaster PID, binary, version, symbols.
 * Reads d->pgdata (empty string = auto-discover).
 * Fills d->postmaster_pid, my_wait_ptr_addr, my_be_entry_addr,
 * pg_major_version, st_query_id_offset.
 * Returns 0 on success, -1 on error. */
int pgwt_discover(struct pgwt_daemon *d);

/* Infer PGDATA from postmaster PID via /proc/<pid>/cwd.
 * Returns 0 on success, -1 on error. */
int pgwt_infer_pgdata(pid_t pid, char *buf, size_t bufsz);

#endif /* PGWT_DISCOVERY_H */
