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
#   PGWT_PGPORT / PGWT_PG_PORT_BASE   forwarded to the remote tests/run_all.sh
#                     (via `sudo env ...`, since sudo's default env_reset would
#                     otherwise drop them) when set on the Mac side; see
#                     tests/run_all.sh and tests/ui_live_smoke.sh for what
#                     each one overrides
#   EPHEMERAL=1       use a throwaway Hetzner VM instead of $PGWT_BOX* (see
#                     above); OS must be ubuntu (the only OS with a
#                     pgwt=gate-snapshot image so far). Needs the Hetzner
#                     token in the macOS Keychain
#                     (`security add-generic-password -s hcloud -a claude_token -w <token>`).
#   KEEP=1            (EPHEMERAL=1 only) do not delete the VM at the end;
#                     print the delete command instead.
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
# Each run gets its own remote directory named after the local branch, and a
# box-wide flock serialises concurrent runs (timing tests must not overlap;
# harmless-but-unnecessary on a private EPHEMERAL box, which nothing else
# ever touches).
# The full log lands in tests/results/box-check-<os>-<ts>.log (or
# box-check-ephemeral-<os>-<ts>.log for EPHEMERAL=1 runs); the last lines
# (run_all.sh summary) are echoed so an agent can paste them into a PR.
set -uo pipefail
cd "$(dirname "$0")/.."

OS="${OS:-ubuntu}"
PG="${PG:-}"
EPHEMERAL="${EPHEMERAL:-0}"
KEEP="${KEEP:-0}"

ts=$(date +%Y%m%d-%H%M%S)
mkdir -p tests/results
if [[ "$EPHEMERAL" == "1" ]]; then
    log="tests/results/box-check-ephemeral-$OS-$ts.log"
else
    log="tests/results/box-check-$OS-$ts.log"
fi
: > "$log"

hcloud_token() {
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
    [[ "$OS" == "ubuntu" ]] || { echo "EPHEMERAL=1 only supports OS=ubuntu (the only OS with a pgwt=gate-snapshot image so far)" >&2; exit 2; }

    token="$(hcloud_token || true)"
    [[ -n "$token" ]] || { echo "no Hetzner token in the Keychain: security add-generic-password -s hcloud -a claude_token -w <token>" >&2; exit 2; }

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

    echo "box-check: EPHEMERAL=1 — creating $server_name from the pgwt=gate-snapshot image (labels: $labels)" | tee -a "$log"
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
        --type cx33 --image-snapshot latest --name "$server_name" --labels "$labels" \
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

    # tests/provision-runner.sh ubuntu is idempotent (comment at its top):
    # a second run against an already-provisioned box (the snapshot IS a
    # provisioned gate box) is a fast no-op modulo apt/PGDG metadata
    # refresh — its real job here is refreshing bpftool for whatever
    # kernel this VM actually booted (may differ from the snapshot's own
    # kernel after an Ubuntu security auto-update between snapshot and
    # boot) and healing the pgbench dataset if it was ever shrunk. Needs
    # the repo on the box first, hence the early minimal rsync (full rsync
    # + build/test happens below like every other box-check run).
    echo "box-check: staging repo + running tests/provision-runner.sh ubuntu on $target (refreshes bpftool for this VM's kernel)" | tee -a "$log"
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$target" "mkdir -p pgwt-provision" 2>>"$log"
    rsync -az -e "ssh -o StrictHostKeyChecking=no" ./tests/provision-runner.sh "$target:pgwt-provision/tests-provision-runner.sh" 2>>"$log"
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$target" "sudo bash pgwt-provision/tests-provision-runner.sh ubuntu" 2>&1 | tee -a "$log"
    provision_rc=${PIPESTATUS[0]}
    if [[ $provision_rc -ne 0 ]]; then
        echo "box-check: tests/provision-runner.sh ubuntu failed (rc=$provision_rc) on the fresh ephemeral VM" | tee -a "$log" >&2
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
fi

# issue #93: this invocation's start time, used below to tell a FRESH
# tests/results/ui_live/run.id (written by run_all.sh's own $PGWT_RUN_MARKER,
# which is always >= this) from a STALE one left over from a previous
# box-check (tests/results is excluded from the up-rsync above, so the box
# keeps the last run's ui_live/ between invocations).
box_check_start_epoch=$(date +%s)

branch=$(git rev-parse --abbrev-ref HEAD | tr '/' '_')
remote_root="${PGWT_BOX_DIR:-pgwt-check}"
remote_dir="$remote_root/$branch"

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
rsync_e="ssh"
[[ -n "$ssh_strict" ]] && rsync_e="ssh $ssh_strict"

echo "box-check: $target  OS=$OS PG=${PG:-all} EPHEMERAL=$EPHEMERAL  -> $remote_dir  (log: $log)"
ssh -o BatchMode=yes $ssh_strict "$target" "mkdir -p '$remote_dir'" || exit 1
# The binary excludes are anchored to the repo root (leading /): an
# unanchored 'pg_wait_tracer*' also matches src/pg_wait_tracer.c,
# src/pg_wait_tracer.h and src/bpf/pg_wait_tracer.bpf.c (rsync excludes
# without a '/' match at any depth), which silently dropped the daemon's
# own source from every box-check rsync and made the remote build fail
# with "No rule to make target 'build/pg_wait_tracer.o'".
rsync -az --delete -e "$rsync_e" \
    --exclude .git --exclude /build \
    --exclude '/pgwt-server' --exclude '/pgwt-server-asan' \
    --exclude '/pg_wait_tracer' --exclude '/pg_wait_tracer-asan' \
    --exclude 'tests/results' --exclude 'web/pgwt' --exclude '__pycache__' \
    --exclude '.pgwt-check.stamp' \
    ./ "$target:$remote_dir/" || exit 1

pgarg=""; [[ -n "$PG" ]] && pgarg="--pg-version $PG"
# The remote command below runs tests/run_all.sh via sudo, and default
# sudoers policy resets the environment (env_reset) -- so PGWT_PGPORT /
# PGWT_PG_PORT_BASE, documented overrides that tests/run_all.sh and
# tests/ui_live_smoke.sh both read directly, never reached run_all.sh even
# when a caller exported them before `make box-check`. Forward only the ones
# actually set on the Mac side, explicitly, via `sudo env VAR=val ...` (sudo
# itself, not the shell inheriting an already-reset environment).
sudo_env=""
[[ -n "${PGWT_PGPORT:-}" ]] && sudo_env="$sudo_env PGWT_PGPORT=$PGWT_PGPORT"
[[ -n "${PGWT_PG_PORT_BASE:-}" ]] && sudo_env="$sudo_env PGWT_PG_PORT_BASE=$PGWT_PG_PORT_BASE"
# Preflight (runs on the box, inside the same flock, before `make`): a
# kernel change without re-provisioning left bpftool's dispatcher unable to
# find a matching linux-tools package ("bpftool not found for kernel
# 6.8.0-139") -- the vmlinux.h generation step then died with an opaque
# Makefile error. Fail loud and self-explaining here instead. Tests the
# REAL failure mode only: `bpftool version` works AND the running kernel's
# BTF is readable. NOT a `linux-tools-$(uname -r)` package-name check --
# tests/provision-runner.sh deliberately falls back to linux-tools-generic
# or a source-built /usr/local/bin/bpftool when the exact-version package
# is unavailable, so that package's mere absence does not mean anything is
# actually broken.
preflight="if ! bpftool version >/dev/null 2>&1 || [ ! -r /sys/kernel/btf/vmlinux ]"
preflight="$preflight; then echo \"gate box needs re-provisioning after a kernel change: run tests/provision-runner.sh ubuntu on the box\"; exit 2; fi;"
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
ssh -o BatchMode=yes $ssh_strict "$target" \
    "cd '$remote_dir' && ([ -w /tmp/pgwt-box-check.lock ] || sudo sh -c 'touch /tmp/pgwt-box-check.lock && chmod 0666 /tmp/pgwt-box-check.lock') && flock /tmp/pgwt-box-check.lock bash -c '$preflight make -j\$(nproc) && make -C tests && make pgwt-client && sudo env$sudo_env tests/run_all.sh --require-live $pgarg'" \
    2>&1 | tee -a "$log"
rc=${PIPESTATUS[0]}

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
