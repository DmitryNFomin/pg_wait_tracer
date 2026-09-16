#!/usr/bin/env bash
# hetzner-vm.sh — Create/delete Hetzner test VMs for pg_wait_tracer
#
# Usage:
#   tests/hetzner-vm.sh create [--type cpx42] [--image rocky-9] [--name NAME] \
#                               [--location LOC] [--cloud-init FILE]
#   tests/hetzner-vm.sh delete <SERVER_ID>
#   tests/hetzner-vm.sh ssh <IP>
#   tests/hetzner-vm.sh list
#
# Requires: HCLOUD_TOKEN env var, jq, curl, ssh-keygen
#
# The script auto-detects the local SSH key in Hetzner by matching
# the MD5 fingerprint of ~/.ssh/id_ed25519.pub (or id_rsa.pub).
#
# Locations are EU-only (fsn1/nbg1/hel1): US (ash) and Singapore (sin) cost
# roughly 3x for the same server type and are never tried automatically.
# --location overrides the whole list with a single explicit location (still
# must be a Hetzner location that offers the requested --type).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEFAULT_CLOUD_INIT="$SCRIPT_DIR/cloud-init-rocky9-pg18.yaml"
DEFAULT_TYPE="cpx42"    # 8 CPU / 16 GB
DEFAULT_IMAGE="rocky-9"
DEFAULT_NAME="pg-wait-tracer-test"
LOCATIONS="fsn1 nbg1 hel1"   # EU only, try in order

API="https://api.hetzner.cloud/v1"

die() { echo "FATAL: $*" >&2; exit 1; }

check_deps() {
    command -v jq   >/dev/null || die "jq not found"
    command -v curl >/dev/null || die "curl not found"
    [[ -n "${HCLOUD_TOKEN:-}" ]] || die "HCLOUD_TOKEN not set"
}

# Find the Hetzner SSH key ID matching the local key
find_ssh_key_id() {
    local pubkey=""
    for f in ~/.ssh/id_ed25519.pub ~/.ssh/id_rsa.pub ~/.ssh/id_ecdsa.pub; do
        [[ -f "$f" ]] && pubkey="$f" && break
    done
    [[ -z "$pubkey" ]] && die "No SSH public key found in ~/.ssh/"

    local local_fp
    local_fp=$(ssh-keygen -lf "$pubkey" -E md5 2>/dev/null | awk '{print $2}' | sed 's/^MD5://')

    local key_id
    key_id=$(curl -s "$API/ssh_keys?per_page=50" \
        -H "Authorization: Bearer $HCLOUD_TOKEN" \
        | jq -r --arg fp "$local_fp" \
            '.ssh_keys[] | select(.fingerprint == $fp) | .id' \
        | head -1)

    if [[ -z "$key_id" || "$key_id" == "null" ]]; then
        echo "Local key fingerprint: $local_fp ($pubkey)" >&2
        echo "Available Hetzner keys:" >&2
        curl -s "$API/ssh_keys?per_page=50" \
            -H "Authorization: Bearer $HCLOUD_TOKEN" \
            | jq -r '.ssh_keys[] | "  \(.id)  \(.name)  \(.fingerprint)"' >&2
        die "Local SSH key not found in Hetzner. Upload it first:
  hcloud ssh-key create --name $(hostname) --public-key-from-file $pubkey"
    fi

    echo "$key_id"
}

cmd_create() {
    local server_type="$DEFAULT_TYPE"
    local cloud_init="$DEFAULT_CLOUD_INIT"
    local image="$DEFAULT_IMAGE"
    local name="$DEFAULT_NAME"
    local location=""

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --type)       server_type="$2"; shift 2 ;;
            --cloud-init) cloud_init="$2"; shift 2 ;;
            --image)      image="$2"; shift 2 ;;
            --name)       name="$2"; shift 2 ;;
            --location)   location="$2"; shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done

    [[ -f "$cloud_init" ]] || die "Cloud-init file not found: $cloud_init"

    local locations_to_try="$LOCATIONS"
    [[ -n "$location" ]] && locations_to_try="$location"

    echo "=== Creating Hetzner VM ===" >&2
    echo "  Name:       $name" >&2
    echo "  Type:       $server_type" >&2
    echo "  Image:      $image" >&2
    echo "  Locations:  $locations_to_try" >&2
    echo "  Cloud-init: $cloud_init" >&2

    local ssh_key_id
    ssh_key_id=$(find_ssh_key_id)
    echo "  SSH key ID: $ssh_key_id" >&2

    local userdata
    userdata=$(cat "$cloud_init")

    # Try locations in order until one works
    local result=""
    for loc in $locations_to_try; do
        echo "  Trying location: $loc ..." >&2
        result=$(jq -n \
            --arg ud "$userdata" \
            --arg name "$name" \
            --arg st "$server_type" \
            --arg img "$image" \
            --arg loc "$loc" \
            --argjson key "$ssh_key_id" \
            '{name:$name, server_type:$st, image:$img, location:$loc, ssh_keys:[$key], user_data:$ud}' \
            | curl -s -X POST "$API/servers" \
                -H "Authorization: Bearer $HCLOUD_TOKEN" \
                -H "Content-Type: application/json" \
                -d @-)

        local err
        err=$(echo "$result" | jq -r '.error.message // empty')
        if [[ -z "$err" ]]; then
            break
        fi
        echo "    Failed: $err" >&2
        result=""
    done

    [[ -z "$result" ]] && die "All locations failed for server type $server_type"

    local server_id server_ip
    server_id=$(echo "$result" | jq -r '.server.id')
    server_ip=$(echo "$result" | jq -r '.server.public_net.ipv4.ip')

    echo "" >&2
    echo "  Server ID: $server_id" >&2
    echo "  Server IP: $server_ip" >&2

    # Clean known_hosts for this IP
    ssh-keygen -R "$server_ip" 2>/dev/null || true

    # Wait for SSH to become available
    echo "" >&2
    echo "  Waiting for SSH ..." >&2
    local attempts=0
    while [[ $attempts -lt 30 ]]; do
        if ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -o BatchMode=yes \
               root@"$server_ip" "true" 2>/dev/null; then
            echo "  SSH ready!" >&2
            break
        fi
        attempts=$((attempts + 1))
        sleep 10
    done

    if [[ $attempts -ge 30 ]]; then
        echo "  WARNING: SSH not ready after 5 minutes" >&2
    fi

    # Wait for cloud-init to finish
    echo "  Waiting for cloud-init ..." >&2
    local ci_attempts=0
    while [[ $ci_attempts -lt 30 ]]; do
        local status
        status=$(ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -o BatchMode=yes \
                     root@"$server_ip" "cloud-init status 2>/dev/null | awk '{print \$NF}'" 2>/dev/null || echo "pending")
        if [[ "$status" == "done" ]]; then
            echo "  Cloud-init done!" >&2
            break
        fi
        echo "    cloud-init: $status (attempt $((ci_attempts+1))/30)" >&2
        ci_attempts=$((ci_attempts + 1))
        sleep 10
    done

    if [[ $ci_attempts -ge 30 ]]; then
        echo "  WARNING: cloud-init did not complete in 5 minutes" >&2
    fi

    # Output machine-readable result (for scripts)
    echo "$server_id $server_ip"
}

cmd_delete() {
    local server_id="${1:?Usage: $0 delete <SERVER_ID>}"

    echo "Deleting server $server_id ..." >&2
    curl -s -X DELETE "$API/servers/$server_id" \
        -H "Authorization: Bearer $HCLOUD_TOKEN" \
        | jq -r '.action.status // "done"'
}

cmd_ssh() {
    local ip="${1:?Usage: $0 ssh <IP>}"
    shift
    exec ssh -o StrictHostKeyChecking=no root@"$ip" "$@"
}

cmd_list() {
    curl -s "$API/servers" \
        -H "Authorization: Bearer $HCLOUD_TOKEN" \
        | jq -r '.servers[] | "\(.id)\t\(.name)\t\(.public_net.ipv4.ip)\t\(.status)\t\(.server_type.name)"'
}

# --- Main ---
check_deps

case "${1:-help}" in
    create) shift; cmd_create "$@" ;;
    delete) shift; cmd_delete "$@" ;;
    ssh)    shift; cmd_ssh "$@" ;;
    list)   shift; cmd_list "$@" ;;
    *)
        echo "Usage: $0 {create|delete|ssh|list}" >&2
        echo "" >&2
        echo "  create [--type cpx42] [--image rocky-9] [--name NAME]" >&2
        echo "         [--location fsn1|nbg1|hel1] [--cloud-init FILE]" >&2
        echo "  delete <SERVER_ID>" >&2
        echo "  ssh <IP> [command...]" >&2
        echo "  list" >&2
        exit 1
        ;;
esac
