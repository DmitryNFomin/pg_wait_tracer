#!/usr/bin/env bash
# box-check.sh — run the LIVE tier (Linux build + C units + synthetic + protocol
# drift + real-PostgreSQL capture) on an x86 Linux box over ssh. Nothing eBPF
# runs on the Mac; this is the definition of done for any capture change.
#
#   make box-check                 # default box ($PGWT_BOX), all PG versions
#   make box-check PG=13           # one PG major (--pg-version)
#   make box-check OS=el8          # $PGWT_BOX_EL8 (Rocky 8 / kernel 4.18)
#   make box-check EPHEMERAL=1     # throwaway Hetzner VM from the gate-box
#                                   snapshot — created, provisioned, run
#                                   against, rsynced back, then ALWAYS
#                                   deleted (issue #141). No PGWT_BOX needed.
#   make box-check EPHEMERAL=1 KEEP=1   # leave the VM up for debugging;
#                                   prints the exact delete command instead
#                                   of deleting it. tests/hetzner-sweep.sh
#                                   will still delete it ~6h later unless
#                                   you raise MAX_AGE_HOURS or delete it
#                                   yourself first.
#
# Env:
#   PGWT_BOX          ssh target for the default (Ubuntu) box, e.g. root@1.2.3.4
#   PGWT_BOX_EL8 / PGWT_BOX_EL9 / PGWT_BOX_UBUNTU   per-OS targets
#   PGWT_BOX_DIR      remote checkout root (default ~/pgwt-check)
#   PGWT_BOX_OUTAGE_SECONDS  total failed-poll/outage budget (default 1800s)
#   PGWT_BOX_POLL_SECONDS    healthy poll interval (default 10s); transport
#                     failures back off 2, 4, 8, 16, then 30s, within that budget
#   PGWT_BOX_RUN_SECONDS     whole detached wait ceiling (default 14400s);
#                     dead worker / ceiling exits 77, transport give-up exits 76
#   PGWT_PGPORT / PGWT_PG_PORT_BASE   forwarded to the remote tests/run_all.sh
#                     (via `sudo env ...`, since sudo's default env_reset would
#                     otherwise drop them) when set on the Mac side; see
#                     tests/run_all.sh and tests/ui_live_smoke.sh for what
#                     each one overrides
#   EPHEMERAL=1       use a throwaway Hetzner VM instead of $PGWT_BOX* (see
#                     above); needs a pgwt=gate-snapshot image for OS.
#                     Needs the Hetzner token from HCLOUD_TOKEN or the macOS Keychain
#                     (`security add-generic-password -s hcloud -a claude_token -w <token>`).
#   KEEP=1            (EPHEMERAL=1 only) do not delete the VM at the end;
#                     print the delete command instead.
#   PGWT_SNAPSHOT_IMAGE  (EPHEMERAL=1 only) use this exact Hetzner image id
#                     instead of resolving "latest" by the pgwt=gate-snapshot
#                     label. For pinning to a specific snapshot (reproducing
#                     a past run) or for testing a candidate image BEFORE it
#                     is promoted to the live gate-snapshot label -- the
#                     promotion itself is a separate, deliberate step (a
#                     label update), not a precondition for proving the
#                     image works end to end.
#
# Ephemeral-VM deletion guarantee (issue #141 — "a throwaway VM must never
# be forgotten"):
#   1. `trap ephemeral_cleanup EXIT` plus DEDICATED `trap '...; exit N' INT
#      TERM HUP` handlers (registered before the network call that creates
#      the VM) all run the same cleanup. Signal-specific, not a bare `trap
#      ephemeral_cleanup EXIT INT TERM`: bash does NOT terminate a
#      non-interactive script on an already-trapped signal by itself — once
#      you `trap` INT/TERM, resuming (or not) after the handler is the
#      script's own job, and a handler that doesn't call `exit` just lets
#      the script keep running from wherever it was interrupted (a second
#      Ctrl-C would then be a no-op, guarded by `$cleanup_done`). HUP is
#      trapped too: a closed terminal / dropped ssh session delivers SIGHUP
#      to this script with no EXIT trap guaranteed to fire otherwise.
#   2. Orphan-window guard: `$server_id` is only assigned after `create`
#      returns, which can be 1-11 minutes after the VM actually exists
#      server-side (SSH + cloud-init wait loops). A signal or failure in
#      that window used to leave `ephemeral_cleanup` with nothing to act on
#      (`$server_id` still empty) — an orphan by construction, and observed
#      live while building this. Fixed: when `$server_id` is empty but
#      `$server_name` is set, cleanup looks the server up by NAME via the
#      Hetzner API before giving up.
#   3. After DELETE, a separate Hetzner API GET confirms the server id is
#      actually gone (404) before this script reports success — the DELETE
#      call's own response is never trusted alone. Both failure paths here
#      (token unreadable; the 404 poll times out) now `exit 1` from inside
#      the trap — loud AND non-zero, not just a log line under an exit-0
#      script.
#   4. `tests/hetzner-sweep.sh` (label pgwt=ephemeral, age > 6h) runs at the
#      start of EVERY box-check invocation, ephemeral or not, so a VM
#      leaked despite all of the above (e.g. `kill -9`, which no trap can
#      ever catch) is still reaped on the next call, and is also available
#      on demand as `make hetzner-sweep`.
#
# Each invocation gets a private remote directory (pruned after artifact pull), and a
# box-wide flock serialises concurrent runs (timing tests must not overlap;
# harmless-but-unnecessary on a private EPHEMERAL box, which nothing else
# ever touches).
# The full log lands in tests/results/box-check-<os>-<ts>.log (or
# box-check-ephemeral-<os>-<ts>.log for EPHEMERAL=1 runs); the last lines
# (run_all.sh summary) are echoed so an agent can paste them into a PR.
set -uo pipefail
# A signal can arrive inside a poll whose stdout is redirected to a reply
# file. Keep the caller's output available for the abort diagnostic.
exec 3>&1
cd "$(dirname "$0")/.."

OS="${OS:-ubuntu}"
PG="${PG:-}"
EPHEMERAL="${EPHEMERAL:-0}"
KEEP="${KEEP:-0}"
outage_bound="${PGWT_BOX_OUTAGE_SECONDS:-1800}"
poll_interval="${PGWT_BOX_POLL_SECONDS:-10}"
run_bound="${PGWT_BOX_RUN_SECONDS:-14400}"
startup_bound=60
for value in "$outage_bound" "$poll_interval" "$run_bound"; do
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo 'box-check: poll/outage/run seconds must be positive integers' >&2; exit 2; }
done

# Bound dead connections so failures reach the EXIT cleanup trap. Keep the
# same transport options on direct SSH calls and rsync's SSH subprocesses.
ssh_keepalive=(-o ServerAliveInterval=30 -o ServerAliveCountMax=6 -o ConnectTimeout=20)

ts=$(date +%Y%m%d-%H%M%S)
mkdir -p tests/results
if [[ "$EPHEMERAL" == "1" ]]; then
    log="tests/results/box-check-ephemeral-$OS-$ts.log"
else
    log="tests/results/box-check-$OS-$ts.log"
fi
: > "$log"

hcloud_token() {
    if [[ -n "${HCLOUD_TOKEN:-}" ]]; then
        printf '%s\n' "$HCLOUD_TOKEN"
        return 0
    fi
    command -v security >/dev/null 2>&1 || return 1
    security find-generic-password -s hcloud -a claude_token -w 2>/dev/null
}

# Sweep first, always — best-effort, never fatal to this run, but now part
# of THIS run's own log (previously ran before $log existed and its output
# went nowhere but the terminal). Catches a VM an earlier run leaked (crash,
# `kill -9`, laptop sleep mid-run: nothing a trap in THIS invocation could
# ever see).
if ! tests/hetzner-sweep.sh 2>&1 | tee -a "$log"; then
    echo "box-check: hetzner-sweep.sh reported a problem (non-fatal, continuing)" | tee -a "$log" >&2
fi

server_id=""
server_ip=""
server_name=""
cleanup_done=0

# See the deletion-guarantee comment block at the top of this file for the
# design. Registered (both this EXIT trap and the INT/TERM/HUP ones below)
# before the network call that creates the VM.
ephemeral_cleanup() {
    [[ "$cleanup_done" -eq 1 ]] && return
    cleanup_done=1

    local token
    token="$(hcloud_token || true)"

    # Orphan-window guard (see top-of-file comment, point 2): $server_id is
    # only set after `create` returns; if we were interrupted or died
    # before that, look the server up by the name we already committed to
    # before starting the create call.
    if [[ -z "$server_id" && -n "$server_name" && -n "$token" ]]; then
        echo "box-check: server_id not yet known at cleanup time -- looking up '$server_name' by name (orphan-window guard)" | tee -a "$log"
        server_id=$(curl -s "https://api.hetzner.cloud/v1/servers?name=$server_name" \
            -H "Authorization: Bearer $token" | jq -r '.servers[0].id // empty' 2>/dev/null)
        [[ -n "$server_id" ]] && echo "box-check: found $server_name as id=$server_id by name lookup" | tee -a "$log"
    fi
    # Genuinely nothing to clean up: either no VM was ever attempted, or the
    # name lookup (with a working token) found nothing.
    [[ -z "$server_id" ]] && return

    if [[ "$KEEP" == "1" ]]; then
        echo "box-check: EPHEMERAL=1 KEEP=1 — leaving $server_name (id=$server_id, ip=$server_ip) up." | tee -a "$log"
        echo "box-check: tests/hetzner-sweep.sh (make hetzner-sweep, cutoff ${MAX_AGE_HOURS:-6}h by default) WILL delete this VM on its own in a few hours unless you raise MAX_AGE_HOURS/--max-age-hours or delete it yourself first:" | tee -a "$log"
        echo "  HCLOUD_TOKEN=\"\$(security find-generic-password -s hcloud -a claude_token -w)\" tests/hetzner-vm.sh delete $server_id" | tee -a "$log"
        return
    fi

    echo "box-check: deleting ephemeral server $server_name (id=$server_id) ..." | tee -a "$log"
    if [[ -z "$token" ]]; then
        echo "box-check: FATAL could not read the Hetzner token from the Keychain to delete $server_id -- DELETE IT MANUALLY NOW: tests/hetzner-vm.sh delete $server_id" | tee -a "$log" >&2
        exit 1
    fi
    local del_rc=0
    HCLOUD_TOKEN="$token" tests/hetzner-vm.sh delete "$server_id" >>"$log" 2>&1 || del_rc=$?
    [[ $del_rc -ne 0 ]] && echo "box-check: tests/hetzner-vm.sh delete reported an error (rc=$del_rc) -- polling the API to find out the real state" | tee -a "$log" >&2

    # Never trust the DELETE call's own response alone: poll the Hetzner API
    # directly for the server id until it 404s (deleted) or we give up.
    local gone=0 code=""
    for _ in $(seq 1 20); do
        code=$(curl -s -o /dev/null -w '%{http_code}' "https://api.hetzner.cloud/v1/servers/$server_id" \
            -H "Authorization: Bearer $token")
        if [[ "$code" == "404" ]]; then
            gone=1
            break
        fi
        sleep 2
    done
    if [[ "$gone" -eq 1 ]]; then
        echo "box-check: confirmed via Hetzner API: server $server_id ($server_name) is gone" | tee -a "$log"
    else
        echo "box-check: FATAL could not confirm server $server_id ($server_name) is deleted (last HTTP $code) -- CHECK MANUALLY: tests/hetzner-vm.sh list" | tee -a "$log" >&2
        exit 1
    fi
}

if [[ "$EPHEMERAL" == "1" ]]; then
    token="$(hcloud_token || true)"
    [[ -n "$token" ]] || { echo "no Hetzner token in the Keychain: security add-generic-password -s hcloud -a claude_token -w <token>" >&2; exit 2; }

    # PGWT_SNAPSHOT_IMAGE pins to an exact image id, bypassing the
    # pgwt=gate-snapshot label lookup entirely -- see the usage comment at
    # the top of this file. Availability is otherwise a property of the
    # snapshot inventory, not an OS allow-list; ubuntu alone accepts the
    # legacy, pre-os-label snapshot.
    snapshot_id="${PGWT_SNAPSHOT_IMAGE:-}"
    if [[ -z "$snapshot_id" ]]; then
        snapshot_os_encoded=$(printf '%s' "$OS" | jq -sRr @uri)
        snapshot_id=$(curl -s "https://api.hetzner.cloud/v1/images?type=snapshot&label_selector=pgwt%3Dgate-snapshot,os%3D$snapshot_os_encoded&sort=created:desc&per_page=1" \
            -H "Authorization: Bearer $token" | jq -r '.images[0].id // empty')
        # Real bug found on review (#749): Hetzner label selectors match on
        # SUBSETS, so a bare `pgwt=gate-snapshot` selector ALSO matches an
        # image that additionally carries os=el8 (or any other os) -- it is
        # not scoped to "no os label", it is scoped to "has this one label,
        # don't care what else it has". Sorted created:desc, a newer el8
        # image would silently outrank the legacy unlabeled ubuntu snapshot
        # here. `!os` (label key os ABSENT) is the actual legacy-only
        # selector; it can never match an os=<anything>-labelled image.
        if [[ -z "$snapshot_id" && "$OS" == "ubuntu" ]]; then
            snapshot_id=$(curl -s "https://api.hetzner.cloud/v1/images?type=snapshot&label_selector=pgwt%3Dgate-snapshot,%21os&sort=created:desc&per_page=1" \
                -H "Authorization: Bearer $token" | jq -r '.images[0].id // empty')
        fi
        if [[ -z "$snapshot_id" ]]; then
            echo "EPHEMERAL=1: no pgwt=gate-snapshot image exists for OS=$OS (selector pgwt=gate-snapshot,os=$OS). Build one with tests/hetzner-vm.sh create and tests/provision-runner.sh $OS, then snapshot it, or pin a candidate with PGWT_SNAPSHOT_IMAGE=<id>." >&2
            exit 2
        fi
    fi

    nonce="$(date +%s)-$RANDOM"
    server_name="pgwt-dev-$nonce"
    # Real bug found running this live: piping hostname's output straight
    # into `tr -c ... '-'` also converts hostname's own trailing newline
    # (not in the allow-set) into a literal trailing '-', which the outer
    # $(...) can no longer strip (it only strips a trailing *newline*, and
    # by then there isn't one) -- e.g. "mac\n" -> "mac-\n" -> owner=mac-,
    # which Hetzner's API rejects with "invalid input in field 'labels'"
    # (label values must start/end alphanumeric). Capture hostname first
    # (command substitution strips ITS trailing newline), then sanitize.
    owner="$(hostname -s 2>/dev/null)"
    owner="$(printf '%s' "$owner" | tr -c 'a-zA-Z0-9.-' '-')"
    owner="${owner#-}"; owner="${owner%-}"
    [[ -n "$owner" ]] || owner="mac"
    created_epoch=$(date +%s)
    labels="pgwt=ephemeral,created=$created_epoch,owner=$owner"

    echo "box-check: EPHEMERAL=1 — creating $server_name from image $snapshot_id (labels: $labels)" | tee -a "$log"
    # Registered here, BEFORE the create() call below, which can block for
    # 1-11 minutes (ssh + cloud-init wait loops) while the VM already
    # exists server-side -- exactly the orphan window point 2 in the
    # top-of-file comment guards against. See that comment for why
    # INT/TERM/HUP each need their own explicit `exit`.
    trap ephemeral_cleanup EXIT
    trap 'ephemeral_cleanup; exit 130' INT
    trap 'ephemeral_cleanup; exit 143' TERM
    trap 'ephemeral_cleanup; exit 129' HUP

    create_out=$(HCLOUD_TOKEN="$token" tests/hetzner-vm.sh create \
        --type cx33 --image-snapshot "$snapshot_id" --snapshot-os "$OS" --name "$server_name" --labels "$labels" \
        2> >(tee -a "$log" >&2))
    create_rc=$?
    if [[ $create_rc -ne 0 || -z "$create_out" ]]; then
        echo "box-check: failed to create ephemeral VM (rc=$create_rc)" | tee -a "$log" >&2
        exit 1
    fi
    # tests/hetzner-vm.sh create's documented contract is a single
    # machine-readable "$server_id $server_ip" line, LAST -- defense in
    # depth against any stray stdout line from a tool it shells out to
    # (real example hit and fixed at the source, issue #141: ssh-keygen -R
    # leaked informational text onto stdout, not just stderr).
    create_last_line=$(tail -n1 <<<"$create_out")
    server_id=$(awk '{print $1}' <<<"$create_last_line")
    server_ip=$(awk '{print $2}' <<<"$create_last_line")
    [[ -n "$server_id" && -n "$server_ip" ]] || { echo "box-check: could not parse server id/ip from hetzner-vm.sh create output: $create_out" | tee -a "$log" >&2; exit 1; }
    echo "box-check: created $server_name id=$server_id ip=$server_ip" | tee -a "$log"

    target="root@$server_ip"

    # tests/provision-runner.sh $OS is idempotent (comment at its top):
    # a second run against an already-provisioned box (the snapshot IS a
    # provisioned gate box) is a fast no-op modulo apt/dnf/PGDG metadata
    # refresh — its real job here is refreshing bpftool for whatever
    # kernel this VM actually booted (may differ from the snapshot's own
    # kernel after a security auto-update between snapshot and
    # boot) and healing the pgbench dataset if it was ever shrunk. Needs
    # the repo on the box first, hence the early minimal rsync (full rsync
    # + build/test happens below like every other box-check run).
    echo "box-check: staging repo + running tests/provision-runner.sh $OS on $target (refreshes bpftool for this VM's kernel)" | tee -a "$log"
    ssh -o BatchMode=yes "${ssh_keepalive[@]}" -o StrictHostKeyChecking=no "$target" "mkdir -p pgwt-provision" 2>>"$log"
    rsync -az -e "ssh ${ssh_keepalive[*]} -o StrictHostKeyChecking=no" ./tests/provision-runner.sh "$target:pgwt-provision/tests-provision-runner.sh" 2>>"$log"
    ssh -o BatchMode=yes "${ssh_keepalive[@]}" -o StrictHostKeyChecking=no "$target" "sudo bash pgwt-provision/tests-provision-runner.sh $OS" 2>&1 | tee -a "$log"
    provision_rc=${PIPESTATUS[0]}
    if [[ $provision_rc -ne 0 ]]; then
        echo "box-check: tests/provision-runner.sh $OS failed (rc=$provision_rc) on the fresh ephemeral VM" | tee -a "$log" >&2
        exit 1
    fi
else
    case "$OS" in
        ubuntu) target="${PGWT_BOX_UBUNTU:-${PGWT_BOX:-}}" ;;
        el8)    target="${PGWT_BOX_EL8:-}" ;;
        el9)    target="${PGWT_BOX_EL9:-}" ;;
        *) echo "OS must be ubuntu|el8|el9"; exit 2 ;;
    esac
    if [[ -z "$target" ]]; then
        echo "no box for OS=$OS: set PGWT_BOX (ubuntu) or PGWT_BOX_EL8/EL9 to an ssh target, or use EPHEMERAL=1." >&2
        echo "Provisioning: tests/provision-runner.sh (step 4 of the dev-loop plan) / tests/hetzner-vm.sh" >&2
        exit 2
    fi

    # BEGIN persistent provisioning
    # Always run the idempotent provisioner: package metadata refresh and
    # dependency checks cost time, but an "already provisioned" flag or a
    # fixed header/package probe goes stale when the dependency set changes.
    # That cost is preferable to a missing dependency misleading a live test.
    # Use a per-run staging name so another tree cannot overwrite the script
    # between staging and execution. Taking the lock first is not an option:
    # the provisioner itself takes it inside the same SSH; an outer flock
    # self-deadlocks against that inner lock.
    # Both provisioning and build/test wait unbounded for the shared lock,
    # queueing behind CI or other box-check runs. Provisioning errors stay
    # fatal so an unprovisioned box cannot produce misleading test failures.
    # Lock window: the provisioner takes /tmp/pgwt-box-check.lock inside its
    # own SSH and releases it when that SSH exits. The build/test phase
    # re-acquires it later via lock_header below. Between them another
    # box-check or CI can provision the same box differently or change cluster
    # state. Persistent box-check re-provisions on every run but does NOT
    # guarantee unchanged box state between provisioning and the tests.
    # Closing this window means running the provisioner inside the build-phase
    # lock after the tree rsync, requiring a nested-lock bypass in the
    # provisioner. Both phases already use bare `flock 9` and wait unbounded
    # by design (.github/workflows/ci.yml intends queueing).
    # Deliberately not done in this change.
    # rc=75 identifies lock contention and stays fatal. Only rc=3 permits
    # using the box as-is: only the provisioner's unsupported-OS arm emits
    # it. The provisioner guarantees this by normalising any child's 3 to 1,
    # so implementing that OS removes the skip along with that arm.
    echo "box-check: staging + running tests/provision-runner.sh $OS on persistent $target" | tee -a "$log"
    prov_stage="pgwt-provision/$(date +%s)-$$-tests-provision-runner.sh"
    ssh -o BatchMode=yes "${ssh_keepalive[@]}" "$target" "mkdir -p pgwt-provision" 2>>"$log" || { echo "box-check: provision directory creation failed on persistent $target (see $log)" | tee -a "$log" >&2; exit 1; }
    rsync -az -e "ssh ${ssh_keepalive[*]}" ./tests/provision-runner.sh "$target:$prov_stage" 2>>"$log" || { echo "box-check: provisioner rsync stage failed on persistent $target (see $log)" | tee -a "$log" >&2; exit 1; }
    ssh -o BatchMode=yes "${ssh_keepalive[@]}" "$target" "sudo bash $prov_stage $OS --lock-wait unbounded" 2>&1 | tee -a "$log"
    provision_rc=${PIPESTATUS[0]}
    ssh -o BatchMode=yes "${ssh_keepalive[@]}" "$target" "rm -f \"$prov_stage\"" 2>>"$log" || true
    if [[ $provision_rc -eq 3 ]]; then
        echo "box-check: WARNING: provisioner does not support OS=$OS (unsupported, rc=3); provisioning skipped for that reason; using persistent $target as-is" | tee -a "$log" >&2
    elif [[ $provision_rc -eq 75 ]]; then
        echo "box-check: persistent $target is busy (rc=75): contention, not a provisioning defect; CI or another box-check held /tmp/pgwt-box-check.lock past the provisioner's lock-wait timeout; caller should retry this run later" | tee -a "$log" >&2
        exit 1
    elif [[ $provision_rc -ne 0 ]]; then
        echo "box-check: tests/provision-runner.sh $OS failed (rc=$provision_rc) on persistent $target; refusing to build/test without successful provisioning" | tee -a "$log" >&2
        exit 1
    fi
    # END persistent provisioning
fi

# issue #93: also refuse artifacts whose run.id predates this invocation.
box_check_start_epoch=$(date +%s)

branch=$(git rev-parse --abbrev-ref HEAD | tr '/' '_')
# Keep detached runs identifiable by short SHA, never the shared name HEAD.
[[ "$branch" != HEAD ]] || branch=$(git rev-parse --short HEAD)
remote_root="${PGWT_BOX_DIR:-pgwt-check}"
remote_dir=""

# EPHEMERAL boxes get a brand-new host key every run (a fresh IP each time)
# -- StrictHostKeyChecking=no is needed so ssh/rsync don't refuse an
# unrecognised key under BatchMode=yes. The persistent $PGWT_BOX* targets
# keep the original, stricter behaviour (their host key is expected to
# already be in known_hosts).
ssh_strict=""
[[ "$EPHEMERAL" == "1" ]] && ssh_strict="-o StrictHostKeyChecking=no"
# openrsync (macOS's /usr/bin/rsync) is stricter about -e's argument than
# GNU rsync -- build it without a trailing space rather than rely on both
# implementations tolerating "ssh " when $ssh_strict is empty.
rsync_e="ssh ${ssh_keepalive[*]}"
[[ -n "$ssh_strict" ]] && rsync_e="ssh ${ssh_keepalive[*]} $ssh_strict"

# An atomic mktemp on the BOX guarantees uniqueness even for two clones with
# the same branch, tree, timestamp and local PID. Syncs cannot overwrite each
# other, so keep them outside the flock: CI waits only for build/tests as before.
# EPHEMERAL uses the same isolation; its existing VM cleanup prunes the directory
# too (KEEP=1 retains it for debugging). Persistent runs remove only their own
# directory after pulling artifacts, on failure as well as success.
source_manifest=""
poll_reply=""
remote_started=0
retain_remote=0
active_ssh=""
# SSH's keepalive bounds a dead connection, but not a responsive, stuck
# command. Bound the poll by the remaining run time and abort by 20 seconds.
bounded_ssh() {
    python3 -c 'import os, signal, subprocess, sys
child = None
def stop(signum, frame):
    if child is not None:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait()
    sys.exit(128 + signum)
signal.signal(signal.SIGTERM, stop)
child = subprocess.Popen(sys.argv[2:], start_new_session=True)
try:
    rc = child.wait(timeout=int(sys.argv[1]))
    sys.exit(rc if rc >= 0 else 1)
except subprocess.TimeoutExpired:
    os.killpg(child.pid, signal.SIGKILL)
    child.wait()
    print("box-check: bounded SSH timed out", file=sys.stderr)
    sys.exit(124)
' "$@" <&0 &
    active_ssh=$!
    # Unlike a foreground command, bash's wait builtin runs signal traps
    # immediately. Preserve stdin explicitly for the remote poll heredoc.
    wait "$active_ssh"
    local rc=$?
    active_ssh=""
    return "$rc"
}
abort_run() {
    local signal_name=$1 exit_code=$2
    trap '' INT TERM HUP
    if [[ -n "$active_ssh" ]]; then
        kill -TERM "$active_ssh" 2>/dev/null || true
        wait "$active_ssh" 2>/dev/null || true
        active_ssh=""
    fi
    if [[ "$remote_started" == 1 && -n "$remote_dir" ]]; then
        echo "box-check: $signal_name abort; terminating worker group on $target at $remote_dir" | tee -a "$log"
        # Read the worker-published setsid leader, even if the launch reply
        # was interrupted. sudo also reaches root descendants of the tier.
        if bounded_ssh 20 ssh -o BatchMode=yes -o ConnectTimeout=10 "${ssh_keepalive[@]}" $ssh_strict "$target" \
            "cd '$remote_dir' && pid=\$(cat .box-check.pid) && case \$pid in ''|*[!0-9]*) exit 1;; esac; [ \"\${pid:-0}\" -gt 1 ] || exit 1; sudo kill -TERM -- -\$pid 2>/dev/null || exit 1; sleep 5; if sudo kill -0 -- -\$pid 2>/dev/null; then sudo kill -KILL -- -\$pid || exit 1; echo \"box-check: abort pid=\$pid TERM then KILL sent\"; else echo \"box-check: abort pid=\$pid group stopped after TERM\"; fi" 2>&1 | tee -a "$log"; then
            echo "box-check: abort remote termination completed" | tee -a "$log"
        else
            echo "box-check: WARNING: abort could not confirm remote termination; log=$target:$remote_dir/.box-check.log" | tee -a "$log" >&2
        fi
    fi
    exit "$exit_code"
}
run_cleanup() {
    local rc=$?
    [[ -z "$source_manifest" ]] || rm -f "$source_manifest" "$source_manifest.paths"
    [[ -z "$poll_reply" ]] || rm -f "$poll_reply"
    if [[ "$EPHEMERAL" == 1 ]]; then
        ephemeral_cleanup
    elif [[ -n "$remote_dir" ]]; then
        # OpenSSH uses the first value; retain cleanup's shorter connect timeout.
        # Retain give-up evidence even if the worker finished while offline.
        if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "${ssh_keepalive[@]}" "$target" \
            "if [ '$remote_started' = 0 ] || { [ '$retain_remote' = 0 ] && { [ -f '$remote_dir/.box-check.exit' ] || { [ -s '$remote_dir/.box-check.pid' ] && ! kill -0 \"\$(cat '$remote_dir/.box-check.pid')\" 2>/dev/null; }; }; }; then sudo rm -rf -- '$remote_dir'; else echo 'box-check: retaining remote run at $target:$remote_dir'; fi"; then
            echo "box-check: WARNING: could not prune $target:$remote_dir; remove it manually" >&2
            [[ "$rc" -ne 0 ]] || rc=1
        fi
    fi
    exit "$rc"
}
trap run_cleanup EXIT
trap 'abort_run INT 130 >&3' INT
trap 'abort_run TERM 143 >&3' TERM
trap 'abort_run HUP 129 >&3' HUP
# shellcheck disable=SC2086,SC2029
remote_dir=$(ssh -o BatchMode=yes "${ssh_keepalive[@]}" $ssh_strict "$target" \
    "mkdir -p '$remote_root' && mktemp -d '$remote_root/$branch.XXXXXXXXXX'") || exit 1
[[ "$remote_dir" == "$remote_root/$branch."* && "$remote_dir" != *$'\n'* ]] || { echo 'box-check: invalid private directory response' >&2; remote_dir=""; exit 1; }

# #622: the launcher knows which tier it provisioned. A caller override
# stays authoritative, including an empty/invalid declaration that must fail.
if [[ "${PGWT_EXPECT_PHASE_PROBES+x}" != "x" ]]; then
    if [[ "$OS" == "el8" ]]; then
        PGWT_EXPECT_PHASE_PROBES=0
    else
        PGWT_EXPECT_PHASE_PROBES=1
    fi
fi

# Record the announced tree and its exact file set. On the box, hash the
# synced BYTES before building: concurrent local edits or rsync's size/mtime
# quick check must cause refusal instead of a false built-from certificate.
commit=$(git rev-parse HEAD) || exit 1
tree_hash=$(scripts/tree-hash.sh) || exit 1
source_manifest=$(mktemp) || exit 1
git ls-tree -rz "$tree_hash" > "$source_manifest" || exit 1
git ls-tree -rz --name-only "$tree_hash" > "$source_manifest.paths" || exit 1
echo "box-check: source commit=$commit tree-hash=$tree_hash (build provenance pending)" | tee -a "$log"
echo "box-check: target=$target OS=$OS PG=${PG:-all} EPHEMERAL=$EPHEMERAL PGWT_EXPECT_PHASE_PROBES=$PGWT_EXPECT_PHASE_PROBES -> $remote_dir (log: $log)" | tee -a "$log"
ssh -o BatchMode=yes "${ssh_keepalive[@]}" $ssh_strict "$target" \
    "mkdir -p '$remote_dir' && printf 'box-check: remote hostname=%s machine_id=%s\\n' \"\$(hostname -f 2>/dev/null || hostname)\" \"\$(cat /etc/machine-id 2>/dev/null || echo unavailable)\"" \
    2>&1 | tee -a "$log"
[[ ${PIPESTATUS[0]} -eq 0 ]] || exit 1
# The binary excludes are anchored to the repo root (leading /): an
# unanchored 'pg_wait_tracer*' also matches src/pg_wait_tracer.c,
# src/pg_wait_tracer.h and src/bpf/pg_wait_tracer.bpf.c (rsync excludes
# without a '/' match at any depth), which silently dropped the daemon's
# own source from every box-check rsync and made the remote build fail
# with "No rule to make target 'build/pg_wait_tracer.o'".
# macOS junk that cannot be a source file, excluded from the up-rsync so it
# never reaches the box. Motivation: tests/live_daemon_lib.sh's served-asset
# guard compares the bridge's embedded asset list against the web/static tree
# and refuses on ANY difference, applying no exclusion rules of its own (an
# earlier version replicated Go's //go:embed rules and that replica caused
# three silent passes). So a stray .DS_Store created under web/static by the
# Finder on this Mac is rsynced to the box and aborts the live run there --
# costing a counted demo-rehearsal slot for a file Go would never have
# embedded.
#
# Each pattern is a name macOS RESERVES; none can be a legitimate repo file,
# and that was checked rather than assumed (no path in `git ls-files`, 1193
# files, matches any of them):
#   .DS_Store        Finder per-directory view metadata
#   ._*              AppleDouble sidecars written beside real files on
#                    filesystems without native resource-fork support
#   .Spotlight-V100  Spotlight index directory
#   .Trashes         per-volume trash directory
#   .fseventsd       FSEvents journal directory
#   .AppleDouble     AppleDouble metadata directory
# Deliberately NOT broad (no *.swp, no .*, no *~): a pattern that matched a
# real file would silently shrink what the box tests, which is the same
# failure class the served-asset guard exists to prevent, one layer further
# out.
# An ARRAY, with every pattern quoted: an unquoted string expansion would be
# glob-expanded by the shell, so `._*` could be replaced by a matching
# filename in the current directory before rsync ever saw the pattern.
junk_excludes=(
    --exclude '.DS_Store'
    --exclude '._*'
    --exclude '.Spotlight-V100'
    --exclude '.Trashes'
    --exclude '.fseventsd'
    --exclude '.AppleDouble'
)

# issue #567: *.o / *.a / *.so are build artifacts, never source -- UNANCHORED
# on purpose, unlike the binary excludes above. The anchoring there exists
# because an unanchored 'pg_wait_tracer*' also matched tracked source
# filenames; these three suffixes match NO tracked file anywhere in the repo
# (`git ls-files | grep -c '\.o$'` / '\.a$' / '\.so$' are all 0), so there is
# nothing for an unanchored pattern to collide with, and anchoring would only
# need updating every time a new object-producing directory appeared.
# Without this, a Mach-O object left in tests/ by a Mac-side `make check` run
# (scripts/check-link-closure.sh compiles tests/map_reader_core.o locally)
# gets rsynced to the Linux box and the remote link fails with "file format
# not recognized" (#567) -- or, worse, a stale but *valid* ELF object from an
# earlier box run survives and gets linked silently, so the test binary is
# not built from the tree under review.
object_excludes=(
    --exclude '*.o'
    --exclude '*.a'
    --exclude '*.so'
)

rsync -az --delete -e "$rsync_e" \
    --exclude .git --exclude /build \
    --exclude '/pgwt-server' --exclude '/pgwt-server-asan' \
    --exclude '/pg_wait_tracer' --exclude '/pg_wait_tracer-asan' \
    --exclude 'tests/results' --exclude 'web/pgwt' --exclude '__pycache__' \
    --exclude '.pgwt-check.stamp' --exclude '/.pgwt-box-built-from' \
    "${junk_excludes[@]}" \
    "${object_excludes[@]}" \
    ./ "$target:$remote_dir/" || exit 1
# The Git tree also contains tracked evidence under tests/results/, which the
# ordinary source sync excludes. Overlay the EXACT announced file set so the
# remote hash can equal tree-hash.sh, including those archived bytes. This does
# not transfer untracked/ignored local results or build products. Explicit -r
# is needed with --files-from; --checksum also closes rsync's size/mtime shortcut
# if a local edit landed during the first sync. Verification still refuses any
# bytes that changed since the announced tree. Both transfers stay outside the
# lock, in this invocation's private directory.
rsync -azr --checksum --from0 --files-from="$source_manifest.paths" -e "$rsync_e" ./ "$target:$remote_dir/" || exit 1
rsync -az -e "$rsync_e" "$source_manifest" "$target:$remote_dir/.pgwt-box-source-manifest" || exit 1

pgarg=""; [[ -n "$PG" ]] && pgarg="--pg-version $PG"
# The shared remote phase runs tests/run_all.sh via sudo, and default
# sudoers policy resets the environment (env_reset) -- so PGWT_PGPORT /
# PGWT_PG_PORT_BASE, documented overrides that tests/run_all.sh and
# tests/ui_live_smoke.sh both read directly, never reached run_all.sh even
# when a caller exported them before `make box-check`. Forward only the ones
# actually set on the Mac side, explicitly, via `sudo env VAR=val ...` (sudo
# itself, not the shell inheriting an already-reset environment).
sudo_env=""
sudo_env="$sudo_env PGWT_EXPECT_PHASE_PROBES=$PGWT_EXPECT_PHASE_PROBES"
# #622: the markers-ABSENT rehearsal on an Ubuntu tier is
#   PGWT_EXPECT_PHASE_PROBES=0 PGWT_TEST_FORCE_NO_PHASE_PROBES=1 make box-check
# which makes the four phase-dependent tests REFUSE (exit 78) with the
# required QUERY_ID/ACTIVITY pair still attached and firing. Without this
# line the switch dies in sudo's env_reset exactly like PGWT_PGPORT did, the
# four tests run markers-PRESENT against a declared-0 tier, and the run goes
# red for the wrong reason -- so the forwarding is part of the seam, not a
# convenience. Only forwarded when set, so a normal run is unchanged.
[[ -n "${PGWT_TEST_FORCE_NO_PHASE_PROBES:-}" ]] && sudo_env="$sudo_env PGWT_TEST_FORCE_NO_PHASE_PROBES=$PGWT_TEST_FORCE_NO_PHASE_PROBES"
[[ -n "${PGWT_PGPORT:-}" ]] && sudo_env="$sudo_env PGWT_PGPORT=$PGWT_PGPORT"
[[ -n "${PGWT_PG_PORT_BASE:-}" ]] && sudo_env="$sudo_env PGWT_PG_PORT_BASE=$PGWT_PG_PORT_BASE"
# shellcheck disable=SC2029
# Lock file self-heal (reviewer round 2): a root-created 0644
# /tmp/pgwt-box-check.lock (e.g. from a bare `provision-runner.sh` run
# before its own chmod fix, or a fresh /tmp after a reboot) wedges any
# non-root caller's `flock`/`exec 9>` against it -- including ci.yml's
# gate-box jobs, which run as the unprivileged 'runner' user. box-check.sh
# itself runs as the box's ssh user (root by convention), so this heals it
# for everyone rather than relying on provision-runner.sh having run first.
# On an EPHEMERAL box this lock is uncontended (nothing else ever touches
# it) but harmless to take anyway — one code path for both cases.
# make pgwt-client: builds web/pgwt (the Go bridge) -- `make` alone (the
# `all` target) only builds the daemon + pgwt-server. Not needed until
# issue #93's live-UI-smoke test (tests/ui_live_smoke.sh checks for
# web/pgwt and fails loudly if it is missing, same as the other binaries).
#
# box-tier.sh preflights the kernel/BPF tools, then calls box-build.sh and
# run_all.sh --require-live (including live UI smoke). box-build.sh verifies
# synced bytes, then unconditionally invalidates outputs
# with make clean-box. It removes the marker BEFORE verification/build, restores
# it only on complete success, and logs built-from alongside tree-hash.
# Keep this inside the existing lock on both box paths. Lock wait messages and
# EPHEMERAL's uncontended acquisition are unchanged; rsync adds no lock time.
# It also retains #567's unconditional tests clean before build-tests.
# issue #615: build-tests forwards the top-level resolved libbpf headers and
# library to the tests sub-make. On EL8 these must be the bundled versions;
# the tests' default -lbpf would silently select the old system library.
lock_header='exec 9>/tmp/pgwt-box-check.lock; if flock -n 9; then echo "box-check: lock acquired (waited_for_lock=no, lock_wait_ms=0)"; else lock_wait_start_ns=$(date +%s%N); echo "box-check: waiting for /tmp/pgwt-box-check.lock"; flock 9 || exit; lock_wait_ms=$(( ($(date +%s%N) - lock_wait_start_ns) / 1000000 )); echo "box-check: lock acquired (waited_for_lock=yes, lock_wait_ms=${lock_wait_ms})"; fi;'
# Detach the shared box tier (including live UI smoke), retaining fd 9
# until it finishes. Provisioning and source rsync stay attached.
# The EXIT trap publishes the status atomically, after ALL worker log writes.
worker="printf '%s\\n' \"\$\$\" > .box-check.pid || exit; trap 'rc=\$?; printf \"%s\\n\" \"\$rc\" > .box-check.exit.tmp; mv .box-check.exit.tmp .box-check.exit' EXIT; $lock_header env OS=$OS$sudo_env bash scripts/box-tier.sh $tree_hash $commit $pgarg"
# Quote the command for the remote login shell, including the trap's quotes.
worker_quoted="'${worker//\'/\'\\\'\'}'"
remote_started=1
remote_pid=$(ssh -o BatchMode=yes "${ssh_keepalive[@]}" $ssh_strict "$target" \
    "cd '$remote_dir' && ([ -w /tmp/pgwt-box-check.lock ] || sudo sh -c 'touch /tmp/pgwt-box-check.lock && chmod 0666 /tmp/pgwt-box-check.lock') && { nohup setsid bash -c $worker_quoted > .box-check.log 2>&1 < /dev/null & echo \$!; }" 2>>"$log")
launch_rc=$?
if [[ "$launch_rc" -ne 0 ]]; then
    remote_started=0
    echo "box-check: FATAL launch SSH failed (exit=$launch_rc) on $target; remote log=$remote_dir/.box-check.log" | tee -a "$log" >&2
    exit 1
fi
[[ "$remote_pid" =~ ^[0-9]+$ ]] || remote_pid=unknown
echo "box-check: detached pid=$remote_pid remote log=$target:$remote_dir/.box-check.log" | tee -a "$log"

poll_reply=$(mktemp) || exit 1
offset=0
outage_used=0
outage_start=0
backoff=2
run_start=$(date +%s)
startup_used=0
transport_give_up() {
    local identity="$target"
    [[ "$EPHEMERAL" != 1 ]] || identity="VM id=$server_id"
    retain_remote=1
    echo "box-check: FATAL transport outage exceeded ${outage_bound}s for $identity; remote log=$remote_dir/.box-check.log pid=$remote_pid" | tee -a "$log" >&2
    exit 76
}
run_failed() {
    local identity="$target"
    [[ "$EPHEMERAL" != 1 ]] || identity="VM id=$server_id"
    echo "box-check: FATAL $1 for $identity; remote log=$remote_dir/.box-check.log pid=$remote_pid" | tee -a "$log" >&2
    exit 77
}
while :; do
    poll_start=$(date +%s)
    run_remaining=$((run_bound - poll_start + run_start))
    (( run_remaining > 0 )) || run_failed "run ceiling exceeded ${run_bound}s"
    if [[ "$outage_start" -ne 0 ]] && (( outage_used + poll_start - outage_start >= outage_bound )); then
        transport_give_up
    fi
    # Status is sampled BEFORE log size: a published exit guarantees this
    # snapshot includes the final bytes. A failed/partial SSH response is
    # discarded, leaving offset unchanged for the next transport attempt.
    if bounded_ssh "$run_remaining" ssh -o BatchMode=yes "${ssh_keepalive[@]}" $ssh_strict "$target" \
        "cd '$remote_dir' && python3 - '$offset'" >"$poll_reply" 2>>"$log" <<'POLL'
import os
import pathlib
import sys
offset = int(sys.argv[1])
status = pathlib.Path('.box-check.exit')
rc = status.read_text().strip() if status.exists() else 'running'
pid_file = pathlib.Path('.box-check.pid')
pid = pid_file.read_text().strip() if pid_file.exists() else 'unknown'
alive = 'unknown'
if pid.isdigit() and int(pid) > 1:
    try:
        os.kill(int(pid), 0)
        alive = 'alive'
    except ProcessLookupError:
        alive = 'dead'
        # The worker can publish between the first status read and death.
        rc = 'running' if not status.exists() else status.read_text().strip()
    except PermissionError:
        alive = 'alive'
else:
    pid = 'unknown'
    rc = 'starting'
if not pathlib.Path('.box-check.log').exists() and rc == 'starting':
    print(0, rc, pid, alive, flush=True)
    sys.exit(0)
with open('.box-check.log', 'rb') as log:
    size = os.fstat(log.fileno()).st_size
    if size < offset:
        raise RuntimeError('remote log shrank')
    print(size, rc, pid, alive, flush=True)
    log.seek(offset)
    remaining = size - offset
    while remaining:
        chunk = log.read(min(65536, remaining))
        if not chunk:
            raise RuntimeError('remote log truncated')
        sys.stdout.buffer.write(chunk)
        remaining -= len(chunk)
POLL
    then
        poll_end=$(date +%s)
        (( poll_end - run_start < run_bound )) || run_failed "run ceiling exceeded ${run_bound}s"
        read -r size status pid alive < "$poll_reply"
        received=$(tail -n +2 "$poll_reply" | wc -c | tr -d ' ')
        if [[ "$size" =~ ^[0-9]+$ &&
              ( ( "$pid" =~ ^[0-9]+$ && ( "$alive" == alive || "$alive" == dead ) &&
                  ( "$status" == running || "$status" =~ ^[0-9]+$ ) ) ||
                ( "$pid" == unknown && "$alive" == unknown && "$status" == starting ) ) ]] &&
              (( size >= offset && received == size - offset )); then
            if [[ "$outage_start" -ne 0 ]]; then
                outage_used=$((outage_used + $(date +%s) - outage_start))
                (( outage_used <= outage_bound )) || transport_give_up
                outage_start=0
            fi
            tail -n +2 "$poll_reply" | tee -a "$log"
            offset=$size
            [[ "$pid" == unknown ]] || remote_pid=$pid
            backoff=2
            if [[ "$status" == starting ]]; then
                startup_used=$((startup_used + poll_end - poll_start))
                if (( startup_used >= startup_bound )); then
                    remote_started=0
                    echo "box-check: FATAL worker pid file absent after ${startup_bound}s of successful polls on $target; remote log=$remote_dir/.box-check.log" | tee -a "$log" >&2
                    exit 1
                fi
                delay=$poll_interval
                (( delay <= startup_bound - startup_used )) || delay=$((startup_bound - startup_used))
                (( delay <= run_bound - poll_end + run_start )) || delay=$((run_bound - poll_end + run_start))
                sleep "$delay"
                startup_used=$((startup_used + delay))
                continue
            fi
            if [[ "$alive" == dead && "$status" == running ]]; then
                run_failed 'worker disappeared without an exit file'
            fi
            if [[ "$status" != running ]]; then
                rc=$status
                break
            fi
            delay=$poll_interval
            (( delay <= run_bound - poll_end + run_start )) || delay=$((run_bound - poll_end + run_start))
            sleep "$delay"
            continue
        fi
    fi
    [[ "$outage_start" -ne 0 ]] || outage_start=$poll_start
    remaining=$((outage_bound - outage_used - $(date +%s) + outage_start))
    run_remaining=$((run_bound - $(date +%s) + run_start))
    (( run_remaining > 0 )) || run_failed "run ceiling exceeded ${run_bound}s"
    if (( remaining <= 0 )); then
        transport_give_up
    fi
    delay=$backoff
    (( delay <= remaining )) || delay=$remaining
    (( delay <= run_remaining )) || delay=$run_remaining
    sleep "$delay"
    backoff=$((backoff * 2))
    (( backoff <= 30 )) || backoff=30
done

# issue #93: pull the live-UI-smoke artifacts back regardless of rc — a
# failing run's frames/video are exactly what a reviewer needs (a `-n` skip
# vs. FAIL, and which tab/tick broke). Unconditional so this never masks
# run_all.sh's own exit code; a missing remote dir (test skipped before
# producing anything) is reported, not fatal. Honours the same run.id
# marker run_all.sh's own re-emit gates on (review item 3): a stale ui_live/
# left over from a previous box-check must never be reported as if it were
# this run's evidence.
mkdir -p tests/results/ui_live
if rsync -az --delete -e "$rsync_e" "$target:$remote_dir/tests/results/ui_live/" tests/results/ui_live/ 2>/dev/null; then
    run_id="$(cat tests/results/ui_live/run.id 2>/dev/null || true)"
    if [[ -n "$run_id" && "$run_id" =~ ^[0-9]+$ && "$run_id" -ge "$box_check_start_epoch" ]]; then
        echo "box-check: tests/results/ui_live/ synced back from $target (run.id=$run_id, this invocation)"
    else
        echo "box-check: tests/results/ui_live/ synced back from $target BUT run.id=${run_id:-<missing>} predates this invocation (started $box_check_start_epoch) -- STALE evidence from a previous run, the smoke likely did not complete this time"
    fi
else
    echo "box-check: no tests/results/ui_live/ on $target (test_ui_live_smoke skipped or produced no artifacts)"
fi

echo
echo "box-check ($OS, PG=${PG:-all}, EPHEMERAL=$EPHEMERAL) exit=$rc — summary:"
tail -n 25 "$log" | sed 's/^/  /'
exit "$rc"
