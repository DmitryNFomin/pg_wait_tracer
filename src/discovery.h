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

/* ── USDT probe-site resolution (#464) ───────────────────────────────────
 *
 * PostgreSQL emits the query__execute__start / query__execute__done USDT
 * probes from TWO code sites for plannable non-SELECT work: once around
 * PortalRun, and again inside the static helper PortalRunMulti calls. A
 * SELECT never reaches the second site, which is why DML doubled (Execs 368
 * / Plans 184) and SELECTs did not (91 / 91). libbpf's
 * bpf_program__attach_usdt() attaches by probe NAME, so it instruments both
 * sites and the BPF side cannot tell them apart.
 *
 * The fix admits ONE canonical execution boundary — the pair inside the
 * PortalRun function — and drops the inner pair before any marker is
 * emitted or any phase state is touched. These helpers resolve the sites
 * FROM THE RUNNING BINARY; measured on gate-1 2026-10-19 against the
 * PG13/16/17/18 Ubuntu packages (tests/results/issue464/probe-sites.txt):
 *
 *   PG18  query__execute__start @ 0x5a6bc0  (no containing FUNC symbol)
 *         query__execute__done  @ 0x5a6c2f  (no containing FUNC symbol)
 *         query__execute__start @ 0x5a6eb8  inside PortalRun [0x5a6e70+1167)
 *         query__execute__done  @ 0x5a711e  inside PortalRun
 *
 * Note the empirical surprise the brief warned about: the inner pair lives
 * in a STATIC function, and the shipped packages carry ZERO local FUNC
 * symbols, so the excluded site matches no symbol at all. The selector
 * therefore keys on "inside the canonical function's range", never on the
 * excluded site having a resolvable name.
 */
#define PGWT_MAX_USDT_SITES 32

/* The canonical execution boundary. PortalRun is a GLOBAL symbol in every
 * packaged build checked (PG13-18), including fully stripped ones. */
#define PGWT_EXEC_CANONICAL_FN "PortalRun"

struct pgwt_usdt_site {
    uint64_t elf_addr;      /* probe location, link-time vaddr, base-corrected */
    uint64_t runtime_addr;  /* elf_addr - elf_base + load_base (0 = unresolved) */
    int      marker;        /* caller's tag: which marker this site emits */
    int      canonical;     /* 1 = inside the canonical function range */
};

/* Why a resolution was refused. Refusal means "attach NOTHING and say so";
 * it never degrades into admitting every site. */
enum pgwt_exec_site_refusal {
    PGWT_EXEC_SITES_OK = 0,
    PGWT_EXEC_SITES_NO_BINARY,       /* binary missing/unreadable/not ELF */
    PGWT_EXEC_SITES_NO_NOTES,        /* no matching stapsdt notes at all */
    PGWT_EXEC_SITES_NO_FUNCTION,     /* canonical FUNC symbol unresolvable */
    PGWT_EXEC_SITES_NO_CANONICAL,    /* notes exist, none inside the function */
    PGWT_EXEC_SITES_AMBIGUOUS,       /* != 1 canonical site for a marker */
    PGWT_EXEC_SITES_NO_LOAD_BASE,    /* runtime load base unresolvable */
    PGWT_EXEC_SITES_OVERFLOW,        /* more notes than PGWT_MAX_USDT_SITES */
};

const char *pgwt_exec_site_refusal_str(enum pgwt_exec_site_refusal r);

struct pgwt_exec_site_plan {
    struct pgwt_usdt_site sites[PGWT_MAX_USDT_SITES];
    int n_sites;
    uint64_t fn_lo, fn_hi;      /* canonical function [lo, hi), link-time */
    int n_canonical;            /* admitted sites */
    int n_excluded;             /* sites seen and deliberately dropped */
    int n_start_canonical;
    int n_done_canonical;
    enum pgwt_exec_site_refusal refusal;
};

/* Marker tags for pgwt_usdt_site.marker. Deliberately NOT the BPF marker
 * constants (those live in pg_wait_tracer.h): the selector is pure and must
 * stay buildable in a unit test that links no BPF headers. */
#define PGWT_SITE_EXEC_START 1
#define PGWT_SITE_EXEC_DONE  2

/* Enumerate `.note.stapsdt` entries matching provider/name.
 *
 * Appends to plan->sites (tagging each with `marker`), applying the standard
 * stapsdt base correction: addr = loc + (sh_addr(".stapsdt.base") - note_base)
 * when that section exists, and loc unchanged when it does not. Malformed
 * notes (bad note name, descsz too small for three addresses, unterminated
 * strings) are skipped, never guessed at.
 *
 * Returns the number of sites appended, or -1 on an unreadable/non-ELF file
 * or on overflow (plan->refusal = PGWT_EXEC_SITES_OVERFLOW). A return of 0
 * is NOT an error here — pgwt_exec_sites_select() turns it into
 * PGWT_EXEC_SITES_NO_NOTES. */
int pgwt_elf_usdt_sites(const char *binary, const char *provider,
                        const char *name, int marker,
                        struct pgwt_exec_site_plan *plan);

/* Resolve a FUNC symbol's link-time range [lo, hi). Requires STT_FUNC with
 * a NON-ZERO st_size: a sizeless symbol cannot bound anything, and treating
 * it as [v, v) would silently admit no site (or, with a sloppy comparison,
 * every site). Returns 0 on success, -1 if unresolvable. */
int pgwt_elf_func_range(const char *binary, const char *symbol,
                        uint64_t *lo, uint64_t *hi);

/* PURE: classify plan->sites against plan->fn_lo/fn_hi and decide the
 * verdict. No I/O, no globals — this is the function tests/test_exec_sites.c
 * drives directly. Sets canonical flags, the four counters and
 * plan->refusal; returns 0 when admitted, -1 when refused. */
int pgwt_exec_sites_select(struct pgwt_exec_site_plan *plan);

/* PURE: link-time vaddr -> runtime vaddr, the same arithmetic
 * pgwt_resolve_symbol() uses (PIE: elf_base 0; non-PIE: elf_base == load_base).
 * Returns 0 (never a wild address) when load_base is 0 or the subtraction
 * would underflow. */
uint64_t pgwt_site_runtime_addr(uint64_t elf_addr, uint64_t elf_base,
                                uint64_t load_base);

/* Full resolution for the exec probes: enumerate both probe names, resolve
 * the canonical function, select, and translate to runtime addresses for
 * `pid`. Returns 0 on success (plan->refusal == PGWT_EXEC_SITES_OK), -1 on
 * refusal with plan->refusal set. On refusal the caller MUST NOT attach the
 * execution probes at all. */
int pgwt_resolve_exec_sites(const char *binary, pid_t pid,
                            struct pgwt_exec_site_plan *plan);

/* Expose the ELF base vaddr (lowest PT_LOAD p_vaddr) for callers that need
 * to redo the translation themselves; 0 for PIE. */
uint64_t pgwt_elf_base_vaddr(const char *binary);

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
