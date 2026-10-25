package main

// #351: a startup refusal reason must reach the BROWSER, not only the
// terminal.
//
// pgwt-server refuses at startup with a reason on stderr (e.g.
// "pgwt-server: no trace files found in <dir>"). The bridge forwarded that
// stderr to its own log, so whoever was watching the pgwt-client terminal
// saw the cause -- while every browser request got
// "server unavailable (reconnecting)" or "server disconnected" and no
// explanation at all. The failure was self-explaining on one surface and
// silent on the other, and the terminal reader is usually the person who
// already knows what they did.
//
// This file is the PURE half: line classification and envelope
// construction, with no process, no socket and no clock, so every case
// (including the ones where there is no reason to report) is a unit test
// rather than something only a live refusal can exercise.

import (
	"bytes"
	"strconv"
	"strings"
	"sync"
)

// refusalKeep is how many recent stderr lines one upstream incarnation's tap
// retains. A refusal prints its reason within a few lines of exiting; a
// bound is what keeps a chatty --verbose server from growing this forever.
const refusalKeep = 32

// unmarkedRefusals are the stderr lines that ARE refusals but carry no
// FATAL:/REFUSED marker of their own, matched on their distinctive text.
//
// Why a text table and not a marker in the server: the only entry,
// src/server.c:3669's "pgwt-server: no trace files found in %s" (the line
// #351 was filed about), is pinned VERBATIM by
// tests/data_current_trace_cache_mutations.py:30, which patches that exact
// string to build a #283 mutant. Adding "FATAL: " to it would break that
// gate, in a file this branch does not own. So the durable fix -- a marker
// at the source -- is left to whoever owns src/server.c, and this table is
// the interim. TestRefusalClassificationCoversEveryServerLine below reads
// src/server.c and FAILS if any "pgwt-server:" line drifts, is added, or
// disappears, so the table cannot rot quietly.
var unmarkedRefusals = []string{
	"no trace files found",
}

// RefusalReason picks the reason to show a browser out of the stderr lines
// one upstream incarnation produced, or returns "" when there is none.
//
// The rule, deliberately narrow: only a line that is unambiguously a
// FAILURE counts.
//
//   - a line containing "FATAL:"
//   - a line containing "REFUSED"
//   - a "pgwt-server:" line matching unmarkedRefusals above
//
// A bare "pgwt-server:" prefix is NOT a marker, and treating it as one was a
// real defect in the first version of this file: src/server.c uses that
// prefix for SEVEN informational lines, one of which
// ("pgwt-server: %d trace files, %d CPUs, ~%lld events", src/server.c:3713)
// prints on every successful start. Every healthy server therefore retained
// a "reason" for its whole life, and an ordinary request timeout shipped
// `"reason":"pgwt-server: loaded 431 query texts"` -- confidently wrong,
// which this file's own contract forbids.
//
// INFO/WARN/DEBUG/NOTE lines are never reasons either. A warning is not why
// the server is gone. The LAST qualifying line wins -- a refusal that
// narrates ("ERROR: ... REFUSED: cannot publish site") ends with the most
// specific statement.
//
// Returns "" for empty input, for whitespace-only input, and for input with
// no qualifying line: "I could not establish a reason" must stay
// distinguishable from a reason, never quietly become one.
func RefusalReason(lines []string) string {
	reason := ""
	for _, raw := range lines {
		line := strings.TrimSpace(raw)
		if line == "" {
			continue
		}
		upper := strings.ToUpper(line)
		if strings.HasPrefix(upper, "INFO:") ||
			strings.HasPrefix(upper, "WARN:") ||
			strings.HasPrefix(upper, "WARNING:") ||
			strings.HasPrefix(upper, "DEBUG:") ||
			strings.HasPrefix(upper, "NOTE:") {
			continue
		}
		if strings.Contains(upper, "FATAL:") ||
			strings.Contains(upper, "REFUSED") {
			reason = line
			continue
		}
		if strings.HasPrefix(line, "pgwt-server:") {
			for _, pat := range unmarkedRefusals {
				if strings.Contains(line, pat) {
					reason = line
					break
				}
			}
		}
	}
	return reason
}

// TransportErrorWithReason builds the bridge's error envelope, carrying the
// upstream's own refusal reason when there is one.
//
// "transport":true keeps its existing meaning (connection-level failure, not
// a command error from a healthy server). The "reason" field is present ONLY
// when a reason was established: a client must be able to tell "the server
// is gone and here is why" from "the server is gone and nobody knows", and
// an empty-string reason would erase that difference. `msg` is never
// replaced, so an existing consumer reading only "error" is unaffected.
func TransportErrorWithReason(id int64, msg, reason string) []byte {
	out := `{"id":` + strconv.FormatInt(id, 10) +
		`,"error":` + strconv.Quote(msg) + `,"transport":true`
	if r := strings.TrimSpace(reason); r != "" {
		out += `,"reason":` + strconv.Quote(r)
	}
	return []byte(out + "}")
}

// stderrTap forwards one upstream incarnation's stderr verbatim to whatever
// it is multiplexed with, while retaining the last refusalKeep lines so a
// refusal reason can be attached to the transport errors that follow.
//
// Reset() is called on every spawn, so a reason always belongs to the
// CURRENT upstream process. Without that, a server that refused once and
// then started cleanly would keep explaining a failure that is over.
type stderrTap struct {
	mu      sync.Mutex
	partial []byte
	lines   []string
}

// Write implements io.Writer. Never returns an error: a tap that cannot
// remember a line must not break stderr forwarding.
// maxPartial bounds the UNTERMINATED tail. `lines` is capped by refusalKeep,
// but a producer that never emits a newline would otherwise grow `partial`
// without limit. A refusal reason is one line; anything past this cannot be
// one, so the oldest bytes are dropped rather than retained.
const maxPartial = 8192

func (t *stderrTap) Write(p []byte) (int, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.partial = append(t.partial, p...)
	for {
		// bytes.IndexByte on the SLICE: string(t.partial) copied the whole
		// buffer on every iteration of this loop.
		i := bytes.IndexByte(t.partial, '\n')
		if i < 0 {
			break
		}
		t.push(string(t.partial[:i]))
		t.partial = t.partial[i+1:]
	}
	if len(t.partial) > maxPartial {
		t.partial = append([]byte(nil), t.partial[len(t.partial)-maxPartial:]...)
	}
	return len(p), nil
}

func (t *stderrTap) push(line string) {
	t.lines = append(t.lines, line)
	if len(t.lines) > refusalKeep {
		t.lines = t.lines[len(t.lines)-refusalKeep:]
	}
}

// Reason returns the refusal reason for this incarnation, or "". The
// unterminated tail counts: a refusal commonly exits without a trailing
// newline on its last write, and dropping that line is exactly how a reason
// goes missing.
func (t *stderrTap) Reason() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	lines := make([]string, 0, len(t.lines)+1)
	lines = append(lines, t.lines...)
	if tail := strings.TrimSpace(string(t.partial)); tail != "" {
		lines = append(lines, tail)
	}
	return RefusalReason(lines)
}

// Reset forgets everything: called at each spawn.
func (t *stderrTap) Reset() {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.partial = nil
	t.lines = nil
}
