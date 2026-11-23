/* provider_timeline.c — the `--mode timeline` provider (#1006 unit 1).
 *
 * S0: an empty compiling unit, registered in the Makefile's USER_SRCS so S3
 * (start, stop, poll and the 5 ms drain thread) and S4 (raw -> assemble ->
 * writer -> summary wiring) never touch the build registration. Including
 * the contract headers here also compiles them in the daemon build.
 */
#include "timeline_wire.h"
#include "timeline_format.h"
#include "timeline_assemble.h"
#include "timeline_writer.h"
#include "timeline_summary.h"
