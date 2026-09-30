#!/usr/bin/env bash
# demo-rehearsal.sh -- issue #157: `make demo-rehearsal`. Runs
# tests/demo_rehearsal.sh (a single long real-PG capture, walked repeatedly
# across the whole window) -- never a gating CI job (this is 30-45x longer
# than `make box-check`'s own live tier).
#
# TARGET (added alongside the PG-version gate below, per the owner's
# 2026-09-28 "persistent boxes first" rule): PGWT_BOX set -> a PERSISTENT
# box (never created, never deleted here). PGWT_BOX unset -> the ORIGINAL
# default, a THROWAWAY Hetzner VM created from the pgwt=gate-snapshot
# image and always deleted at the end -- still the right choice for an
# ad-hoc run, but no longer the default for a COUNTED rehearsal attempt
# (docs/DEMO_REHEARSAL_CRITERIA.md), which should set PGWT_BOX.
#
# PG VERSION: defaults to 18 (the criteria doc's pin) and is ENFORCED, not
# merely requested -- before the long capture starts, this script
# independently confirms a matching postmaster is actually running on the
# target and refuses loudly, exiting non-zero, if it is not. The failure
# this closes: a run against whatever PostgreSQL happened to be reachable
# looks exactly like a correct run right up until someone tries to count
# it -- which is exactly how the last capture-side attempt ran against
# port 5413 (PG 13) and could never have counted.
#
# issue #176 -- the run is now DRIVEN FROM THE VM, not the Mac. A 35-minute
# ssh session held open from this laptop used to depend on the laptop
# staying up for 35 minutes; it hasn't (this Mac's own memory manager has
# SIGKILLed a live run four times). SIGKILL cannot be trapped, so the old
# foreground design had no way to save itself: the ssh channel dropped, the
# remote demo_rehearsal.sh got SIGHUP and died with it, and the VM leaked
# unless the (never-firing) trap happened to run.
#
# The fix: the launcher starts the capture DETACHED on the VM (nohup +
# `setsid -w`, so it is a session leader owned by nothing the ssh
# connection controls -- see scripts/demo-rehearsal-remote-run.sh), records
# just enough state locally (tests/results/.demo-rehearsal-state.sh) to
# reattach, and then only WAITS for it -- with one long sleep, not a
# polling loop. If the launcher itself gets killed mid-wait, the capture on
# the VM is completely unaffected; a later `--collect` invocation reads the
# state file, reattaches, waits out whatever budget remains, and finishes
# the job (sync results back, delete the VM) exactly as if it had never
# been interrupted. See tests/demo_rehearsal_orchestrator_lib.py for the
# state-classification / results-validation logic this depends on (and its
# own unit tests, tests/test_demo_rehearsal_orchestrator_lib.py, for the
# bypass suite it closes: a missing marker read as done, an empty/foreign
# results dir clobbering a previous run's, a truncated transfer accepted as
# whole).
#
# Reuses tests/hetzner-vm.sh (create/delete) and the SAME deletion-
# guarantee pattern scripts/box-check.sh's EPHEMERAL=1 path already proved
# out (issue #141: signal-specific traps registered before the VM exists,
# an orphan-window name-lookup guard, a post-DELETE 404 poll that never
# just trusts the DELETE response) -- duplicated here rather than sharing
# code with box-check.sh's EPHEMERAL block for the same reason as before:
# this script's shape (single OS, a single pinned PG version rather than a
# whole matrix, a detached remote job instead of one foreground command, a
# different rsync-back target) is different enough that forcing the two
# through one code path would risk the box-check.sh path this project's
# whole CI gate depends on -- though the persistent-box target selection
# and the /tmp/pgwt-box-check.lock flock ARE now shared with it. If a third
# ephemeral-VM caller shows up, factor the trap/create/delete block out then.
#
#   make demo-rehearsal                       # default 35 min
#   make demo-rehearsal DURATION_MIN=3         # self-test the mechanics
#   make demo-rehearsal KEEP=1                 # leave the VM up; prints
#                                                the delete command instead
#   make demo-rehearsal PGWT_BOX=root@1.2.3.4  # run on a PERSISTENT box
#                                                instead of a throwaway VM
#                                                (owner rule 2026-09-28:
#                                                persistent boxes first)
#   make demo-rehearsal PG=18                  # pin the PostgreSQL major
#                                                version (default 18 --
#                                                docs/DEMO_REHEARSAL_
#                                                CRITERIA.md's pin; a run
#                                                against any other version
#                                                does not count)
#   make demo-rehearsal-collect                # finish a run whose
#                                                launcher got killed --
#                                                see tests/results/
#                                                .demo-rehearsal-state.sh
#
# Env:
#   DURATION_MIN   total capture window in minutes (default 35; the
#                  issue's documented range is 30-45). Only lower this for
#                  the issue's own explicit self-test step, never for a
#                  real baseline run.
#   KEEP=1         leave the VM up for debugging instead of deleting it;
#                  tests/hetzner-sweep.sh (make hetzner-sweep, 6h default)
#                  will still delete it on its own eventually. Ignored (a
#                  no-op) for a PGWT_BOX run -- a persistent box is never
#                  deleted regardless.
#   PGWT_BOX       ssh target of a PERSISTENT box
#                  (same variable box-check.sh reads, e.g.
#                  root@<gate-box-ip>) -- when set, this run targets that
#                  box instead of creating a throwaway Hetzner VM, and
#                  shares box-check.sh's OWN flock
#                  (/tmp/pgwt-box-check.lock) so it queues behind CI and
#                  any other agent's box-check/demo-rehearsal on that same
#                  box rather than colliding with it. Unset (the default):
#                  the throwaway-VM path below, for ad-hoc runs -- it is
#                  no longer the default for a COUNTED rehearsal attempt
#                  (docs/DEMO_REHEARSAL_CRITERIA.md), which should be run
#                  with PGWT_BOX set. Prefer pgwt-gate over pgwt-gate-2 --
#                  gate-2 produced two unrelated timing failures (#237);
#                  check both are actually idle before choosing.
#   PG             PostgreSQL major version to require (default 18, the
#                  criteria doc's pin). tests/demo_rehearsal.sh's own
#                  --pg-version flag is now plumbed through from here
#                  (it used to only be reachable by a caller invoking that
#                  script directly) -- a run against the wrong version
#                  looks identical to a correct one right up until someone
#                  tries to count it, which is exactly the failure this
#                  closes: BEFORE the long capture starts, this script
#                  independently confirms (not just requests) that a
#                  PostgreSQL $PG postmaster is actually running on the
#                  target, refuses loudly and exits non-zero if not, and
#                  records both the requested and the confirmed version in
#                  the final verdict.
#   RETAIN_TRACE   docs/DEMO_DELIVERY_QUEUE.md item 2 -- opt-in, OFF (0) by
#                  default: normal runs delete the trace dir as before. Set
#                  to 1 to keep it (synced back to tests/results/
#                  demo_rehearsal/trace/, alongside the raw aas/time_model
#                  responses saved to .../raw/) instead of deleting it at
#                  teardown -- unblocks an offline read (criteria doc §3's
#                  per-class wait-CPU number, or any future check) without
#                  paying for a whole new 30-45 minute capture. A 40-minute
#                  --mode full trace is large, which is why this is never
#                  the default.
#
# Needs the Hetzner token in the macOS Keychain (same as box-check.sh) for
# the throwaway-VM path (PGWT_BOX unset); not needed when PGWT_BOX is set:
#   security add-generic-password -s hcloud -a claude_token -w <token>
set -uo pipefail
cd "$(dirname "$0")/.."

MODE="launch"
[[ "${1:-}" == "--collect" ]] && MODE="collect"

DURATION_MIN="${DURATION_MIN:-35}"
KEEP="${KEEP:-0}"
PG="${PG:-18}"
# docs/DEMO_DELIVERY_QUEUE.md item 2: opt-in, OFF by default -- keeps the
# trace dir + raw aas/time_model responses (tests/demo_rehearsal.sh) instead
# of deleting them, so they land in the already-rsynced tests/results/
# demo_rehearsal/ with no separate sync path. Threaded through to the
# detached remote job exactly like DURATION_MIN/PG below (positional arg to
# scripts/demo-rehearsal-remote-run.sh).
RETAIN_TRACE="${RETAIN_TRACE:-0}"
STATE_FILE="${DEMO_REHEARSAL_STATE_FILE:-tests/results/.demo-rehearsal-state.sh}"

# PERSISTENT=1 when PGWT_BOX names a persistent box (pgwt-gate/pgwt-gate-2)
# instead of the default throwaway-VM path. Same variable box-check.sh
# reads, same "root@ip" convention (CLAUDE.md).
PERSISTENT=0
[[ -n "${PGWT_BOX:-}" ]] && PERSISTENT=1

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

# Hetzner token: only actually required for the throwaway-VM path
# (PERSISTENT=0) -- checked below, once PERSISTENT is final for both MODEs
# (collect can override it from the state file). A PGWT_BOX run needs no
# Hetzner token at all.
token="$(hcloud_token || true)"

server_id=""
server_ip=""
target=""
remote_dir=""
demo_rehearsal_start_epoch=""
cleanup_done=0
synced_back=0
outcome_rc=1
PG_VERSION_CONFIRMED=""

# ── --collect: reattach to a run an earlier (possibly killed) launcher
# started -- this is the whole point of issue #176: a killed launcher must
# not lose the run or leak the machine, and this is how a NEW invocation
# finishes what it started. ────────────────────────────────────────────
if [[ "$MODE" == "collect" ]]; then
    if [[ ! -f "$STATE_FILE" ]]; then
        echo "demo-rehearsal --collect: no state file at $STATE_FILE -- nothing to collect" | tee -a "$log"
        exit 3
    fi
    # Review finding: PERSISTENT was already "1" here whenever PGWT_BOX is
    # exported (CLAUDE.md's own setup instructions export it) -- so
    # `PERSISTENT="${PERSISTENT:-0}"` below was a no-op REGARDLESS of what
    # (or whether) the state file itself said, and a state file predating
    # this field would silently take the persistent (never-delete)
    # cleanup branch for what might actually be an ephemeral VM sitting in
    # SERVER_ID, permanently leaking it. Unset every field this block
    # reads BEFORE sourcing so the file's own content (or its absence) is
    # the only thing that can set them -- the ambient environment must
    # never leak into a --collect reattach.
    unset PERSISTENT PG_VERSION_REQUESTED PG_VERSION_CONFIRMED TARGET REHEARSAL_STARTED_EPOCH
    # shellcheck disable=SC1090
    source "$STATE_FILE"
    : "${SERVER_ID:?state file missing SERVER_ID}"
    : "${SERVER_IP:?state file missing SERVER_IP}"
    : "${REMOTE_DIR:?state file missing REMOTE_DIR}"
    : "${START_EPOCH:?state file missing START_EPOCH}"
    : "${DURATION_MIN:?state file missing DURATION_MIN}"
    server_id="$SERVER_ID"
    server_ip="$SERVER_IP"
    remote_dir="$REMOTE_DIR"
    demo_rehearsal_start_epoch="$START_EPOCH"
    # PERSISTENT is derived from SERVER_ID, the unambiguous discriminator
    # (the sentinel "persistent" this script itself writes below), never
    # trusted as a standalone field even when present -- belt and braces
    # on top of the unset-before-source fix above.
    if [[ "$server_id" == "persistent" ]]; then
        PERSISTENT=1
    else
        PERSISTENT=0
    fi
    # TARGET: the exact ssh target string recorded at launch (may name a
    # non-root user, unlike the ephemeral path's assumed "root@$server_ip"
    # reconstruction) -- falls back to that reconstruction for a state
    # file written before this field existed.
    target="${TARGET:-root@$server_ip}"
    PG="${PG_VERSION_REQUESTED:-$PG}"
    PG_VERSION_CONFIRMED="${PG_VERSION_CONFIRMED:-}"
    echo "demo-rehearsal --collect: reattaching to $target (persistent=$PERSISTENT, server id=$server_id, remote_dir=$remote_dir, PG=$PG, capture_started=${REHEARSAL_STARTED_EPOCH:-not yet known}, launched $(date -r "$demo_rehearsal_start_epoch" 2>/dev/null || echo "@$demo_rehearsal_start_epoch"))" | tee -a "$log"
fi

if [[ "$PERSISTENT" != "1" && -z "$token" ]]; then
    echo "no Hetzner token in the Keychain: security add-generic-password -s hcloud -a claude_token -w <token>" | tee -a "$log" >&2
    exit 2
fi

server_name="pgwt-demo-${demo_rehearsal_start_epoch:-$(date +%s)}-$RANDOM"
owner="$(hostname -s 2>/dev/null)"
owner="$(printf '%s' "$owner" | tr -c 'a-zA-Z0-9.-' '-')"
owner="${owner#-}"; owner="${owner%-}"
[[ -n "$owner" ]] || owner="mac"

# Best-effort rsync of tests/results/demo_rehearsal/ back from the VM,
# validated BEFORE it is ever allowed to replace the local directory
# (issue #176 bypass case: "an empty results directory copied over a
# previous run's", and "a transfer interrupted midway" must fail loudly,
# not yield a summary that looks whole). Staged first; only swapped in if
# tests/demo_rehearsal_orchestrator_lib.py validate-results says the
# staged copy is COMPLETE and matches THIS invocation's run.id.
results_ok=0
collect_results() {
    [[ "$synced_back" -eq 1 ]] && return
    synced_back=1
    [[ -z "$target" || -z "$remote_dir" ]] && return

    local staging="tests/results/.demo_rehearsal_staging"
    rm -rf "$staging"
    mkdir -p "$staging"
    # A persistent box's host key is expected already known -- only a
    # fresh throwaway VM's (a new IP every run) needs StrictHostKeyChecking
    # disabled.
    local strict=""
    [[ "$PERSISTENT" != "1" ]] && strict="-o StrictHostKeyChecking=no"
    local rsync_rc=0
    rsync -az -e "ssh $strict -o ConnectTimeout=10" \
        "$target:$remote_dir/tests/results/demo_rehearsal/" "$staging/" \
        2>>"$log" || rsync_rc=$?

    local reason
    if [[ $rsync_rc -eq 0 ]] && reason=$(python3 tests/demo_rehearsal_orchestrator_lib.py \
            validate-results "$staging" "$demo_rehearsal_start_epoch" 2>>"$log"); then
        results_ok=1
        mkdir -p tests/results
        rm -rf tests/results/demo_rehearsal
        mv "$staging" tests/results/demo_rehearsal
        echo "demo-rehearsal: tests/results/demo_rehearsal/ synced back from $target -- validated complete ($reason), run.id matches this invocation ($demo_rehearsal_start_epoch)" | tee -a "$log"
    else
        results_ok=0
        echo "demo-rehearsal: COULD NOT sync back a valid, complete tests/results/demo_rehearsal/ (rsync_rc=$rsync_rc, reason: ${reason:-see log}) -- leaving any PREVIOUS tests/results/demo_rehearsal/ untouched; the (possibly partial/foreign) staged copy is kept at $staging for inspection" | tee -a "$log" >&2
    fi
}

# See the top-of-file comment: same guarantee shape as
# scripts/box-check.sh's ephemeral_cleanup (issue #141).
ephemeral_cleanup() {
    [[ "$cleanup_done" -eq 1 ]] && return
    cleanup_done=1

    # A persistent box (PGWT_BOX) is never created or deleted by this
    # script -- it is shared, always-on infrastructure. KEEP is a no-op
    # here for the same reason it would be meaningless: there is nothing
    # to "leave up" that wasn't already up before this run started.
    #
    # $STATE_FILE still needs clearing here, though -- unlike the
    # ephemeral path (where a SUCCESSFUL run clears it as a side effect of
    # the VM actually getting deleted), nothing here ever "completes" in a
    # way that would otherwise remove the guard, so every persistent-box
    # run would leave the NEXT launch permanently refusing with "a state
    # file already exists" (found live, self-testing this script: a
    # PG-version refusal before the detached job ever started left the
    # guard in place with nothing to protect, since nothing had started).
    # Only clear it once the run's fate is actually known: refused before
    # the detached job started at all ($last_state never set), or the
    # remote lifecycle reached a terminal state (finished/died/
    # not-started). Never clear it while genuinely running or while the
    # target was unreachable (same reasoning as the ephemeral path's own
    # "unreachable never deletes" rule) -- a state file is a false
    # negative's only defense against a second launch racing a first one
    # that might still be alive.
    if [[ "$PERSISTENT" == "1" ]]; then
        case "${last_state:-}" in
            ""|finished|died|not-started)
                echo "demo-rehearsal: target $target is a persistent box (PGWT_BOX) -- never deleted; run's fate is known (state=${last_state:-never started}), clearing $STATE_FILE so the next launch is not blocked" | tee -a "$log"
                rm -f "$STATE_FILE"
                ;;
            *)
                echo "demo-rehearsal: target $target is a persistent box (PGWT_BOX) -- never deleted; leaving $STATE_FILE in place (state=${last_state}) so a later --collect can still find/finish this run" | tee -a "$log"
                ;;
        esac
        return
    fi

    local token
    token="$(hcloud_token || true)"

    if [[ -z "$server_id" && -n "$server_name" && -n "$token" ]]; then
        echo "demo-rehearsal: server_id not yet known at cleanup time -- looking up '$server_name' by name (orphan-window guard)" | tee -a "$log"
        server_id=$(curl -s "https://api.hetzner.cloud/v1/servers?name=$server_name" \
            -H "Authorization: Bearer $token" | jq -r '.servers[0].id // empty' 2>/dev/null)
        [[ -n "$server_id" ]] && echo "demo-rehearsal: found $server_name as id=$server_id by name lookup" | tee -a "$log"
    fi
    [[ -z "$server_id" ]] && return

    # Never delete on "unreachable" -- deletion is for a CONFIRMED died,
    # not-started or finished state only. `remote_state` already retries
    # several times with a backoff before returning "unreachable" (see its
    # own comment), so by the time we get here it isn't a single blip --
    # but we still do not know the real state, and the two possible
    # mistakes are not symmetric: a leaked VM costs cents and is visible
    # (this message, plus hetzner-sweep.sh's 6h cutoff); deleting a
    # finished 35-45 minute rehearsal's results is not recoverable. This
    # overrides KEEP entirely -- unreachable is kept regardless of KEEP.
    if [[ "${last_state:-}" == "unreachable" ]]; then
        echo "demo-rehearsal: remote state could not be determined (unreachable after $REMOTE_STATE_PROBE_ATTEMPTS retries) -- REFUSING to delete $server_name (id=$server_id, ip=$server_ip). A network blip must not destroy a possibly-completed rehearsal. Leaving it up; confirm its real state, then delete it yourself:" | tee -a "$log" >&2
        echo "  HCLOUD_TOKEN=\"\$(security find-generic-password -s hcloud -a claude_token -w)\" tests/hetzner-vm.sh delete $server_id" | tee -a "$log" >&2
        # $STATE_FILE is deliberately left in place too, same reasoning as
        # the KEEP=1 case below: it is what lets a later
        # 'make demo-rehearsal-collect' find this VM again and try the
        # probe once more instead of a fresh launch creating a second one.
        return
    fi

    if [[ "$KEEP" == "1" ]]; then
        echo "demo-rehearsal: KEEP=1 -- leaving $server_name (id=$server_id, ip=$server_ip) up." | tee -a "$log"
        echo "demo-rehearsal: tests/hetzner-sweep.sh (make hetzner-sweep, cutoff ${MAX_AGE_HOURS:-6}h by default) WILL delete this VM on its own in a few hours unless you raise MAX_AGE_HOURS/--max-age-hours or delete it yourself first:" | tee -a "$log"
        echo "  HCLOUD_TOKEN=\"\$(security find-generic-password -s hcloud -a claude_token -w)\" tests/hetzner-vm.sh delete $server_id" | tee -a "$log"
        # Deliberately NOT removing $STATE_FILE here (issue #176 bug found
        # live: a removed state file while the VM stays up means the very
        # NEXT launch can't see it -- the "a state file already exists"
        # guard above never fires, so a fresh `make demo-rehearsal` creates
        # ANOTHER new VM instead of refusing/pointing at this one. That is
        # exactly how three ephemeral VMs piled up and exhausted Hetzner's
        # shared-core quota during this issue's own testing. Leaving the
        # state file in place makes a subsequent launch attempt refuse (as
        # the top-of-launch guard already does) and 'make
        # demo-rehearsal-collect' still able to find and re-report on this
        # kept VM.
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
        rm -f "$STATE_FILE"
    else
        echo "demo-rehearsal: FATAL could not confirm server $server_id ($server_name) is deleted (last HTTP $code) -- CHECK MANUALLY: tests/hetzner-vm.sh list" | tee -a "$log" >&2
        exit 1
    fi
}

trap ephemeral_cleanup EXIT
trap 'collect_results; ephemeral_cleanup; exit 130' INT
trap 'collect_results; ephemeral_cleanup; exit 143' TERM
trap 'collect_results; ephemeral_cleanup; exit 129' HUP

if [[ "$MODE" == "launch" ]]; then
    if [[ -f "$STATE_FILE" ]]; then
        echo "demo-rehearsal: a state file already exists at $STATE_FILE -- an earlier launch either is still running or was killed before collecting it. Run 'make demo-rehearsal-collect' first (it finishes the job and deletes the VM), or remove $STATE_FILE yourself if you already know that VM is gone." | tee -a "$log" >&2
        exit 3
    fi

    demo_rehearsal_start_epoch=$(date +%s)
    ssh_strict="-o StrictHostKeyChecking=no"

    if [[ "$PERSISTENT" == "1" ]]; then
        # Persistent box (owner rule 2026-09-28: persistent boxes first --
        # both pgwt-gate/pgwt-gate-2 are paid for whether idle or not).
        # No VM to create, no tests-provision-runner.sh to run (a
        # persistent box is expected already-provisioned, exactly like
        # scripts/box-check.sh's non-EPHEMERAL path) -- and its host key is
        # expected to already be known, so StrictHostKeyChecking stays on
        # (unlike the throwaway-VM branch below, whose IP is fresh every
        # run).
        server_id="persistent"
        # target is PGWT_BOX verbatim -- do NOT reconstruct via
        # "root@${PGWT_BOX#*@}" (review nit: that silently discarded a
        # non-root user, unlike box-check.sh's own `target="$PGWT_BOX"`
        # for the exact same variable). server_ip keeps the best-effort
        # bare-host form purely for log/state-file readability -- it is
        # never used to rebuild target again (see TARGET below).
        target="$PGWT_BOX"
        server_ip="${PGWT_BOX#*@}"
        branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null | tr '/' '_')
        remote_dir="pgwt-demo-rehearsal-${branch:-detached}"
        ssh_strict=""
        echo "demo-rehearsal: targeting persistent box $target (PGWT_BOX) -- remote_dir=$remote_dir, sharing box-check.sh's own flock (/tmp/pgwt-box-check.lock) so this queues behind CI/other agents rather than colliding" | tee -a "$log"
    else
        created_epoch=$(date +%s)
        labels="pgwt=ephemeral,created=$created_epoch,owner=$owner,task=demo-rehearsal"

        echo "demo-rehearsal: creating $server_name from the pgwt=gate-snapshot image (labels: $labels)" | tee -a "$log"
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
        remote_dir="pgwt-demo-rehearsal"
    fi

    # Write the state-file guard NOW -- right after the target is known
    # (VM created, or the persistent box selected), before provisioning or
    # rsync get a chance to fail. Narrow bug found in review: the guard
    # used to only get written after the detached capture had already
    # started, so a provisioning/rsync failure with KEEP=1 left NO state
    # file at all -- the exact same "next launch can't see the kept VM,
    # creates a second one" shape as the earlier KEEP=1-cleanup bug, just
    # on an earlier path. All the fields below are already final at this
    # point (remote_dir is fixed, demo_rehearsal_start_epoch is fixed now
    # and reused unchanged as the PGWT_RUN_MARKER below), so there is
    # nothing to re-write later except PG_VERSION_CONFIRMED once the
    # PG-version probe below has run.
    cat > "$STATE_FILE" <<EOF
SERVER_ID=$server_id
SERVER_IP=$server_ip
TARGET=$target
REMOTE_DIR=$remote_dir
START_EPOCH=$demo_rehearsal_start_epoch
DURATION_MIN=$DURATION_MIN
LOG=$log
PERSISTENT=$PERSISTENT
PG_VERSION_REQUESTED=$PG
PG_VERSION_CONFIRMED=
REHEARSAL_STARTED_EPOCH=
EOF
    echo "demo-rehearsal: state saved to $STATE_FILE (before provisioning) -- a killed launcher, or a provisioning/rsync failure, still leaves a guard so a next launch refuses to create a second VM/target" | tee -a "$log"

    if [[ "$PERSISTENT" != "1" ]]; then
        echo "demo-rehearsal: staging repo + running tests/provision-runner.sh ubuntu on $target" | tee -a "$log"
        ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$target" "mkdir -p pgwt-provision" 2>>"$log"
        rsync -az -e "ssh -o StrictHostKeyChecking=no" ./tests/provision-runner.sh "$target:pgwt-provision/tests-provision-runner.sh" 2>>"$log"
        ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$target" "sudo bash pgwt-provision/tests-provision-runner.sh ubuntu" 2>&1 | tee -a "$log"
        provision_rc=${PIPESTATUS[0]}
        if [[ $provision_rc -ne 0 ]]; then
            echo "demo-rehearsal: tests/provision-runner.sh ubuntu failed (rc=$provision_rc) on the fresh ephemeral VM -- never started (bypass case: a machine that never finished provisioning must not be waited on as if a run were in progress)" | tee -a "$log" >&2
            exit 1
        fi
    fi

    echo "demo-rehearsal: $target  DURATION_MIN=$DURATION_MIN PG=$PG RETAIN_TRACE=$RETAIN_TRACE -> $remote_dir  (log: $log)"
    ssh -o BatchMode=yes $ssh_strict "$target" "mkdir -p '$remote_dir'" || exit 1
    rsync -az --delete -e "ssh $ssh_strict" \
        --exclude .git --exclude /build \
        --exclude '/pgwt-server' --exclude '/pgwt-server-asan' \
        --exclude '/pg_wait_tracer' --exclude '/pg_wait_tracer-asan' \
        --exclude 'tests/results' --exclude 'web/pgwt' --exclude '__pycache__' \
        --exclude '.pgwt-check.stamp' \
        ./ "$target:$remote_dir/" || exit 1

    # ── PG-version gate: refuse LOUDLY here, before the long capture ever
    # starts, rather than discovering 35+ minutes later that the run could
    # never have counted (docs/DEMO_REHEARSAL_CRITERIA.md pins PG $PG; the
    # bug this closes: the last capture-side attempt ran against port
    # 5413, i.e. PG 13, silently). tests/testutil.sh's find_postmaster/
    # postmaster_version are sourced remotely (same functions
    # tests/demo_rehearsal.sh itself uses) -- this is an INDEPENDENT
    # confirmation, not a re-statement of the --pg-version flag passed to
    # that script below: a probe that cannot even be parsed (ssh drop,
    # empty output) is refused, never silently read as "no postmaster"
    # (demo_rehearsal_orchestrator_lib.parse_pg_probe_line raises on
    # anything unparseable). ──────────────────────────────────────────────
    echo "demo-rehearsal: verifying a PostgreSQL $PG postmaster is actually running on $target before starting" | tee -a "$log"
    pg_probe_out=$(ssh -o BatchMode=yes $ssh_strict -o ConnectTimeout=10 "$target" \
        "cd '$remote_dir' 2>/dev/null && . tests/testutil.sh 2>/dev/null && \
         pmpid=\$(find_postmaster --pg-version '$PG' 2>/dev/null); \
         if [ -n \"\$pmpid\" ]; then ver=\$(postmaster_version \"\$pmpid\" 2>/dev/null); echo \"PG_CONFIRMED=\${ver:-UNKNOWN} PID=\$pmpid\"; \
         else echo 'PG_CONFIRMED=NONE'; fi" \
        2>>"$log")
    pg_probe_line=$(tail -n1 <<<"$pg_probe_out")
    if ! pg_parsed=$(python3 tests/demo_rehearsal_orchestrator_lib.py parse-pg-probe "$pg_probe_line" 2>>"$log"); then
        echo "demo-rehearsal: could not parse the PG-version probe's own output ('$pg_probe_line') -- refusing rather than guessing whether $PG is really running" | tee -a "$log" >&2
        exit 1
    fi
    pg_confirmed=$(cut -f1 <<<"$pg_parsed")
    pg_pid=$(cut -f2 <<<"$pg_parsed")
    pg_verdict_msg=$(python3 tests/demo_rehearsal_orchestrator_lib.py pg-version-verdict "$PG" "$pg_confirmed" "$pg_pid")
    pg_verdict_rc=$?
    echo "demo-rehearsal: $pg_verdict_msg" | tee -a "$log"
    if [[ $pg_verdict_rc -ne 0 ]]; then
        exit 1
    fi
    PG_VERSION_CONFIRMED="$pg_confirmed"
    sed -i.bak "s/^PG_VERSION_CONFIRMED=.*/PG_VERSION_CONFIRMED=$PG_VERSION_CONFIRMED/" "$STATE_FILE" && rm -f "$STATE_FILE.bak"

    # ── Kick off the capture DETACHED (issue #176) ──────────────────────
    # nohup + `setsid -w`: setsid makes the child its own session leader
    # (nothing to SIGHUP when this ssh connection drops), -w makes setsid
    # itself wait for it and adopt its exit code, which is what makes `$!`
    # below (the PID of the backgrounded setsid) a reliable liveness check
    # for the WHOLE run, not just setsid's own fork. disown removes it from
    # this remote shell's job table as a second, independent guard.
    echo "demo-rehearsal: starting the capture DETACHED on $target (survives this launcher exiting, even via SIGKILL)" | tee -a "$log"
    # NOTE: `cd ... && rm ... & echo ...` would background the WHOLE
    # and-or chain (cd included), not just the nohup command -- found live
    # while testing this against a real VM: rehearsal.pid ended up written
    # in the ssh login's home directory instead of $remote_dir, and every
    # later remote-state probe (which does `cd $remote_dir` first) then
    # read that as "not-started" even though the job was actually running.
    # `cd && rm` is its own statement (`;`-terminated, runs in THIS shell
    # so the cd sticks), and only the single `nohup ... &` command is
    # backgrounded.
    launch_out=$(ssh -o BatchMode=yes $ssh_strict "$target" \
        "cd '$remote_dir' && rm -f rehearsal.done rehearsal.rc rehearsal.pid rehearsal.out rehearsal.started; \
         nohup setsid -w bash scripts/demo-rehearsal-remote-run.sh '$DURATION_MIN' '$demo_rehearsal_start_epoch' '$PG' '$RETAIN_TRACE' </dev/null >/dev/null 2>&1 & \
         echo \$! > rehearsal.pid; disown; \
         sleep 1; \
         if [[ -s rehearsal.pid ]] && kill -0 \$(cat rehearsal.pid) 2>/dev/null; then echo LAUNCHED=1; else echo LAUNCHED=0; fi" \
        2>>"$log")
    if [[ "$launch_out" != *"LAUNCHED=1"* ]]; then
        echo "demo-rehearsal: FAILED to start the detached capture on $target (got: $launch_out) -- never started" | tee -a "$log" >&2
        exit 1
    fi
    echo "demo-rehearsal: detached capture running on $target (pid recorded remotely at $remote_dir/rehearsal.pid)" | tee -a "$log"
    # $STATE_FILE was already written above, before provisioning -- nothing
    # in it changed since (see the comment there), so there is no second
    # write here. A killed launcher, from this point on, can be resumed
    # with 'make demo-rehearsal-collect'.
fi

# ── Poll for the completion marker with coarse-grained sleeps, never a
# tight loop ────────────────────────────────────────────────────────────
# (issue #176). wait-budget is relative to demo_rehearsal_start_epoch, so a
# --collect invoked any time after a kill sleeps only the REMAINING budget,
# never the full window again.
#
# BLOCKER fixed by review: that "relative to demo_rehearsal_start_epoch"
# was ALWAYS true, even once PGWT_BOX made the target a SHARED box --
# demo_rehearsal_start_epoch is stamped before the flock in
# scripts/demo-rehearsal-remote-run.sh is even attempted, so a run that
# queued behind CI/another agent (that box's own CI spread is 29-117
# minutes, n=9) had its budget consumed by the QUEUE, not the capture, and
# a perfectly healthy capture got declared "stuck". Fixed with
# rehearsal.started (written by remote-run.sh the instant its flock
# resolves, see that script's comment): once known, every subsequent
# budget calculation is anchored on it instead, i.e. on when the capture
# actually began. Until it is known, remote_started_epoch() below is
# polled once per round -- each round being a normal wait-budget-length
# sleep, never a tight loop -- for up to MAX_QUEUE_ROUNDS rounds, so a run
# that is genuinely still queued is retried rather than declared stuck
# using the wrong (queue-inclusive) clock.
#
# REMOTE_STATE_PROBE_ATTEMPTS / _BACKOFF_S: a single ssh call used to be
# enough to declare a completed 35-45 minute rehearsal "unreachable" --
# one dropped packet, a restarted sshd, a transient "connection refused"
# (exactly the ssh exit 255 seen on the gate box) was indistinguishable
# from a genuinely dead VM, and decide-outcome + ephemeral_cleanup then
# DELETED the machine along with results that could never be recreated.
# A blip costs a few seconds; losing 35+ minutes of capture is not
# recoverable -- so "unreachable" is only allowed to mean anything after
# several attempts, spread out, have all failed the same way.
REMOTE_STATE_PROBE_ATTEMPTS="${REMOTE_STATE_PROBE_ATTEMPTS:-5}"
REMOTE_STATE_PROBE_BACKOFF_S="${REMOTE_STATE_PROBE_BACKOFF_S:-10}"
# MAX_QUEUE_ROUNDS: bounds how long this launcher will keep re-arming its
# wait while the capture has never even started (still queued behind
# /tmp/pgwt-box-check.lock). Default 4 extra rounds of a ~50min (default
# DURATION_MIN=35) budget each = ~200min of extra tolerance on top of the
# first round, comfortably above the observed 29-117min CI spread on the
# gate box -- generous on purpose, since giving up too early on a
# genuinely-queued (not stuck) run is exactly the bug this closes.
MAX_QUEUE_ROUNDS="${MAX_QUEUE_ROUNDS:-4}"
# QUEUE_ROUND_MIN_SLEEP_S: floor applied (by
# tests/demo_rehearsal_orchestrator_lib.py's queue_wait_sleep_s, called
# below as `queue-wait-sleep-s`) under the "still queued" round sleep
# (review round 3 finding). That sleep is a launch-anchored
# compute_wait_budget_s, which clamps to 0 once its own deadline has
# passed -- true on a fresh launch only near the very end of the window,
# but ALWAYS true on a `--collect` invoked more than one budget window
# after launch (exactly the case sustained contention produces: a killed
# launcher, reattached later, while the capture is STILL queued). Without
# a floor, 0s sleeps let all MAX_QUEUE_ROUNDS elapse in about a second,
# reporting "never started" for a run that is healthy and about to start
# -- the same false-failure class fix 1 closed, surviving on the
# --collect recovery path. 600s matches GRACE_S below (same order of
# magnitude as the OTHER "wait a bit longer, cheaply" sleep already in
# this script) -- long enough that four rounds cannot busy-loop, short
# enough that MAX_QUEUE_ROUNDS still bounds the total wait to a sane
# multiple of it. The SAME function (unfloored) also drives the re-anchor
# sleep once rehearsal.started is known -- see its own docstring.
QUEUE_ROUND_MIN_SLEEP_S="${QUEUE_ROUND_MIN_SLEEP_S:-600}"

remote_started_epoch() {
    # Best-effort, single probe (not remote_state()'s retry-with-backoff --
    # that function already establishes genuine unreachability at the same
    # checkpoints this is called from; this is purely opportunistic).
    # Prints the epoch rehearsal.started records, or nothing at all if the
    # file does not exist yet (capture genuinely not started), the ssh
    # call failed, or its content is not a bare integer -- any of those
    # must read as "not yet known", never as a wrong number treated as
    # real.
    local strict="" out
    [[ "$PERSISTENT" != "1" ]] && strict="-o StrictHostKeyChecking=no"
    out=$(ssh -o BatchMode=yes $strict -o ConnectTimeout=10 "$target" \
        "cat '$remote_dir/rehearsal.started' 2>/dev/null" 2>>"$log")
    [[ "$out" =~ ^[0-9]+$ ]] && echo "$out"
}

remote_state() {
    local out ssh_rc attempt strict=""
    [[ "$PERSISTENT" != "1" ]] && strict="-o StrictHostKeyChecking=no"
    for ((attempt = 1; attempt <= REMOTE_STATE_PROBE_ATTEMPTS; attempt++)); do
        out=$(ssh -o BatchMode=yes $strict -o ConnectTimeout=10 "$target" \
            "cd '$remote_dir' 2>/dev/null || { echo 'STATE=unreachable RC=-'; exit 0; }; \
             if [[ -f rehearsal.done ]]; then rc=\$(cat rehearsal.rc 2>/dev/null || echo -); echo \"STATE=finished RC=\$rc\"; \
             elif [[ -f rehearsal.pid ]] && kill -0 \$(cat rehearsal.pid 2>/dev/null) 2>/dev/null; then echo 'STATE=running RC=-'; \
             elif [[ -f rehearsal.pid ]]; then echo 'STATE=died RC=-'; \
             else echo 'STATE=not-started RC=-'; fi" 2>>"$log")
        ssh_rc=$?
        if [[ $ssh_rc -eq 0 && -n "$out" ]]; then
            tail -n1 <<<"$out"
            return
        fi
        echo "demo-rehearsal: remote-state probe attempt $attempt/$REMOTE_STATE_PROBE_ATTEMPTS failed (ssh_rc=$ssh_rc) -- not yet calling this unreachable" | tee -a "$log" >&2
        [[ $attempt -lt $REMOTE_STATE_PROBE_ATTEMPTS ]] && sleep "$REMOTE_STATE_PROBE_BACKOFF_S"
    done
    echo "STATE=unreachable RC=-"
}

check_and_finish() {
    local line parsed state rc
    line=$(remote_state)
    echo "demo-rehearsal: remote state: $line" | tee -a "$log"
    if ! parsed=$(python3 tests/demo_rehearsal_orchestrator_lib.py parse-state "$line" 2>>"$log"); then
        echo "demo-rehearsal: could not parse remote state line '$line' -- treating as unreachable" | tee -a "$log" >&2
        parsed=$'unreachable\t-'
    fi
    state=$(cut -f1 <<<"$parsed")
    rc=$(cut -f2 <<<"$parsed")

    if [[ "$state" == "running" ]]; then
        last_state="running"
        return 3
    fi

    collect_results

    local decide results_flag
    results_flag=0; [[ "$results_ok" == "1" ]] && results_flag=1
    decide=$(python3 tests/demo_rehearsal_orchestrator_lib.py decide-outcome "$state" "$rc" "$results_flag")
    outcome_rc=$(cut -f1 <<<"$decide")
    outcome_verdict=$(cut -f2 <<<"$decide")
    last_state="$state"
    return 0
}

# rehearsal_started_epoch: known already (persisted from a previous pass
# through this same launcher invocation, or reloaded on --collect from
# REHEARSAL_STARTED_EPOCH in $STATE_FILE) or empty (not yet learned --
# the normal case for a fresh launch).
rehearsal_started_epoch="${REHEARSAL_STARTED_EPOCH:-}"

remember_started_epoch() {
    # Persists rehearsal_started_epoch into $STATE_FILE so a LATER
    # --collect (after this launcher itself got killed) reattaches already
    # knowing it, instead of needing to relearn it. Best-effort: a failed
    # sed here does not change correctness, only whether a future
    # --collect has to re-probe for it.
    [[ -f "$STATE_FILE" ]] || return
    sed -i.bak "s/^REHEARSAL_STARTED_EPOCH=.*/REHEARSAL_STARTED_EPOCH=$rehearsal_started_epoch/" \
        "$STATE_FILE" 2>/dev/null && rm -f "$STATE_FILE.bak"
}

# budget_anchor_epoch: the launch time on the very first round (matches
# today's behaviour exactly when the box is uncontended -- the common
# case), the actual capture-start time on every round after it is learned.
budget_anchor_epoch="${rehearsal_started_epoch:-$demo_rehearsal_start_epoch}"
budget_s=$(python3 tests/demo_rehearsal_orchestrator_lib.py wait-budget \
    "$budget_anchor_epoch" "$(date +%s)" "$DURATION_MIN")
if [[ "$budget_s" -gt 0 ]]; then
    echo "demo-rehearsal: sleeping ${budget_s}s before checking remote state (anchor: ${rehearsal_started_epoch:+capture start}${rehearsal_started_epoch:-launch time, capture not yet confirmed started})" | tee -a "$log"
    sleep "$budget_s"
fi

last_state=""
outcome_verdict=""
queue_round=1
while true; do
    if check_and_finish; then
        break
    fi
    # Only "running" reaches here. If we already know the real start time,
    # this is genuine staleness (already slept the accurate budget, or
    # this is the grace pass below) -- one grace sleep, then give up.
    if [[ -n "$rehearsal_started_epoch" ]]; then
        echo "demo-rehearsal: still running past its capture-accurate budget -- ONE grace sleep (600s), then a final check" | tee -a "$log"
        sleep 600
        if check_and_finish; then
            break
        fi
        echo "demo-rehearsal: still running after the grace sleep too -- treating as stuck; collecting whatever exists" | tee -a "$log" >&2
        collect_results
        outcome_rc=1
        outcome_verdict="remote run exceeded its capture-accurate wait budget + grace period (stuck)"
        break
    fi

    # Not yet known to have started -- find out whether it has, cheaply,
    # once per round (never a tight poll).
    rehearsal_started_epoch=$(remote_started_epoch)
    if [[ -n "$rehearsal_started_epoch" ]]; then
        remember_started_epoch
        queued_for_s=$(( rehearsal_started_epoch - demo_rehearsal_start_epoch ))
        echo "demo-rehearsal: capture actually started at epoch $rehearsal_started_epoch (queued ${queued_for_s}s behind the box lock) -- recomputing the real remaining budget from that instant" | tee -a "$log"
        queue_sleep_s=$(python3 tests/demo_rehearsal_orchestrator_lib.py queue-wait-sleep-s \
            "$rehearsal_started_epoch" "$demo_rehearsal_start_epoch" "$(date +%s)" "$DURATION_MIN" "$QUEUE_ROUND_MIN_SLEEP_S")
        if [[ "$queue_sleep_s" -gt 0 ]]; then
            echo "demo-rehearsal: sleeping the corrected ${queue_sleep_s}s (capture-relative, not queue-relative, never floored -- a low number here means the capture's own duration has genuinely elapsed)" | tee -a "$log"
            sleep "$queue_sleep_s"
        fi
        continue
    fi

    # Still genuinely queued (never started at all yet) -- bounded retry,
    # never unbounded, never a tight loop: each round re-sleeps a full
    # launch-anchored budget window before checking again -- FLOORED at
    # QUEUE_ROUND_MIN_SLEEP_S by queue-wait-sleep-s (see its own docstring,
    # tests/demo_rehearsal_orchestrator_lib.py): the launch-anchored budget
    # this reuses (rehearsal_started_epoch is STILL unknown here) is
    # clamped to 0 by compute_wait_budget_s once ITS OWN deadline has
    # passed -- always true on a `--collect` invoked more than one budget
    # window after the original launch. Without the floor this degenerates
    # into MAX_QUEUE_ROUNDS back-to-back `sleep 0`s, exhausting the retry
    # budget in about a second for a run that is healthy and simply still
    # queued (review round 3 finding, reproduced in
    # tests/test_demo_rehearsal_orchestrator_lib.py).
    queue_round=$((queue_round + 1))
    if [[ "$queue_round" -gt "$MAX_QUEUE_ROUNDS" ]]; then
        echo "demo-rehearsal: still queued behind the box lock after $MAX_QUEUE_ROUNDS extended waits -- giving up (the box may be under sustained contention; this is NOT the same failure as a stuck/hung capture). The remote job is presumed still alive (queued, not confirmed dead) so \$STATE_FILE is kept -- re-run 'make demo-rehearsal-collect' to try again once the box frees up." | tee -a "$log" >&2
        collect_results
        outcome_rc=1
        outcome_verdict="capture never started within $MAX_QUEUE_ROUNDS extended waits (still queued behind /tmp/pgwt-box-check.lock, not stuck mid-capture) -- retry with 'make demo-rehearsal-collect'"
        break
    fi
    queue_sleep_s=$(python3 tests/demo_rehearsal_orchestrator_lib.py queue-wait-sleep-s \
        "-" "$demo_rehearsal_start_epoch" "$(date +%s)" "$DURATION_MIN" "$QUEUE_ROUND_MIN_SLEEP_S")
    echo "demo-rehearsal: still queued behind the box lock (round $queue_round/$MAX_QUEUE_ROUNDS) -- sleeping another ${queue_sleep_s}s window before checking again" | tee -a "$log"
    sleep "$queue_sleep_s"
done

echo
echo "demo-rehearsal (DURATION_MIN=$DURATION_MIN PG=$PG RETAIN_TRACE=$RETAIN_TRACE target=${target:-unknown} persistent=$PERSISTENT pg_confirmed=${PG_VERSION_CONFIRMED:-not verified} rehearsal_started=${rehearsal_started_epoch:-never confirmed}) verdict: $outcome_verdict (exit=$outcome_rc) -- summary:"
tail -n 30 "$log" | sed 's/^/  /'
exit "$outcome_rc"
