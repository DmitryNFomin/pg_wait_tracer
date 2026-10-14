package main

import (
	"net/http"
	"net/http/httptest"
	"testing"
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
