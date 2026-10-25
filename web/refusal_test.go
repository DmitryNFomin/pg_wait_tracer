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
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
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

// ── #580 BLOCKER regression: a benign "pgwt-server:" line is NOT a reason ──
//
// The first version of this file treated the bare "pgwt-server:" prefix as a
// refusal marker. src/server.c uses it for SEVEN informational lines, and
// src/server.c:3713 ("%d trace files, %d CPUs, ~%lld events") prints on
// every successful start -- so every healthy server retained a "reason" for
// its whole life, and an ordinary request timeout shipped
// `"reason":"pgwt-server: loaded 431 query texts"`. Confidently wrong, which
// this file's own contract forbids. These are the exact inputs from the
// review.
func TestBenignServerPrefixLinesAreNotReasons(t *testing.T) {
	benign := []string{
		"pgwt-server: 12 trace files, 8 CPUs, ~3412004 events",
		"pgwt-server: loaded 431 query texts",
		"pgwt-server: loaded 17 backend metadata entries",
		"pgwt-server: curcache enabled=1 lo=0 blocks=64 bytes=1024",
		"pgwt-server: loaded event names from /tmp/x/wait_event_names.json",
		"pgwt-server: no usable wait_event_names.json in /tmp/x",
	}
	for _, line := range benign {
		if got := RefusalReason([]string{line}); got != "" {
			t.Errorf("benign line classified as a refusal reason: %q -> %q",
				line, got)
		}
	}
	// The whole healthy-start sequence, in order, still yields nothing.
	if got := RefusalReason(benign); got != "" {
		t.Errorf("a healthy start sequence produced a reason: %q", got)
	}
	// And the real refusal still does, even with benign lines before it.
	withRefusal := append(append([]string{}, benign[4:]...),
		"pgwt-server: no trace files found in /tmp/empty")
	want := "pgwt-server: no trace files found in /tmp/empty"
	if got := RefusalReason(withRefusal); got != want {
		t.Errorf("the real refusal was lost: %q, want %q", got, want)
	}
}

// DRIFT GATE for the unmarkedRefusals table. Reads src/server.c, extracts
// every "pgwt-server: ..." stderr line, and asserts that EXACTLY ONE of them
// classifies as a refusal reason -- the no-trace-files refusal #351 is about.
//
// This is what keeps a text table from rotting: a new "pgwt-server:" line, a
// reworded refusal, or a marker finally added at the source all turn this
// red and name what to do. It REFUSES rather than approves when it cannot
// look: a missing src/server.c, or an extraction that finds implausibly few
// lines, fails.
func TestRefusalClassificationCoversEveryServerLine(t *testing.T) {
	src, err := os.ReadFile(filepath.Join("..", "src", "server.c"))
	if err != nil {
		t.Fatalf("cannot read src/server.c, so this gate cannot see what it "+
			"checks: %v", err)
	}
	re := regexp.MustCompile(`"pgwt-server: ([^"]*)"`)
	ms := re.FindAllStringSubmatch(string(src), -1)
	if len(ms) < 7 {
		t.Fatalf("extracted only %d \"pgwt-server:\" line(s) from "+
			"src/server.c -- below the plausibility floor, so this gate is "+
			"not looking at what it thinks it is", len(ms))
	}
	subst := strings.NewReplacer(
		"%d", "7", "%s", "/tmp/x", "%lld", "123456", "%u", "7", "\\n", "")
	var classified []string
	for _, m := range ms {
		line := "pgwt-server: " + subst.Replace(m[1])
		if RefusalReason([]string{line}) != "" {
			classified = append(classified, line)
		}
	}
	if len(classified) != 1 ||
		!strings.Contains(classified[0], "no trace files found") {
		t.Errorf("src/server.c's \"pgwt-server:\" lines classified as "+
			"refusals = %q; want exactly one, the no-trace-files refusal.\n"+
			"If a line was added or reworded, update unmarkedRefusals (or, "+
			"better, give the line a FATAL:/REFUSED marker at the source).",
			classified)
	}
}

// A `timeout` against a LIVE server must carry no reason. This is the second
// half of the #580 fix: even a correctly classified FATAL line on the tap
// says nothing about why one request to a still-running server was slow.
func TestLiveServerTimeoutCarriesNoReason(t *testing.T) {
	b := NewBridge(func() *exec.Cmd {
		return exec.Command("/bin/sh", "-c", "cat >/dev/null")
	}, "")
	if err := b.Start(); err != nil {
		t.Fatalf("bridge start: %v", err)
	}
	t.Cleanup(b.Close)
	// Poison AFTER Start(), so spawn()'s Reset() cannot be what clears it:
	// the `up` gate has to be what suppresses the reason.
	if _, err := io.WriteString(b.stderrTap(),
		"FATAL: something that happened earlier\n"); err != nil {
		t.Fatal(err)
	}
	if b.refusalReason() == "" {
		t.Fatal("precondition: the tap must hold a classified reason, or " +
			"this case proves nothing about the up gate")
	}
	for _, msg := range []string{"timeout", "server disconnected"} {
		env := string(b.transportError(9, msg))
		if strings.Contains(env, `"reason"`) {
			t.Errorf("a live upstream's %q envelope carries a reason: %s",
				msg, env)
		}
		if !strings.Contains(env, `"transport":true`) {
			t.Errorf("%q envelope lost its transport flag: %s", msg, env)
		}
	}
	// And once the upstream is gone, the same reason IS attached.
	b.mu.Lock()
	b.up = false
	b.mu.Unlock()
	env := string(b.transportError(9, "server disconnected"))
	if !strings.Contains(env, `"reason":"FATAL: something that happened earlier"`) {
		t.Errorf("a dead upstream's envelope carries no reason: %s", env)
	}
}
