package main

import (
	"bytes"
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"testing/fstest"
	"time"
)

// #401: Chrome's implicit GET /favicon.ico must not 404 -- a 404 there is
// what refused tab 1 of every automated walk (scripts/demo-walk-record.py's
// console-error gate whitelists nothing). The input that makes this fail if
// the route were ever dropped or changed to a 404: a plain GET /favicon.ico.
func TestFaviconHandlerServesNoContent(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/favicon.ico", nil)
	rec := httptest.NewRecorder()
	faviconHandler(rec, req)
	if rec.Code != http.StatusNoContent {
		t.Fatalf("favicon status = %d, want %d (No Content)", rec.Code, http.StatusNoContent)
	}
	if body := rec.Body.Bytes(); len(body) != 0 {
		t.Fatalf("favicon body = %q, want empty", body)
	}
}

// #401 follow-up: a direct call to faviconHandler (above) cannot tell "the
// favicon route exists" from "a handler answering 204 for EVERY path got
// wired in" -- both pass it identically. Route staticRoutes on a REAL,
// fresh http.ServeMux and make actual HTTP requests through it
// (net/http/httptest), so dispatch itself is exercised: /favicon.ico must
// be 204, "/" must still serve the UI tree (not swallowed by a catch-all
// favicon handler), and an unrelated path must still 404 (not caught by an
// over-broad favicon registration either).
func TestStaticRoutesDispatchThroughRealMux(t *testing.T) {
	ui := fstest.MapFS{
		"index.html": {Data: []byte("<html>pgwt UI</html>")},
	}
	mux := http.NewServeMux()
	staticRoutes(mux, ui)
	server := httptest.NewServer(mux)
	defer server.Close()

	get := func(path string) (int, string) {
		resp, err := http.Get(server.URL + path)
		if err != nil {
			t.Fatalf("GET %s: %v", path, err)
		}
		defer resp.Body.Close()
		body, err := io.ReadAll(resp.Body)
		if err != nil {
			t.Fatalf("GET %s: reading body: %v", path, err)
		}
		return resp.StatusCode, string(body)
	}

	if code, body := get("/favicon.ico"); code != http.StatusNoContent {
		t.Fatalf("GET /favicon.ico = %d %q, want %d", code, body, http.StatusNoContent)
	}
	if code, body := get("/"); code != http.StatusOK || body != "<html>pgwt UI</html>" {
		t.Fatalf("GET / = %d %q, want 200 with the UI's index.html", code, body)
	}
	if code, _ := get("/no-such-path"); code != http.StatusNotFound {
		t.Fatalf("GET /no-such-path = %d, want %d", code, http.StatusNotFound)
	}
}

// #521: unstampedBuildRefusal is the decision main() acts on. Pinned
// directly so the message content (which names the fix) is covered without
// needing a subprocess for every case; TestPlainGoBuildRefusesRatherThanServeDev
// below covers the wiring itself (that main() actually calls this).
func TestUnstampedBuildRefusal(t *testing.T) {
	if got := unstampedBuildRefusal("v0.13-429-gc723e96a"); got != "" {
		t.Errorf("stamped version refused: %q", got)
	}
	got := unstampedBuildRefusal("dev")
	if got == "" {
		t.Fatal("unstamped \"dev\" was not refused")
	}
	if !strings.Contains(got, "make pgwt-client") {
		t.Errorf("refusal does not name the supported build path: %q", got)
	}
}

// #521: a plain `go build` (no -ldflags) leaves buildVersion at its unset
// "dev" default, which used to be served as the client's version -- a value
// guaranteed to mismatch any real pgwt-server, silently tripping the UI's
// skew banner and, downstream, the demo recorder's panel-health refusal.
//
// This runs the REAL, unmodified `go build .` command -- the exact one the
// issue is about -- rather than asserting on unstampedBuildRefusal in
// isolation: a regression that reverted main()'s call to that check (while
// leaving the function itself, and its unit test above, untouched) would
// still pass a test that only called the function directly. Only exercising
// the actual binary catches that.
//
// It must not hang: the bug's failure mode is not a clean error, it is
// UI-3's respawn-with-backoff retrying SSH forever, which is exactly what a
// bare (timeout-less) cmd.Wait() would reproduce here. The bounded context
// turns "the fix is absent" into an explicit, fast failure instead of a
// test that never returns.
func TestPlainGoBuildRefusesRatherThanServeDev(t *testing.T) {
	bin := filepath.Join(t.TempDir(), "pgwt-plainbuild")
	build := exec.Command("go", "build", "-o", bin, ".")
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("plain `go build .` failed: %v\n%s", err, out)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	run := exec.CommandContext(ctx, bin, "--server-path", "/nonexistent", "nobody@127.0.0.1")
	var stderr bytes.Buffer
	run.Stderr = &stderr
	if err := run.Start(); err != nil {
		t.Fatalf("starting plain-built binary: %v", err)
	}
	waitErr := run.Wait()

	if ctx.Err() == context.DeadlineExceeded {
		t.Fatalf("plain-built binary was still running after 5s instead of "+
			"refusing immediately -- it kept retrying a connection rather "+
			"than refusing an unstamped build (stderr so far: %s)", stderr.String())
	}
	if waitErr == nil {
		t.Fatalf("plain-built binary exited 0; want a refusal before it "+
			"ever attempts to connect (stderr: %s)", stderr.String())
	}
	got := stderr.String()
	if !strings.Contains(got, "make pgwt-client") {
		t.Errorf("refusal does not name the supported build path: %s", got)
	}
	if strings.Contains(got, "SSH connected") {
		t.Errorf("binary proceeded to connect despite an unstamped build: %s", got)
	}
}
