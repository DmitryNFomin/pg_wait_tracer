#!/usr/bin/env bash
# test_served_asset_guard.sh -- unit test for assert_served_ui_matches_tree()
# in tests/live_daemon_lib.sh, the guard that refuses when the Go bridge is not
# serving the web/static of the tree under test (web/main.go compiles
# web/static into the binary, so a stale web/pgwt serves the old UI silently
# and a live walk -- or a counted demo rehearsal -- grades the wrong assets
# while reporting green).
#
# The guard takes its asset list from the BINARY (GET /embedded-assets, added
# to web/main.go for exactly this) rather than re-deriving Go's //go:embed
# exclusions in Python. Three silent passes were found in review while it did
# the latter -- dot/underscore handling, a chmod-444 directory, and finally a
# dangling `static/mod/go.mod` symlink that made the replica prune a subtree
# Go had embedded. Every one was a gap between the replica and the real rules,
# invisible to any invariant computed afterwards, because an exclusion shrinks
# the denominator. Hence case 24 below.
#
# Hermetic: a throwaway "disk" tree plus a python3 http.server that serves a
# SEPARATE copy of it AND publishes its own /embedded-assets manifest. The
# served copy plus its manifest play the binary's role, which is faithful
# because the guard's subject is exactly "what the binary says it has and what
# it serves, versus what is on disk" -- it has no opinion about which program
# answers. No Go build, no PostgreSQL, no root, so this runs on the Mac via
# `make check` and not only in the box tier. The REAL Go endpoint is exercised
# separately by tests/results/served_asset_guard/mac_teeth.sh.
#
# The cases are weighted toward FALSE NEGATIVES -- the ways a guard like this
# approves without having looked: an absent or unparseable or empty manifest,
# a manifest that contradicts itself, a dead server, an unreadable directory
# or file, a missing dependency, a difference in a LATER file (no
# short-circuit), a difference only in LENGTH, and asymmetric set differences
# in both directions.
#
# Usage: tests/test_served_asset_guard.sh (run from anywhere)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# PGWT_SAG_LIB is a TEST-only override (nothing in the product reads it): it
# lets this file be pointed at an older copy of live_daemon_lib.sh to show a
# regression going RED against the pre-fix function, instead of asserting a
# red nobody has seen.
# shellcheck disable=SC1091
source "${PGWT_SAG_LIB:-$SCRIPT_DIR/live_daemon_lib.sh}"

tests_run=0
tests_passed=0
tests_failed=0

report() {
    local ok="$1" msg="$2"
    tests_run=$((tests_run + 1))
    if [[ "$ok" == "true" ]]; then
        tests_passed=$((tests_passed + 1))
        echo "  PASS: $msg"
    else
        tests_failed=$((tests_failed + 1))
        echo "  FAIL: $msg"
    fi
}

TMP="$(mktemp -d "${TMPDIR:-/tmp}/pgwt_sag_XXXXXX")"
SRV_PID=""
# EVERY server this file has started, not just the current one. The first
# version of this harness tracked only $SRV_PID, so a start_server that
# replaced a running server orphaned the old one: six of them were found
# alive two hours after the fact, reparented to init. One had inherited the
# caller's stdout and held a shell pipeline open for 2h45m. Same class of
# defect as the leaked-process guard in tests/ui_live_smoke.sh, which is why
# there is now an end-of-run check below as well.
SRV_ALL=()
SRV_STARTED=0
# Every server writes its OWN pid here before it begins serving, so a run
# interrupted between `&` and the shell-side bookkeeping still leaves a
# record. Shell cannot spawn-and-register atomically; the child can.
PIDDIR="$TMP/pids"
mkdir -p "$PIDDIR"

# is_ours PID -- positive identification, never "the pid is alive so it must
# be mine". Pids are recycled; $TMP is unique to this run and appears in every
# server's argv.
is_ours() {
    local p="$1" cmd
    cmd="$(ps -o command= -p "$p" 2>/dev/null)" || return 1
    [[ -n "$cmd" && "$cmd" == *"$TMP"* ]]
}

# Anything whose command line mentions this run's unique temp dir, whatever
# the shell did or did not record. This is the authoritative survivor check:
# it does not depend on the bookkeeping it is meant to audit.
survivors_by_scan() {
    pgrep -f "$TMP" 2>/dev/null | grep -v "^$$\$" || true
}

cleanup() {
    local p
    # Registry first (survives an interrupted registration), then the scan.
    for p in $(cat "$PIDDIR"/* 2>/dev/null) "${SRV_ALL[@]:-}" $(survivors_by_scan); do
        [[ -n "$p" ]] || continue
        is_ours "$p" || continue
        # Bounded TERM -> poll -> KILL (live_daemon_lib.sh), never an
        # unbounded wait on a process that may ignore TERM.
        stop_pid "$p" 5
    done
    chmod -R u+rwX "$TMP" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

mk_disk() {
    local d="$1"
    mkdir -p "$d/lib/builders" "$d/vendor"
    printf 'export const a = 1;\n' > "$d/app.js"
    printf 'body { color: #fff }\n' > "$d/style.css"
    printf 'export function buildHeatmapOption() { return 0; }\n' \
        > "$d/lib/builders/histogram.js"
    printf 'export function buildConcurrency() { return 0; }\n' \
        > "$d/lib/builders/zzz_last.js"
    printf '/* vendored */\n' > "$d/vendor/echarts.min.js"
}

# start_server DIR [MODE] [MANIFEST_FILE]
#   Serves DIR, and publishes /embedded-assets describing what it serves --
#   i.e. it impersonates a bridge binary built from DIR.
#   MODE: normal (default) | none (no endpoint, an older bridge) | badjson |
#         empty | countskew | fromfile (paths read from MANIFEST_FILE, one per
#         line -- lets a manifest claim a path the server cannot serve)
start_server() {
    local dir="$1" mode="${2:-normal}" manifest="${3:-}"
    # Never leave a previous server running: replacing $SRV_PID without
    # stopping it is exactly how this harness leaked six of them.
    [[ -n "$SRV_PID" ]] && stop_server
    SRV_STARTED=$((SRV_STARTED + 1))
    local portfile="$TMP/port.$$"
    rm -f "$portfile"
    # >/dev/null 2>&1: a backgrounded child that inherits the caller's stdout
    # keeps the pipe open for as long as it lives, which wedged a `... | grep`
    # pipeline for 2h45m.
    python3 - "$dir" "$portfile" "$mode" "$manifest" "$PIDDIR" >/dev/null 2>&1 <<'PYEOF' &
import functools
import http.server
import json
import os
import sys

directory, portfile, mode, manifest = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
# Self-register BEFORE serving: if this run is interrupted between the shell's
# `&` and its own bookkeeping, this file is still the record that something
# was started.
with open(os.path.join(sys.argv[5], str(os.getpid())), "w") as _fh:
    _fh.write(str(os.getpid()))


def served_paths():
    out = []
    for dirpath, dirnames, filenames in os.walk(directory):
        for fn in filenames:
            out.append(os.path.relpath(os.path.join(dirpath, fn),
                                       directory).replace(os.sep, "/"))
    return sorted(out)


class H(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a, **k):
        pass

    def do_GET(self):
        if self.path == "/embedded-assets":
            if mode == "none":
                self.send_error(404, "no such endpoint")
                return
            if mode == "badjson":
                body = b"<html>not json</html>"
            elif mode == "empty":
                body = json.dumps({"paths": [], "count": 0}).encode()
            elif mode == "countskew":
                p = served_paths()
                body = json.dumps({"paths": p, "count": len(p) + 7}).encode()
            elif mode == "fromfile":
                p = [ln.strip() for ln in open(manifest) if ln.strip()]
                body = json.dumps({"paths": p, "count": len(p)}).encode()
            else:
                p = served_paths()
                body = json.dumps({"paths": p, "count": len(p)}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        return super().do_GET()


handler = functools.partial(H, directory=directory)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
with open(portfile + ".tmp", "w") as fh:
    fh.write(str(srv.server_address[1]))
os.rename(portfile + ".tmp", portfile)
srv.serve_forever()
PYEOF
    SRV_PID=$!
    SRV_ALL+=("$SRV_PID")
    local n=0
    while [[ ! -s "$portfile" && $n -lt 100 ]]; do
        sleep 0.05
        n=$((n + 1))
    done
    SRV_PORT="$(cat "$portfile" 2>/dev/null)"
    [[ -n "$SRV_PORT" ]] || { echo "  FAIL: test server never reported a port"; exit 1; }
    local m=0
    while ! url_ready "http://127.0.0.1:$SRV_PORT/" && [[ $m -lt 100 ]]; do
        sleep 0.05
        m=$((m + 1))
    done
}

stop_server() {
    if [[ -n "$SRV_PID" ]]; then
        # Only signal a pid we can still identify as ours: between the last
        # check and now it may have exited and had its pid recycled.
        if is_ours "$SRV_PID"; then
            stop_pid "$SRV_PID" 5
        fi
        # The pid file is NOT removed: $PIDDIR is the cumulative record of
        # what this run STARTED, which is what the end-of-run audit compares
        # against its own independent start counter. Deleting on stop made the
        # registry a list of live servers instead, and the audit then read
        # "recorded=1 started=18" -- it caught its own design error on the
        # first run, which is the behaviour wanted from it.
    fi
    SRV_PID=""
}

run_guard() {
    OUT="$(assert_served_ui_matches_tree "$@" 2>&1)"
    RC=$?
}

DISK="$TMP/disk"
SERVED="$TMP/served"
mk_disk "$DISK"
cp -R "$DISK/" "$SERVED/"
start_server "$SERVED"
BASE="http://127.0.0.1:$SRV_PORT/"

# 1. Identical trees -> PASS, stating all three counts. A pass that cannot say
#    how many assets it compared is the vacuous case.
run_guard "$BASE" "$DISK" "case1"
if [[ "$RC" -eq 0 ]] && grep -q "OK -- embedded=5 compared=5 matched=5" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "identical trees -> rc=0 stating embedded=compared=matched=5 (rc=$RC)"

# 2. A LATER file differs (zzz_last.js sorts last) -> MISMATCH with both
#    hashes. Catches a guard that short-circuits on the first match.
printf 'export function buildConcurrency() { return 1; }\n' \
    > "$SERVED/lib/builders/zzz_last.js"
run_guard "$BASE" "$DISK" "case2"
disk_h="$(python3 -c "import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$DISK/lib/builders/zzz_last.js")"
srv_h="$(python3 -c "import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$SERVED/lib/builders/zzz_last.js")"
if [[ "$RC" -eq 1 ]] \
   && grep -q "MISMATCH lib/builders/zzz_last.js" <<<"$OUT" \
   && grep -q "$disk_h" <<<"$OUT" && grep -q "$srv_h" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "a later file's served bytes differ -> rc=1, names the asset and BOTH hashes (rc=$RC)"
cp "$DISK/lib/builders/zzz_last.js" "$SERVED/lib/builders/zzz_last.js"

# 3. Truncation only (served is a strict prefix of disk) -> still MISMATCH.
head -c 10 "$DISK/app.js" > "$SERVED/app.js"
run_guard "$BASE" "$DISK" "case3"
if [[ "$RC" -eq 1 ]] && grep -q "MISMATCH app.js" <<<"$OUT" && grep -q "10 bytes" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "served asset truncated to a prefix -> rc=1 with both byte counts (rc=$RC)"
cp "$DISK/app.js" "$SERVED/app.js"
stop_server

# 4. The manifest claims a path the server will NOT serve (404) -> CANNOT
#    VERIFY. A binary that lists an asset it cannot produce is not a match.
MANI="$TMP/manifest.txt"
printf 'app.js\nstyle.css\nlib/builders/histogram.js\nlib/builders/zzz_last.js\nvendor/echarts.min.js\nghost.js\n' > "$MANI"
printf 'export const g = 1;\n' > "$DISK/ghost.js"      # on disk, not served
start_server "$SERVED" fromfile "$MANI"
run_guard "http://127.0.0.1:$SRV_PORT/" "$DISK" "case4"
if [[ "$RC" -eq 2 ]] \
   && grep -q "COULD NOT CHECK -- FETCH ghost.js" <<<"$OUT" \
   && grep -q "CANNOT VERIFY" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "manifest lists an asset the bridge cannot serve -> rc=2 CANNOT VERIFY (rc=$RC)"
rm -f "$DISK/ghost.js"
stop_server

# 5. Unfetchable AND mismatched at once -> CANNOT VERIFY wins (rc=2), and the
#    mismatch is still printed.
printf 'different\n' > "$SERVED/style.css"
printf 'export const g = 1;\n' > "$DISK/ghost.js"
start_server "$SERVED" fromfile "$MANI"
run_guard "http://127.0.0.1:$SRV_PORT/" "$DISK" "case5"
if [[ "$RC" -eq 2 ]] \
   && grep -q "MISMATCH style.css" <<<"$OUT" \
   && grep -q "COULD NOT CHECK -- FETCH ghost.js" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "unfetchable + mismatch together -> rc=2 (cannot-verify wins), both printed (rc=$RC)"
cp "$DISK/style.css" "$SERVED/style.css"
rm -f "$DISK/ghost.js"
stop_server

start_server "$SERVED"
BASE="http://127.0.0.1:$SRV_PORT/"

# 6. A file on disk that the binary does NOT carry -> rc=2, naming it. This is
#    the deliberate trade-off of taking the list from the binary: the guard
#    applies NO exclusion rules of its own, so a stray .DS_Store refuses
#    rather than being guessed at. The alternative is the replica that caused
#    three silent passes. The message says what to do about it.
printf 'junk\n' > "$DISK/.DS_Store"
run_guard "$BASE" "$DISK" "case6"
if [[ "$RC" -eq 2 ]] \
   && grep -q "ON DISK, NOT IN BINARY -- .DS_Store" <<<"$OUT" \
   && grep -q "remove it from web/static or rebuild" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "a disk file absent from the binary's list -> rc=2 naming it, no guessing (rc=$RC)"
rm -f "$DISK/.DS_Store"

# 7. The server is GONE -> the manifest cannot be fetched -> rc=2.
stop_server
run_guard "$BASE" "$DISK" "case7"
if [[ "$RC" -eq 2 ]] && grep -q "could not fetch" <<<"$OUT" \
   && grep -q "nothing to compare against" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "server down -> rc=2, no manifest means nothing to compare (rc=$RC)"

start_server "$SERVED"
BASE="http://127.0.0.1:$SRV_PORT/"

# 8. Empty disk tree while the binary carries 5 -> every path is only-in-binary
#    -> rc=2. The round-1 "known limit" (a stale binary serving files the tree
#    no longer has) is now VISIBLE, which is the point of this inversion.
mkdir -p "$TMP/empty"
run_guard "$BASE" "$TMP/empty" "case8"
if [[ "$RC" -eq 2 ]] \
   && grep -q "IN BINARY, NOT ON DISK -- app.js" <<<"$OUT" \
   && grep -q "5 only in the binary" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "binary carries files the tree lacks -> rc=2 naming them (the round-1 limit, now closed) (rc=$RC)"

# 9. STATIC_DIR does not exist -> refuse.
run_guard "$BASE" "$TMP/nope" "case9"
if [[ "$RC" -eq 2 ]] && grep -q "does not exist" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "missing static dir -> rc=2 (rc=$RC)"

# 10. python3 unavailable -> rc=3 TOOLING, refuse rather than skip.
OUT="$(PATH=/nonexistent-dir assert_served_ui_matches_tree "$BASE" "$DISK" "case10" 2>&1)"
RC=$?
if [[ "$RC" -eq 3 ]] && grep -q "TOOLING" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "python3 missing -> rc=3 TOOLING refusal, not a skip (rc=$RC)"

# 11. A "#" in a filename. Go DOES embed "a.js#v" (measured, see
#     tests/results/served_asset_guard/embed_semantics/). An unquoted URL
#     sends it as "a.js" plus a fragment, so the guard would fetch a.js twice
#     and never look at a.js#v. The fixture makes the two files IDENTICAL on
#     disk and only the SERVED a.js#v different -- with differing disk bytes
#     the bug reports a mismatch by accident, on the wrong asset, and the test
#     would pass against it.
HASHDISK="$TMP/hashdisk"; HASHSERVED="$TMP/hashserved"
mkdir -p "$HASHDISK" "$HASHSERVED"
printf 'same\n' > "$HASHDISK/a.js";    printf 'same\n' > "$HASHSERVED/a.js"
printf 'same\n' > "$HASHDISK/a.js#v"
printf 'SERVED-DIFFERENT\n' > "$HASHSERVED/a.js#v"
stop_server
start_server "$HASHSERVED"
run_guard "http://127.0.0.1:$SRV_PORT/" "$HASHDISK" "case11"
if [[ "$RC" -eq 1 ]] && grep -q 'MISMATCH a.js#v' <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "a '#' in a filename is percent-quoted, so its mismatch is SEEN -> rc=1 (rc=$RC)"
stop_server

start_server "$SERVED"
BASE="http://127.0.0.1:$SRV_PORT/"

# 12/13. An unreadable directory and an unreadable file must both be rc=2.
#     Both are simulated with chmod, which root bypasses -- so under root they
#     are reported NOT APPLICABLE by name rather than silently passing.
#     scripts/check.sh, where this test is registered, runs as the developer.
if [[ "$EUID" -ne 0 ]]; then
    chmod 000 "$DISK/lib"
    run_guard "$BASE" "$DISK" "case12"
    chmod 755 "$DISK/lib"
    if [[ "$RC" -eq 2 ]] \
       && grep -qE "COULD NOT CHECK -- (TRAVERSAL|LSTAT)" <<<"$OUT" \
       && grep -q "prevented a complete check" <<<"$OUT"; then ok=true; else ok=false; fi
    report "$ok" "unreadable subdir -> rc=2, not a pass over fewer files (rc=$RC)"

    chmod 000 "$DISK/app.js"
    run_guard "$BASE" "$DISK" "case13"
    chmod 644 "$DISK/app.js"
    if [[ "$RC" -eq 2 ]] \
       && grep -q "COULD NOT CHECK -- DISK READ app.js" <<<"$OUT" \
       && ! grep -q "Traceback" <<<"$OUT"; then ok=true; else ok=false; fi
    report "$ok" "unreadable file -> rc=2 CANNOT VERIFY, no traceback, never rc=1 (rc=$RC)"

    # The round-2 reproduction: a readable but NOT searchable directory.
    # scandir lists it, so os.walk's onerror never fires, but every stat
    # inside raises EACCES -- and os.path.isfile()/islink() report that as a
    # plain False, which is how an asset left the list without a trace.
    chmod 444 "$DISK/lib"
    run_guard "$BASE" "$DISK" "case14"
    chmod 755 "$DISK/lib"
    if [[ "$RC" -eq 2 ]] && grep -q "prevented a complete check" <<<"$OUT"; then ok=true; else ok=false; fi
    report "$ok" "readable-but-not-searchable subdir (chmod 444) -> rc=2 (rc=$RC)"
else
    echo "  NOT APPLICABLE as root (uid 0 bypasses chmod): cases 12, 13, 14" \
         "-- live under scripts/check.sh, which runs as the developer"
fi

# 15. A server that ACCEPTS and never answers -> rc=2 within seconds, a
#     refusal rather than a hang.
stop_server
SRV_STARTED=$((SRV_STARTED + 1))
python3 - "$TMP/stall.port" "$PIDDIR" >/dev/null 2>&1 <<'PYEOF' &
import os
import socket
import sys

with open(os.path.join(sys.argv[2], str(os.getpid())), "w") as _fh:
    _fh.write(str(os.getpid()))
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 0))
srv.listen(8)
with open(sys.argv[1] + ".tmp", "w") as fh:
    fh.write(str(srv.getsockname()[1]))
os.rename(sys.argv[1] + ".tmp", sys.argv[1])
held = []
while True:
    c, _ = srv.accept()
    held.append(c)
PYEOF
STALL_PID=$!
SRV_ALL+=("$STALL_PID")
n=0
while [[ ! -s "$TMP/stall.port" && $n -lt 100 ]]; do sleep 0.05; n=$((n + 1)); done
STALL_PORT="$(cat "$TMP/stall.port" 2>/dev/null)"
t0=$SECONDS
OUT="$(PGWT_SERVED_ASSET_TIMEOUT_S=1 assert_served_ui_matches_tree \
        "http://127.0.0.1:$STALL_PORT/" "$DISK" "case15" 2>&1)"
RC=$?
elapsed=$((SECONDS - t0))
kill "$STALL_PID" 2>/dev/null; wait "$STALL_PID" 2>/dev/null
if [[ "$RC" -eq 2 ]] && grep -q "could not fetch" <<<"$OUT" && [[ "$elapsed" -lt 30 ]]; then ok=true; else ok=false; fi
report "$ok" "server accepts but never answers -> rc=2 within ${elapsed}s, a refusal not a hang (rc=$RC)"

start_server "$SERVED"
BASE="http://127.0.0.1:$SRV_PORT/"

# 16. A malformed timeout override refuses rather than silently defaulting.
OUT="$(PGWT_SERVED_ASSET_TIMEOUT_S=banana assert_served_ui_matches_tree "$BASE" "$DISK" "case16" 2>&1)"
RC=$?
if [[ "$RC" -eq 2 ]] && grep -q "not a positive number" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "malformed PGWT_SERVED_ASSET_TIMEOUT_S -> rc=2, never a silent default (rc=$RC)"
stop_server

# 17-20. The manifest itself is the new single point of trust, so every way it
#     can be useless must refuse. An older bridge with no endpoint is the one
#     that will actually happen.
start_server "$SERVED" none
run_guard "http://127.0.0.1:$SRV_PORT/" "$DISK" "case17"
if [[ "$RC" -eq 2 ]] && grep -q "returned HTTP 404" <<<"$OUT" \
   && grep -q "make pgwt-client" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "a bridge with NO /embedded-assets endpoint -> rc=2 with a rebuild hint (rc=$RC)"
stop_server

start_server "$SERVED" badjson
run_guard "http://127.0.0.1:$SRV_PORT/" "$DISK" "case18"
if [[ "$RC" -eq 2 ]] && grep -q "did not return a usable asset list" <<<"$OUT" \
   && grep -q "not an empty one" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "an unparseable manifest -> rc=2, never read as 'no assets' (rc=$RC)"
stop_server

start_server "$SERVED" empty
run_guard "http://127.0.0.1:$SRV_PORT/" "$DISK" "case19"
if [[ "$RC" -eq 2 ]] && grep -q "reports ZERO embedded assets" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "a manifest claiming zero assets -> rc=2, an empty subject is never a pass (rc=$RC)"
stop_server

start_server "$SERVED" countskew
run_guard "http://127.0.0.1:$SRV_PORT/" "$DISK" "case20"
if [[ "$RC" -eq 2 ]] && grep -q "contradicts itself" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "a manifest whose count disagrees with its own list -> rc=2 (rc=$RC)"
stop_server

# 20b. ROUND-5 BLOCKER 1: a manifest that lists a path TWICE. Measured
#     against the previous commit: "OK -- embedded=4 compared=4 matched=4"
#     over THREE distinct files, rc 0. The count invariant was satisfied by
#     counting one file twice, so every number the guard printed was false. I
#     had argued from the code that duplicates could not produce a false pass;
#     they could, and the measurement is the reason this case exists.
DUPDISK="$TMP/dupdisk"; DUPSERVED="$TMP/dupserved"
mkdir -p "$DUPDISK" "$DUPSERVED"
for i in 1 2 3; do
    printf 'content %d\n' "$i" > "$DUPDISK/f$i.js"
    cp "$DUPDISK/f$i.js" "$DUPSERVED/f$i.js"
done
DUPMANI="$TMP/dupmanifest.txt"
printf 'f1.js\nf1.js\nf2.js\nf3.js\n' > "$DUPMANI"
start_server "$DUPSERVED" fromfile "$DUPMANI"
run_guard "http://127.0.0.1:$SRV_PORT/" "$DUPDISK" "case20b"
if [[ "$RC" -eq 2 ]] \
   && grep -q "only 3 are distinct" <<<"$OUT" \
   && grep -q "duplicated: f1.js" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "a manifest listing a path twice -> rc=2 naming it (was rc=0 with inflated counts) (rc=$RC)"
stop_server

# 20c. ROUND-5 BLOCKER 4: a file named like the manifest endpoint. The
#     ServeMux pattern wins, so its real bytes are unreachable -- and
#     comparing the handler's JSON against that file PASSES whenever the two
#     happen to be equal, stale embedded contents and all. Unverifiable is
#     not verified, so refuse by name.
#     The fixture is built to be the WORST case rather than a convenient one:
#     the disk file is given the handler's EXACT response bytes (fetched from
#     the running server), so a guard that just hashes what it gets back
#     MATCHES and passes. A fixture whose bytes merely differed would be
#     caught as a plain mismatch and would prove nothing.
SHADOWDISK="$TMP/shadowdisk"; SHADOWSERVED="$TMP/shadowserved"
mkdir -p "$SHADOWDISK" "$SHADOWSERVED"
printf 'top\n' > "$SHADOWDISK/app.js";          printf 'top\n' > "$SHADOWSERVED/app.js"
printf 'real embedded bytes nobody can read\n' > "$SHADOWSERVED/embedded-assets"
SHADOWMANI="$TMP/shadowmanifest.txt"
printf 'app.js\nembedded-assets\n' > "$SHADOWMANI"
start_server "$SHADOWSERVED" fromfile "$SHADOWMANI"
# the handler's own response, byte for byte, becomes the disk file
curl -fsS "http://127.0.0.1:$SRV_PORT/embedded-assets" > "$SHADOWDISK/embedded-assets"
run_guard "http://127.0.0.1:$SRV_PORT/" "$SHADOWDISK" "case20c"
if [[ "$RC" -eq 2 ]] && grep -q "which the manifest endpoint shadows" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "an embedded file named 'embedded-assets' is unverifiable -> rc=2, not a pass (rc=$RC)"
stop_server

# 21. ROUND-4's EXACT REPRODUCTION: a DANGLING static/mod/go.mod symlink.
#     lstat() on it succeeds, so the old Python replica pruned mod/ as a
#     nested module -- while Go 1.26 embedded mod/x.js anyway. With stale
#     served bytes for mod/x.js the old guard returned rc=0 with counts
#     1/1/1: the exclusion had shrunk the denominator, so the count invariant
#     could not see the loss.
#
#     The new design's correct answer is rc=2, not rc=1, and deliberately so:
#     the binary's set and the disk set differ (mod/go.mod is on disk and not
#     in the manifest), which is unreconcilable WITHOUT re-deriving Go's
#     rules -- the very thing that caused these bugs. Cannot-verify outranks
#     mismatch. The stale mod/x.js is still hashed and its MISMATCH line is
#     still printed, so the log names both facts; only the verdict is the
#     stricter one.
MODDISK="$TMP/moddisk"; MODSERVED="$TMP/modserved"
mkdir -p "$MODDISK/mod" "$MODSERVED/mod"
printf 'top\n' > "$MODDISK/app.js";        printf 'top\n' > "$MODSERVED/app.js"
printf 'fresh\n' > "$MODDISK/mod/x.js";    printf 'STALE\n' > "$MODSERVED/mod/x.js"
ln -sf /nonexistent-target "$MODDISK/mod/go.mod"      # dangling
MODMANI="$TMP/modmanifest.txt"
printf 'app.js\nmod/x.js\n' > "$MODMANI"              # what Go 1.26 embeds
start_server "$MODSERVED" fromfile "$MODMANI"
run_guard "http://127.0.0.1:$SRV_PORT/" "$MODDISK" "case21"
if [[ "$RC" -eq 2 ]] \
   && grep -q "ON DISK, NOT IN BINARY -- mod/go.mod" <<<"$OUT" \
   && grep -q "MISMATCH mod/x.js" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "dangling mod/go.mod symlink + stale mod/x.js -> rc=2 naming BOTH (was rc=0 1/1/1) (rc=$RC)"
stop_server

# 22. The same tree WITHOUT the dangling symlink must still see the stale
#     asset as a plain mismatch -- so case 21's rc=2 is the set difference
#     talking, not the guard losing track of mod/x.js.
rm -f "$MODDISK/mod/go.mod"
start_server "$MODSERVED" fromfile "$MODMANI"
run_guard "http://127.0.0.1:$SRV_PORT/" "$MODDISK" "case22"
if [[ "$RC" -eq 1 ]] && grep -q "MISMATCH mod/x.js" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "remove the dangling symlink and the same stale asset is rc=1 MISMATCH (rc=$RC)"
stop_server

# 23. FALSE-REFUSAL regression on the REAL tree: this repo's actual web/static
#     (59 assets, vendored minified JS, nested dirs) served byte-for-byte with
#     a manifest to match must be rc=0. A refusal here would abort every
#     live-tier run, which is the expensive direction.
REAL_STATIC="$(cd "$SCRIPT_DIR/.." && pwd)/web/static"
if [[ -d "$REAL_STATIC" ]]; then
    n_real="$(python3 - "$REAL_STATIC" <<'PYEOF'
import os
import sys
n = 0
for dirpath, dirnames, filenames in os.walk(sys.argv[1]):
    n += len(filenames)
print(n)
PYEOF
)"
    start_server "$REAL_STATIC"
    run_guard "http://127.0.0.1:$SRV_PORT/" "$REAL_STATIC" "case23"
    if [[ "$RC" -eq 0 ]] \
       && grep -q "OK -- embedded=$n_real compared=$n_real matched=$n_real" <<<"$OUT"; then ok=true; else ok=false; fi
    report "$ok" "the REAL web/static ($n_real assets) served byte-for-byte -> rc=0 with embedded=compared=matched (rc=$RC)"
    stop_server
fi

# 24. ROUND-5 BLOCKER 2/3: the survivor scan must find a server the SHELL
#     never recorded. Start one, then deliberately lose every shell-side
#     trace of it (exactly what an interrupt between `&` and the bookkeeping
#     produces) and require the authoritative scan to still see it. If this
#     only consulted SRV_ALL it would report "nothing leaked" while a server
#     ran -- "I recorded nothing" read as "nothing survived".
start_server "$SERVED"
forgotten_pid="$SRV_PID"
SRV_PID=""
SRV_ALL=()                       # shell bookkeeping deliberately destroyed
found=""
for p in $(survivors_by_scan); do
    [[ "$p" == "$forgotten_pid" ]] && found="$p"
done
if [[ -n "$found" ]] && is_ours "$forgotten_pid"; then ok=true; else ok=false; fi
report "$ok" "a server with NO shell-side record is still found by the \$TMP scan (pid ${forgotten_pid:-none})"
# ... and is identified positively, not by liveness alone: a pid that is not
# ours must not be claimed. $$ (this shell) is alive but is not a server.
if is_ours "$$"; then ok=false; else ok=true; fi
report "$ok" "is_ours() does not claim an unrelated live pid (this shell, $$)"
stop_pid "$forgotten_pid" 5
SRV_ALL=("$forgotten_pid")       # restore the audit's expectations
if is_ours "$forgotten_pid"; then ok=false; else ok=true; fi
report "$ok" "the forgotten server is reaped by pid once the scan has named it"

# ── reachability: a guard nobody calls is the defect, not the fallback ──────
wiring_ok() {   # $1 = script, $2 = the walk script that must be invoked AFTER
    local f="$1" walk="$2"
    local call_line walk_line
    call_line="$(grep -nE '^[[:space:]]*(if ! )?assert_served_ui_matches_tree' "$f" \
        | head -1 | cut -d: -f1)"
    # The INVOCATION, not the header comment that names the same file.
    walk_line="$(grep -nE "^[[:space:]]*python3 .*${walk//./\\.}" "$f" \
        | head -1 | cut -d: -f1)"
    [[ -n "$call_line" && -n "$walk_line" && "$call_line" -lt "$walk_line" ]]
}

for pair in "ui_live_smoke.sh:ui_live_smoke.py" "demo_rehearsal.sh:demo_rehearsal.py"; do
    f="$SCRIPT_DIR/${pair%%:*}"
    walk="${pair##*:}"
    if [[ -f "$f" ]] && wiring_ok "$f" "$walk"; then ok=true; else ok=false; fi
    report "$ok" "${pair%%:*} calls the guard BEFORE $walk (an uncalled guard is the bug)"
done

# The wiring check must FAIL when the call is removed -- otherwise the two
# cases above are greps that can only ever pass.
stripped="$TMP/stripped_ui_live_smoke.sh"
grep -v 'assert_served_ui_matches_tree' "$SCRIPT_DIR/ui_live_smoke.sh" > "$stripped"
if wiring_ok "$stripped" "ui_live_smoke.py"; then ok=false; else ok=true; fi
report "$ok" "the wiring check itself goes RED on a copy with the call deleted"

# web/main.go must actually expose the endpoint the guard depends on.
MAIN_GO="$(cd "$SCRIPT_DIR/.." && pwd)/web/main.go"
if [[ -f "$MAIN_GO" ]] && grep -q '"/embedded-assets"' "$MAIN_GO" \
   && grep -q 'fs.WalkDir' "$MAIN_GO"; then ok=true; else ok=false; fi
report "$ok" "web/main.go serves /embedded-assets by walking the embedded FS"

# Leaked-process guard (mirrors tests/ui_live_smoke.sh's own). Round 5: the
# first version of this check could PASS BLIND -- with empty or incomplete pid
# tracking it reported success, unable to tell "nothing leaked" from "I
# recorded nothing". It now has to prove it looked:
#   * the number of pids RECORDED must equal the number of servers STARTED
#     (counted independently, before each spawn), else the bookkeeping is
#     incomplete and the answer is unknown -> fail;
#   * the authoritative survivor list comes from scanning for this run's
#     unique $TMP in process command lines, which does not depend on that
#     bookkeeping at all;
#   * a pid is only treated as ours after positive identification, because
#     pids are recycled and killing or blaming a stranger is its own bug;
#   * if the scanning tool is missing, that is "could not look" -> fail, not
#     a pass.
stop_server
recorded=0
for p in $(ls "$PIDDIR" 2>/dev/null); do recorded=$((recorded + 1)); done
leak_problems=()
if ! command -v pgrep >/dev/null 2>&1; then
    leak_problems+=("pgrep unavailable: cannot scan for survivors")
fi
if [[ "$recorded" -ne "$SRV_STARTED" ]]; then
    leak_problems+=("recorded $recorded pid(s) but started $SRV_STARTED server(s): bookkeeping incomplete, survivors unknowable")
fi
leaked=()
for p in $(survivors_by_scan); do
    is_ours "$p" && leaked+=("$p")
done
for p in "${SRV_ALL[@]:-}"; do
    [[ -n "$p" ]] || continue
    if is_ours "$p"; then
        [[ " ${leaked[*]:-} " == *" $p "* ]] || leaked+=("$p")
    fi
done
if [[ "${#leaked[@]}" -eq 0 && "${#leak_problems[@]}" -eq 0 ]]; then ok=true; else ok=false; fi
report "$ok" "no test HTTP server outlived the run (started=$SRV_STARTED recorded=$recorded, survivors: ${leaked[*]:-none}${leak_problems:+; ${leak_problems[*]}})"

echo ""
echo "test_served_asset_guard: $tests_passed/$tests_run passed, $tests_failed failed"
[[ "$tests_failed" -eq 0 ]]
