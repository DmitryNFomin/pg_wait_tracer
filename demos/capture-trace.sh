#!/usr/bin/env bash
# capture-trace.sh — End-to-end trace capture on a fresh Hetzner VM.
#
# Steps:
#   1. Provision Rocky 9 + PG18 VM via tests/hetzner-vm.sh (cloud-init does
#      everything: PG18, pg_wait_sampling, pgbench init scale 10, build deps)
#   2. rsync local repo to VM, run `make` to build pg_wait_tracer + pgwt-server
#   3. Start pg_wait_tracer --daemon --mode full in tmux
#   4. Run demos/workload.sh on the VM (60s pgbench TPC-B + locks + reads)
#   5. Stop tracer, rsync trace dir back to demos/local-trace/
#
# By default does NOT delete the VM — the IP is printed so you can re-run
# capture or `pgwt root@IP` to interact live. Use `make clean-vm` (or
# tests/hetzner-vm.sh delete <ID>) when done.
#
# Requires:
#   HCLOUD_TOKEN env var
#   ssh / scp / rsync
#   ~/.ssh/id_ed25519.pub (or id_rsa.pub) registered in your Hetzner account
#
# Env overrides:
#   VM_TYPE — Hetzner server type (default: cpx22 = 2 vCPU, 4 GB)
#   VM_LOCATION — single Hetzner location (default: fsn1; no creation retries)
#   VM_NAME — unique demo server name (default: pgwt-demo-<timestamp>-<pid>)
#   TRACE_DIR — local destination for trace files (default: demos/local-trace)
#   REUSE_VM_IP — skip provisioning, use this IP (must already have repo built)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEMOS_DIR="$REPO_ROOT/demos"
LOCAL_TRACE_DIR="${TRACE_DIR:-$DEMOS_DIR/local-trace}"
VM_TYPE="${VM_TYPE:-cpx22}"
BUILD_VERSION="${PGWT_BUILD_VERSION:-$(git -C "$REPO_ROOT" rev-parse HEAD)}"

[[ -n "${HCLOUD_TOKEN:-}" ]] || {
    echo "FATAL: HCLOUD_TOKEN not set (export your Hetzner Cloud API token)" >&2
    exit 1
}

# --- 1. Provision (or reuse) ---
if [[ -n "${REUSE_VM_IP:-}" ]]; then
    VM_IP="$REUSE_VM_IP"
    echo "==> Reusing VM at $VM_IP"
else
    [[ ! -f "$DEMOS_DIR/.vm-id" ]] || {
        echo "FATAL: demos/.vm-id already exists — run 'make clean-vm' first" >&2
        exit 1
    }
    echo "==> Provisioning Hetzner VM ($VM_TYPE) ..."
    # Preserve the ID even if provisioning fails after the API creates a VM.
    PROVISION_LOG=$(mktemp)
    PROVISION_STATUS=0
    VM_RESULT=$("$REPO_ROOT/tests/hetzner-vm.sh" create --type "$VM_TYPE" \
        --location "${VM_LOCATION:-fsn1}" \
        --name "${VM_NAME:-pgwt-demo-$(date +%Y%m%d%H%M%S)-$$}" \
        2> >(tee "$PROVISION_LOG" >&2)) || PROVISION_STATUS=$?
    VM_ID=$(awk '/Server ID:/ {print $3}' "$PROVISION_LOG")
    VM_IP=$(awk '/Server IP:/ {print $3}' "$PROVISION_LOG")
    rm -f "$PROVISION_LOG"
    if [[ -n "$VM_ID" ]]; then
        echo "$VM_ID" > "$DEMOS_DIR/.vm-id"
        echo "$VM_IP" > "$DEMOS_DIR/.vm-ip"
    fi
    [[ "$PROVISION_STATUS" -eq 0 ]] || exit "$PROVISION_STATUS"
    read -r VM_ID VM_IP <<< "$VM_RESULT"
    echo "==> VM ready: id=$VM_ID ip=$VM_IP"
    echo "$VM_ID" > "$DEMOS_DIR/.vm-id"
    echo "$VM_IP" > "$DEMOS_DIR/.vm-ip"
fi

SSH="ssh -o StrictHostKeyChecking=no root@$VM_IP"
SCP="scp -o StrictHostKeyChecking=no"

# --- 2. Sync + build ---
echo "==> Syncing repo to VM and building ..."
rsync -a --delete \
    --exclude=.git --exclude=build --exclude=pg_wait_tracer --exclude=pgwt-server \
    --exclude='*.o' --exclude=node_modules --exclude=demos/local-trace \
    -e "ssh -o StrictHostKeyChecking=no" \
    "$REPO_ROOT/" "root@$VM_IP:/root/pg_wait_tracer/"

$SSH "set -euo pipefail
    mkdir -p /root/pg_wait_tracer/include
    cd /root/pg_wait_tracer && make PGWT_BUILD_VERSION='$BUILD_VERSION' 2>&1 | tee /tmp/pgwt-demo-build.log
    command -v tmux >/dev/null || dnf install -y tmux >/dev/null 2>&1
    mkdir -p /var/lib/pgwt/traces
"

# --- 3. Start tracer in tmux ---
echo "==> Starting pg_wait_tracer --daemon in tmux ..."
$SSH "tmux kill-server 2>/dev/null || true
    tmux new-session -d -s pgwt -x 200 -y 50
    tmux new-window -t pgwt -n tracer
    tmux send-keys -t pgwt:tracer 'cd /root/pg_wait_tracer && ./pg_wait_tracer --daemon --mode full -T /var/lib/pgwt/traces 2>&1 | tee /tmp/tracer.log' C-m
    sleep 3
    tail -20 /tmp/tracer.log
    pgrep -x pg_wait_tracer >/dev/null
"

# --- 4. Run workload ---
echo "==> Running workload on VM ..."
$SCP "$DEMOS_DIR/workload.sh" "root@$VM_IP:/root/workload.sh"
$SSH "set -euo pipefail
    export PATH=/usr/pgsql-18/bin:\$PATH
    chmod +x /root/workload.sh
    PGUSER=postgres /root/workload.sh 2>&1 | tee /tmp/workload.log"

# --- 5. Stop tracer, sync traces back ---
echo "==> Stopping tracer ..."
$SSH "set -euo pipefail
    pkill -INT -x pg_wait_tracer
    for _ in {1..30}; do
        pgrep -x pg_wait_tracer >/dev/null || break
        sleep 1
    done
    if pgrep -x pg_wait_tracer >/dev/null; then
        echo 'FATAL: tracer did not stop in 30s' >&2
        exit 1
    fi
    tail -10 /tmp/tracer.log"

echo "==> Downloading trace files to $LOCAL_TRACE_DIR ..."
mkdir -p "$LOCAL_TRACE_DIR"
rsync -a --delete -e "ssh -o StrictHostKeyChecking=no" \
    "root@$VM_IP:/var/lib/pgwt/traces/" "$LOCAL_TRACE_DIR/"
$SSH "getconf _NPROCESSORS_ONLN" > "$LOCAL_TRACE_DIR/capture-cpus.txt"
printf '%s\n' "$BUILD_VERSION" > "$LOCAL_TRACE_DIR/capture-source-sha.txt"
ls -la "$LOCAL_TRACE_DIR"
[[ -n "$(find "$LOCAL_TRACE_DIR" \( -name '*.trace.lz4' -o -name 'current.trace' \) -size +0c -print -quit)" ]] || {
    echo "FATAL: no nonempty trace files downloaded" >&2
    exit 1
}

echo ""
echo "==> Trace captured. Next steps:"
echo "    make dump-gif    # render demos/dump.gif"
echo "    make web-gif     # render demos/web.gif"
echo "    make clean-vm    # delete the Hetzner VM (saves \$\$\$)"
echo ""
echo "VM still running at: root@$VM_IP"
if [[ -n "${VM_ID:-}" ]]; then
    echo "VM id: $VM_ID  (saved to demos/.vm-id)"
fi
