/* provider_coop.h — Cooperative capture provider (Track A, Phase A6)
 *
 * ───────────────────────────────────────────────────────────────────────────
 *  INTERFACE FREEZE ONLY. The cooperative tier is NOT implemented here.
 * ───────────────────────────────────────────────────────────────────────────
 *
 * The cooperative ("coop") tier captures wait events with help from
 * PostgreSQL itself — a patched PG or a loadable extension that hooks the
 * wait-event start/stop path and reports transitions to the daemon, instead
 * of the daemon observing PGPROC memory from the outside (watchpoints in the
 * `full` tier, process_vm_readv sampling in the `sampled` tier).
 *
 * That implementation lives in a SEPARATE extension track, not in this repo.
 * Phase A6's only deliverable is to FREEZE the provider contract so the
 * extension can drop in later with zero churn in the daemon: this file plus
 * provider_coop.c define the exact struct the extension must populate, and
 * provider selection already recognizes `--mode coop`. Until the extension
 * exists, the stub's start() cleanly reports "not available in this build"
 * and refuses activation (no crash, clear message) — see provider_coop.c.
 *
 * ── THE FROZEN CONTRACT (what the extension track must satisfy) ────────────
 *
 * The cooperative provider is one more implementation of the single
 * `struct pgwt_capture_provider` vtable (src/provider.h). It MUST:
 *
 *  1. SAME SCHEMA. Emit the SAME trace records as the other tiers — it does
 *     NOT invent a new on-disk format. A wait transition becomes a v2
 *     TRANSITIONS record (old_event → new_event with a real duration_ns),
 *     byte-identical to what the `full` tier produces; the reader, replay,
 *     pgwt-server and every compute view consume it unchanged. (If a future
 *     coop source can only deliver point observations rather than intervals,
 *     it must emit SAMPLES records and advertise SAMPLED instead — but the
 *     intended cooperative source reports start/stop, i.e. intervals, so the
 *     contract here is EXACT/TRANSITIONS.)
 *
 *  2. EXACT FIDELITY. Advertise `PGWT_FIDELITY_EXACT` — the extension reports
 *     every transition, so coop windows satisfy the EXACT-required views
 *     (histogram, transitions, lock chains, interference) exactly like the
 *     `full` tier. compute.c keys view availability off this field alone; no
 *     view needs to know whether EXACT data came from watchpoints or from
 *     the extension.
 *
 *  3. THE WRITER PATH. Hand every event to the trace writer through the SAME
 *     entry point the other tiers use — there is no second writer. Concretely,
 *     poll() drains whatever the extension delivered (e.g. a shared-memory
 *     ring the extension fills, or a ringbuf/socket the daemon reads) and, for
 *     each transition, calls the daemon's per-event handler exactly as the
 *     full tier's ringbuf callback does (see src/event_stream.c
 *     `handle_event` → src/event_writer.c `pgwt_writer_*`). Backend identity
 *     (pid → query_id, session metadata) comes from the SHARED registry
 *     (src/backend.c), which the daemon keeps populated via the fork/exit
 *     tracepoints and the active tier's attribution source — the coop provider
 *     does NOT maintain its own backend table.
 *
 *  4. THE VTABLE. Implement all four entry points (src/provider.h):
 *       - start(d):  arm cooperative capture (open the shm ring / register
 *                    with the extension / verify the patched PG is present).
 *                    Return 0 on success; -1 if the cooperative source is
 *                    absent — the daemon prints a clear message and aborts
 *                    startup, exactly as it would for any provider whose
 *                    capture mechanism is unavailable. (This stub always
 *                    takes the -1 path.)
 *       - stop(d):   disarm (unregister, close the ring). Idempotent.
 *       - poll(d):   drain delivered events into the writer (item 3). Called
 *                    every daemon event-loop tick.
 *       - self_metrics(d, m): fill tier-specific counters (e.g. transitions
 *                    delivered, ring overruns) into struct pgwt_metrics for
 *                    the control socket.
 *
 *  5. SHARED LIFECYCLE. Like `full` and `sampled`, the coop provider does NOT
 *     own backend discovery. The daemon's registry/lifecycle machinery is
 *     mode-independent; the provider only arms/drains its own capture path.
 *
 * Wiring already in place for the extension to slot into:
 *   - enum pgwt_mode gains PGWT_MODE_COOP (src/provider.h).
 *   - --mode coop parses to PGWT_MODE_COOP (src/pg_wait_tracer.c).
 *   - pgwt_daemon_init() selects pgwt_provider_coop for that mode
 *     (src/daemon.c). Today start() returns -1 → clean "not available".
 * The extension track replaces the stub bodies in provider_coop.c with the
 * real implementation; nothing else in the daemon needs to change.
 */
#ifndef PGWT_PROVIDER_COOP_H
#define PGWT_PROVIDER_COOP_H

#include "provider.h"

/* The cooperative provider vtable (A6 stub; extension track fills it in). */
extern const struct pgwt_capture_provider pgwt_provider_coop;

#endif /* PGWT_PROVIDER_COOP_H */
