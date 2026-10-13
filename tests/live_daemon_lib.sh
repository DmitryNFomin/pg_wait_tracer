#!/bin/bash
# live_daemon_lib.sh -- shared helpers for driving a real daemon + real Go
# bridge session against a real PostgreSQL. Sourced (never executed
# directly) by tests/ui_live_smoke.sh (issue #93, ~30 min) and
# tests/demo_rehearsal.sh (issue #157, 30-45 min) so the two scripts share
# one PGPORT-derivation / process-teardown / readiness-polling
# implementation instead of drifting copies (CLAUDE.md "Extend, do not
# fork"). Every function is a plain bash function; nothing here forks a
# background process itself except the readiness probe, so `bash -n` plus
# careful review is what verifies it on the Mac -- like both callers, it
# only actually RUNS on Linux with root + a live PostgreSQL.

# stop_pid PID [BUDGET_S] [SIGNAL]
# Bounded wait: signal, then poll for exit, then KILL if it outlives the
# budget. Mirrors test_capture_smoke.py's terminate_and_wait (TERM + timeout
# + KILL) -- `wait` has no native timeout in bash. sig defaults to TERM.
stop_pid() {
    local pid="$1" budget_s="${2:-10}" sig="${3:-TERM}"
    [[ -z "$pid" ]] && return 0
    kill -0 "$pid" 2>/dev/null || return 0
    kill "-$sig" "$pid" 2>/dev/null
    local steps=$((budget_s * 2))
    local n=0
    while kill -0 "$pid" 2>/dev/null && [[ $n -lt $steps ]]; do
        sleep 0.5
        n=$((n + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null
    fi
    wait "$pid" 2>/dev/null
    return 0
}

# url_ready URL
# GET a URL, exit 0 iff it answers 200. Python (not curl) to avoid adding a
# new external-tool dependency on top of what the rest of the suite needs.
url_ready() {
    python3 - "$1" <<'PYEOF'
import sys, urllib.request
try:
    with urllib.request.urlopen(sys.argv[1], timeout=2) as r:
        sys.exit(0 if r.status == 200 else 1)
except Exception:
    sys.exit(1)
PYEOF
}

# assert_served_ui_matches_tree BASE_URL [STATIC_DIR] [LABEL]
# Refuses when the Go bridge is serving a DIFFERENT web/static than the tree
# under test. web/main.go embeds web/static into the binary (`//go:embed
# static`), so a bridge built before a UI edit keeps serving the old assets
# with no outward sign: a live-tier walk -- or a counted demo rehearsal --
# then passes against the wrong UI and reports green. Nothing else in the
# live tier can see that; `make pgwt-client` succeeding says only that SOME
# binary exists.
#
# Check: sha256 of every asset on disk vs the bytes the bridge actually
# serves for the same path. Not one sampled file -- the whole tree, so the
# guard is meaningful for ANY web/static change, not just the one that
# prompted it. ~59 files / 1.8 MB over localhost, well under a second.
#
# Exit codes are deliberately distinct, because "served bytes differ" and
# "could not look" are different failures and the second must never be
# reported as a match (that is the bug this guard exists to prevent, one
# level up):
#   0  every asset on disk is served byte-identically, and there was at
#      least one asset to check
#   1  MISMATCH: at least one asset's served bytes differ from disk
#   2  CANNOT VERIFY: an asset could not be fetched at all (404, refused
#      connection, timeout), or there are no assets on disk, or STATIC_DIR
#      does not exist. CANNOT VERIFY wins over MISMATCH when both occur --
#      an incomplete look is never downgraded to a partial pass
#   3  TOOLING: python3 is missing, so the check could not run at all
# Every non-zero code names the asset and prints BOTH hashes (or the fetch
# error), so the log says which file and in which direction.
#
# Known limit, stated rather than implied: this enumerates the DISK tree, so
# a stale binary that still serves a file deleted from disk since is not
# detected. Any file added to or modified on disk is.
#
# Mirrors Go's `//go:embed static` pattern rules: a path component starting
# with "." or "_" is NOT embedded (that needs `all:`), so those are skipped
# here too -- otherwise a stray .DS_Store in a Mac-side tree would refuse a
# perfectly current bridge.
assert_served_ui_matches_tree() {
    local base="$1" root="${2:-}" label="${3:-live}"
    if [[ -z "$root" ]]; then
        root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/web/static"
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        echo "$label: REFUSE served-asset check: TOOLING -- python3 not found, cannot verify the bridge serves this tree's UI" >&2
        return 3
    fi
    python3 - "$base" "$root" "$label" <<'PYEOF'
import hashlib
import os
import sys
import urllib.error
import urllib.request

base, root, label = sys.argv[1], sys.argv[2], sys.argv[3]
if not base.endswith("/"):
    base += "/"


def say(msg):
    print("%s: %s" % (label, msg), flush=True)


if not os.path.isdir(root):
    say("REFUSE served-asset check: CANNOT VERIFY -- static dir %r does not exist" % root)
    sys.exit(2)

# Go's //go:embed dir rules: skip any component starting with '.' or '_'.
rels = []
for dirpath, dirnames, filenames in os.walk(root):
    dirnames[:] = [d for d in dirnames if not d.startswith((".", "_"))]
    for fn in filenames:
        if fn.startswith((".", "_")):
            continue
        full = os.path.join(dirpath, fn)
        if not os.path.isfile(full):
            continue
        rels.append(os.path.relpath(full, root))
rels.sort()

if not rels:
    say("REFUSE served-asset check: CANNOT VERIFY -- no embeddable assets found under %s "
        "(an empty subject is never a pass)" % root)
    sys.exit(2)

mismatch, unfetchable = [], []
for rel in rels:
    url = base + rel.replace(os.sep, "/")
    with open(os.path.join(root, rel), "rb") as fh:
        disk = fh.read()
    try:
        with urllib.request.urlopen(url, timeout=15) as r:
            if r.status != 200:
                unfetchable.append((rel, url, "HTTP %s" % r.status))
                continue
            served = r.read()
    except Exception as e:
        unfetchable.append((rel, url, "%s: %s" % (type(e).__name__, e)))
        continue
    dh = hashlib.sha256(disk).hexdigest()
    sh = hashlib.sha256(served).hexdigest()
    if dh != sh:
        mismatch.append((rel, dh, len(disk), sh, len(served)))

for rel, url, err in unfetchable:
    say("served-asset check: COULD NOT FETCH %s (%s) -- %s" % (rel, url, err))
for rel, dh, dn, sh, sn in mismatch:
    say("served-asset check: MISMATCH %s -- disk sha256=%s (%d bytes), served sha256=%s (%d bytes)"
        % (rel, dh, dn, sh, sn))

if unfetchable:
    say("REFUSE served-asset check: CANNOT VERIFY -- %d of %d assets could not be fetched at all; "
        "an unfetchable asset is absence, not a match" % (len(unfetchable), len(rels)))
    sys.exit(2)
if mismatch:
    say("REFUSE served-asset check: MISMATCH -- %d of %d assets differ; the bridge binary was built "
        "from a different web/static than this tree (rebuild: make pgwt-client)"
        % (len(mismatch), len(rels)))
    sys.exit(1)
say("served-asset check: OK -- all %d embeddable assets under %s are served byte-identically"
    % (len(rels), root))
sys.exit(0)
PYEOF
}

# derive_pgport PM_PID [PG_MAJOR] [LABEL]
# Sets and exports PGPORT for postmaster PM_PID. Three tiers, in order:
#   1. PGWT_PGPORT: explicit caller override, always wins.
#   2. Line 4 of the RESOLVED PM_PID's own postmaster.pid, via
#      /proc/$PM_PID/cwd (postmaster's CWD is its data directory) --
#      authoritative for any layout.
#   3. PGWT_PG_PORT_BASE + PG_MAJOR (derived from the exe path if not
#      given), only if postmaster.pid can't be read.
# NEVER trust an ambient/inherited PGPORT -- the gate box sets one in
# /etc/environment, which silently overrode an explicit --pg-version once
# and ran a whole walk against the wrong cluster (an 11/11 false failure).
# Exits 1 (via the caller's own `set -uo pipefail`) if PGPORT cannot be
# derived at all. LABEL prefixes every echo line so a shared log stays
# attributable to the right caller.
derive_pgport() {
    local pm_pid="$1" pg_major="${2:-}" label="${3:-live_daemon}"
    local pg_port_base="${PGWT_PG_PORT_BASE:-5400}"
    if [[ -z "$pg_major" ]]; then
        pg_major=$(readlink "/proc/$pm_pid/exe" 2>/dev/null | grep -oP 'postgresql/\K\d+(?=/)' || true)
    fi
    local pid_port=""
    local pm_cwd
    pm_cwd=$(readlink "/proc/$pm_pid/cwd" 2>/dev/null || true)
    if [[ -n "$pm_cwd" && -f "$pm_cwd/postmaster.pid" ]]; then
        pid_port=$(sed -n '4p' "$pm_cwd/postmaster.pid" 2>/dev/null || true)
        [[ "$pid_port" =~ ^[0-9]+$ ]] || pid_port=""
    fi
    if [[ -n "${PGWT_PGPORT:-}" ]]; then
        export PGPORT="$PGWT_PGPORT"
        echo "$label: PGPORT=$PGPORT (caller override via PGWT_PGPORT)"
    elif [[ -n "$pid_port" ]]; then
        export PGPORT="$pid_port"
        echo "$label: PGPORT=$PGPORT (read from postmaster.pid for PID $pm_pid)"
    elif [[ -n "$pg_major" ]]; then
        export PGPORT=$((pg_port_base + pg_major))
        echo "$label: PG major $pg_major -> PGPORT=$PGPORT (derived: PGWT_PG_PORT_BASE=$pg_port_base + PG$pg_major, postmaster.pid unreadable)"
    else
        echo "$label: ERROR: could not derive PGPORT for PID $pm_pid (postmaster.pid unreadable, no PG major, no PGWT_PGPORT set)" >&2
        exit 1
    fi
}

# check_pgbench_provisioned LABEL LOGFILE
# Fails loudly (never re-`pgbench -i`s) if the box's provisioned pgbench
# tables are missing -- `-i` drops and recreates every pgbench table, which
# would silently destroy the box's provisioned scale-10 dataset for every
# OTHER test sharing it (issue #93 review item 1).
check_pgbench_provisioned() {
    local label="$1" logfile="$2"
    echo "$label: checking for provisioned pgbench tables"
    local rows
    rows=$(psql -U postgres -d postgres -tAc \
        "SELECT count(*) FROM pgbench_accounts" 2>>"$logfile")
    if [[ -z "$rows" || "$rows" -lt 1 ]]; then
        echo "$label: ERROR: pgbench_accounts is missing/empty on PGPORT=$PGPORT --" \
             "tests/provision-runner.sh should have initialized it. Refusing to" \
             "silently 'pgbench -i' here (would destroy the box's provisioned" \
             "scale-10 dataset and change overhead_trend.csv's baseline for" \
             "every other test)."
        tail -n 20 "$logfile"
        exit 1
    fi
    echo "$label: pgbench_accounts has $rows rows (provisioned) -- reusing"
}

# tag_cli_args TAG -- the `--tag` CLI arguments to forward onto the next
# command in the demo-rehearsal chain (scripts/demo-rehearsal.sh ->
# scripts/demo-rehearsal-remote-run.sh -> tests/demo_rehearsal.sh ->
# tests/demo_rehearsal.py --tag), one argument per output line so a caller
# can `mapfile -t ARR < <(tag_cli_args "$TAG")` and splice "${ARR[@]}" into
# a command line. Prints NOTHING when TAG is empty/unset: the caller must
# never synthesize a flag out of an absent tag, because
# demo_rehearsal.py's own `args.tag or _detect_git_tag()` already treats
# "no --tag given" as "try to auto-detect, else record null" -- the
# correct, honest answer (docs/DEMO_REHEARSAL_CRITERIA.md "same tag, not
# merely the same tree") -- and `_detect_git_tag()` cannot even run
# remotely (scripts/demo-rehearsal.sh's rsync excludes .git), so an
# attempt with no tag supplied must still reach summary.json as tag: null,
# not silently acquire one.
#
# Pure function, no Linux/root/live-PostgreSQL dependency unlike the rest
# of this file -- testable directly on the Mac, see
# tests/test_tag_cli_args.sh.
tag_cli_args() {
    local tag="${1:-}"
    if [[ -n "$tag" ]]; then
        printf '%s\n%s\n' "--tag" "$tag"
    fi
}
