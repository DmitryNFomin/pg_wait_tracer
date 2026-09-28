#!/usr/bin/env bash
# ui_mac_walk.sh -- thin runner for tests/ui_mac_walk.py: the FIRST walk of
# the actual demo path (real Chrome + Go bridge on THIS Mac, ssh out to a
# VM running the daemon + PostgreSQL), as opposed to every prior rehearsal,
# which drove headless Chromium and the bridge on the VM itself.
#
# What this script does, in order:
#   1. Provisions ONE throwaway Hetzner VM (or reuses one left up by a
#      previous KEEP=1 invocation -- state file below), same
#      pgwt=gate-snapshot image + tests/provision-runner.sh ubuntu flow
#      scripts/box-check.sh's EPHEMERAL=1 path and scripts/demo-rehearsal.sh
#      already use. Builds the daemon + pgwt-server ON the VM.
#   2. Seeds this Mac's ~/.ssh/known_hosts for the VM's IP -- issue #174
#      applies here too, and worse: web/bridge.go's NewSSHBridge runs
#      `ssh -o BatchMode=yes host serverPath traceDir` with NO
#      StrictHostKeyChecking override at all, so an untrusted host key
#      makes the bridge refuse silently rather than prompt.
#   3. Starts pgbench + the looping lock/sleep workload + the daemon
#      (--mode full) on the VM, backgrounded with nohup + a PID file each
#      (same idiom tests/ui_live_smoke.sh uses locally, just issued over
#      ssh) -- torn down explicitly in this script's own cleanup, never
#      left detached beyond this invocation.
#   4. Builds and starts the Go bridge (web/pgwt) ON THIS MAC, pointed at
#      the VM over ssh.
#   5. Runs tests/ui_mac_walk.py against the Mac-local bridge.
#   6. Tears down in reverse order: bridge, remote daemon/workload/pgbench,
#      then the VM (deleted by id unless KEEP=1, which leaves it up and
#      prints the delete command -- CLAUDE.md "One throwaway VM per agent,
#      reused across rounds").
#
# Does NOT modify tests/demo_rehearsal*.sh/.py or tests/ui_live_smoke*.py --
# this is new code with the same VM-lifecycle SHAPE as scripts/demo-
# rehearsal.sh (not shared via a function, for the same "different enough"
# reason that script gives for not sharing with box-check.sh: single OS,
# a different local/remote split, a different cleanup command set).
#
# Usage:
#   tests/ui_mac_walk.sh                # create/reuse a VM, run once, delete it
#   DRIVER=safari tests/ui_mac_walk.sh  # same VM/build pipeline, but the walk
#                                          itself is tests/ui_mac_walk_safari.py
#                                          (real Safari via safaridriver, full
#                                          screen, gated on the Mac going quiet)
#   KEEP=1 tests/ui_mac_walk.sh          # leave the VM up; a later invocation
#                                          reuses it via the state file
#   tests/ui_mac_walk.sh --delete-vm     # delete a VM a KEEP=1 run left up
#
# Env:
#   DRIVER                   chrome (default) | safari
#   PG_MAJOR                 PostgreSQL major to trace (default 18, matches
#                             the pre-registered demo config)
#   PGWT_UI_MAC_DURATION_S   remote workload/daemon lifetime budget (default
#                             900s -- generous; this script tears them down
#                             itself well before that on a normal run)
#   PGWT_UI_MAC_PORT          local bridge port (default 8385, distinct from
#                             ui_live_smoke's 8384 so both can coexist)
#
# Needs the Hetzner token in the Keychain (same as box-check.sh):
#   security add-generic-password -s hcloud -a claude_token -w <token>
set -uo pipefail
cd "$(dirname "$0")/.."

# live_daemon_lib.sh's stop_pid/url_ready are plain bash/python, portable to
# macOS (only derive_pgport/check_pgbench_provisioned touch /proc and stay
# remote-only, invoked over ssh below instead).
source tests/live_daemon_lib.sh

STATE_FILE="${UI_MAC_WALK_STATE_FILE:-tests/results/.ui-mac-walk-state.sh}"
KEEP="${KEEP:-0}"
DRIVER="${DRIVER:-chrome}"
PG_MAJOR="${PG_MAJOR:-18}"
DURATION_S="${PGWT_UI_MAC_DURATION_S:-900}"
PORT="${PGWT_UI_MAC_PORT:-8385}"
REMOTE_DIR="pgwt-uimacwalk"

ts=$(date +%Y%m%d-%H%M%S)
mkdir -p tests/results
log="tests/results/ui-mac-walk-$ts.log"
: > "$log"

hcloud_token() {
    command -v security >/dev/null 2>&1 || return 1
    security find-generic-password -s hcloud -a claude_token -w 2>/dev/null
}

if [[ "${1:-}" == "--delete-vm" ]]; then
    [[ -f "$STATE_FILE" ]] || { echo "ui-mac-walk: no state file at $STATE_FILE -- nothing to delete"; exit 0; }
    # shellcheck disable=SC1090
    source "$STATE_FILE"
    token="$(hcloud_token || true)"
    [[ -n "$token" ]] || { echo "ui-mac-walk: no Hetzner token in the Keychain" >&2; exit 1; }
    HCLOUD_TOKEN="$token" tests/hetzner-vm.sh delete "$SERVER_ID"
    rm -f "$STATE_FILE"
    exit 0
fi

if ! tests/hetzner-sweep.sh 2>&1 | tee -a "$log"; then
    echo "ui-mac-walk: hetzner-sweep.sh reported a problem (non-fatal, continuing)" | tee -a "$log" >&2
fi

token="$(hcloud_token || true)"
[[ -n "$token" ]] || { echo "ui-mac-walk: no Hetzner token in the Keychain: security add-generic-password -s hcloud -a claude_token -w <token>" | tee -a "$log" >&2; exit 2; }

server_id=""
server_ip=""
server_name=""
reused=0
using_existing_host=0
# PGWT_UI_MAC_HOST takes precedence over any leftover KEEP=1 VM state file:
# without this guard, a stale $STATE_FILE plus PGWT_UI_MAC_HOST set together
# would have the VM-reuse block below populate server_id from the file, then
# the PGWT_UI_MAC_HOST block (further down) silently reset it to "" --
# abandoning that VM with no cleanup, since server_name (the orphan-window
# guard's own fallback) is never set on the state-file path either.
if [[ -z "${PGWT_UI_MAC_HOST:-}" && -f "$STATE_FILE" ]]; then
    # shellcheck disable=SC1090
    source "$STATE_FILE"
    code=$(curl -s -o /dev/null -w '%{http_code}' "https://api.hetzner.cloud/v1/servers/$SERVER_ID" \
        -H "Authorization: Bearer $token")
    if [[ "$code" == "200" ]]; then
        server_id="$SERVER_ID"; server_ip="$SERVER_IP"; reused=1
        echo "ui-mac-walk: reusing VM id=$server_id ip=$server_ip from $STATE_FILE" | tee -a "$log"
    else
        echo "ui-mac-walk: state file pointed at a VM that is gone (HTTP $code) -- discarding it, creating a new one" | tee -a "$log"
        rm -f "$STATE_FILE"
    fi
fi

target=""
bridge_pid=""
cleanup_done=0
ephemeral_cleanup() {
    [[ "$cleanup_done" -eq 1 ]] && return
    cleanup_done=1

    [[ -n "$bridge_pid" ]] && { echo "ui-mac-walk: stopping local bridge (pid $bridge_pid)" | tee -a "$log"; stop_pid "$bridge_pid" 10 INT; }

    if [[ -n "$target" ]]; then
        echo "ui-mac-walk: stopping remote daemon/workload/pgbench" | tee -a "$log"
        ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$target" '
            for f in /tmp/pgwt_uimacwalk_daemon.pid /tmp/pgwt_uimacwalk_workload.pid /tmp/pgwt_uimacwalk_pgbench.pid; do
                [[ -f "$f" ]] || continue
                p=$(cat "$f" 2>/dev/null)
                [[ -n "$p" ]] && kill "$p" 2>/dev/null
            done
            sleep 2
            for f in /tmp/pgwt_uimacwalk_daemon.pid /tmp/pgwt_uimacwalk_workload.pid /tmp/pgwt_uimacwalk_pgbench.pid; do
                [[ -f "$f" ]] || continue
                p=$(cat "$f" 2>/dev/null)
                [[ -n "$p" ]] && kill -9 "$p" 2>/dev/null
            done
            true
        ' >>"$log" 2>&1
    fi

    # Existing-host path (PGWT_UI_MAC_HOST): no VM to delete, but the repo
    # checkout + build artifacts this run rsynced to the box are removed
    # explicitly -- "leave the box exactly as found", the same standard
    # applied to every persistent-box staging directory this session
    # (2026-09-28). Left alone, $REMOTE_DIR is self-healing on the NEXT
    # PGWT_UI_MAC_HOST run (rsync --delete overwrites it), but a box left
    # idle between runs would otherwise carry a full checkout + built
    # binaries indefinitely.
    if [[ "$using_existing_host" == "1" && -n "$target" ]]; then
        echo "ui-mac-walk: removing remote checkout $REMOTE_DIR from $target" | tee -a "$log"
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$target" "rm -rf '$REMOTE_DIR'" >>"$log" 2>&1
    fi

    # ORPHAN-WINDOW GUARD (2026-09-28: found live, this exact gap leaked a
    # VM). server_id is only assigned AFTER `hetzner-vm.sh create`'s command
    # substitution returns -- a SIGTERM delivered while still inside that
    # substitution (VM already created, e.g. waiting for SSH) used to hit
    # the `-z "$server_id"` check below and return immediately, never
    # deleting the VM it had already paid to create. This mirrors the same
    # guard scripts/box-check.sh's EPHEMERAL=1 path and scripts/demo-
    # rehearsal.sh already use -- server_name is set (and pre-declared
    # empty under `set -u`, see above) BEFORE the create call, so it
    # survives an interruption that server_id does not.
    if [[ -z "$server_id" && -n "$server_name" ]]; then
        local lookup_token="$token"
        [[ -z "$lookup_token" ]] && lookup_token="$(hcloud_token || true)"
        if [[ -n "$lookup_token" ]]; then
            echo "ui-mac-walk: server_id not yet known at cleanup time -- looking up '$server_name' by name (orphan-window guard)" | tee -a "$log"
            server_id=$(curl -s "https://api.hetzner.cloud/v1/servers?name=$server_name" \
                -H "Authorization: Bearer $lookup_token" | jq -r '.servers[0].id // empty' 2>/dev/null)
            [[ -n "$server_id" ]] && echo "ui-mac-walk: found $server_name as id=$server_id by name lookup" | tee -a "$log"
        fi
    fi
    [[ -z "$server_id" ]] && return
    if [[ "$KEEP" == "1" ]]; then
        cat > "$STATE_FILE" <<EOF
SERVER_ID=$server_id
SERVER_IP=$server_ip
EOF
        echo "ui-mac-walk: KEEP=1 -- leaving id=$server_id ip=$server_ip up (state saved to $STATE_FILE)." | tee -a "$log"
        echo "  Delete: HCLOUD_TOKEN=\"\$(security find-generic-password -s hcloud -a claude_token -w)\" tests/hetzner-vm.sh delete $server_id" | tee -a "$log"
        return
    fi
    echo "ui-mac-walk: deleting ephemeral server id=$server_id ..." | tee -a "$log"
    HCLOUD_TOKEN="$token" tests/hetzner-vm.sh delete "$server_id" >>"$log" 2>&1
    rc=$?
    rm -f "$STATE_FILE"
    [[ $rc -ne 0 ]] && echo "ui-mac-walk: delete may have failed (rc=$rc) -- check: tests/hetzner-vm.sh list" | tee -a "$log" >&2
}
trap ephemeral_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# PGWT_UI_MAC_HOST: run against an EXISTING box instead of creating a VM.
# Owner rule 2026-09-28 (master's CLAUDE.md, "Persistent boxes FIRST"): both
# gate boxes bill whether or not they run anything, so an idle one is used
# before a throwaway machine is created. With this set no VM is created, none
# is deleted, and ephemeral_cleanup has nothing to do.
if [[ -n "${PGWT_UI_MAC_HOST:-}" ]]; then
    server_ip="${PGWT_UI_MAC_HOST#root@}"
    server_id=""
    reused=1
    using_existing_host=1
    echo "ui-mac-walk: using EXISTING host $server_ip (PGWT_UI_MAC_HOST) -- no VM created" | tee -a "$log"
fi

if [[ "$reused" != "1" ]]; then
    # tr -c replaces hostname -s's own trailing newline with a literal '-'
    # too (the newline is in tr's complement set) -- strip a leading/
    # trailing '-' afterward, same fix scripts/demo-rehearsal.sh already
    # applies to the identical hostname-sanitization idiom, or Hetzner's
    # API rejects the whole create call with "invalid input in field
    # 'labels'" (labels must start/end alphanumeric) -- confirmed live: an
    # untrimmed "owner=mac-" failed VM creation in every location.
    owner="$(hostname -s 2>/dev/null | tr -c 'a-zA-Z0-9.-' '-')"
    owner="${owner#-}"; owner="${owner%-}"
    server_name="pgwt-uimacwalk-$(date +%s)-$RANDOM"
    labels="pgwt=ephemeral,created=$(date +%s),owner=${owner:-mac},task=ui-mac-walk"
    echo "ui-mac-walk: creating $server_name from the pgwt=gate-snapshot image (labels: $labels)" | tee -a "$log"
    create_out=$(HCLOUD_TOKEN="$token" tests/hetzner-vm.sh create \
        --type cx33 --image-snapshot latest --name "$server_name" --labels "$labels" \
        2> >(tee -a "$log" >&2))
    create_rc=$?
    if [[ $create_rc -ne 0 || -z "$create_out" ]]; then
        echo "ui-mac-walk: failed to create VM (rc=$create_rc)" | tee -a "$log" >&2
        exit 1
    fi
    last_line=$(tail -n1 <<<"$create_out")
    server_id=$(awk '{print $1}' <<<"$last_line")
    server_ip=$(awk '{print $2}' <<<"$last_line")
    [[ -n "$server_id" && -n "$server_ip" ]] || { echo "ui-mac-walk: could not parse id/ip from: $create_out" | tee -a "$log" >&2; exit 1; }
    cat > "$STATE_FILE" <<EOF
SERVER_ID=$server_id
SERVER_IP=$server_ip
EOF
    echo "ui-mac-walk: created id=$server_id ip=$server_ip (state saved before provisioning)" | tee -a "$log"
fi

target="root@$server_ip"

# ── Host-key trust, issue #174 applied to the MAC (see top comment) ────────
echo "ui-mac-walk: seeding known_hosts for $server_ip" | tee -a "$log"
ssh-keygen -R "$server_ip" >/dev/null 2>&1 || true
keyscan_ok=0
for _ in $(seq 1 30); do
    if ssh-keyscan -H "$server_ip" >> ~/.ssh/known_hosts 2>>"$log"; then
        keyscan_ok=1
        break
    fi
    sleep 5
done
if [[ $keyscan_ok -ne 1 ]]; then
    echo "ui-mac-walk: ssh-keyscan never succeeded for $server_ip (VM still booting, or unreachable)" | tee -a "$log" >&2
    exit 1
fi

ssh_ok=0
for _ in $(seq 1 30); do
    if ssh -o BatchMode=yes -o ConnectTimeout=5 "$target" true 2>>"$log"; then
        ssh_ok=1
        break
    fi
    sleep 5
done
if [[ $ssh_ok -ne 1 ]]; then
    echo "ui-mac-walk: ssh to $target never became ready" | tee -a "$log" >&2
    exit 1
fi
echo "ui-mac-walk: ssh to $target OK, host key trusted" | tee -a "$log"

# ── Provision + build on the VM (idempotent, see tests/provision-runner.sh) ─
echo "ui-mac-walk: staging repo + running tests/provision-runner.sh ubuntu on $target" | tee -a "$log"
ssh -o BatchMode=yes "$target" "mkdir -p pgwt-provision" 2>>"$log"
rsync -az -e "ssh -o StrictHostKeyChecking=no" ./tests/provision-runner.sh "$target:pgwt-provision/tests-provision-runner.sh" 2>>"$log"
ssh -o BatchMode=yes "$target" "sudo bash pgwt-provision/tests-provision-runner.sh ubuntu" 2>&1 | tee -a "$log"
provision_rc=${PIPESTATUS[0]}
if [[ $provision_rc -ne 0 ]]; then
    echo "ui-mac-walk: provision-runner.sh ubuntu failed (rc=$provision_rc)" | tee -a "$log" >&2
    exit 1
fi

echo "ui-mac-walk: rsyncing repo to $target:$REMOTE_DIR" | tee -a "$log"
ssh -o BatchMode=yes "$target" "mkdir -p '$REMOTE_DIR'" || exit 1
rsync -az --delete -e "ssh -o StrictHostKeyChecking=no" \
    --exclude .git --exclude /build \
    --exclude '/pgwt-server' --exclude '/pgwt-server-asan' \
    --exclude '/pg_wait_tracer' --exclude '/pg_wait_tracer-asan' \
    --exclude 'tests/results' --exclude 'web/pgwt' --exclude '__pycache__' \
    --exclude '.pgwt-check.stamp' \
    ./ "$target:$REMOTE_DIR/" || exit 1

# PGWT_BUILD_VERSION, pinned ONCE here and passed to BOTH builds below --
# found live (2026-09-28): the rsync above excludes .git (deliberately, it
# is large and irrelevant to the build), so a bare remote `make` runs
# Makefile's own `git describe --tags --always --dirty` with no .git
# present, which fails and falls back to the Makefile's own "unknown"
# literal -- while the LOCAL `make pgwt-client` below runs inside this
# Mac's real git worktree and gets a real describe string. That mismatch
# is a GUARANTEED "⚠ version skew" badge (web/static/lib/format.js
# versionSkew) on every run of this script, regardless of which commit is
# actually checked out -- not a real build/commit divergence, an artifact
# of this script's own rsync excludes. Pinning the same string into both
# builds' PGWT_BUILD_VERSION closes it, and is exactly the "something
# enforcing it" a real demo run needs: the bridge and pgwt-server cannot
# embed two different version strings if both are told the same one.
PGWT_BUILD_VERSION_PINNED=$(git describe --tags --always --dirty 2>/dev/null || echo unknown)
echo "ui-mac-walk: pinning PGWT_BUILD_VERSION=$PGWT_BUILD_VERSION_PINNED for both the remote pgwt-server build and the local web/pgwt build" | tee -a "$log"

echo "ui-mac-walk: building daemon + pgwt-server on $target" | tee -a "$log"
ssh -o BatchMode=yes "$target" \
    "cd '$REMOTE_DIR' && ([ -w /tmp/pgwt-box-check.lock ] || sudo sh -c 'touch /tmp/pgwt-box-check.lock && chmod 0666 /tmp/pgwt-box-check.lock') && flock /tmp/pgwt-box-check.lock bash -c 'make -j\$(nproc) PGWT_BUILD_VERSION=$PGWT_BUILD_VERSION_PINNED'" \
    2>&1 | tee -a "$log"
build_rc=${PIPESTATUS[0]}
if [[ $build_rc -ne 0 ]]; then
    echo "ui-mac-walk: remote build failed (rc=$build_rc)" | tee -a "$log" >&2
    exit 1
fi

# ── Build the LOCAL Mac bridge ──────────────────────────────────────────────
echo "ui-mac-walk: building web/pgwt (Mac-local Go bridge)" | tee -a "$log"
if ! make pgwt-client PGWT_BUILD_VERSION="$PGWT_BUILD_VERSION_PINNED" >>"$log" 2>&1; then
    echo "ui-mac-walk: make pgwt-client failed" | tee -a "$log" >&2
    exit 1
fi

# ── Start the remote workload + daemon, all backgrounded with nohup + a PID
# file each, torn down explicitly by ephemeral_cleanup above ────────────────
echo "ui-mac-walk: starting pgbench + lock/sleep workload + daemon (--mode full, PG $PG_MAJOR) on $target" | tee -a "$log"
# Recorded right before the ssh call that starts pgbench/workload/daemon --
# passed to ui_mac_walk_safari.py's --capture-start-epoch so its minimum
# capture-age precondition (2026-09-28 review: a run whose workload had
# barely started produced a near-empty walk regardless of the product) has
# a real start time to measure against, not an omitted one that would
# refuse the walk outright.
REMOTE_CAPTURE_START_EPOCH=$(date +%s)
remote_start_out=$(ssh -o BatchMode=yes "$target" bash -s -- "$PG_MAJOR" "$DURATION_S" "$REMOTE_DIR" <<'REMOTE_EOF'
set -uo pipefail
pg_major="$1"; duration_s="$2"; remote_dir="$3"
cd "$HOME/$remote_dir" || exit 1
source tests/testutil.sh
source tests/live_daemon_lib.sh
PM_PID=$(find_postmaster --pg-version "$pg_major") || { echo "ERROR: no PG$pg_major postmaster found"; exit 1; }
derive_pgport "$PM_PID" "$pg_major" "ui-mac-walk-remote"
check_pgbench_provisioned "ui-mac-walk-remote" /tmp/pgwt_uimacwalk_pgbench_check.log

TRACE_DIR="$HOME/$remote_dir/.pgwt_uimacwalk_trace"
rm -rf "$TRACE_DIR"; mkdir -p "$TRACE_DIR"
rm -f /tmp/pgwt_uimacwalk_{pgbench,workload,daemon}.pid /tmp/pgwt_uimacwalk_{pgbench,workload,daemon}.log

nohup pgbench -U postgres -d postgres -c 4 -T "$duration_s" --rate=25 \
    >/tmp/pgwt_uimacwalk_pgbench.log 2>&1 &
echo $! > /tmp/pgwt_uimacwalk_pgbench.pid
disown
sleep 1
kill -0 "$(cat /tmp/pgwt_uimacwalk_pgbench.pid)" 2>/dev/null || { echo "ERROR: pgbench died immediately"; tail -n20 /tmp/pgwt_uimacwalk_pgbench.log; exit 1; }

nohup python3 tests/live_loop_workload.py "$duration_s" \
    >/tmp/pgwt_uimacwalk_workload.log 2>&1 &
echo $! > /tmp/pgwt_uimacwalk_workload.pid
disown
sleep 2
kill -0 "$(cat /tmp/pgwt_uimacwalk_workload.pid)" 2>/dev/null || { echo "ERROR: workload died immediately"; tail -n40 /tmp/pgwt_uimacwalk_workload.log; exit 1; }

nohup "$HOME/$remote_dir/pg_wait_tracer" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" --mode full -v \
    >/tmp/pgwt_uimacwalk_daemon.log 2>&1 &
echo $! > /tmp/pgwt_uimacwalk_daemon.pid
disown

for _ in $(seq 1 60); do
    [[ -S "$TRACE_DIR/pgwt.sock" ]] && break
    kill -0 "$(cat /tmp/pgwt_uimacwalk_daemon.pid)" 2>/dev/null || { echo "ERROR: daemon died during startup"; tail -n40 /tmp/pgwt_uimacwalk_daemon.log; exit 1; }
    sleep 0.5
done
[[ -S "$TRACE_DIR/pgwt.sock" ]] || { echo "ERROR: control socket never appeared"; tail -n40 /tmp/pgwt_uimacwalk_daemon.log; exit 1; }

echo "OK TRACE_DIR=$TRACE_DIR SERVER_BIN=$HOME/$remote_dir/pgwt-server"
REMOTE_EOF
)
remote_start_rc=$?
echo "$remote_start_out" | tee -a "$log"
if [[ $remote_start_rc -ne 0 ]] || ! grep -q '^OK ' <<<"$remote_start_out"; then
    echo "ui-mac-walk: remote startup failed (rc=$remote_start_rc)" | tee -a "$log" >&2
    exit 1
fi
ok_line=$(grep '^OK ' <<<"$remote_start_out")
# NOT grep -oP: this runs on the MAC, whose /usr/bin/grep is BSD grep with
# no -P/\K support -- confirmed live (2026-09-28): it silently errored
# ("invalid option -- P") and both variables came back empty, which made
# every one of the 11 tabs fail identically ("WebSocket never reached
# #status.connected") because web/pgwt was launched with --trace-dir ""
# --server-path "" and the bridge's `ssh host serverPath traceDir` ran with
# both args blank, causing pgwt-server to fail and the bridge to cycle
# respawn/backoff forever. sed's BRE with a capture group is portable to
# both BSD and GNU sed.
TRACE_DIR=$(sed -n 's/.*TRACE_DIR=\([^ ]*\).*/\1/p' <<<"$ok_line")
SERVER_BIN=$(sed -n 's/.*SERVER_BIN=\([^ ]*\).*/\1/p' <<<"$ok_line")
if [[ -z "$TRACE_DIR" || -z "$SERVER_BIN" ]]; then
    echo "ui-mac-walk: could not parse TRACE_DIR/SERVER_BIN from remote startup output -- refusing to start the bridge with a blank --trace-dir/--server-path (that is exactly what silently cycled respawn/backoff last time). ok_line was: $ok_line" | tee -a "$log" >&2
    exit 1
fi
echo "ui-mac-walk: remote ready -- TRACE_DIR=$TRACE_DIR SERVER_BIN=$SERVER_BIN" | tee -a "$log"

# ── Start the Mac-local bridge, pointed at the VM over ssh ─────────────────
echo "ui-mac-walk: starting web/pgwt on :$PORT against $target" | tee -a "$log"
BRIDGE_LOG=$(mktemp /tmp/pgwt_ui_mac_bridge_XXXXXX.log)
web/pgwt --port "$PORT" --trace-dir "$TRACE_DIR" --server-path "$SERVER_BIN" "$target" \
    >"$BRIDGE_LOG" 2>&1 &
bridge_pid=$!

BASE_URL="http://localhost:$PORT/"
bridge_ok=0
for _ in $(seq 1 60); do
    if url_ready "${BASE_URL}session"; then
        bridge_ok=1
        break
    fi
    kill -0 "$bridge_pid" 2>/dev/null || { echo "ui-mac-walk: bridge exited during startup"; break; }
    sleep 0.5
done
if [[ $bridge_ok -ne 1 ]]; then
    echo "ui-mac-walk: bridge never answered ${BASE_URL}session" | tee -a "$log" >&2
    tail -n 60 "$BRIDGE_LOG" | tee -a "$log" >&2
    exit 1
fi
echo "ui-mac-walk: bridge ready at $BASE_URL" | tee -a "$log"

# ── Let the capture reach the minimum age before walking ────────────────────
# ui_mac_walk_safari.py refuses a walk whose capture is younger than
# --min-capture-age-s (default 120), because a run whose workload had barely
# started produces a near-empty walk regardless of the product -- that
# precondition was added on 2026-09-28 after a run whose AAS chart showed
# activity only in the last ~30s of its window. But this script starts the
# capture and walked immediately, so it could never satisfy its own
# precondition: the first live run refused itself at 7s. Wait the remainder
# here, once, in the foreground -- no polling loop, nothing detached.
MIN_CAPTURE_AGE_S="${PGWT_UI_MAC_MIN_CAPTURE_AGE_S:-120}"
capture_age=$(( $(date +%s) - REMOTE_CAPTURE_START_EPOCH ))
if (( capture_age < MIN_CAPTURE_AGE_S )); then
    wait_s=$(( MIN_CAPTURE_AGE_S - capture_age + 5 ))
    echo "ui-mac-walk: capture is ${capture_age}s old; sleeping ${wait_s}s so it reaches the ${MIN_CAPTURE_AGE_S}s minimum before the walk" | tee -a "$log"
    sleep "$wait_s"
fi

# ── The walk itself ─────────────────────────────────────────────────────────
# DRIVER=chrome (default, Playwright real Chrome) | safari (real Safari via
# safaridriver/WebDriver, tests/ui_mac_walk_safari.py -- full screen, gated
# on the Mac going quiet first, see that script's own docstring).
export PGWT_RUN_MARKER="${PGWT_RUN_MARKER:-$(date +%s)}"
if [[ "$DRIVER" == "safari" ]]; then
    # 127.0.0.1, NOT localhost: Safari persists a per-HOSTNAME page zoom
    # (measured 2026-09-28: ~115% for the hostname "localhost" specifically
    # on this Mac, silently shrinking the achieved fullscreen viewport;
    # 127.0.0.1 measured clean at the un-zoomed size in the same window).
    # web/bridge.go's CheckOrigin already accepts both localhost and
    # 127.0.0.1 origins, so this is a pure test-harness workaround, no
    # product-side change. tests/ui_mac_walk_safari.py's own
    # reset_zoom()/effective_zoom assertion is kept as defense in depth on
    # top of this, not instead of it.
    SAFARI_BASE_URL="http://127.0.0.1:$PORT/"
    python3 tests/ui_mac_walk_safari.py --url "$SAFARI_BASE_URL" \
        --safaridriver-port "${PGWT_UI_MAC_SAFARIDRIVER_PORT:-9994}" \
        --max-quiet-wait-s "${PGWT_UI_MAC_MAX_QUIET_WAIT_S:-300}" \
        --capture-start-epoch "$REMOTE_CAPTURE_START_EPOCH" \
        --min-capture-age-s "${PGWT_UI_MAC_MIN_CAPTURE_AGE_S:-120}"
else
    python3 tests/ui_mac_walk.py --url "$BASE_URL"
fi
walk_rc=$?

echo "ui-mac-walk: bridge log tail:" | tee -a "$log"
tail -n 40 "$BRIDGE_LOG" | tee -a "$log"
rm -f "$BRIDGE_LOG"

echo
echo "ui-mac-walk: exit=$walk_rc -- see tests/results/ui_mac_walk/summary.json and $log"
exit "$walk_rc"
