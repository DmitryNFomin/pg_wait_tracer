/* wait_event.h — Wait event decode: ID → human-readable name */
#ifndef PGWT_WAIT_EVENT_H
#define PGWT_WAIT_EVENT_H

#include <stdint.h>
#include <stddef.h>

/* Initialize event name tables for the given PG major version.
 * Must be called before any event name lookup functions.
 * Falls back to PG18 tables for unknown versions. */
void pgwt_init_event_names(int pg_major);

/* Load dynamic event names from a running PostgreSQL instance.
 * Runs: psql -U <user> -p <port> -d postgres -tAF'|' -c "SELECT ..."
 * On PG17+, queries pg_wait_events for all event names.
 * Returns 0 on success, -1 on failure (falls back to hardcoded tables). */
int pgwt_load_event_names_from_pg(const char *pg_bindir, int pg_port,
                                  const char *pg_user);

/* Load dynamic event names from an in-memory buffer of "Type|Name\n"
 * lines (the pg_wait_events query output format). Same id-mapping logic
 * as pgwt_load_event_names_from_pg, but without a live PG — used by unit
 * tests. Returns 0 on success, -1 on failure. */
int pgwt_load_event_names_from_buffer(const char *data);

/* Write current event name mapping to a JSON sidecar file.
 * Path: <trace_dir>/wait_event_names.json
 * Returns 0 on success, -1 on failure. */
int pgwt_write_names_json(const char *trace_dir);

/* Load event name mapping from a JSON sidecar file.
 * Path: <trace_dir>/wait_event_names.json
 * Returns 0 on success (overrides hardcoded tables), -1 if not found. */
int pgwt_load_names_json(const char *trace_dir);

/* Establish the event-name tables AND the Timeout pacing mask for a TRACE
 * DIRECTORY, in the only order that is safe:
 *
 *   1. the sidecar (wait_event_names.json) -- authoritative, because it carries
 *      both the major the trace was written with and the provenance of its
 *      names (observed from PostgreSQL vs this build's static fallback);
 *   2. failing that, `header_major` -- the PostgreSQL major recorded in the
 *      trace file header, which every trace this build writes carries. The
 *      dynamic names are lost but the version-selected static tables are
 *      right, so the classification survives;
 *   3. failing that, the UNKNOWN-major state: PG18 tables for rendering and an
 *      EMPTY pacing mask, so every Timeout event stays in DB Time.
 *
 * Pass header_major = 0 when no header is available. Returns the major it
 * settled on, or 0 for the unknown state.
 *
 * This exists as a function rather than a sequence inlined in pgwt-server
 * because step 2 was missing there: a missing or truncated sidecar left the
 * PG18 default in force and classified a PG13/PG16 trace with PG18's Timeout
 * ids. Having one implementation is what lets tests/test_wait_event.c drive
 * all three branches instead of re-deriving the order. */
int pgwt_init_event_names_for_trace(const char *trace_dir, int header_major);

/* Returns class name: "IO", "LWLock", "Lock", "CPU", etc. */
const char *pgwt_class_name(uint32_t wait_event_info);

/* Returns event name within class: "DataFileRead", "WALInsert", etc.
 * For event=0 (CPU), returns "CPU". */
const char *pgwt_event_name(uint32_t wait_event_info);

/* Writes "class:event" (e.g. "IO:DataFileRead") to buf.
 * For event=0, writes "CPU*" (asterisk: not all CPU time is instrumented). */
void pgwt_event_full_name(uint32_t wait_event_info, char *buf, size_t bufsz);

/* LOAD vs VISIBILITY: pgwt_is_idle_event(), pgwt_is_hidden_event() and the
 * narrower pgwt_is_session_idle_event() are declared in idle_rule.h, which
 * also documents the Timeout pacing set and why the predicate is derived from
 * resolved NAMES rather than version-dependent ids. Included here so the ~45
 * existing call sites that only include wait_event.h keep compiling. */
#include "idle_rule.h"

#endif /* PGWT_WAIT_EVENT_H */
