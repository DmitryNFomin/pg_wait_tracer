#!/usr/bin/env bash
# demo-rehearsal.sh -- issue #157: `make demo-rehearsal`. Runs
# tests/demo_rehearsal.sh (a single long real-PG capture, walked repeatedly
# across the whole window) on a THROWAWAY Hetzner VM created from the
# pgwt=gate-snapshot image, always deleted at the end -- never the
# persistent gate box, never a gating CI job (this is 30-45x longer than
# `make box-check`'s own live tier).
#
# Reuses tests/hetzner-vm.sh (create/delete) and the SAME deletion-
# guarantee pattern scripts/box-check.sh's EPHEMERAL=1 path already proved
# out (issue #141: signal-specific traps registered before the VM exists,
# an orphan-window name-lookup guard, a post-DELETE 404 poll that never
# just trusts the DELETE response) -- duplicated here rather than sharing
# code with box-check.sh's EPHEMERAL block because this script's own
# caller/shape (single OS, no PG-version matrix, a completely different
# remote command and a different rsync-back target) is different enough
# that forcing the two through one code path would risk the box-check.sh
# path this project's whole CI gate depends on, for a script this task
# cannot verify with a live box-check run itself. If a third ephemeral-VM
# caller shows up, factor the trap/create/delete block out then.
#
#   make demo-rehearsal                       # default 35 min
#   make demo-rehearsal DURATION_MIN=3         # self-test the mechanics
#   make demo-rehearsal KEEP=1                 # leave the VM up; prints
#                                                the delete command instead
#
# Env:
#   DURATION_MIN   total capture window in minutes (default 35; the
#                  issue's documented range is 30-45). Only lower this for
#                  the issue's own explicit self-test step, never for a
#                  real baseline run.
#   KEEP=1         leave the VM up for debugging instead of deleting it;
#                  tests/hetzner-sweep.sh (make hetzner-sweep, 6h default)
#                  will still delete it on its own eventually.
#
# Needs the Hetzner token in the macOS Keychain (same as box-check.sh):
#   security add-generic-password -s hcloud -a claude_token -w <token>
set -uo pipefail
cd "$(dirname "$0")/.."

DURATION_MIN="${DURATION_MIN:-35}"
KEEP="${KEEP:-0}"

ts=$(date +%Y%m%d-%H%M%S)
mkdir -p tests/results
log="tests/results/demo-rehearsal-$ts.log"
: > "$log"

hcloud_token() {
    command -v security >/dev/null 2>&1 || return 1
    security find-generic-password -s hcloud -a claude_token -w 2>/dev/null
}

# Sweep first, always -- best-effort, catches a VM an earlier run leaked
# (crash, kill -9, laptop sleep mid-run).
if ! tests/hetzner-sweep.sh 2>&1 | tee -a "$log"; then
    echo "demo-rehearsal: hetzner-sweep.sh reported a problem (non-fatal, continuing)" | tee -a "$log" >&2
fi

token="$(hcloud_token || true)"
[[ -n "$token" ]] || { echo "no Hetzner token in the Keychain: security add-generic-password -s hcloud -a claude_token -w <token>" | tee -a "$log" >&2; exit 2; }

nonce="$(date +%s)-$RANDOM"
server_name="pgwt-demo-$nonce"
owner="$(hostname -s 2>/dev/null)"
owner="$(printf '%s' "$owner" | tr -c 'a-zA-Z0-9.-' '-')"
owner="${owner#-}"; owner="${owner%-}"
[[ -n "$owner" ]] || owner="mac"
created_epoch=$(date +%s)
labels="pgwt=ephemeral,created=$created_epoch,owner=$owner,task=demo-rehearsal"

server_id=""
server_ip=""
cleanup_done=0

# See the top-of-file comment: same guarantee shape as
# scripts/box-check.sh's ephemeral_cleanup (issue #141).
ephemeral_cleanup() {
    [[ "$cleanup_done" -eq 1 ]] && return
    cleanup_done=1

    local token
    token="$(hcloud_token || true)"

    if [[ -z "$server_id" && -n "$server_name" && -n "$token" ]]; then
        echo "demo-rehearsal: server_id not yet known at cleanup time -- looking up '$server_name' by name (orphan-window guard)" | tee -a "$log"
        server_id=$(curl -s "https://api.hetzner.cloud/v1/servers?name=$server_name" \
            -H "Authorization: Bearer $token" | jq -r '.servers[0].id // empty' 2>/dev/null)
        [[ -n "$server_id" ]] && echo "demo-rehearsal: found $server_name as id=$server_id by name lookup" | tee -a "$log"
    fi
    [[ -z "$server_id" ]] && return

    if [[ "$KEEP" == "1" ]]; then
        echo "demo-rehearsal: KEEP=1 -- leaving $server_name (id=$server_id, ip=$server_ip) up." | tee -a "$log"
        echo "demo-rehearsal: tests/hetzner-sweep.sh (make hetzner-sweep, cutoff ${MAX_AGE_HOURS:-6}h by default) WILL delete this VM on its own in a few hours unless you raise MAX_AGE_HOURS/--max-age-hours or delete it yourself first:" | tee -a "$log"
        echo "  HCLOUD_TOKEN=\"\$(security find-generic-password -s hcloud -a claude_token -w)\" tests/hetzner-vm.sh delete $server_id" | tee -a "$log"
        return
    fi

    echo "demo-rehearsal: deleting ephemeral server $server_name (id=$server_id) ..." | tee -a "$log"
    if [[ -z "$token" ]]; then
        echo "demo-rehearsal: FATAL could not read the Hetzner token from the Keychain to delete $server_id -- DELETE IT MANUALLY NOW: tests/hetzner-vm.sh delete $server_id" | tee -a "$log" >&2
        exit 1
    fi
    local del_rc=0
    HCLOUD_TOKEN="$token" tests/hetzner-vm.sh delete "$server_id" >>"$log" 2>&1 || del_rc=$?
    [[ $del_rc -ne 0 ]] && echo "demo-rehearsal: tests/hetzner-vm.sh delete reported an error (rc=$del_rc) -- polling the API to find out the real state" | tee -a "$log" >&2

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
        echo "demo-rehearsal: confirmed via Hetzner API: server $server_id ($server_name) is gone" | tee -a "$log"
    else
        echo "demo-rehearsal: FATAL could not confirm server $server_id ($server_name) is deleted (last HTTP $code) -- CHECK MANUALLY: tests/hetzner-vm.sh list" | tee -a "$log" >&2
        exit 1
    fi
}

echo "demo-rehearsal: creating $server_name from the pgwt=gate-snapshot image (labels: $labels)" | tee -a "$log"
trap ephemeral_cleanup EXIT
trap 'ephemeral_cleanup; exit 130' INT
trap 'ephemeral_cleanup; exit 143' TERM
trap 'ephemeral_cleanup; exit 129' HUP

create_out=$(HCLOUD_TOKEN="$token" tests/hetzner-vm.sh create \
    --type cx33 --image-snapshot latest --name "$server_name" --labels "$labels" \
    2> >(tee -a "$log" >&2))
create_rc=$?
if [[ $create_rc -ne 0 || -z "$create_out" ]]; then
    echo "demo-rehearsal: failed to create ephemeral VM (rc=$create_rc)" | tee -a "$log" >&2
    exit 1
fi
create_last_line=$(tail -n1 <<<"$create_out")
server_id=$(awk '{print $1}' <<<"$create_last_line")
server_ip=$(awk '{print $2}' <<<"$create_last_line")
[[ -n "$server_id" && -n "$server_ip" ]] || { echo "demo-rehearsal: could not parse server id/ip from hetzner-vm.sh create output: $create_out" | tee -a "$log" >&2; exit 1; }
echo "demo-rehearsal: created $server_name id=$server_id ip=$server_ip" | tee -a "$log"

target="root@$server_ip"

echo "demo-rehearsal: staging repo + running tests/provision-runner.sh ubuntu on $target" | tee -a "$log"
ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$target" "mkdir -p pgwt-provision" 2>>"$log"
rsync -az -e "ssh -o StrictHostKeyChecking=no" ./tests/provision-runner.sh "$target:pgwt-provision/tests-provision-runner.sh" 2>>"$log"
ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$target" "sudo bash pgwt-provision/tests-provision-runner.sh ubuntu" 2>&1 | tee -a "$log"
provision_rc=${PIPESTATUS[0]}
if [[ $provision_rc -ne 0 ]]; then
    echo "demo-rehearsal: tests/provision-runner.sh ubuntu failed (rc=$provision_rc) on the fresh ephemeral VM" | tee -a "$log" >&2
    exit 1
fi

remote_dir="pgwt-demo-rehearsal"
echo "demo-rehearsal: $target  DURATION_MIN=$DURATION_MIN  -> $remote_dir  (log: $log)"
ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$target" "mkdir -p '$remote_dir'" || exit 1
rsync -az --delete -e "ssh -o StrictHostKeyChecking=no" \
    --exclude .git --exclude /build \
    --exclude '/pgwt-server' --exclude '/pgwt-server-asan' \
    --exclude '/pg_wait_tracer' --exclude '/pg_wait_tracer-asan' \
    --exclude 'tests/results' --exclude 'web/pgwt' --exclude '__pycache__' \
    --exclude '.pgwt-check.stamp' \
    ./ "$target:$remote_dir/" || exit 1

demo_rehearsal_start_epoch=$(date +%s)

# Preflight (same rationale as scripts/box-check.sh): fail loud and self-
# explaining if the snapshot's bpftool can't see this VM's kernel BTF,
# instead of an opaque Makefile error partway through the build.
preflight="if ! bpftool version >/dev/null 2>&1 || [ ! -r /sys/kernel/btf/vmlinux ]"
preflight="$preflight; then echo \"ephemeral VM needs re-provisioning: tests/provision-runner.sh ubuntu did not leave a working bpftool for this kernel\"; exit 2; fi;"
# shellcheck disable=SC2029
ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$target" \
    "cd '$remote_dir' && bash -c '$preflight make -j\$(nproc) && make -C tests && make pgwt-client && sudo DURATION_MIN=$DURATION_MIN tests/demo_rehearsal.sh'" \
    2>&1 | tee -a "$log"
rc=${PIPESTATUS[0]}

mkdir -p tests/results/demo_rehearsal
if rsync -az --delete -e "ssh -o StrictHostKeyChecking=no" "$target:$remote_dir/tests/results/demo_rehearsal/" tests/results/demo_rehearsal/ 2>/dev/null; then
    run_id="$(cat tests/results/demo_rehearsal/run.id 2>/dev/null || true)"
    if [[ -n "$run_id" && "$run_id" =~ ^[0-9]+$ && "$run_id" -ge "$demo_rehearsal_start_epoch" ]]; then
        echo "demo-rehearsal: tests/results/demo_rehearsal/ synced back from $target (run.id=$run_id, this invocation)"
    else
        echo "demo-rehearsal: tests/results/demo_rehearsal/ synced back from $target BUT run.id=${run_id:-<missing>} predates this invocation (started $demo_rehearsal_start_epoch) -- STALE evidence, the rehearsal likely did not complete this time"
    fi
else
    echo "demo-rehearsal: no tests/results/demo_rehearsal/ on $target (the rehearsal was skipped or produced no artifacts)"
fi

echo
echo "demo-rehearsal (DURATION_MIN=$DURATION_MIN) exit=$rc -- summary:"
tail -n 30 "$log" | sed 's/^/  /'
exit "$rc"
