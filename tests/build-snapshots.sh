#!/usr/bin/env bash
# build-snapshots.sh — build a `pgwt=gate-snapshot` Hetzner image for OS from
# a freshly-provisioned box, the way docs/DEV_LOOP_PLAN.md's Step 4 item 2
# always intended (previously unwritten; `scripts/box-check.sh EPHEMERAL=1`
# could only ever consume a snapshot someone built by hand).
#
#   tests/build-snapshots.sh ubuntu
#   tests/build-snapshots.sh el8
#   tests/build-snapshots.sh el9      # refuses: provision-runner.sh el9 is still a stub
#
# Why this script exists, not another hand-run sequence of curl calls: a real
# el8 gate-snapshot built by hand (2026-11-02) came out CRASH-INCONSISTENT —
# `create_image` was called against a server that was still `running`, with
# PostgreSQL mid-restart and its data directory's writes still sitting dirty
# in the page cache. The resulting image booted with NUL bytes (0x00) baked
# into the tail of postgresql.conf (confirmed via `cat -A` and `tr -cd '\0' |
# wc -c`: 60/82/82/0 stray bytes across the four PG13/16/17/18 config files)
# -- Postgres's parser reports this as "syntax error ... near token \"\"",
# which reads like a config-generation bug but is actually a storage-layer
# one: the filesystem's own metadata said the file was longer than the bytes
# Hetzner's live block-level snapshot actually captured.
#
# THE GUARD (the actual fix — "kill the class, don't patch one boot"):
# this script refuses to call create_image unless the Hetzner API reports
# the server's status as `off`. There is no override flag. A snapshot of a
# `running` server is exactly the defect above; this script will not take
# one, not even for convenience.
#
# Sequence:
#   1. create a VM from the OS's base image (tests/hetzner-vm.sh)
#   2. rsync + run tests/provision-runner.sh <OS> on it
#   3. stop every postgresql-* service it started, then `sync`
#   4. power the SERVER off via the Hetzner API (POST .../actions/shutdown,
#      falling back to the forcing .../actions/poweroff if it does not reach
#      `off` within the timeout) and poll until status == off -- THE GUARD
#      above refuses to proceed past this point otherwise
#   5. create_image, labelled `pgwt=<OS>-snapshot-wip` (NOT the live
#      `pgwt=gate-snapshot` label yet -- see step 7)
#   6. delete the build VM by id
#   7. BOOT-VALIDATE: create a second, throwaway VM from the new image and
#      confirm every postgresql-NN unit is `active` and every
#      postgresql.conf is free of NUL bytes, with NO provisioning re-run —
#      exactly the cold-boot, no-manual-intervention path `scripts/
#      box-check.sh EPHEMERAL=1` will take. Only on a pass does the image
#      get relabelled `pgwt=gate-snapshot,os=<OS>` (a label update, not a
#      new image) and the validation VM deleted. On a failure the image is
#      LEFT at `pgwt=<OS>-snapshot-wip` (never promoted) and this script
#      exits non-zero naming exactly what failed.
#
# Labelling a new OS's image `pgwt=gate-snapshot` BEFORE it passes step 7 is
# actively dangerous on a shared account: tests/hetzner-vm.sh's `--image-
# snapshot latest` with no `--snapshot-os` filter (every caller before this
# branch, and every caller on a different, unmerged branch) resolves
# unscoped `label_selector=pgwt=gate-snapshot&sort=created:desc` — the
# newest image with that bare label wins regardless of OS. A premature label
# here was observed live: it make every concurrent `make box-check
# EPHEMERAL=1` (which defaults to OS=ubuntu) boot THIS os's box and then run
# `tests/provision-runner.sh ubuntu` against it.
#
# Requires: HCLOUD_TOKEN (env or macOS Keychain, see tests/hetzner-vm.sh),
# jq, curl, an SSH key Hetzner already knows (tests/hetzner-vm.sh).
set -uo pipefail
cd "$(dirname "$0")/.."

OS="${1:-}"
KEEP="${KEEP:-0}"

if [[ -z "$OS" ]]; then
    echo "Usage: $0 <ubuntu|el8|el9>" >&2
    exit 2
fi
case "$OS" in
    ubuntu) BASE_IMAGE="ubuntu-24.04" ;;
    el8)    BASE_IMAGE="rocky-8" ;;
    el9)
        echo "el9: tests/provision-runner.sh el9 is still a stub (docs/DEV_LOOP_PLAN.md step 4) -- nothing to build yet" >&2
        exit 1
        ;;
    *) echo "OS must be ubuntu|el8|el9" >&2; exit 2 ;;
esac

API="https://api.hetzner.cloud/v1"

hcloud_token() {
    if [[ -n "${HCLOUD_TOKEN:-}" ]]; then
        printf '%s\n' "$HCLOUD_TOKEN"
        return 0
    fi
    command -v security >/dev/null 2>&1 || return 1
    security find-generic-password -s hcloud -a claude_token -w 2>/dev/null
}
token="$(hcloud_token || true)"
[[ -n "$token" ]] || { echo "no Hetzner token: set HCLOUD_TOKEN or security add-generic-password -s hcloud -a claude_token -w <token>" >&2; exit 2; }
export HCLOUD_TOKEN="$token"

log() { echo "[build-snapshots:$OS] $*"; }

api_get()    { curl -s "$API/$1" -H "Authorization: Bearer $HCLOUD_TOKEN"; }
api_post()   { curl -s -X POST "$API/$1" -H "Authorization: Bearer $HCLOUD_TOKEN" -H "Content-Type: application/json" -d "$2"; }
server_status() { api_get "servers/$1" | python3 -c 'import json,sys; print(json.load(sys.stdin)["server"]["status"])'; }
action_status() { api_get "actions/$1" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("action",{}).get("status",""))'; }
# Orphan-window guard (same pattern as scripts/box-check.sh's
# ephemeral_cleanup, "orphan window point 2"): find a server by its NAME
# when its id isn't known yet -- the gap this closes is a Ctrl-C or crash
# during `create`'s own 1-11 minute ssh/cloud-init wait, after the VM
# already exists server-side but before this script's own variable holding
# its id has been assigned.
find_server_by_name() {
    api_get "servers?name=$1" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["servers"][0]["id"] if d.get("servers") else "")'
}

# -----------------------------------------------------------------------
# 1. create the build VM
# -----------------------------------------------------------------------
epoch=$(date +%s)
build_name="pgwt-${OS}-snapbuild-$epoch"
build_id=""
cleanup_build_vm() {
    local id="$build_id"
    [[ -z "$id" ]] && id="$(find_server_by_name "$build_name")"
    [[ -z "$id" ]] && return
    [[ "$KEEP" == "1" ]] && { log "KEEP=1 -- leaving build VM $id up: tests/hetzner-vm.sh delete $id"; return; }
    log "deleting build VM $id"
    tests/hetzner-vm.sh delete "$id"
}
# Registered BEFORE create, not after: real bug found on review (#749) --
# both delete traps in this script registered after their create call,
# reopening the exact orphan window scripts/box-check.sh already closed.
trap cleanup_build_vm EXIT

log "creating $build_name ($BASE_IMAGE, cx33)"
create_out=$(tests/hetzner-vm.sh create --type cx33 --image "$BASE_IMAGE" --name "$build_name" \
    --labels "pgwt=ephemeral,created=$epoch,owner=build-snapshots,purpose=${OS}-snapshot-build")
create_rc=$?
[[ $create_rc -eq 0 ]] || { echo "FATAL: create failed (rc=$create_rc)" >&2; exit 1; }
last_line=$(tail -n1 <<<"$create_out")
build_id=$(awk '{print $1}' <<<"$last_line")
build_ip=$(awk '{print $2}' <<<"$last_line")
[[ -n "$build_id" && -n "$build_ip" ]] || { echo "FATAL: could not parse server id/ip from: $create_out" >&2; exit 1; }
log "build VM: id=$build_id ip=$build_ip"
target="root@$build_ip"

# -----------------------------------------------------------------------
# 2. provision
# -----------------------------------------------------------------------
log "staging + running tests/provision-runner.sh $OS"
ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$target" "mkdir -p pgwt-provision"
rsync -az -e "ssh -o StrictHostKeyChecking=no" ./tests/provision-runner.sh "$target:pgwt-provision/tests-provision-runner.sh"
ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$target" "sudo bash pgwt-provision/tests-provision-runner.sh $OS"
prov_rc=$?
[[ $prov_rc -eq 0 ]] || { echo "FATAL: provisioning failed (rc=$prov_rc) on $build_id ($build_ip)" >&2; exit 1; }

kernel=$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$target" "uname -r")
log "booted kernel: $kernel"

# -----------------------------------------------------------------------
# 3. stop PostgreSQL cleanly, then sync
# -----------------------------------------------------------------------
log "stopping postgresql-13/16/17/18, then sync"
ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$target" \
    "systemctl stop postgresql-13 postgresql-16 postgresql-17 postgresql-18 2>/dev/null; pg_ctlcluster 13 main stop 2>/dev/null; pg_ctlcluster 16 main stop 2>/dev/null; pg_ctlcluster 17 main stop 2>/dev/null; pg_ctlcluster 18 main stop 2>/dev/null; sync; sleep 2; sync" || true

# -----------------------------------------------------------------------
# 4. THE GUARD: power off via the Hetzner API, poll, refuse create_image
#    unless status == off. No override.
# -----------------------------------------------------------------------
log "powering off server $build_id via the Hetzner API"
api_post "servers/$build_id/actions/shutdown" '{}' >/dev/null
status=""
for _ in $(seq 1 30); do
    status=$(server_status "$build_id")
    [[ "$status" == "off" ]] && break
    sleep 5
done
if [[ "$status" != "off" ]]; then
    log "graceful shutdown did not reach 'off' (last=$status) -- forcing poweroff"
    api_post "servers/$build_id/actions/poweroff" '{}' >/dev/null
    for _ in $(seq 1 20); do
        status=$(server_status "$build_id")
        [[ "$status" == "off" ]] && break
        sleep 5
    done
fi
log "server status before create_image: $status"
if [[ "$status" != "off" ]]; then
    echo "FATAL: refusing create_image -- server $build_id status is '$status', not 'off'. This is THE GUARD; there is no override. VM left up for inspection (not deleted): tests/hetzner-vm.sh delete $build_id" >&2
    trap - EXIT
    exit 1
fi

# -----------------------------------------------------------------------
# 5. create_image (WIP label only)
# -----------------------------------------------------------------------
wip_label="${OS}-snapshot-wip"
log "create_image (server confirmed off), labelled pgwt=$wip_label (not gate-snapshot yet)"
snap_out=$(api_post "servers/$build_id/actions/create_image" \
    "{\"type\":\"snapshot\",\"description\":\"pgwt-gate-$OS-$(date -u +%Y%m%d)\",\"labels\":{\"pgwt\":\"$wip_label\",\"os\":\"$OS\",\"created\":\"$(date -u +%Y%m%d)\"}}")
action_id=$(echo "$snap_out" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("action",{}).get("id",""))')
image_id=$(echo "$snap_out" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("image",{}).get("id",""))')
[[ -n "$action_id" ]] || { echo "FATAL: create_image did not return an action id: $snap_out" >&2; exit 1; }
# Real bug found on review (#749): only $action_id was checked. An empty
# $image_id silently reaches step 7's `--image-snapshot "$image_id"` as
# `--image-snapshot ""`, which tests/hetzner-vm.sh's `[[ -n ... ]]` reads
# as "no snapshot requested" -- the snapshot branch is skipped entirely and
# the validation VM boots DEFAULT_IMAGE (a stock rocky-9 public image) with
# the rocky9 cloud-init instead. Boot-validation then fails against a box
# that was never the candidate at all, pointing the diagnosis at the wrong
# thing (it still fails closed -- never mis-promotes -- but for the wrong
# reason).
[[ -n "$image_id" ]] || { echo "FATAL: create_image did not return an image id: $snap_out" >&2; exit 1; }
for _ in $(seq 1 90); do
    s=$(action_status "$action_id")
    [[ "$s" == "success" ]] && break
    [[ "$s" == "error" ]] && { echo "FATAL: create_image action errored: $(api_get "actions/$action_id")" >&2; exit 1; }
    sleep 10
done
[[ "$s" == "success" ]] || { echo "FATAL: create_image did not reach success in time (last=$s)" >&2; exit 1; }
log "image $image_id created (pgwt=$wip_label,os=$OS)"

# Build VM's job is done; delete it now via the EXIT trap below by falling
# through (trap already set). Do it explicitly here too so step 7's VM has
# no dependency on cleanup ordering.
cleanup_build_vm
trap - EXIT

# -----------------------------------------------------------------------
# 7. boot-validate from the new image, cold boot, no provisioning re-run
# -----------------------------------------------------------------------
vepoch=$(date +%s)
vname="pgwt-${OS}-snapvalidate-$vepoch"
vid=""
cleanup_validate_vm() {
    local id="$vid"
    [[ -z "$id" ]] && id="$(find_server_by_name "$vname")"
    [[ -z "$id" ]] && return
    tests/hetzner-vm.sh delete "$id"
}
# Registered BEFORE create -- see the build-VM trap's comment above.
trap cleanup_validate_vm EXIT

log "boot-validating: creating $vname from image $image_id"
vcreate_out=$(tests/hetzner-vm.sh create --type cx33 --image-snapshot "$image_id" --snapshot-os "$OS" --name "$vname" \
    --labels "pgwt=ephemeral,created=$vepoch,owner=build-snapshots,purpose=${OS}-snapshot-validate")
vlast=$(tail -n1 <<<"$vcreate_out")
vid=$(awk '{print $1}' <<<"$vlast")
vip=$(awk '{print $2}' <<<"$vlast")
[[ -n "$vid" && -n "$vip" ]] || { echo "FATAL: could not parse validation VM id/ip from: $vcreate_out" >&2; exit 1; }
vtarget="root@$vip"

sleep 15
pass=1
reason=""

if [[ "$OS" == "ubuntu" ]]; then
    # Debian's pg_ctlcluster-managed clusters are not systemd units by
    # default on this image's provisioning path -- check via pg_lsclusters
    # instead, looking for four "online" rows.
    online=$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$vtarget" "pg_lsclusters -h | awk '\$4==\"online\"' | wc -l")
    [[ "$online" == "4" ]] || { pass=0; reason="pg_lsclusters: $online/4 clusters online"; }
else
    if ! ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$vtarget" \
        "systemctl is-active postgresql-13 postgresql-16 postgresql-17 postgresql-18" >/dev/null; then
        pass=0
        reason="not all postgresql-{13,16,17,18} units are active"
    fi
    nul_total=$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$vtarget" \
        "t=0; for v in 13 16 17 18; do n=\$(tr -cd '\\0' < /var/lib/pgsql/\$v/data/postgresql.conf | wc -c); t=\$((t+n)); done; echo \$t")
    if [[ "${nul_total:-1}" != "0" ]]; then
        pass=0
        reason="${reason:+$reason; }postgresql.conf NUL-byte total across PG13/16/17/18 = $nul_total (want 0)"
    fi
fi

if [[ "$pass" -eq 1 ]]; then
    log "VALIDATION PASSED -- promoting image $image_id to pgwt=gate-snapshot,os=$OS"
    label_out=$(curl -s -X PUT "$API/images/$image_id" -H "Authorization: Bearer $HCLOUD_TOKEN" \
        -H "Content-Type: application/json" \
        -d "{\"labels\":{\"pgwt\":\"gate-snapshot\",\"os\":\"$OS\",\"created\":\"$(date -u +%Y%m%d)\"}}")
    new_labels=$(echo "$label_out" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("image",{}).get("labels",{}))' 2>/dev/null || echo "$label_out")
    log "image $image_id labels now: $new_labels"
else
    echo "FATAL: boot-validation FAILED ($reason) -- image $image_id stays labelled pgwt=$wip_label, NOT promoted to gate-snapshot" >&2
    trap - EXIT
    cleanup_validate_vm
    exit 1
fi

cleanup_validate_vm
trap - EXIT
log "done: image $image_id is pgwt=gate-snapshot,os=$OS"
