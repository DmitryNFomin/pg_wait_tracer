/* trace_closure.h — how a trace file ENDED, read out of the file header.
 *
 * The problem (#424, deliverable 1): a capture whose daemon was killed loses
 * every interval that was open at that moment — no closing record was ever
 * written — and until now a reader could not even tell that had happened. A
 * footer is not evidence: rotation writes one, crash recovery writes one, and
 * a clean close writes one, so "has a footer" certifies nothing about whether
 * the open intervals were flushed.
 *
 * The fix needs no layout change and no version bump: the file header already
 * carries a 16-bit `flags` field of which exactly one bit (LZ4) was used. The
 * writer sets a closure bit there, and ONLY after every pending buffer flushed
 * without error, the footer was written without error, and fsync() returned 0.
 * A failure anywhere in that chain leaves the bit clear, which reads back as
 * "not finalized" — never as clean.
 *
 * Two rules this file exists to enforce:
 *
 *   1. A file written by a writer that did not track closure (every file
 *      produced before this change) reads as UNKNOWN. Not clean. Absence of
 *      evidence is never recorded as a verified match. That is why the
 *      TRACKED capability bit exists and why it is checked FIRST: without it
 *      no other bit in the field is trusted, because no writer we know of set
 *      them, so their value is noise.
 *   2. Contradictory evidence resolves to UNCLEAN, never to clean.
 *
 * Header-only and pure so the reader, the server and the tests can all map
 * the same bits to the same verdict without linking the writer.
 */
#ifndef PGWT_TRACE_CLOSURE_H
#define PGWT_TRACE_CLOSURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Header flag bits (struct pgwt_trace_file_header.flags) ──
 *
 * 0x0001 is the pre-existing LZ4 payload-codec bit (PGWT_FLAG_LZ4 in
 * event_writer.h). The closure bits are spare bits of the same field. */

/* The writer that created this file records closure status. Written once,
 * with the header, at file creation. Without it the closure bits below mean
 * nothing — see rule 1 above. */
#define PGWT_FLAG_CLOSURE_TRACKED 0x0002

/* The file was finalized by a CHECKED, DURABLE close: all buffers flushed,
 * footer written, fsync() returned 0 — and only then was this bit written and
 * itself fsynced. Open intervals at the end of the capture were flushed by
 * the shutdown path. Set by pgwt_writer_close(); preserved through recovery. */
#define PGWT_FLAG_CLOSED_CLEAN    0x0004

/* The file was finalized by ROTATION: the capture did not end here, it
 * continues in the next file, so an interval open across the boundary is
 * closed there. Set by pgwt_writer_check_rotation() under the same checked +
 * durable conditions; preserved through recovery. */
#define PGWT_FLAG_ROTATED         0x0008

/* The file was finalized by CRASH RECOVERY on a later daemon start: the
 * previous daemon died without closing it. Intervals open at the end are
 * missing. Set only on files whose writer tracked closure (a legacy file
 * stays UNKNOWN — we cannot tell how a pre-tracking daemon ended, and
 * guessing "unclean" would be as made-up as guessing "clean"). */
#define PGWT_FLAG_RECOVERED       0x0010

/* Every closure bit this build understands. Bits outside the mask are left
 * for a future writer and are ignored, never treated as a defect. */
#define PGWT_FLAG_CLOSURE_MASK                                                \
    (PGWT_FLAG_CLOSURE_TRACKED | PGWT_FLAG_CLOSED_CLEAN |                     \
     PGWT_FLAG_ROTATED | PGWT_FLAG_RECOVERED)

enum pgwt_closure_status {
    /* Written by a pre-#424 writer. How it ended is not recorded anywhere;
     * it is NOT known to be clean. */
    PGWT_CLOSURE_UNKNOWN = 0,
    /* Tracked, but never finalized. For the live current.trace that is the
     * normal state of a running capture. For an ARCHIVED file it means the
     * finalization did not complete — treat it like UNCLEAN. */
    PGWT_CLOSURE_IN_PROGRESS,
    /* Finalized by rotation: the capture continues in the next file. */
    PGWT_CLOSURE_ROTATED,
    /* Finalized by a checked, durable close. */
    PGWT_CLOSURE_CLEAN,
    /* Finalized by crash recovery, or the bits contradict each other. */
    PGWT_CLOSURE_UNCLEAN,
};

static inline enum pgwt_closure_status pgwt_closure_from_flags(uint16_t flags)
{
    /* Rule 1: no capability bit → the other bits are not evidence. */
    if (!(flags & PGWT_FLAG_CLOSURE_TRACKED))
        return PGWT_CLOSURE_UNKNOWN;
    /* Rule 2 (a): recovery's verdict outranks any other bit. A file that was
     * closed clean and then recovered anyway is a file whose close did not
     * actually take effect. */
    if (flags & PGWT_FLAG_RECOVERED)
        return PGWT_CLOSURE_UNCLEAN;
    /* Rule 2 (b): "closed" and "rotated" are mutually exclusive outcomes. If
     * both appear, the field is not trustworthy — do not report clean. */
    if ((flags & PGWT_FLAG_CLOSED_CLEAN) && (flags & PGWT_FLAG_ROTATED))
        return PGWT_CLOSURE_UNCLEAN;
    if (flags & PGWT_FLAG_CLOSED_CLEAN)
        return PGWT_CLOSURE_CLEAN;
    if (flags & PGWT_FLAG_ROTATED)
        return PGWT_CLOSURE_ROTATED;
    return PGWT_CLOSURE_IN_PROGRESS;
}

/* "Intervals open at the end of this file may be missing from it."
 *
 * `is_live` means "a daemon is appending to this file RIGHT NOW, and this
 * caller knows it" — the daemon's own live path, or a reader that has
 * verified a running writer. It must NEVER be inferred from the file's name.
 * A capture copied off a server keeps the name `current.trace` and is not
 * live, and it is precisely the case we hit when a retained capture is pulled
 * to a laptop: a crashed daemon's file read without a restart has no
 * RECOVERED bit (that is stamped only by recover_current_trace), so it reads
 * IN_PROGRESS. With is_live=false it therefore warns exactly like UNCLEAN,
 * which is the honest answer — the capture did stop mid-interval, whoever
 * has not yet run recovery.
 *
 * For every non-live file, anything not positively certified CLEAN or ROTATED
 * answers true — including UNKNOWN. */
static inline bool
pgwt_closure_open_intervals_may_be_missing(enum pgwt_closure_status s,
                                           bool is_live)
{
    switch (s) {
    case PGWT_CLOSURE_CLEAN:
    case PGWT_CLOSURE_ROTATED:
        return false;
    case PGWT_CLOSURE_IN_PROGRESS:
    case PGWT_CLOSURE_UNKNOWN:
        /* Both mean "we cannot say how this ends". While a writer is
         * VERIFIABLY running, there is no end yet and nothing is missing; the
         * moment nobody is writing, both are the same hazard as UNCLEAN. A
         * legacy file is no exception: not tracking closure does not make a
         * running capture incomplete. */
        return !is_live;
    case PGWT_CLOSURE_UNCLEAN:
    default:
        return true;
    }
}

/* The sentence a reader must be shown, or NULL when there is nothing to warn
 * about. One function so the API and the UI cannot word it differently, and
 * so the two cases that mean "we do not know what we are missing" cannot
 * drift apart: an unclean capture and an unfinalized one say the SAME thing,
 * because for a file nobody is writing to they are the same hazard. */
static inline const char *
pgwt_closure_warning(enum pgwt_closure_status s, bool is_live)
{
    if (!pgwt_closure_open_intervals_may_be_missing(s, is_live))
        return NULL;
    if (s == PGWT_CLOSURE_UNKNOWN)
        return "Closure unknown; open intervals may be missing.";
    return "This capture ended uncleanly; intervals open at the end may be "
           "missing.";
}

/* Stable machine-readable token for the API/UI surface. */
static inline const char *pgwt_closure_name(enum pgwt_closure_status s)
{
    switch (s) {
    case PGWT_CLOSURE_UNKNOWN:     return "unknown";
    case PGWT_CLOSURE_IN_PROGRESS: return "in_progress";
    case PGWT_CLOSURE_ROTATED:     return "rotated";
    case PGWT_CLOSURE_CLEAN:       return "clean";
    case PGWT_CLOSURE_UNCLEAN:     return "unclean";
    default:                       return "unknown";
    }
}

#endif /* PGWT_TRACE_CLOSURE_H */
