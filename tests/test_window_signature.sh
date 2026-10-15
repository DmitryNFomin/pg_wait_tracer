#!/usr/bin/env bash
# test_window_signature.sh — #424 gate 6: AN OMISSION MUST BE CAUGHT BY THE
# BUILD, NOT BY REVIEW.
#
# The repair for #424's live half only works if every handler that needs live
# time actually consumes the open-interval overlay. "Remember to pass the
# window" is not a mechanism; this checks that the mechanism exists, by
# compiling two probe translation units against the real src/compute.h:
#
#   NEGATIVE probe — the pre-#424 records-only call
#       pgwt_compute_aas(events, count, &f, ...)
#     MUST FAIL to compile. If it compiles, the records-only signature is
#     back and a handler can silently reduce over closed records alone.
#
#   POSITIVE probe — the window call
#       pgwt_compute_aas(&w, &f, ...)
#     MUST COMPILE. Without this half the gate would also "pass" when the
#     compiler is broken, the include path is wrong, compute.h does not
#     parse, or the probe has a typo — i.e. when the gate can see nothing at
#     all. A gate that cannot see must refuse, never approve.
#
# BYPASSES THIS SCRIPT REFUSES RATHER THAN SKIPS (each has a case in
# tests/test_window_signature_selftest.sh):
#   - no compiler on PATH, or a compiler that exits 126/127
#   - a compiler that fails on EVERYTHING (positive probe red)
#   - a compiler that succeeds on EVERYTHING (negative probe green)
#   - src/compute.h missing
# None of these print "skip". All of them exit non-zero.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../src"
CC="${CC:-cc}"
rc=0
pass() { echo "  ok: $1"; }
fail() { echo "  FAIL: $1"; rc=1; }

if [[ ! -f "$SRC/compute.h" ]]; then
    echo "  FAIL: src/compute.h not found at $SRC — cannot check anything"
    exit 1
fi

# The compiler must exist and be runnable. 126 (not executable) and 127
# (not found) are the two ways a dependency "passes" a naive `|| true`.
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "  FAIL: compiler '$CC' not found — the build gate cannot be checked"
    exit 1
fi
"$CC" --version >/dev/null 2>&1
probe_rc=$?
if [[ $probe_rc -eq 126 || $probe_rc -eq 127 ]]; then
    echo "  FAIL: compiler '$CC' exits $probe_rc — not a usable compiler"
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Platform shims: this script only ever asks the compiler to PARSE and
# TYPE-CHECK compute.h, so it must run on a developer Mac too.
CFLAGS=(-fsyntax-only -I"$SRC" -I"$HERE/../include" -DPGWT_SERVER)

cat > "$TMP/positive.c" <<'EOF'
#include "compute.h"
#include "time_window.h"
void probe(const struct pgwt_window *w, const struct pgwt_filter *f,
           struct pgwt_aas_result *out);
void probe(const struct pgwt_window *w, const struct pgwt_filter *f,
           struct pgwt_aas_result *out)
{
    pgwt_compute_aas(w, f, 0, 1000, 4, 0, AAS_MAX_EVENT_SERIES, out);
}
EOF

cat > "$TMP/negative.c" <<'EOF'
#include "compute.h"
/* The pre-#424 call: records and a count, no overlay. This is EXACTLY what a
 * handler that forgets the window would write. */
void probe(const struct pgwt_trace_event *events, int count,
           const struct pgwt_filter *f, struct pgwt_aas_result *out);
void probe(const struct pgwt_trace_event *events, int count,
           const struct pgwt_filter *f, struct pgwt_aas_result *out)
{
    pgwt_compute_aas(events, count, f, 0, 1000, 4, 0, AAS_MAX_EVENT_SERIES,
                     out);
}
EOF

"$CC" "${CFLAGS[@]}" "$TMP/positive.c" >"$TMP/pos.log" 2>&1
pos_rc=$?
"$CC" "${CFLAGS[@]}" "$TMP/negative.c" >"$TMP/neg.log" 2>&1
neg_rc=$?

if [[ $pos_rc -eq 0 ]]; then
    pass "the window call compiles (the gate can see)"
else
    fail "the window call does NOT compile — this gate is blind, not green"
    sed -n '1,8p' "$TMP/pos.log" | sed 's/^/      /'
fi

if [[ $neg_rc -ne 0 ]]; then
    pass "the records-only call is REFUSED by the compiler (rc=$neg_rc)"
else
    fail "the records-only call still COMPILES — a handler can forget the overlay"
fi

# Every one of the six reducers, not just the sampled one: a signature
# restored on five of six is the partial repair this whole change exists to
# prevent.
for fn in pgwt_compute_aas pgwt_compute_time_model pgwt_compute_top_events \
          pgwt_compute_top_sessions pgwt_compute_top_queries \
          pgwt_compute_concurrency; do
    # The declaration's first parameter must be the window.
    if grep -A1 "^void ${fn}(" "$SRC/compute.h" | head -2 \
         | grep -q "const struct pgwt_window \*w"; then
        pass "$fn takes the window as its first parameter"
    else
        fail "$fn does not take a window — its records-only form is back"
    fi
done

if [[ $rc -eq 0 ]]; then
    echo "PASS: test_window_signature"
else
    echo "FAIL: test_window_signature"
fi
exit $rc
