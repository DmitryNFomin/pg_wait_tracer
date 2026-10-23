#!/bin/bash
# test_trace_sidecar_perms.sh — #499 and #500, end to end against a real
# daemon: after a traced run, is the wait-event name sidecar there, and can a
# non-root process actually open the trace?
#
# Why a live test and not only a unit test:
#
#   #499 is a startup-ORDER bug. src/discovery.c writes
#   trace/wait_event_names.json at the end of pgwt_discover(); the mkdir that
#   creates the trace directory used to live only in pgwt_writer_init(), which
#   pgwt_daemon_init() calls afterwards. No unit test can see that — only the
#   real binary's startup sequence can. And the reason it survived this long is
#   that every existing live test creates its trace dir with `mktemp -d` and
#   passes THAT to -T, so the directory already exists and the sidecar lands.
#   This test deliberately points -T at a path that does not exist yet, which
#   is what a first daemon start (and the demo walk) does.
#
#   #500 is about reader ACCESS, not mode bits. The default trace group was the
#   literal string "dba"; on a host without it getgrnam() failed and every
#   trace file was written root:root 0640. The mode is 0640 in both the broken
#   and the fixed case, so a mode-bit assertion proves nothing: this test drops
#   to a real non-root account in the resolved trace group and opens the files.
#   The negative control is in the same directory — a root:root 0640 file the
#   same child must still be refused, which is what keeps the reader check from
#   passing because the child silently kept root, or because something widened
#   the mode to world-readable.
#
# Requires: root, running PostgreSQL, python3.
# Usage: sudo tests/test_trace_sidecar_perms.sh [--pid POSTMASTER_PID]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRACER="$SCRIPT_DIR/../pg_wait_tracer"
source "$SCRIPT_DIR/testutil.sh"

PM_PID=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID]"; exit 1 ;;
    esac
done

echo "=== test_trace_sidecar_perms ==="

if [[ $(id -u) -ne 0 ]]; then
    echo "FAIL: needs root (the reader check chowns to a foreign group and"
    echo "      drops privileges to a non-root account)"
    exit 1
fi
if [[ ! -x "$TRACER" ]]; then
    echo "FAIL: $TRACER not built"
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "FAIL: python3 is required for the privilege-dropping reader"
    exit 1
fi
if [[ -z "$PM_PID" ]]; then
    PM_PID=$(find_postmaster)
    if [[ -z "$PM_PID" ]]; then
        echo "FAIL: cannot find postmaster PID"
        exit 1
    fi
fi
echo "Postmaster PID: $PM_PID"

passed=0
failed=0
check() {
    if [[ "$1" -eq 0 ]]; then
        echo "  PASS: $2"
        passed=$((passed + 1))
    else
        echo "  FAIL: $2"
        failed=$((failed + 1))
    fi
}

BASE=$(mktemp -d /tmp/pgwt_sidecar_XXXXXX)
# $BASE stands in for the operator's own directory (INSTALL.md's
# /var/lib/pgwt, mode 0755); mktemp -d makes it 0700 root:root, which would
# deny the reader at the PARENT level and so test the scaffolding instead of
# the product. The daemon owns $TRACE_DIR and the files in it — nothing above
# it — and must never widen a directory the operator created. Measured: with
# $BASE left at 0700 the reader child gets EACCES (errno 13) on a trace file
# that is correctly root:postgres 0640 inside a correctly root:postgres 0750
# trace dir.
chmod 0755 "$BASE" || { echo "FAIL: cannot chmod $BASE"; exit 1; }
# NOT created on purpose: a first daemon start against a fresh trace dir is
# the case #499 broke, and the case every other live test accidentally avoids.
TRACE_DIR="$BASE/trace"
LOG="$BASE/daemon.log"
SOCK="$TRACE_DIR/pgwt.sock"
TRACER_PID=""

cleanup() {
    if [[ -n "$TRACER_PID" ]] && kill -0 "$TRACER_PID" 2>/dev/null; then
        kill -TERM "$TRACER_PID" 2>/dev/null
        wait "$TRACER_PID" 2>/dev/null
    fi
    rm -rf "$BASE"
}
trap cleanup EXIT

[[ ! -e "$TRACE_DIR" ]]
check $? "trace dir $TRACE_DIR does not exist before the run"

# Deliberately NO --trace-group: the DEFAULT is what #500 is about.
"$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" -v \
    >/dev/null 2>"$LOG" &
TRACER_PID=$!

# Bounded wait, never an unbounded one: if the daemon dies or never comes up
# this test fails, it does not hang.
for _ in $(seq 1 40); do
    [[ -S "$SOCK" ]] && break
    if ! kill -0 "$TRACER_PID" 2>/dev/null; then
        echo "FAIL: daemon exited during startup:"
        tail -30 "$LOG"
        exit 1
    fi
    sleep 0.5
done
[[ -S "$SOCK" ]]
check $? "daemon came up (control socket present)"
if [[ ! -S "$SOCK" ]]; then
    tail -30 "$LOG"
    exit 1
fi

# The three WARN assertions below are greps for the ABSENCE of a line. An
# empty daemon log would satisfy all three while proving nothing, so the log
# has to be shown to contain something first — `-v` makes the daemon chatty,
# and a silent one here means the greps are blind, not that the warnings are
# gone. (The two assertions that actually carry this test — the sidecar
# existing and the non-root read succeeding — are positive, so they cannot go
# vacuous this way.)
[[ -s "$LOG" ]]
check $? "daemon wrote a -v log ($(wc -l < "$LOG" 2>/dev/null) lines), so the WARN greps below can see"

# ── #499: the name sidecar ────────────────────────────────────
# The sidecar is written at the END of pgwt_discover(), which runs BEFORE the
# control socket exists — so by here the write has already happened or failed.
NAMES="$TRACE_DIR/wait_event_names.json"
[[ -s "$NAMES" ]]
check $? "$NAMES exists and is non-empty (#499)"

if [[ -s "$NAMES" ]]; then
    python3 - "$NAMES" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert isinstance(d.get("pg_version"), int) and d["pg_version"] > 0, d.get("pg_version")
assert d.get("names_source") in ("observed", "static"), d.get("names_source")
assert isinstance(d.get("Timeout"), list) and len(d["Timeout"]) > 1, "Timeout table"
PYEOF
    check $? "sidecar carries pg_version, names_source and the name tables"
fi

! grep -q "could not write .*wait_event_names.json" "$LOG"
check $? "no 'could not write wait_event_names.json' WARN (#499)"

! grep -qE "group '[^']*' (not found|does not exist)" "$LOG"
check $? "no missing-trace-group WARN (#500)"

! grep -q "no default trace group exists" "$LOG"
check $? "a default trace group resolved on this host (#500)"

# ── produce some trace bytes ──────────────────────────────────
for _ in 1 2 3; do
    psql -U postgres -d postgres -tAc \
        "SELECT count(*) FROM generate_series(1,200000); SELECT pg_sleep(0.2);" \
        >/dev/null 2>&1
done

TRACE_FILE=""
for _ in $(seq 1 40); do
    TRACE_FILE=$(find "$TRACE_DIR" -maxdepth 1 -name '*.trace' -size +0 2>/dev/null | head -1)
    [[ -n "$TRACE_FILE" ]] && break
    sleep 0.5
done
[[ -n "$TRACE_FILE" ]]
check $? "a non-empty trace file was written (${TRACE_FILE:-none})"
if [[ -z "$TRACE_FILE" ]]; then
    tail -30 "$LOG"
    exit 1
fi

# ── #500: who owns it, and who can read it ────────────────────
TRACE_GID=$(stat -c %g "$TRACE_FILE")
TRACE_MODE=$(stat -c %a "$TRACE_FILE")
echo "  info: $(basename "$TRACE_FILE") gid=$TRACE_GID mode=$TRACE_MODE group=$(getent group "$TRACE_GID" | cut -d: -f1)"

[[ "$TRACE_GID" != "0" ]]
check $? "trace file is group-owned by a non-root group (gid $TRACE_GID)"

[[ "$TRACE_MODE" == "640" ]]
check $? "trace file mode is 0640, not world-readable (got $TRACE_MODE)"

# The layer the daemon owns: the trace directory itself must be enterable by
# the trace group, or the file mode below is moot. Asserted separately from
# the file so a reader denial is attributable to one layer, not "somewhere in
# the path".
DIR_MODE=$(stat -c %a "$TRACE_DIR")
DIR_GID=$(stat -c %g "$TRACE_DIR")
[[ "$DIR_MODE" == "750" && "$DIR_GID" == "$TRACE_GID" ]]
check $? "trace dir is 0750 group $TRACE_GID (got $DIR_MODE/$DIR_GID)"

# And the layer the operator owns, which this test is standing in for: if
# $BASE were not traversable the reader would be denied above the trace dir
# and the two reader checks below would be red for a reason that is not the
# product's.
BASE_MODE=$(stat -c %a "$BASE")
[[ "$BASE_MODE" == "755" ]]
check $? "scaffolding: $BASE is traversable (0755), got $BASE_MODE"

NAMES_GID=$(stat -c %g "$NAMES" 2>/dev/null || echo "")
[[ "$NAMES_GID" == "$TRACE_GID" ]]
check $? "the sidecar carries the same group as the trace (got '$NAMES_GID')"

# A non-root account in that group. Members come from the group's own member
# list AND from every passwd entry whose primary gid matches.
READER=$(python3 - "$TRACE_GID" <<'PYEOF'
import grp, pwd, sys
gid = int(sys.argv[1])
cands = []
try:
    cands += list(grp.getgrgid(gid).gr_mem)
except KeyError:
    pass
cands += [p.pw_name for p in pwd.getpwall() if p.pw_gid == gid]
for name in cands:
    try:
        if pwd.getpwnam(name).pw_uid != 0:
            print(name)
            break
    except KeyError:
        continue
PYEOF
)
[[ -n "$READER" ]]
check $? "found a non-root member of gid $TRACE_GID (got '${READER:-none}')"
if [[ -z "$READER" ]]; then
    echo "  (a trace group with no non-root member cannot be read by anyone"
    echo "   but root, which is the defect #500 describes)"
    exit 1
fi

# The negative control, in the same directory: a file left exactly the way the
# old root-only default left every trace. The same child must be refused it.
CONTROL="$TRACE_DIR/rootonly.trace"
head -c 64 "$TRACE_FILE" > "$CONTROL"
chown 0:0 "$CONTROL"
chmod 0640 "$CONTROL"

READER_OUT=$(python3 - "$READER" "$TRACE_FILE" "$NAMES" "$CONTROL" <<'PYEOF'
import os, pwd, sys

name, trace, names, control = sys.argv[1:5]
pw = pwd.getpwnam(name)
r, w = os.pipe()
pid = os.fork()
if pid == 0:
    os.close(r)
    try:
        os.setgroups([pw.pw_gid])
        os.setgid(pw.pw_gid)
        os.setuid(pw.pw_uid)
        res = []
        res.append("dropped_root" if os.getuid() != 0 else "STILL_ROOT")
        for label, path in (("trace", trace), ("sidecar", names)):
            try:
                with open(path, "rb") as f:
                    res.append("read_%s_%d" % (label, len(f.read(64))))
            except OSError as e:
                res.append("denied_%s_%s" % (label, e.errno))
        try:
            with open(control, "rb") as f:
                f.read(1)
            res.append("CONTROL_READABLE")
        except PermissionError:
            res.append("control_denied")
        except OSError as e:
            res.append("control_error_%s" % e.errno)
        os.write(w, (" ".join(res)).encode())
    finally:
        os._exit(0)
os.close(w)
out = b""
while True:
    chunk = os.read(r, 4096)
    if not chunk:
        break
    out += chunk
os.waitpid(pid, 0)
print(out.decode().strip())
PYEOF
)
echo "  info: reader($READER) -> ${READER_OUT:-<no output>}"

grep -q "dropped_root" <<<"$READER_OUT"
check $? "reader child really dropped root"

grep -qE "read_trace_[1-9][0-9]*" <<<"$READER_OUT"
check $? "non-root user '$READER' opened and read the trace file (#500)"

grep -qE "read_sidecar_[1-9][0-9]*" <<<"$READER_OUT"
check $? "non-root user '$READER' read wait_event_names.json (#500)"

grep -q "control_denied" <<<"$READER_OUT"
check $? "the root:root 0640 control file stayed unreadable (the check has teeth)"

# ── clean shutdown ────────────────────────────────────────────
kill -TERM "$TRACER_PID" 2>/dev/null
wait "$TRACER_PID" 2>/dev/null
TRACER_PID=""

echo ""
echo "=== test_trace_sidecar_perms: $passed passed, $failed failed ==="
[[ $failed -eq 0 ]]
