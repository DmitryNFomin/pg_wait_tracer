#!/usr/bin/env bash
# hetzner-sweep.sh — issue #141's deletion guarantee, second half: delete
# any Hetzner server labelled `pgwt=ephemeral` whose `created=<epoch>` label
# is older than the max age, and warn (never auto-delete) about any
# `pgwt-dev-*` server that is missing the `pgwt=ephemeral` label (a manual
# creation this sweeper cannot safely judge).
#
#   tests/hetzner-sweep.sh                  # sweep, 6h cutoff
#   tests/hetzner-sweep.sh --max-age-hours 1
#   tests/hetzner-sweep.sh --dry-run         # list what would be deleted, delete nothing
#
# Run automatically at the start of every scripts/box-check.sh invocation
# (ephemeral or not — a leaked VM from an earlier, interrupted run is
# exactly what this catches) and on demand as `make hetzner-sweep`.
#
# Safety: the persistent gate box (`pgwt-gate`) is protected TWICE — it
# carries no `pgwt=ephemeral` label (so the age check never even looks at
# it), and its name is hard-coded below as a second, independent guard, so
# a future mislabelling can never delete it via this script.
#
# Token: read from the macOS Keychain in a subshell, at the point of use,
# only (`security find-generic-password -s hcloud -a claude_token -w`) —
# never echoed, never exported into this script's own environment (it is
# captured into a local shell variable, not `export`ed, and passed to curl
# only as an HTTP header value), never written to disk. On a host with no
# Keychain access (no `security` binary, or the entry is absent) this is a
# soft no-op: one warning line, exit 0 — box-check.sh must never fail
# because the sweep itself could not authenticate.
set -uo pipefail
cd "$(dirname "$0")/.."

API="https://api.hetzner.cloud/v1"
MAX_AGE_HOURS=6
DRY_RUN=0
# Hard-coded, independent of any label: never delete the persistent gate box.
PROTECTED_NAME="pgwt-gate"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --max-age-hours) MAX_AGE_HOURS="$2"; shift 2 ;;
        --dry-run)       DRY_RUN=1; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

hcloud_token() {
    command -v security >/dev/null 2>&1 || return 1
    security find-generic-password -s hcloud -a claude_token -w 2>/dev/null
}

token="$(hcloud_token || true)"
if [[ -z "$token" ]]; then
    echo "hetzner-sweep: no Hetzner token in the Keychain (security find-generic-password -s hcloud -a claude_token) — skipping sweep" >&2
    exit 0
fi

command -v jq >/dev/null || { echo "hetzner-sweep: jq not found — skipping sweep" >&2; exit 0; }

servers_json=$(curl -s "$API/servers?per_page=50" -H "Authorization: Bearer $token")
if ! echo "$servers_json" | jq -e '.servers' >/dev/null 2>&1; then
    echo "hetzner-sweep: could not list servers (bad token or API error) — skipping sweep" >&2
    exit 0
fi

now=$(date +%s)
max_age=$((MAX_AGE_HOURS * 3600))
deleted=0
warned=0

# Process substitution, not a pipe: a pipe would run the loop body in a
# subshell and silently drop the $deleted/$warned counters below.
while IFS=$'\t' read -r id name pgwt_label created_label; do
    [[ -z "$id" ]] && continue

    if [[ "$name" == "$PROTECTED_NAME" ]]; then
        continue
    fi

    if [[ "$pgwt_label" == "ephemeral" ]]; then
        if [[ "$created_label" =~ ^[0-9]+$ ]]; then
            age=$((now - created_label))
        else
            # No/garbage created= label on a pgwt=ephemeral server: treat as
            # infinitely old rather than silently skipping it forever.
            age=$((max_age + 1))
        fi
        if [[ "$age" -ge "$max_age" ]]; then
            echo "hetzner-sweep: deleting stale ephemeral server $name (id=$id, age=$((age / 3600))h, cutoff=${MAX_AGE_HOURS}h)"
            if [[ "$DRY_RUN" -eq 0 ]]; then
                # Real bug found on review (same class as hetzner-vm.sh's
                # cmd_delete): discarding the DELETE response entirely
                # meant a failed delete (bad/expired token, server locked,
                # already-gone id) was silently counted as deleted. Check
                # .error.message and only count it on an actual success.
                del_result=$(curl -s -X DELETE "$API/servers/$id" -H "Authorization: Bearer $token")
                del_err=$(echo "$del_result" | jq -r '.error.message // empty')
                if [[ -n "$del_err" ]]; then
                    echo "hetzner-sweep: FAILED to delete $name (id=$id): $del_err" >&2
                else
                    deleted=$((deleted + 1))
                fi
            else
                deleted=$((deleted + 1))
            fi
        fi
    elif [[ "$name" == pgwt-dev-* ]]; then
        echo "hetzner-sweep: WARNING unlabelled dev server $name (id=$id) — no pgwt=ephemeral label, not auto-deleted, check manually"
        warned=$((warned + 1))
    fi
done < <(echo "$servers_json" | jq -r '.servers[] | [.id, .name, (.labels.pgwt // ""), (.labels.created // "")] | @tsv')

echo "hetzner-sweep: done (deleted=$deleted warned=$warned dry_run=$DRY_RUN cutoff=${MAX_AGE_HOURS}h)"
