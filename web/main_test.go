package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"testing/fstest"
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
