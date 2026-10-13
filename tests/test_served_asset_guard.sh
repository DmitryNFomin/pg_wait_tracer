#!/usr/bin/env bash
# test_served_asset_guard.sh -- unit test for assert_served_ui_matches_tree()
# in tests/live_daemon_lib.sh, the guard that refuses when the Go bridge is
# serving a web/static DIFFERENT from the tree under test (web/main.go embeds
# web/static into the binary, so a stale bridge serves the old UI silently and
# a live walk -- or a counted demo rehearsal -- passes against the wrong UI).
#
# Hermetic: a throwaway "disk" tree plus a plain python3 http.server serving a
# SEPARATE copy of it. That is a faithful stand-in for a stale bridge, because
# the guard's whole subject is "served bytes vs disk bytes" -- it has no
# opinion about what program is serving them. No Go build, no PostgreSQL, no
# root, so this runs on the Mac via `make check` (scripts/check.sh) and not
# only in the box tier.
#
# The cases below are deliberately weighted toward FALSE NEGATIVES -- the ways
# a guard like this silently approves instead of looking:
#   * the thing being checked is absent (404, dead server, empty tree,
#     missing dir) -- each must REFUSE as "cannot verify", never pass;
#   * the dependency is missing (no python3) -- must refuse, not skip;
#   * the difference is in a LATER file (no short-circuit on the first match);
#   * the difference is only in LENGTH (truncation), which a prefix-ish
#     comparison would miss;
#   * both a mismatch and an unfetchable asset at once -- "cannot verify"
#     must win, so an incomplete look is never downgraded to a partial pass.
#
# Usage: tests/test_served_asset_guard.sh (run from anywhere)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# PGWT_SAG_LIB is a TEST-only override (nothing in the product reads it): it
# lets this file be pointed at an older copy of live_daemon_lib.sh to show a
# regression going RED against the pre-fix function, instead of asserting a
# red that nobody has seen. Production code gets no such hook.
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
cleanup() {
    [[ -n "$SRV_PID" ]] && kill "$SRV_PID" 2>/dev/null
    [[ -n "$SRV_PID" ]] && wait "$SRV_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# ── a disk tree, and a served copy we can corrupt per case ──────────────────
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

start_server() {   # $1 = dir to serve; sets SRV_PORT
    local dir="$1"
    local portfile="$TMP/port.$$"
    rm -f "$portfile"
    python3 - "$dir" "$portfile" <<'PYEOF' &
import functools
import http.server
import os
import sys
import threading

directory, portfile = sys.argv[1], sys.argv[2]
class Quiet(http.server.SimpleHTTPRequestHandler):
    # Subclass, not an attribute poke at a functools.partial: setting
    # log_message on the partial leaves the class's own method in place and
    # the test's output fills with 404 lines.
    def log_message(self, *a, **k):
        pass


handler = functools.partial(Quiet, directory=directory)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
with open(portfile + ".tmp", "w") as fh:
    fh.write(str(srv.server_address[1]))
os.rename(portfile + ".tmp", portfile)
srv.serve_forever()
PYEOF
    SRV_PID=$!
    local n=0
    while [[ ! -s "$portfile" && $n -lt 100 ]]; do
        sleep 0.05
        n=$((n + 1))
    done
    SRV_PORT="$(cat "$portfile" 2>/dev/null)"
    [[ -n "$SRV_PORT" ]] || { echo "  FAIL: test server never reported a port"; exit 1; }
    local m=0
    while ! url_ready "http://127.0.0.1:$SRV_PORT/app.js" && [[ $m -lt 100 ]]; do
        sleep 0.05
        m=$((m + 1))
    done
}

stop_server() {
    [[ -n "$SRV_PID" ]] && kill "$SRV_PID" 2>/dev/null
    [[ -n "$SRV_PID" ]] && wait "$SRV_PID" 2>/dev/null
    SRV_PID=""
}

DISK="$TMP/disk"
SERVED="$TMP/served"
mk_disk "$DISK"
cp -R "$DISK/" "$SERVED/"
start_server "$SERVED"
BASE="http://127.0.0.1:$SRV_PORT/"

run_guard() {   # prints output to $OUT, sets RC
    OUT="$(assert_served_ui_matches_tree "$@" 2>&1)"
    RC=$?
}

# 1. Identical trees -> PASS, and it says how many assets it actually looked
#    at (a count of 0 with a pass would be the vacuous case).
run_guard "$BASE" "$DISK" "case1"
if [[ "$RC" -eq 0 ]] && grep -q "OK -- enumerated=5 compared=5 matched=5" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "identical trees -> rc=0 and the verdict states enumerated=compared=matched=5 (rc=$RC)"

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

# 4. The asset is MISSING from the server (404) -> CANNOT VERIFY, not a pass.
rm -f "$SERVED/vendor/echarts.min.js"
run_guard "$BASE" "$DISK" "case4"
if [[ "$RC" -eq 2 ]] \
   && grep -q "COULD NOT CHECK -- FETCH vendor/echarts.min.js" <<<"$OUT" \
   && grep -q "CANNOT VERIFY" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "asset 404s -> rc=2 CANNOT VERIFY, reported as absence not a match (rc=$RC)"

# 5. Unfetchable AND mismatched at once -> CANNOT VERIFY wins (rc=2), and the
#    mismatch is still printed. An incomplete look must never be downgraded
#    to a partial pass.
printf 'different\n' > "$SERVED/style.css"
run_guard "$BASE" "$DISK" "case5"
if [[ "$RC" -eq 2 ]] \
   && grep -q "MISMATCH style.css" <<<"$OUT" \
   && grep -q "COULD NOT CHECK -- FETCH vendor/echarts.min.js" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "404 + mismatch together -> rc=2 (cannot-verify wins) and both are printed (rc=$RC)"
cp "$DISK/style.css" "$SERVED/style.css"
cp "$DISK/vendor/echarts.min.js" "$SERVED/vendor/echarts.min.js"

# 6. A dotfile / underscore file on disk is NOT embedded by Go, so it must not
#    make a current bridge refuse.
printf 'junk\n' > "$DISK/.DS_Store"
printf 'junk\n' > "$DISK/_notes.txt"
mkdir -p "$DISK/.cache" && printf 'junk\n' > "$DISK/.cache/x.js"
run_guard "$BASE" "$DISK" "case6"
if [[ "$RC" -eq 0 ]] && grep -q "all 5 embeddable assets" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "dot/underscore paths are skipped like //go:embed does -> still rc=0 on 5 assets (rc=$RC)"
rm -rf "$DISK/.DS_Store" "$DISK/_notes.txt" "$DISK/.cache"

# 7. The server is GONE (connection refused) -> CANNOT VERIFY, never a pass.
stop_server
run_guard "$BASE" "$DISK" "case7"
if [[ "$RC" -eq 2 ]] && grep -q "CANNOT VERIFY" <<<"$OUT" && grep -q "COULD NOT CHECK -- FETCH" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "server down -> rc=2 CANNOT VERIFY (rc=$RC)"

# 8. No assets on disk at all -> refuse. An empty subject is the classic
#    vacuous pass; it must be rc=2.
mkdir -p "$TMP/empty"
run_guard "$BASE" "$TMP/empty" "case8"
if [[ "$RC" -eq 2 ]] && grep -q "no embeddable assets" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "empty static dir -> rc=2, an empty subject is never a pass (rc=$RC)"

# 9. STATIC_DIR does not exist -> refuse (not "nothing to check, fine").
run_guard "$BASE" "$TMP/nope" "case9"
if [[ "$RC" -eq 2 ]] && grep -q "does not exist" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "missing static dir -> rc=2 (rc=$RC)"

# 10. python3 unavailable -> rc=3 TOOLING, refuse rather than skip. (A
#     dependency that cannot run is not evidence of a match -- the 126/127
#     case CLAUDE.md calls out.)
OUT="$(PATH=/nonexistent-dir assert_served_ui_matches_tree "$BASE" "$DISK" "case10" 2>&1)"
RC=$?
if [[ "$RC" -eq 3 ]] && grep -q "TOOLING" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "python3 missing -> rc=3 TOOLING refusal, not a skip (rc=$RC)"

# 11. The real tree, if this checkout has one: a current web/static against a
#     server that does NOT have it must refuse rather than pass. Proves the
#     default STATIC_DIR resolution points at this repo's web/static.
REAL_STATIC="$(cd "$SCRIPT_DIR/.." && pwd)/web/static"
if [[ -d "$REAL_STATIC" ]]; then
    start_server "$TMP/empty"
    run_guard "http://127.0.0.1:$SRV_PORT/" "" "case11"
    # Counted with the SAME //go:embed rule the guard uses, in python -- a
    # `find -not -path '*/.*'` filter matches every path under a worktree
    # whose own directory name starts with a dot (.claude/worktrees/...) and
    # silently counts 0.
    n_real="$(python3 - "$REAL_STATIC" <<'PYEOF'
import os
import sys
root = sys.argv[1]
n = 0
for dirpath, dirnames, filenames in os.walk(root):
    dirnames[:] = [d for d in dirnames if not d.startswith((".", "_"))]
    n += len([f for f in filenames if not f.startswith((".", "_"))])
print(n)
PYEOF
)"
    if [[ "$RC" -eq 2 ]] \
       && grep -q "$n_real problem(s) prevented a complete check (enumerated=$n_real, compared=0)" <<<"$OUT"; then ok=true; else ok=false; fi
    report "$ok" "default STATIC_DIR resolves to this repo's web/static ($n_real files) and refuses an empty server (rc=$RC)"
    stop_server
fi

# ── review round 1 blockers: the guard passing while BLIND ──────────────────
# Each case below returned 0 (or a wrong code) before the fix; the pre-fix
# reds are in tests/results/served_asset_guard/review1_red.log, produced by
# running this same file with PGWT_SAG_LIB pointed at the old lib.

# 15. A "#" in a filename. Go embeds "a.js#v" (MEASURED, see
#     tests/results/served_asset_guard/embed_semantics/), but an UNQUOTED URL
#     sends it as "a.js" plus a fragment: the guard fetches a.js twice, never
#     looks at a.js#v, and returns 0 even though the bridge serves different
#     bytes for it.
#
#     The fixture matters. The obvious one -- a.js and a.js#v with DIFFERENT
#     disk bytes -- is passed by the pre-fix code too: it compares a.js's
#     served bytes against a.js#v's disk bytes, which differ, so it reports a
#     mismatch by accident, on the wrong asset. The discriminating fixture is
#     this one: the two files are IDENTICAL on disk and only the SERVED
#     a.js#v differs. Then fetching a.js twice matches twice and the pre-fix
#     guard returns 0 while the bridge is serving something else entirely.
HASHDISK="$TMP/hashdisk"
HASHSERVED="$TMP/hashserved"
mkdir -p "$HASHDISK" "$HASHSERVED"
printf 'same\n' > "$HASHDISK/a.js";    printf 'same\n' > "$HASHSERVED/a.js"
printf 'same\n' > "$HASHDISK/a.js#v"
printf 'SERVED-DIFFERENT\n' > "$HASHSERVED/a.js#v"
start_server "$HASHSERVED"
run_guard "http://127.0.0.1:$SRV_PORT/" "$HASHDISK" "case15"
if [[ "$RC" -eq 1 ]] && grep -q 'MISMATCH a.js#v' <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "a '#' in a filename is percent-quoted, so its mismatch is SEEN -> rc=1 (rc=$RC)"
stop_server

# 16/17. A traversal error and a disk-read error must both be rc=2. Both are
#     simulated with chmod 000, which root bypasses -- so when this runs as
#     root the cases are reported NOT APPLICABLE by name rather than silently
#     skipped or quietly counted as passes. scripts/check.sh (where this test
#     is registered) runs as the developer, never root, which is exactly
#     where the regression is live.
start_server "$SERVED"
BASE="http://127.0.0.1:$SRV_PORT/"
if [[ "$EUID" -ne 0 ]]; then
    # 16: os.walk's default behaviour swallows the scandir error on an
    # unreadable directory and yields a SHORTER list; everything it can still
    # see matches, so the pre-fix guard returned 0 having checked 3 of 5.
    chmod 000 "$DISK/lib"
    run_guard "$BASE" "$DISK" "case16"
    chmod 755 "$DISK/lib"
    if [[ "$RC" -eq 2 ]] \
       && grep -qE "COULD NOT CHECK -- (TRAVERSAL|LSTAT|STAT|DISK READ)" <<<"$OUT" \
       && grep -q "prevented a complete check" <<<"$OUT"; then ok=true; else ok=false; fi
    report "$ok" "unreadable subdir -> rc=2 TRAVERSAL ERROR, not a pass over fewer files (rc=$RC)"

    # 17: an unreadable FILE used to escape as an OSError traceback, which the
    # caller would have seen as rc=1 "the bytes differ" -- a wrong answer
    # rather than a refusal.
    chmod 000 "$DISK/app.js"
    run_guard "$BASE" "$DISK" "case17"
    chmod 644 "$DISK/app.js"
    if [[ "$RC" -eq 2 ]] \
       && grep -qE "COULD NOT CHECK -- (DISK READ|LSTAT) app.js" <<<"$OUT" \
       && grep -q "prevented a complete check" <<<"$OUT" \
       && ! grep -q "Traceback" <<<"$OUT"; then ok=true; else ok=false; fi
    report "$ok" "unreadable file -> rc=2 CANNOT VERIFY, no traceback, never rc=1 (rc=$RC)"
else
    echo "  NOT APPLICABLE as root (uid 0 bypasses chmod 000): cases 16 and 17" \
         "-- they are live under scripts/check.sh, which runs as the developer"
fi

# 22. REVIEW ROUND 2's EXACT REPRODUCTION: a subdirectory that is READABLE
#     but not SEARCHABLE (chmod 444). scandir() lists it fine, so os.walk's
#     onerror never fires -- but every stat inside it raises EACCES, and
#     os.path.isfile()/islink() answer that by returning False. The asset
#     silently left the list and the guard returned 0 having compared 1 of 2.
#     Two rounds of per-call patches did not catch this; the fix is structural
#     (os.lstat, which raises, plus one blocked[] list and a count invariant),
#     so this case is the one that proves the structure, not the patch.
if [[ "$EUID" -ne 0 ]]; then
    NONSEARCH="$TMP/nonsearch"
    NONSEARCH_SRV="$TMP/nonsearch_srv"
    mkdir -p "$NONSEARCH/lib" "$NONSEARCH_SRV/lib"
    printf 'top\n' > "$NONSEARCH/app.js";      printf 'top\n' > "$NONSEARCH_SRV/app.js"
    printf 'inner\n' > "$NONSEARCH/lib/in.js"; printf 'inner\n' > "$NONSEARCH_SRV/lib/in.js"
    start_server "$NONSEARCH_SRV"
    chmod 444 "$NONSEARCH/lib"
    run_guard "http://127.0.0.1:$SRV_PORT/" "$NONSEARCH" "case22"
    chmod 755 "$NONSEARCH/lib"
    stop_server
    if [[ "$RC" -eq 2 ]] && grep -q "prevented a complete check" <<<"$OUT"; then ok=true; else ok=false; fi
    report "$ok" "readable-but-not-searchable subdir (chmod 444, every other asset matching) -> rc=2 (rc=$RC)"
else
    echo "  NOT APPLICABLE as root (uid 0 bypasses chmod 444): case 22" \
         "-- live under scripts/check.sh, which runs as the developer"
fi

# 23. go.mod as a DIRECTORY. MEASURED (go1.26.0, fixture in
#     tests/results/served_asset_guard/embed_semantics/): Go excludes the
#     subtree whether go.mod is a file or a directory, so an isfile()-only
#     test would enumerate assets the bridge does not serve -- a FALSE
#     REFUSAL. The served copy deliberately lacks the subtree.
MODDIR="$TMP/moddir_disk"
mkdir -p "$MODDIR/mod/go.mod"
printf 'top\n' > "$MODDIR/app.js"
printf 'nested\n' > "$MODDIR/mod/x.js"
printf 'inner\n' > "$MODDIR/mod/go.mod/inner.txt"
MODSRV="$TMP/moddir_srv"
mkdir -p "$MODSRV"
printf 'top\n' > "$MODSRV/app.js"
start_server "$MODSRV"
run_guard "http://127.0.0.1:$SRV_PORT/" "$MODDIR" "case23"
stop_server
if [[ "$RC" -eq 0 ]] && grep -q "enumerated=1 compared=1 matched=1" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "a go.mod DIRECTORY excludes its subtree like Go does -> rc=0, no false refusal (rc=$RC)"

# 18. A server that ACCEPTS the connection and never answers. Without a
#     working timeout the guard would hang forever (a stalled gate, not a
#     failing one); with it, this is "could not fetch" = rc=2.
python3 - "$TMP/stall.port" <<'PYEOF' &
import os
import socket
import sys

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
    held.append(c)          # accepted, deliberately never answered
PYEOF
STALL_PID=$!
n=0
while [[ ! -s "$TMP/stall.port" && $n -lt 100 ]]; do sleep 0.05; n=$((n + 1)); done
STALL_PORT="$(cat "$TMP/stall.port" 2>/dev/null)"
t0=$SECONDS
OUT="$(PGWT_SERVED_ASSET_TIMEOUT_S=1 assert_served_ui_matches_tree \
        "http://127.0.0.1:$STALL_PORT/" "$DISK" "case18" 2>&1)"
RC=$?
elapsed=$((SECONDS - t0))
kill "$STALL_PID" 2>/dev/null; wait "$STALL_PID" 2>/dev/null
if [[ "$RC" -eq 2 ]] && grep -q "COULD NOT CHECK -- FETCH" <<<"$OUT" && [[ "$elapsed" -lt 60 ]]; then ok=true; else ok=false; fi
report "$ok" "server accepts but never answers -> rc=2 within ${elapsed}s, a refusal not a hang (rc=$RC)"

# 19. A malformed timeout override must refuse, not silently use the default:
#     a guard that ignores its own configuration has unknown behaviour.
OUT="$(PGWT_SERVED_ASSET_TIMEOUT_S=banana assert_served_ui_matches_tree "$BASE" "$DISK" "case19" 2>&1)"
RC=$?
if [[ "$RC" -eq 2 ]] && grep -q "not a positive number" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "malformed PGWT_SERVED_ASSET_TIMEOUT_S -> rc=2, never a silent default (rc=$RC)"

# 20. FALSE-REFUSAL regression (the expensive direction): a symlink to a
#     regular file and a nested-module subtree exist on disk but are NOT
#     embedded by Go -- measured on go1.26.0 and go1.22.2. Including them
#     would make the guard refuse a perfectly current bridge, i.e. abort every
#     live run. The served copy deliberately does NOT have them.
ln -sf app.js "$DISK/link.js"
mkdir -p "$DISK/mod"
printf 'module nested\n\ngo 1.21\n' > "$DISK/mod/go.mod"
printf 'nested\n' > "$DISK/mod/m.js"
run_guard "$BASE" "$DISK" "case20"
if [[ "$RC" -eq 0 ]] && grep -q "all 5 embeddable assets" <<<"$OUT"; then ok=true; else ok=false; fi
report "$ok" "symlink + nested-module subtree are excluded like Go does -> still rc=0, no false refusal (rc=$RC)"
rm -rf "$DISK/link.js" "$DISK/mod"
stop_server

# 21. FALSE-REFUSAL regression on the REAL tree. Cases 1-20 run on a 5-file
#     fixture; this one points the guard at this repo's actual web/static (59
#     assets, 1.8 MB, vendored minified JS, nested dirs) served byte-for-byte,
#     and requires rc=0. If any of the enumeration rules (dot/underscore,
#     symlink, nested module) or the percent-quoting were wrong for a real
#     asset name, this is where it shows up as a refusal -- and a refusal here
#     means every live-tier run aborts, so it is the expensive direction.
#     NOTE on what this does NOT cover: served by python3, not by a Go bridge,
#     so "the embedded set equals the disk set" is not re-proved here. That is
#     covered by tests/results/served_asset_guard/embed_semantics/ (measured on
#     both toolchains) and by the real-bridge log in the same directory.
REAL_STATIC="$(cd "$SCRIPT_DIR/.." && pwd)/web/static"
if [[ -d "$REAL_STATIC" ]]; then
    start_server "$REAL_STATIC"
    run_guard "http://127.0.0.1:$SRV_PORT/" "$REAL_STATIC" "case21"
    n_real2="$(python3 - "$REAL_STATIC" <<'PYEOF'
import os
import sys
root = sys.argv[1]
n = 0
for dirpath, dirnames, filenames in os.walk(root):
    dirnames[:] = [d for d in dirnames if not d.startswith((".", "_"))]
    n += len([f for f in filenames if not f.startswith((".", "_"))])
print(n)
PYEOF
)"
    if [[ "$RC" -eq 0 ]] \
       && grep -q "OK -- enumerated=$n_real2 compared=$n_real2 matched=$n_real2" <<<"$OUT"; then ok=true; else ok=false; fi
    report "$ok" "the REAL web/static ($n_real2 assets) served byte-for-byte -> rc=0 with enumerated=compared=matched (rc=$RC)"
    stop_server
fi

# ── reachability: a guard nobody calls is the defect, not the fallback ──────
# The cases above prove the function refuses. These prove it is actually
# REACHED in the live tier, before the walk that would otherwise grade the
# wrong UI -- and case 14 proves this very check can go red, so it is not
# itself a vacuous grep.
wiring_ok() {   # $1 = script, $2 = the walk script that must be invoked AFTER
    local f="$1" walk="$2"
    local call_line walk_line
    # The CALL, not a mention: an `if ! assert_...` line, never a comment.
    call_line="$(grep -nE '^[[:space:]]*(if ! )?assert_served_ui_matches_tree' "$f" \
        | head -1 | cut -d: -f1)"
    # The INVOCATION, not the header comment that names the same file (both
    # scripts mention their .py counterpart in their first 20 lines, which is
    # what made the first version of this check fail on correct wiring).
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

# 14. The wiring check must FAIL when the call is removed -- otherwise cases
#     12/13 above are two greps that can only ever pass.
stripped="$TMP/stripped_ui_live_smoke.sh"
grep -v 'assert_served_ui_matches_tree' "$SCRIPT_DIR/ui_live_smoke.sh" > "$stripped"
if wiring_ok "$stripped" "ui_live_smoke.py"; then ok=false; else ok=true; fi
report "$ok" "the wiring check itself goes RED on a copy with the call deleted"

echo ""
echo "test_served_asset_guard: $tests_passed/$tests_run passed, $tests_failed failed"
[[ "$tests_failed" -eq 0 ]]
