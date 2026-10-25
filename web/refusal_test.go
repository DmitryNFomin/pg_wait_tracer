package main

// #351 server half: the upstream's startup refusal reason reaches the
// browser's error envelope instead of being collapsed into a generic
// disconnect.
//
// The cases are weighted toward FALSE NEGATIVES, which is the failure mode
// a "surface the reason" feature actually has:
//
//   - no stderr at all, or only INFO/WARN noise -> NO reason field. "Nobody
//     knows why" must stay distinguishable from a reason; a tap that
//     reported the last line it saw whatever it was would pass a
//     positive-only test and mislabel every clean disconnect.
//   - a reason from a PREVIOUS incarnation -> gone after Reset(). A reason
//     that outlives its process explains a failure that is over.
//   - an unterminated final line -> still counted. A refusal often exits
//     without a trailing newline, and silently dropping that line is how
//     the reason goes missing on exactly the run that has one.
//   - the envelope must stay byte-compatible with transportError() when
//     there is no reason, so the existing client path is untouched.

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os/exec"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func TestRefusalReasonPicksTheServersOwnRefusal(t *testing.T) {
	cases := []struct {
		name  string
		lines []string
		want  string
	}{
		{
			name:  "the startup refusal this issue was filed for",
			lines: []string{"pgwt-server: no trace files found in /var/lib/pgwt"},
			want:  "pgwt-server: no trace files found in /var/lib/pgwt",
		},
		{
			name: "noise before the refusal is skipped",
			lines: []string{
				"INFO: trace file PG version: 18",
				"WARN: optional full-mode plan USDT phase markers unavailable",
				"pgwt-server: no trace files found in /tmp/x",
			},
			want: "pgwt-server: no trace files found in /tmp/x",
		},
		{
			name:  "an explicit FATAL counts",
			lines: []string{"FATAL: cannot open trace directory: Permission denied"},
			want:  "FATAL: cannot open trace directory: Permission denied",
		},
		{
			name:  "a REFUSED line counts wherever the word sits",
			lines: []string{"ERROR: execution phase markers REFUSED: allowlist published 0 sites"},
			want:  "ERROR: execution phase markers REFUSED: allowlist published 0 sites",
		},
		{
			name: "the LAST qualifying line wins (a narrating refusal)",
			lines: []string{
				"FATAL: something general",
				"pgwt-server: no trace files found in /tmp/y",
			},
			want: "pgwt-server: no trace files found in /tmp/y",
		},
		// ── false negatives: these must NOT produce a reason ──
		{name: "no stderr at all", lines: nil, want: ""},
		{name: "empty lines only", lines: []string{"", "   ", "\t"}, want: ""},
		{
			name: "INFO and WARN are not reasons",
			lines: []string{
				"INFO: summary fast path enabled",
				"WARN: CPU accounting: could not attach sched_switch",
				"NOTE: this trace contains sampled data",
				"DEBUG: whatever",
			},
			want: "",
		},
		{
			name:  "an unrelated line is not a reason",
			lines: []string{"bash: line 1: pgwt-serverr: command not found"},
			want:  "",
		},
	}
	for _, c := range cases {
		if got := RefusalReason(c.lines); got != c.want {
			t.Errorf("%s: RefusalReason() = %q, want %q", c.name, got, c.want)
		}
	}
}

func TestTransportErrorWithReasonOmitsAnAbsentReason(t *testing.T) {
	// No reason: byte-identical to the envelope the bridge produced before
	// this change, so no existing client path is disturbed.
	want := transportError(7, "server disconnected")
	got := TransportErrorWithReason(7, "server disconnected", "")
	if !bytes.Equal(got, want) {
		t.Fatalf("no-reason envelope = %s, want %s", got, want)
	}
	// Whitespace is not a reason either.
	got = TransportErrorWithReason(7, "server disconnected", "   \n")
	if !bytes.Equal(got, want) {
		t.Fatalf("whitespace-reason envelope = %s, want %s", got, want)
	}
	var probe map[string]any
	if err := json.Unmarshal(got, &probe); err != nil {
		t.Fatalf("no-reason envelope is not JSON: %v", err)
	}
	if _, present := probe["reason"]; present {
		t.Errorf("no-reason envelope must not carry a reason key: %s", got)
	}
}

func TestTransportErrorWithReasonCarriesTheReason(t *testing.T) {
	reason := `pgwt-server: no trace files found in /tmp/"odd" dir`
	raw := TransportErrorWithReason(42, "server unavailable (reconnecting)", reason)
	var env struct {
		ID        int64  `json:"id"`
		Error     string `json:"error"`
		Transport bool   `json:"transport"`
		Reason    string `json:"reason"`
	}
	if err := json.Unmarshal(raw, &env); err != nil {
		t.Fatalf("envelope is not valid JSON (%v): %s", err, raw)
	}
	if env.ID != 42 {
		t.Errorf("id = %d, want 42", env.ID)
	}
	if env.Error != "server unavailable (reconnecting)" {
		t.Errorf("error = %q, want the generic transport message", env.Error)
	}
	if !env.Transport {
		t.Errorf("transport flag lost: %s", raw)
	}
	if env.Reason != reason {
		t.Errorf("reason = %q, want %q (quoting must survive)", env.Reason, reason)
	}
}

func TestStderrTapForwardsVerbatimAndRemembersTheReason(t *testing.T) {
	var sink bytes.Buffer
	tap := &stderrTap{}
	w := io.MultiWriter(&sink, tap)

	if _, err := io.WriteString(w, "INFO: starting\n"); err != nil {
		t.Fatal(err)
	}
	if got := tap.Reason(); got != "" {
		t.Errorf("an INFO line alone produced a reason: %q", got)
	}
	// Split across writes, and the final line has NO trailing newline --
	// which is how a refusing process commonly exits.
	if _, err := io.WriteString(w, "pgwt-server: no trace files "); err != nil {
		t.Fatal(err)
	}
	if _, err := io.WriteString(w, "found in /tmp/z"); err != nil {
		t.Fatal(err)
	}
	want := "pgwt-server: no trace files found in /tmp/z"
	if got := tap.Reason(); got != want {
		t.Errorf("Reason() = %q, want %q (unterminated tail must count)", got, want)
	}
	// The log surface still sees every byte, unaltered.
	if sink.String() != "INFO: starting\npgwt-server: no trace files found in /tmp/z" {
		t.Errorf("stderr forwarding altered the stream: %q", sink.String())
	}
}

func TestStderrTapResetForgetsAPreviousIncarnation(t *testing.T) {
	tap := &stderrTap{}
	if _, err := io.WriteString(tap, "pgwt-server: no trace files found in /tmp/a\n"); err != nil {
		t.Fatal(err)
	}
	if tap.Reason() == "" {
		t.Fatal("precondition: the first incarnation's reason must be captured")
	}
	tap.Reset()
	if got := tap.Reason(); got != "" {
		t.Errorf("a reason survived Reset() and would explain a failure that is over: %q", got)
	}
	// A clean second incarnation that says nothing fatal stays reason-less.
	if _, err := io.WriteString(tap, "INFO: 3 trace file(s) found\n"); err != nil {
		t.Fatal(err)
	}
	if got := tap.Reason(); got != "" {
		t.Errorf("a healthy incarnation produced a reason: %q", got)
	}
}

func TestStderrTapIsBoundedAndKeepsTheLatest(t *testing.T) {
	tap := &stderrTap{}
	for i := 0; i < refusalKeep*4; i++ {
		if _, err := io.WriteString(tap, "INFO: noise\n"); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := io.WriteString(tap, "FATAL: the real cause\n"); err != nil {
		t.Fatal(err)
	}
	if got := tap.Reason(); got != "FATAL: the real cause" {
		t.Errorf("Reason() = %q after %d noise lines", got, refusalKeep*4)
	}
	tap.mu.Lock()
	n := len(tap.lines)
	tap.mu.Unlock()
	if n > refusalKeep {
		t.Errorf("tap retained %d lines, bound is %d", n, refusalKeep)
	}
	// And the bound must not be a way to LOSE the reason: a reason pushed
	// first and then buried under refusalKeep newer lines is genuinely gone,
	// which is correct (it is no longer recent) -- assert that explicitly so
	// nobody later "fixes" it into unbounded growth by accident.
	tap.Reset()
	if _, err := io.WriteString(tap, "FATAL: buried\n"); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < refusalKeep; i++ {
		if _, err := io.WriteString(tap, "INFO: after\n"); err != nil {
			t.Fatal(err)
		}
	}
	if got := tap.Reason(); got != "" {
		t.Errorf("a reason %d lines old is not recent; got %q", refusalKeep, got)
	}
}

func TestBridgeAttachesTheRefusalReasonToTransportErrors(t *testing.T) {
	b := NewBridge(nil, "")
	if got := b.refusalReason(); got != "" {
		t.Errorf("a fresh bridge reports a reason: %q", got)
	}
	if b.stderrTap() == nil {
		t.Fatal("the bridge has no stderr tap, so no reason can ever be captured")
	}
	if _, err := io.WriteString(b.stderrTap(),
		"pgwt-server: no trace files found in /tmp/b\n"); err != nil {
		t.Fatal(err)
	}
	if got := b.refusalReason(); got != "pgwt-server: no trace files found in /tmp/b" {
		t.Fatalf("bridge refusalReason() = %q", got)
	}
	env := string(b.transportError(5, "server unavailable (reconnecting)"))
	if !strings.Contains(env, `"reason":"pgwt-server: no trace files found in /tmp/b"`) {
		t.Errorf("the bridge's transport error carries no reason: %s", env)
	}
	if !strings.Contains(env, `"transport":true`) {
		t.Errorf("the transport flag was lost: %s", env)
	}
}

// THE WIRING, not the accessors. Every case above writes into the tap
// directly, so a build that never connected cmd.Stderr to the tap would
// pass all of them -- the exact false negative #351 is about. This case
// spawns a REAL process that refuses on stderr and exits, the way
// pgwt-server does when the trace directory holds nothing, and reads the
// reason back out of a browser WebSocket response.
//
// No pgwt-server binary needed (so it runs on a developer machine, where the
// package's other integration tests skip): /bin/sh reproduces the only two
// properties that matter -- a reason on stderr, then exit.
func TestBridgeSurfacesARealUpstreamRefusalToTheBrowser(t *testing.T) {
	const want = "pgwt-server: no trace files found in /tmp/laneD-fake"
	b := NewBridge(func() *exec.Cmd {
		return exec.Command("/bin/sh", "-c",
			"echo '"+want+"' >&2; exit 1")
	}, "")
	if err := b.Start(); err != nil {
		t.Fatalf("bridge start: %v", err)
	}
	srv := httptest.NewServer(http.HandlerFunc(b.HandleWebSocket))
	t.Cleanup(func() { srv.Close(); b.Close() })

	// Wait for the first incarnation's stderr to be tapped. Bounded, and a
	// timeout FAILS: "I could not look" is how this defect survived.
	deadline := time.Now().Add(10 * time.Second)
	for b.refusalReason() == "" {
		if time.Now().After(deadline) {
			t.Fatal("the upstream's stderr refusal never reached the bridge's " +
				"tap: cmd.Stderr is not wired to it")
		}
		time.Sleep(10 * time.Millisecond)
	}
	if got := b.refusalReason(); got != want {
		t.Fatalf("refusalReason() = %q, want %q", got, want)
	}

	conn, _, err := websocket.DefaultDialer.Dial(
		"ws"+strings.TrimPrefix(srv.URL, "http"), nil)
	if err != nil {
		t.Fatalf("ws dial: %v", err)
	}
	defer conn.Close()
	if err := conn.WriteMessage(websocket.TextMessage,
		[]byte(`{"id":1,"cmd":"info"}`)); err != nil {
		t.Fatalf("ws write: %v", err)
	}
	conn.SetReadDeadline(time.Now().Add(10 * time.Second))
	_, raw, err := conn.ReadMessage()
	if err != nil {
		t.Fatalf("ws read: %v", err)
	}
	var env struct {
		Error     string `json:"error"`
		Transport bool   `json:"transport"`
		Reason    string `json:"reason"`
	}
	if err := json.Unmarshal(raw, &env); err != nil {
		t.Fatalf("bad envelope %q: %v", raw, err)
	}
	if !env.Transport || env.Error == "" {
		t.Errorf("expected a transport error envelope, got %s", raw)
	}
	if env.Reason != want {
		t.Errorf("the browser got no reason for the refusal: %s", raw)
	}
}

// The OTHER half of the wiring, and the one no direct Reset() test can
// reach: spawn() must clear the tap, so a reason never survives into a
// healthy incarnation and explains a failure that is already over. Asserted
// through the real spawn path with a poisoned tap and an upstream that says
// nothing.
func TestBridgeSpawnClearsAPreviousIncarnationsReason(t *testing.T) {
	b := NewBridge(func() *exec.Cmd {
		// Stays alive reading stdin, writes nothing anywhere.
		return exec.Command("/bin/sh", "-c", "cat >/dev/null")
	}, "")
	if _, err := io.WriteString(b.stderrTap(),
		"pgwt-server: no trace files found in /tmp/stale\n"); err != nil {
		t.Fatal(err)
	}
	if b.refusalReason() == "" {
		t.Fatal("precondition: the stale reason must be present before Start()")
	}
	if err := b.Start(); err != nil {
		t.Fatalf("bridge start: %v", err)
	}
	t.Cleanup(b.Close)
	// spawn() runs synchronously inside Start(), so this is deterministic.
	if got := b.refusalReason(); got != "" {
		t.Errorf("spawn() did not clear the tap: a healthy upstream still "+
			"reports %q", got)
	}
}
