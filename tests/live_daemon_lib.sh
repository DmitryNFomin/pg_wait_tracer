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
        # Bounded wait for the KILL to take effect. A process in
        # uninterruptible sleep survives SIGKILL until its syscall returns,
        # and the `wait` below is UNBOUNDED for a child -- so reaping without
        # this poll turns a cleanup helper into a hang, which is the failure
        # mode this file's own callers exist to avoid.
        local k=0
        while kill -0 "$pid" 2>/dev/null && [[ $k -lt 10 ]]; do
            sleep 0.2
            k=$((k + 1))
        done
    fi
    if kill -0 "$pid" 2>/dev/null; then
        # Survived SIGKILL. Report it rather than blocking on `wait`: every
        # caller here runs under `set -uo pipefail` (no -e) and ignores this
        # return, so a non-zero is safe, and a caller that cares can now see
        # that the process is still there.
        return 1
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

# Preflight is NOT a port reservation. bridge_ready checks ownership again
# around the HTTP request, closing the check/start race even if our new PID
# is still alive before bind while an older bridge answers HTTP 200 (#544).
bridge_port_state() {
    python3 "$(dirname "${BASH_SOURCE[0]}")/bridge_owner.py" "$@"
}

bridge_ready() {
    local url="$1" port="$2" pid="$3" rc
    bridge_port_state "$port" "$pid"; rc=$?
    [[ "$rc" == 0 ]] || return "$rc"
    url_ready "$url" || return 1
    bridge_port_state "$port" "$pid"
}

wait_bridge_ready() {
    local url="$1" port="$2" pid="$3" log="$4" rc
    for _ in $(seq 1 60); do
        bridge_ready "$url" "$port" "$pid"; rc=$?
        [[ "$rc" == 0 ]] && return 0
        if [[ "$rc" != 1 ]] || ! kill -0 "$pid" 2>/dev/null; then
            echo "REFUSE bridge startup: port=$port expected PID=$pid (ownership unavailable, foreign listener, or bridge exited)" >&2
            tail -n 40 "$log" >&2
            return 1
        fi
        sleep 0.5
    done
    echo "REFUSE bridge startup: PID=$pid never owned a ready listener on port=$port" >&2
    tail -n 40 "$log" >&2
    return 1
}

# exec preserves $! as the bridge PID and gives its local descendants a
# dedicated process group. Unlike pkill -f, teardown cannot match another
# worktree. Even a bridge that exits on bind failure can leave children;
# stop the group whether or not its leader is still alive.
run_bridge() {
    exec python3 -c 'import os, sys; os.setsid(); os.execv(sys.argv[1], sys.argv[1:])' "$@"
}

stop_bridge() {
    local pid="$1" n
    [[ -n "$pid" ]] || return 0
    kill -s INT -- "-$pid" 2>/dev/null || true
    # Also handles termination between fork and setsid, before the group
    # exists. Never signal the harness's own group.
    stop_pid "$pid" 2 INT
    for n in $(seq 1 10); do
        kill -0 -- "-$pid" 2>/dev/null || return 0
        sleep 0.2
    done
    kill -s KILL -- "-$pid" 2>/dev/null || true
    for n in $(seq 1 10); do
        kill -0 -- "-$pid" 2>/dev/null || return 0
        sleep 0.2
    done
    echo "ERROR: bridge process group $pid survived teardown" >&2
    return 1
}

# assert_served_ui_matches_tree BASE_URL [STATIC_DIR] [LABEL]
# Refuses when the Go bridge is not serving the web/static of the tree under
# test. web/main.go compiles web/static into the binary (`//go:embed static`),
# so a bridge built before a UI edit serves the OLD assets with no outward
# sign: a live-UI-smoke walk -- or a counted demo rehearsal -- then grades the
# wrong UI and reports green, and nothing in the summary afterwards can tell.
# `make pgwt-client` succeeding says only that some binary exists.
#
# THE ASSET LIST COMES FROM THE BINARY, not from a reimplementation of Go's
# rules. Earlier versions of this function derived "what should be embedded"
# by replicating //go:embed's exclusions in Python -- dot/underscore prefixes,
# symlinks, nested modules, go.mod as a file versus a directory -- and every
# gap between the replica and the real rules was a SILENT PASS. Three
# distinct ones were found in review, the last being a dangling
# `static/mod/go.mod` symlink: lstat() succeeded, the replica pruned `mod/` as
# a nested module, Go embedded `mod/x.js` anyway, and the guard reported
# success over a shrunken denominator. No invariant computed after an
# exclusion can see what the exclusion removed. So the replica is gone:
# GET $BASE_URL/embedded-assets returns the paths the running binary actually
# carries (web/main.go), and this function compares that set against the disk
# tree and hashes exactly those paths.
#
# Consequences worth stating plainly:
#   * the round-1 "known limit" is CLOSED. A file deleted from disk that the
#     stale binary still serves is now a visible asymmetric difference, not an
#     invisible one.
#   * the disk walk applies NO exclusion rules at all. Any path present on
#     one side and not the other refuses, including a file Go would not embed
#     (a stray .DS_Store, a symlink, a nested module). That is deliberate: the
#     alternative is re-introducing the replica that caused three silent
#     passes. The refusal names the paths and says to remove the file or
#     rebuild.
#     STATED PLAINLY, because an earlier version of this comment called it a
#     Mac-only annoyance and that was wrong: scripts/box-check.sh's rsync
#     excludes .git, /build, the binaries, tests/results, web/pgwt,
#     __pycache__ and .pgwt-check.stamp -- NOT .DS_Store and not other junk.
#     So a stray file created under web/static on a developer's Mac IS copied
#     to the box and WILL abort the live run there until it is removed. The
#     failure is loud and names the file, but it is a real way to lose a run,
#     and the cheap mitigation (adding junk patterns to that rsync) belongs in
#     scripts/box-check.sh rather than here.
#
# Exit codes, derived ONCE at the end from state:
#   0  the binary's set and the disk set are identical, and
#      embedded == compared == matched with embedded > 0
#   1  MISMATCH: a path present in BOTH sets has different bytes
#   2  CANNOT VERIFY: the manifest could not be fetched or parsed (an older
#      bridge with no /embedded-assets endpoint lands here -- that is "could
#      not look", never a pass), the two sets differ either way, an asset
#      could not be fetched or read, the walk or a stat failed, the manifest
#      is empty, STATIC_DIR is missing, the timeout override is malformed, a
#      count invariant fails, or anything unanticipated raises
#   3  TOOLING: python3 is missing, so the check could not run at all
# CANNOT VERIFY always wins over MISMATCH: an incomplete look is never
# downgraded to a partial pass.
#
# FAIL-CLOSED BY CONSTRUCTION: every failure from any source lands in one
# append-only blocked[] list; stat is os.lstat(), which raises, never
# os.path.isfile()/islink()/isdir(), which report "I could not look" as a
# plain False; the exit code is computed from blocked[] plus the counts at the
# end, never at the point of failure; and the body runs under a catch-all that
# turns any unanticipated exception into 2 (python exits 1 on an uncaught
# exception, which is this function's MISMATCH code, so a crash must not be
# allowed to masquerade as a wrong answer).
#
# PGWT_SERVED_ASSET_TIMEOUT_S (default 15) exists so the timeout path can be
# tested; a non-numeric or non-positive value refuses rather than silently
# reverting to the default.
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
import json
import os
import stat
import sys
import urllib.error
import urllib.parse
import urllib.request

base, root, label = sys.argv[1], sys.argv[2], sys.argv[3]
if not base.endswith("/"):
    base += "/"

MANIFEST = "embedded-assets"

blocked = []


def say(msg):
    print("%s: %s" % (label, msg), flush=True)


def block(kind, where, exc):
    blocked.append("%s %s: %s: %s" % (kind, where, type(exc).__name__, exc))


def run():
    raw_timeout = os.environ.get("PGWT_SERVED_ASSET_TIMEOUT_S", "15")
    try:
        timeout_s = float(raw_timeout)
        if not timeout_s > 0:
            raise ValueError("must be > 0")
    except ValueError as e:
        say("REFUSE served-asset check: CANNOT VERIFY -- PGWT_SERVED_ASSET_TIMEOUT_S=%r "
            "is not a positive number (%s)" % (raw_timeout, e))
        return 2

    # os.stat, not os.path.isdir: isdir() returns False both for "missing" and
    # for "I was not allowed to look", and those deserve different sentences.
    try:
        root_st = os.stat(root)
    except FileNotFoundError:
        say("REFUSE served-asset check: CANNOT VERIFY -- static dir %r does not exist" % root)
        return 2
    except OSError as e:
        say("REFUSE served-asset check: CANNOT VERIFY -- cannot stat static dir %r: %s: %s"
            % (root, type(e).__name__, e))
        return 2
    if not stat.S_ISDIR(root_st.st_mode):
        say("REFUSE served-asset check: CANNOT VERIFY -- static dir %r is not a directory" % root)
        return 2

    # ── 1. what the BINARY says it carries ───────────────────────────────────
    url = base + MANIFEST
    try:
        with urllib.request.urlopen(url, timeout=timeout_s) as r:
            if r.status != 200:
                say("REFUSE served-asset check: CANNOT VERIFY -- %s returned HTTP %s; this "
                    "bridge cannot say which assets it embeds (a bridge built before the "
                    "/%s endpoint existed lands here -- rebuild: make pgwt-client)"
                    % (url, r.status, MANIFEST))
                return 2
            raw = r.read()
    except urllib.error.HTTPError as e:
        # Caught separately and BEFORE the generic handler: urllib RAISES on
        # 404 rather than returning it, so the "older bridge, rebuild" hint --
        # the single most likely reason this check ever fires -- would
        # otherwise be reported as a generic fetch failure.
        say("REFUSE served-asset check: CANNOT VERIFY -- %s returned HTTP %s; this bridge "
            "cannot say which assets it embeds (a bridge built before the /%s endpoint "
            "existed lands here -- rebuild: make pgwt-client)" % (url, e.code, MANIFEST))
        return 2
    except Exception as e:
        say("REFUSE served-asset check: CANNOT VERIFY -- could not fetch %s: %s: %s; without "
            "the binary's own asset list there is nothing to compare against"
            % (url, type(e).__name__, e))
        return 2
    try:
        doc = json.loads(raw.decode("utf-8"))
        listed = doc["paths"]
        if not isinstance(listed, list) or not all(isinstance(p, str) for p in listed):
            raise ValueError("paths is not a list of strings")
        listed = sorted(listed)
    except Exception as e:
        say("REFUSE served-asset check: CANNOT VERIFY -- %s did not return a usable asset "
            "list (%s: %s); an unparseable manifest is not an empty one"
            % (url, type(e).__name__, e))
        return 2
    if not listed:
        say("REFUSE served-asset check: CANNOT VERIFY -- %s reports ZERO embedded assets; "
            "an empty subject is never a pass" % url)
        return 2
    if doc.get("count") is not None and doc["count"] != len(listed):
        say("REFUSE served-asset check: CANNOT VERIFY -- %s reports count=%s but listed %d "
            "paths; the manifest contradicts itself"
            % (url, doc["count"], len(listed)))
        return 2
    # DUPLICATES. Measured, after I argued from the code that they could not
    # produce a false pass and was wrong: a manifest listing one path twice
    # gave "OK -- embedded=4 compared=4 matched=4" over THREE distinct files.
    # The count invariant was satisfied by counting the same file twice, so
    # every number the guard reported was false. Refuse by name, and derive
    # all three counts from DISTINCT paths below so a count can never again be
    # inflated by repetition.
    embedded = sorted(set(listed))
    if len(embedded) != len(listed):
        dups = sorted({p for p in listed if listed.count(p) > 1})
        say("REFUSE served-asset check: CANNOT VERIFY -- %s lists %d paths but only %d are "
            "distinct; duplicated: %s. A manifest that repeats a path cannot be used to "
            "count what was checked" % (url, len(listed), len(embedded), ", ".join(dups)))
        return 2
    # The manifest endpoint SHADOWS any embedded file of the same name: the
    # ServeMux pattern wins, so GET /embedded-assets returns JSON and that
    # file's real bytes are unreachable and therefore unverifiable. Refuse
    # rather than compare the handler's output against a file (which would
    # PASS whenever the two happened to be equal, stale contents and all).
    if MANIFEST in embedded:
        say("REFUSE served-asset check: CANNOT VERIFY -- web/static contains a file named %r, "
            "which the manifest endpoint shadows: its served bytes can never be read, so it "
            "cannot be verified. Rename it." % MANIFEST)
        return 2

    # ── 2. what is on DISK -- no exclusion rules, by design ──────────────────
    def on_walk_error(err):
        block("TRAVERSAL", getattr(err, "filename", root) or root, err)

    on_disk = []
    for dirpath, dirnames, filenames in os.walk(root, onerror=on_walk_error):
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            try:
                os.lstat(full)
            except OSError as e:
                # A stat that fails is a failure to look, not a "no".
                block("LSTAT", full, e)
                continue
            on_disk.append(os.path.relpath(full, root).replace(os.sep, "/"))
    on_disk_set = set(on_disk)
    embedded_set = set(embedded)

    only_disk = sorted(on_disk_set - embedded_set)
    only_binary = sorted(embedded_set - on_disk_set)

    # ── 3. hash exactly what the binary says it has ──────────────────────────
    compared = 0
    matched = 0
    mismatch = []
    for rel in embedded:
        if rel in only_binary:
            continue            # already recorded as an asymmetric difference
        asset_url = base + urllib.parse.quote(rel, safe="/")
        try:
            with open(os.path.join(root, *rel.split("/")), "rb") as fh:
                disk = fh.read()
        except OSError as e:
            block("DISK READ", rel, e)
            continue
        try:
            with urllib.request.urlopen(asset_url, timeout=timeout_s) as r:
                if r.status != 200:
                    blocked.append("FETCH %s (%s): HTTP %s" % (rel, asset_url, r.status))
                    continue
                served = r.read()
        except Exception as e:
            block("FETCH", "%s (%s)" % (rel, asset_url), e)
            continue
        compared += 1
        dh = hashlib.sha256(disk).hexdigest()
        sh = hashlib.sha256(served).hexdigest()
        if dh == sh:
            matched += 1
        else:
            mismatch.append((rel, dh, len(disk), sh, len(served)))

    for rel, dh, dn, sh, sn in mismatch:
        say("served-asset check: MISMATCH %s -- disk sha256=%s (%d bytes), "
            "served sha256=%s (%d bytes)" % (rel, dh, dn, sh, sn))
    for p in only_binary:
        say("served-asset check: IN BINARY, NOT ON DISK -- %s (the bridge still carries a "
            "file this tree no longer has)" % p)
    for p in only_disk:
        say("served-asset check: ON DISK, NOT IN BINARY -- %s (the bridge predates this "
            "file, or Go does not embed it -- remove it from web/static or rebuild)" % p)
    for b in blocked:
        say("served-asset check: COULD NOT CHECK -- %s" % b)

    # ── 4. one place decides, from state ─────────────────────────────────────
    if blocked:
        say("REFUSE served-asset check: CANNOT VERIFY -- %d problem(s) prevented a complete "
            "check (embedded=%d, compared=%d); an incomplete look is never a pass"
            % (len(blocked), len(embedded), compared))
        return 2
    if only_binary or only_disk:
        say("REFUSE served-asset check: CANNOT VERIFY -- the binary's asset set and this "
            "tree's differ: %d only in the binary, %d only on disk (embedded=%d, "
            "on_disk=%d). Rebuild with 'make pgwt-client'; if a listed path is one Go does "
            "not embed, remove it from web/static"
            % (len(only_binary), len(only_disk), len(embedded), len(on_disk_set)))
        return 2
    if compared != len(embedded):
        say("REFUSE served-asset check: CANNOT VERIFY -- compared %d of %d embedded assets "
            "with nothing recorded as blocked; the count invariant failed"
            % (compared, len(embedded)))
        return 2
    if mismatch:
        say("REFUSE served-asset check: MISMATCH -- %d of %d assets differ; the bridge binary "
            "was built from a different web/static than this tree (rebuild: make pgwt-client)"
            % (len(mismatch), len(embedded)))
        return 1
    if matched != len(embedded):
        say("REFUSE served-asset check: CANNOT VERIFY -- %d matched but %d embedded and no "
            "mismatch recorded; the count invariant failed" % (matched, len(embedded)))
        return 2
    say("served-asset check: OK -- embedded=%d compared=%d matched=%d (asset list taken from "
        "the binary's own /%s), all served byte-identically from %s"
        % (len(embedded), compared, matched, MANIFEST, root))
    return 0


try:
    code = run()
except SystemExit:
    raise
except BaseException as e:
    say("REFUSE served-asset check: CANNOT VERIFY -- unexpected error: %s: %s"
        % (type(e).__name__, e))
    code = 2
sys.exit(code)
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
