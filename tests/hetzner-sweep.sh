#!/usr/bin/env bash
# hetzner-sweep.sh — issue #141's deletion guarantee, second half: delete
# any Hetzner server labelled `pgwt=ephemeral` whose `created=<epoch>` label
# parses AND is older than the max age; warn (never auto-delete) about any
# `pgwt=ephemeral` server whose `created=` label is missing/unparseable
# (unknown age), and any `pgwt-dev-*` server that is missing the
# `pgwt=ephemeral` label entirely (a manual creation this sweeper cannot
# safely judge).
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
# issue #162 — "a janitor for stale VMs silently became delete-everything":
# an agent ran `MAX_AGE_HOURS=0 make hetzner-sweep` to remove its OWN VM and
# it matched (and deleted) every `pgwt=ephemeral` VM, including a different
# agent's 0-hour-old, in-flight box-check. Two independent guards now stand
# between a cutoff argument and a mass deletion:
#
#   1. A cutoff below MIN_AGE_HOURS_FLOOR is REFUSED outright (exit 2,
#      nothing deleted) unless --force-all is also passed. The refusal
#      prints exactly what the sweep would have matched and how to remove
#      ONE machine by id instead — deleting your own VM is a by-id
#      operation (`tests/hetzner-vm.sh delete <id>`) and should never have
#      gone through the sweep's cutoff at all.
#   2. Independent of the cutoff (so --force-all does not reopen this hole):
#      a `pgwt=ephemeral` server younger than MIN_PROTECTED_AGE_SECONDS is
#      NEVER deleted. This mirrors scripts/box-check.sh's own orphan-window
#      guard (1-11 minutes between a VM existing server-side and this
#      script's own bookkeeping catching up) — a machine created seconds or
#      a few minutes ago by a concurrent agent is safe by construction, no
#      matter what cutoff (even 0, even --force-all) is passed. This
#      requires knowing the age: a `pgwt=ephemeral` server whose `created=`
#      label is MISSING or fails to parse as an epoch integer has UNKNOWN
#      age, and unknown age is treated as "possibly seconds old", never as
#      "infinitely old" — a first review of this fix found exactly that
#      inversion (unparseable age was scored older than any real cutoff, so
#      it bypassed this very guard on the very next ordinary sweep). Such a
#      server is skipped and warned about, the same posture as an
#      unlabelled `pgwt-dev-*` server below — never auto-deleted.
#
# Testing without the API: --servers-file FILE feeds a synthetic
# `GET /servers` JSON body instead of calling Hetzner (see
# tests/test_hetzner_sweep.sh) — combined with --dry-run this exercises the
# full decision logic (floor refusal, protected-age, force-all, normal
# sweep) without ever touching a real machine or a real token.
#
# Token: read from the macOS Keychain in a subshell, at the point of use,
# only (`security find-generic-password -s hcloud -a claude_token -w`) —
# never echoed, never exported into this script's own environment (it is
# captured into a local shell variable, not `export`ed, and passed to curl
# only as an HTTP header value), never written to disk. On a host with no
# Keychain access (no `security` binary, or the entry is absent) this is a
# soft no-op: one warning line, exit 0 — box-check.sh must never fail
# because the sweep itself could not authenticate. (--servers-file skips
# the token entirely — see above.)
set -uo pipefail
cd "$(dirname "$0")/.."

API="https://api.hetzner.cloud/v1"
MAX_AGE_HOURS=6
DRY_RUN=0
FORCE_ALL=0
SERVERS_FILE=""
# Hard-coded, independent of any label: never delete the persistent gate box.
PROTECTED_NAME="pgwt-gate"

# issue #162 guard 1: refuse a cutoff below this floor unless --force-all.
MIN_AGE_HOURS_FLOOR=1
# issue #162 guard 2: never delete a pgwt=ephemeral server younger than
# this, regardless of cutoff or --force-all — mirrors the 1-11 minute
# orphan window scripts/box-check.sh already has to account for.
MIN_PROTECTED_AGE_SECONDS="${PGWT_SWEEP_MIN_PROTECTED_AGE_S:-900}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --max-age-hours) MAX_AGE_HOURS="$2"; shift 2 ;;
        --dry-run)       DRY_RUN=1; shift ;;
        --force-all)     FORCE_ALL=1; shift ;;
        --servers-file)  SERVERS_FILE="$2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ "$MAX_AGE_HOURS" =~ ^[0-9]+$ ]] || { echo "hetzner-sweep: --max-age-hours must be a non-negative integer, got '$MAX_AGE_HOURS'" >&2; exit 2; }

# --servers-file is a test-only hook (tests/test_hetzner_sweep.sh): refuse it
# outright without --dry-run rather than relying on the live-API code path
# (an empty token / no curl call at all) to incidentally save us — a typo'd
# invocation must never delete anything through a synthetic list.
if [[ -n "$SERVERS_FILE" && "$DRY_RUN" -ne 1 ]]; then
    echo "hetzner-sweep: --servers-file requires --dry-run (it is a test-only hook; it must never drive a real delete)" >&2
    exit 2
fi

hcloud_token() {
    command -v security >/dev/null 2>&1 || return 1
    security find-generic-password -s hcloud -a claude_token -w 2>/dev/null
}

command -v jq >/dev/null || { echo "hetzner-sweep: jq not found — skipping sweep" >&2; exit 0; }

token=""
if [[ -n "$SERVERS_FILE" ]]; then
    [[ -f "$SERVERS_FILE" ]] || { echo "hetzner-sweep: --servers-file $SERVERS_FILE not found" >&2; exit 2; }
    servers_json="$(cat "$SERVERS_FILE")"
else
    token="$(hcloud_token || true)"
    if [[ -z "$token" ]]; then
        echo "hetzner-sweep: no Hetzner token in the Keychain (security find-generic-password -s hcloud -a claude_token) — skipping sweep" >&2
        exit 0
    fi
    servers_json=$(curl -s "$API/servers?per_page=50" -H "Authorization: Bearer $token")
fi

if ! echo "$servers_json" | jq -e '.servers' >/dev/null 2>&1; then
    echo "hetzner-sweep: could not list servers (bad token or API error) — skipping sweep" >&2
    exit 0
fi

now=$(date +%s)
max_age=$((MAX_AGE_HOURS * 3600))

# issue #162 guard 1: a cutoff below the floor is a completely different,
# far more destructive operation ("delete everything labelled ephemeral")
# reached through a plausible-looking argument. Refuse it loudly, unless
# the caller deliberately opted in with --force-all, and say exactly what
# it would have matched plus the safe, by-id alternative.
if [[ "$MAX_AGE_HOURS" -lt "$MIN_AGE_HOURS_FLOOR" && "$FORCE_ALL" -ne 1 ]]; then
    echo "hetzner-sweep: REFUSING --max-age-hours $MAX_AGE_HOURS (below the ${MIN_AGE_HOURS_FLOOR}h floor)." >&2
    echo "hetzner-sweep: a cutoff this low would match every pgwt=ephemeral VM regardless of age, including" >&2
    echo "hetzner-sweep: another agent's in-flight box-check (issue #162). It would have matched:" >&2
    matched=0
    while IFS=$'\t' read -r id name pgwt_label created_label; do
        [[ -z "$id" ]] && continue
        [[ "$name" == "$PROTECTED_NAME" ]] && continue
        [[ "$pgwt_label" == "ephemeral" ]] || continue
        if [[ "$created_label" =~ ^[0-9]+$ ]]; then
            age=$((now - created_label))
        else
            age=-1
        fi
        matched=$((matched + 1))
        if [[ "$age" -ge 0 ]]; then
            echo "hetzner-sweep:   id=$id name=$name age=${age}s" >&2
        else
            echo "hetzner-sweep:   id=$id name=$name age=unknown (no/garbage created= label)" >&2
        fi
    done < <(echo "$servers_json" | jq -r '.servers[] | [.id, .name, (.labels.pgwt // ""), (.labels.created // "")] | @tsv')
    [[ "$matched" -eq 0 ]] && echo "hetzner-sweep:   (none currently — but the next agent to create one would be at risk)" >&2
    echo "hetzner-sweep: deleting YOUR OWN machine is a by-id operation, not a sweep — use:" >&2
    echo "hetzner-sweep:   HCLOUD_TOKEN=\"\$(security find-generic-password -s hcloud -a claude_token -w)\" tests/hetzner-vm.sh delete <id>" >&2
    echo "hetzner-sweep: pass --force-all to sweep below the floor anyway (rarely correct outside a deliberate mass cleanup)." >&2
    exit 2
fi

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
        if ! [[ "$created_label" =~ ^[0-9]+$ ]]; then
            # issue #162 (review round 2): a missing/unparseable created=
            # label means UNKNOWN age. Unknown must never be scored as
            # "infinitely old" (that inversion is exactly how the previous
            # version of this guard bypassed itself on a plain, ordinary
            # sweep) -- it is treated the same as an unlabelled pgwt-dev-*
            # server: skipped and warned about, never auto-deleted.
            echo "hetzner-sweep: WARNING $name (id=$id) has a missing/unparseable created= label ('$created_label') — unknown age, never auto-deleted, check manually"
            warned=$((warned + 1))
            continue
        fi

        age=$((now - created_label))

        # issue #162 guard 2: independent of cutoff/--force-all, never touch
        # a server younger than the protected-age floor.
        if [[ "$age" -lt "$MIN_PROTECTED_AGE_SECONDS" ]]; then
            echo "hetzner-sweep: skipping $name (id=$id, age=${age}s) — younger than the ${MIN_PROTECTED_AGE_SECONDS}s protected-age floor (issue #162), never swept regardless of cutoff"
            continue
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

echo "hetzner-sweep: done (deleted=$deleted warned=$warned dry_run=$DRY_RUN cutoff=${MAX_AGE_HOURS}h force_all=$FORCE_ALL)"
