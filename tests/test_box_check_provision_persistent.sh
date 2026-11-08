#!/usr/bin/env bash
# Exercise the REAL launcher under network shims, like the real-invocation
# probes in test_box_check_object_excludes.sh. Source-string presence cannot
# prove the persistent branch actually stages and invokes the provisioner.
# A scratch repo keeps sweep/tree-hash helpers and result writes hermetic;
# the launcher itself is copied verbatim, never extracted or rewritten.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOX_CHECK="${1:-$SCRIPT_DIR/../scripts/box-check.sh}"
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
[[ -f "$BOX_CHECK" ]] || fail "launcher missing: $BOX_CHECK"

# Exercise the actual privilege check on every uid, including root live-tier
# callers. Guard every early mutation so a broken root check stays hermetic.
mkdir -p "$tmpdir/prov" "$tmpdir/guard-bin"
chmod 755 "$tmpdir" "$tmpdir/prov" "$tmpdir/guard-bin"
cp "$SCRIPT_DIR/provision-runner.sh" "$tmpdir/prov/provision-runner.sh"
chmod 755 "$tmpdir/prov/provision-runner.sh"
: > "$tmpdir/guard-log"
chmod 666 "$tmpdir/guard-log"
cat > "$tmpdir/guard-bin/guard" <<'SHIM'
#!/usr/bin/env bash
printf '%s\n' "${0##*/}" >> "$GUARD_LOG"
exit 99
SHIM
chmod 755 "$tmpdir/guard-bin/guard"
for command in touch chmod systemctl apt-get dnf flock; do
    ln -s guard "$tmpdir/guard-bin/$command"
    [[ $(PATH="/usr/local/bin:$tmpdir/guard-bin:$PATH" command -v "$command") == "$tmpdir/guard-bin/$command" ]] ||
        fail "$command guard would be bypassed by the provisioner PATH"
done
NONROOT_LAUNCH=()
if [[ $(id -u) -eq 0 ]]; then
    if command -v setpriv >/dev/null 2>&1; then
        NONROOT_LAUNCH=(setpriv --reuid=65534 --regid=65534 --clear-groups)
    elif command -v runuser >/dev/null 2>&1; then
        NONROOT_LAUNCH=(runuser -u nobody --)
    else
        fail 'root privilege probe requires setpriv or runuser; cannot skip'
    fi
fi

run_provisioner_as_nonroot() {
    local script="$1" os="$2"
    "${NONROOT_LAUNCH[@]}" env PATH="$tmpdir/guard-bin:$PATH" \
        GUARD_LOG="$tmpdir/guard-log" bash "$script" "$os"
}

assert_privilege_dispatch() {
    local script="$1" rc
    : > "$tmpdir/guard-log"
    run_provisioner_as_nonroot "$script" el9 > "$tmpdir/real-el9" 2>&1
    rc=$?
    [[ $rc -eq 3 ]] || {
        echo "FAIL: non-root el9 provisioner must return 3 (got $rc): $(cat "$tmpdir/real-el9")" >&2
        return 1
    }
    echo 'PASS: real non-shimmed el9 provisioner exits 3 without root'
    run_provisioner_as_nonroot "$script" ubuntu > "$tmpdir/real-ubuntu" 2>&1
    rc=$?
    [[ $rc -eq 1 ]] || {
        echo "FAIL: non-root ubuntu provisioner must return 1, not 3 (got $rc): $(cat "$tmpdir/real-ubuntu")" >&2
        return 1
    }
    /usr/bin/grep -Fq 'provision-runner.sh must run as root' "$tmpdir/real-ubuntu" ||
        { echo 'FAIL: ubuntu provisioner did not reach the root check' >&2; return 1; }
    [[ ! -s "$tmpdir/guard-log" ]] ||
        { echo 'FAIL: non-root provisioner reached a mutation guard' >&2; return 1; }
    echo 'PASS: real non-shimmed ubuntu provisioner reaches root check and exits 1, not 3'
    echo 'PASS: non-root privilege probes leave mutation guard log empty'
}
assert_privilege_dispatch "$tmpdir/prov/provision-runner.sh" || exit $?

# Each mutant must find exactly one root check and OS dispatch. Buffer the
# source so the root block can be moved before the earlier OS dispatch.
for mutation in root-first root-deleted; do
    mutant="$tmpdir/prov/$mutation.sh"
    awk -v mutation="$mutation" '
        { source[NR] = $0 }
        /^if \[\[ \$\(id -u\) -ne 0 \]\]; then$/ {
            root_count++; root_start = NR; in_root = 1
        }
        in_root && /^fi$/ { root_end = NR; in_root = 0 }
        /^case "\$OS" in$/ && !root_start { dispatch_count++; dispatch = NR }
        END {
            if (root_count != 1 || dispatch_count != 1 || !root_end || dispatch >= root_start) exit 2
            for (i = 1; i <= NR; i++) {
                if (i == dispatch && mutation == "root-first")
                    for (j = root_start; j <= root_end; j++) print source[j]
                if (i < root_start || i > root_end) print source[i]
            }
        }
    ' "$SCRIPT_DIR/provision-runner.sh" > "$mutant" || {
        echo "FAIL: cannot construct $mutation provisioner mutant (rc=2)" >&2
        exit 2
    }
    chmod 755 "$mutant"
    assert_privilege_dispatch "$mutant" > "$tmpdir/privilege-mutant-verdict" 2>&1
    mutant_rc=$?
    [[ $mutant_rc -ne 0 ]] || fail "$mutation provisioner mutant escaped privilege assertions"
    if [[ "$mutation" == root-first ]]; then
        /usr/bin/grep -Fq 'el9 provisioner must return 3 (got 1)' "$tmpdir/privilege-mutant-verdict" ||
            fail 'root-first mutant failed for an unexpected reason'
        [[ ! -s "$tmpdir/guard-log" ]] || fail 'root-first mutant reached a mutation guard'
    else
        /usr/bin/grep -Fq 'ubuntu provisioner must return 1, not 3 (got 99)' "$tmpdir/privilege-mutant-verdict" ||
            fail 'root-deleted mutant failed for an unexpected reason'
        [[ -s "$tmpdir/guard-log" ]] || fail 'root-deleted mutant did not hit a mutation guard'
    fi
    echo "PASS: $mutation provisioner mutant rejected by the same privilege assertions"
done

mkdir -p "$tmpdir/bin" "$tmpdir/repo/scripts" "$tmpdir/repo/tests"

# Log argv as tab-separated fields (these fixture arguments contain no tabs).
# Never execute a remote command. PROVISION_REAL runs only the local provisioner
# under child shims that stop it at touch. Even accidental cloud/ephemeral calls
# are intercepted, so inherited credentials cannot turn this into a network test.
cat > "$tmpdir/bin/shim" <<'SHIM'
#!/usr/bin/env bash
name=${0##*/}
printf '%s' "$name" >> "$CALL_LOG"
printf '\t%s' "$@" >> "$CALL_LOG"
printf '\n' >> "$CALL_LOG"
case "$name" in
    git) printf 'fixture-head\n' ;;
    ssh)
        remote_command="${*: -1}"
        case "$remote_command" in
            'mkdir -p pgwt-provision') exit "${MKDIR_RC:-0}" ;;
            'rm -f '*) exit "${CLEANUP_RC:-0}" ;;
            'sudo bash pgwt-provision/'*-tests-provision-runner.sh\ *)
                if [[ -n "${PROVISION_REAL:-}" ]]; then
                    read -r -a provision_args <<< "$remote_command"
                    PATH="$CHILD_SHIM_BIN:$PATH" bash "$PROVISION_REAL" "${provision_args[@]:3}"
                    exit $?
                fi
                exit "${PROVISION_RC:-0}"
                ;;
        esac ;;
    rsync)
        case "${*: -1}" in
            *:pgwt-provision/*-tests-provision-runner.sh) exit "${STAGE_RC:-0}" ;;
        esac ;;
esac
exit 0
SHIM
chmod +x "$tmpdir/bin/shim"
for command in ssh rsync scp curl security hcloud git; do
    ln -s shim "$tmpdir/bin/$command"
done
printf '#!/usr/bin/env bash\nexit 0\n' > "$tmpdir/repo/tests/hetzner-sweep.sh"
printf '#!/usr/bin/env bash\nprintf "fixture-tree\\n"\n' > "$tmpdir/repo/scripts/tree-hash.sh"
chmod +x "$tmpdir/repo/tests/hetzner-sweep.sh" "$tmpdir/repo/scripts/tree-hash.sh"

run_launcher() {
    local script="$1" os="$2" provision_rc="${3:-0}" stage_rc="${4:-0}" mkdir_rc="${5:-0}"
    cp "$script" "$tmpdir/repo/scripts/box-check.sh" || return 2
    : > "$tmpdir/calls"
    PATH="$tmpdir/bin:$PATH" CALL_LOG="$tmpdir/calls" \
        OS="$os" EPHEMERAL=0 PGWT_BOX=fixture-fallback \
        PGWT_BOX_UBUNTU=fixture-ubuntu PGWT_BOX_EL8=fixture-el8 PGWT_BOX_EL9=fixture-el9 \
        PROVISION_RC="$provision_rc" STAGE_RC="$stage_rc" MKDIR_RC="$mkdir_rc" \
        bash "$tmpdir/repo/scripts/box-check.sh" > "$tmpdir/output" 2>&1
}

# Refuse with 2 if no invocation was emitted, as the precedent's parser
# does: an empty probe is not proof that the command is safely unlocked.
provision_command() {
    local command
    command=$(awk -F '\t' '$1 == "ssh" && $NF ~ /^sudo bash .*tests-provision-runner[.]sh / { print $NF }' "$1")
    if [[ -z "$command" ]]; then
        echo 'FAIL: persistent provisioner invocation missing (probe refuses rc=2)' >&2
        return 2
    fi
    printf '%s\n' "$command"
}

staged_destination() {
    awk -F '\t' '$1 == "rsync" && $(NF-1) == "./tests/provision-runner.sh" { print $NF }' "$1"
}

assert_cleanup() {
    local os="$1" command provision_path
    command=$(provision_command "$tmpdir/calls") || return $?
    provision_path=${command#sudo bash }
    provision_path=${provision_path%% *}
    awk -F '\t' -v target="fixture-$os" -v command="$command" \
        -v cleanup_command="rm -f \"$provision_path\"" '
        $1 == "ssh" && $(NF-1) == target && $NF == command { provision = NR }
        $1 == "ssh" && $(NF-1) == target && $NF == cleanup_command { cleanup = NR; count++ }
        END { exit !(provision && count == 1 && provision < cleanup) }
    ' "$tmpdir/calls" || { echo "FAIL: persistent $os must clean up its staged provisioner after execution" >&2; return 1; }
    echo "PASS: persistent $os issues rm -f for its staged provisioner after execution"
}

assert_provisioned() {
    local os="$1" command provision_path stage_destination run_pattern
    command=$(provision_command "$tmpdir/calls") || return $?
    run_pattern="^sudo bash (pgwt-provision/[0-9]+-[0-9]+-tests-provision-runner[.]sh) $os --lock-wait unbounded$"
    [[ "$command" =~ $run_pattern ]] || {
        echo "FAIL: persistent $os provisioner must receive OS without an outer flock: $command" >&2
        return 1
    }
    provision_path=${BASH_REMATCH[1]}
    stage_destination=$(staged_destination "$tmpdir/calls")
    [[ "$stage_destination" == "fixture-$os:$provision_path" ]] || {
        echo "FAIL: persistent $os staging destination must match its executed path: $stage_destination vs $provision_path" >&2
        return 1
    }
    echo "PASS: persistent $os stages and executes the same per-run provisioner path"
    # Check the emitted remote command, not a comment in the source. This
    # also catches a provisioner appended to the existing locked build SSH.
    if [[ "$command" == *flock* || "$command" == */tmp/pgwt-box-check.lock* ]]; then
        echo 'FAIL: persistent provisioner nested inside outer box-check flock' >&2
        return 1
    fi
    # Verify real staging argv, correct per-OS target, and stage -> provision
    # -> build ordering. Finding none of any required operation refuses too.
    awk -F '\t' -v target="fixture-$os" -v path="$provision_path" -v command="$command" '
        $1 == "rsync" && $(NF-1) == "./tests/provision-runner.sh" &&
            $NF == target ":" path { stage = NR }
        $1 == "ssh" && $(NF-1) == target &&
            $NF == command { provision = NR }
        $1 == "ssh" && $NF ~ /tests\/run_all[.]sh --require-live/ { build = NR }
        END { exit !(stage && provision && build && stage < provision && provision < build) }
    ' "$tmpdir/calls" || { echo "FAIL: persistent $os must stage to its target before provisioning and build" >&2; return 1; }
    assert_cleanup "$os" || return $?
}

for os in ubuntu el8 el9; do
    run_launcher "$BOX_CHECK" "$os" || fail "persistent $os launcher failed: $(cat "$tmpdir/output")"
    assert_provisioned "$os" || exit $?
    echo "PASS: persistent $os stages and provisions with $os before build, without outer flock"
done

run_launcher "$BOX_CHECK" ubuntu || fail "first uniqueness run failed: $(cat "$tmpdir/output")"
assert_provisioned ubuntu || exit $?
first_stage=$(staged_destination "$tmpdir/calls")
run_launcher "$BOX_CHECK" ubuntu || fail "second uniqueness run failed: $(cat "$tmpdir/output")"
assert_provisioned ubuntu || exit $?
second_stage=$(staged_destination "$tmpdir/calls")
[[ "$first_stage" != "$second_stage" ]] || fail "consecutive launchers reused staged path: $first_stage"
echo 'PASS: consecutive launcher processes use different staged provisioner paths'

CLEANUP_RC=7 run_launcher "$BOX_CHECK" ubuntu || fail "cleanup failure changed successful launcher result: $(cat "$tmpdir/output")"
assert_provisioned ubuntu || exit $?
echo 'PASS: failing best-effort cleanup SSH preserves successful provision and launcher result'

run_launcher "$BOX_CHECK" el9 3 || fail "unsupported el9 launcher failed: $(cat "$tmpdir/output")"
assert_provisioned el9 || exit $?
/usr/bin/grep -Eiq 'unsupported' "$tmpdir/output" ||
    fail 'unsupported el9 provisioning skip was not announced'
echo 'PASS: persistent el9 rc=3 announces unsupported-OS skip and reaches build'

# Negative control: remove the persistent provisioner SSH invocation from a
# copy, keeping staging and status handling intact. Match the command itself,
# independently of BEGIN/END markers; refuse if it does not occur exactly once.
mutant="$tmpdir/box-check-mutant.sh"
awk '
    /^[[:space:]]*ssh -o BatchMode=yes "\$target" / &&
        /sudo bash \$prov_stage \$OS/ { removed++; next }
    { print }
    END { if (removed != 1) exit 2 }
' "$BOX_CHECK" > "$mutant" || fail "cannot construct provision-invocation deletion mutant (rc=2)"
run_launcher "$mutant" ubuntu || fail 'deletion mutant did not reach the build'
assert_provisioned ubuntu > "$tmpdir/mutant-verdict" 2>&1
mutant_rc=$?
[[ $mutant_rc -eq 2 ]] || fail "deletion mutant escaped missing-invocation refusal (rc=$mutant_rc)"
echo "PASS: deletion mutant rejected: $(cat "$tmpdir/mutant-verdict")"

# Provisioning errors and the distinct contention code must stop before
# build. Contention changes the diagnostic, never the fatal outcome.
for provision_rc in 1 7 75; do
    run_launcher "$BOX_CHECK" ubuntu "$provision_rc"
    rc=$?
    [[ $rc -eq 1 ]] || fail "provision failure rc=$provision_rc must be fatal (got $rc)"
    provision_command "$tmpdir/calls" >/dev/null || exit $?
    assert_cleanup ubuntu || exit $?
    if /usr/bin/grep -q 'tests/run_all.sh --require-live' "$tmpdir/calls"; then
        fail "build ran after provision failure rc=$provision_rc"
    fi
    if [[ $provision_rc -eq 75 ]]; then
        /usr/bin/grep -Eiq 'busy|contention' "$tmpdir/output" ||
            fail 'rc=75 did not announce lock contention'
        echo 'PASS: rc=75 announces contention and remains fatal before build'
    fi
    echo "PASS: provision failure rc=$provision_rc is fatal before build"
done

# Force the first child after the root check to fail. Never try CHILD_RC=0:
# that would continue into real host provisioning instead of stopping at touch.
CHILD_SHIM_BIN="$tmpdir/child-bin"
mkdir -p "$CHILD_SHIM_BIN"
cat > "$CHILD_SHIM_BIN/id" <<'SHIM'
#!/usr/bin/env bash
[[ "${1:-}" == -u ]] || exit 1
printf '0\n'
SHIM
cat > "$CHILD_SHIM_BIN/touch" <<'SHIM'
#!/usr/bin/env bash
exit "${CHILD_RC:?}"
SHIM
chmod +x "$CHILD_SHIM_BIN/id" "$CHILD_SHIM_BIN/touch"
# The provisioner prepends /usr/local/bin after checking root. Refuse before
# running it if that would shadow the failing touch shim on this host.
[[ $(PATH="/usr/local/bin:$CHILD_SHIM_BIN:$PATH" command -v touch) == "$CHILD_SHIM_BIN/touch" ]] ||
    fail 'touch shim would be bypassed by the provisioner PATH'
echo 'PASS: provisioner PATH resolves touch to the failing child shim'

assert_stopped_before_lock() {
    if /usr/bin/grep -Fq 'waiting for /tmp/pgwt-box-check.lock' "$1"; then
        fail "$2 reached the lock wait: touch shim bypassed"
    fi
    echo "PASS: $2 stops before the lock wait"
}

PATH="$CHILD_SHIM_BIN:$PATH" CHILD_RC=3 \
    bash "$SCRIPT_DIR/provision-runner.sh" ubuntu > "$tmpdir/child-output" 2>&1
rc=$?
assert_stopped_before_lock "$tmpdir/child-output" 'direct provisioner with child rc=3'
[[ $rc -eq 1 ]] || fail "child rc=3 must be normalised to 1 (got $rc): $(cat "$tmpdir/child-output")"
echo 'PASS: real provisioner normalises child rc=3 to 1'

PATH="$CHILD_SHIM_BIN:$PATH" CHILD_RC=75 \
    bash "$SCRIPT_DIR/provision-runner.sh" ubuntu > "$tmpdir/child-output" 2>&1
rc=$?
assert_stopped_before_lock "$tmpdir/child-output" 'direct provisioner with child rc=75'
[[ $rc -eq 1 ]] || fail "child rc=75 must be normalised to 1 (got $rc): $(cat "$tmpdir/child-output")"
/usr/bin/grep -Fq 'a child command exited 75; normalised to 1' "$tmpdir/child-output" ||
    fail 'child rc=75 did not announce normalisation'
echo 'PASS: real provisioner normalises child rc=75 to 1'

# Both reserved statuses from children must fail provisioning without being
# mistaken for the deliberate unsupported-OS or bounded-timeout paths.
for child_rc in 3 75; do
    PROVISION_REAL="$SCRIPT_DIR/provision-runner.sh" CHILD_SHIM_BIN="$CHILD_SHIM_BIN" CHILD_RC="$child_rc" \
        run_launcher "$BOX_CHECK" ubuntu
    rc=$?
    assert_stopped_before_lock "$tmpdir/output" "launcher with child rc=$child_rc"
    [[ $rc -eq 1 ]] || fail "launcher with real child rc=$child_rc must fail with 1 (got $rc): $(cat "$tmpdir/output")"
    assert_cleanup ubuntu || exit $?
    echo "PASS: launcher with real provisioner child rc=$child_rc exits 1"
    if /usr/bin/grep -Fq 'does not support OS' "$tmpdir/output"; then
        fail "child rc=$child_rc was misreported as unsupported OS"
    fi
    echo "PASS: child rc=$child_rc is not reported as unsupported OS"
    if /usr/bin/grep -Fq 'tests/run_all.sh --require-live' "$tmpdir/calls"; then
        fail "build ran after real provisioner child rc=$child_rc"
    fi
    if [[ $child_rc -eq 75 ]]; then
        if /usr/bin/grep -Eiq 'busy|contention' "$tmpdir/output"; then
            fail 'child rc=75 was misreported as lock contention'
        fi
        echo 'PASS: child rc=75 stops launcher before build without a contention diagnostic'
    else
        echo "PASS: child rc=$child_rc stops launcher before build"
    fi
done

# Stop at flock, before any provisioning. The lock file is a scratch path;
# id/touch/chmod are shims and systemctl is a logged last-resort guard.
LOCK_SHIM_BIN="$tmpdir/lock-bin"
mkdir -p "$LOCK_SHIM_BIN"
cp "$CHILD_SHIM_BIN/id" "$LOCK_SHIM_BIN/id"
printf '#!/usr/bin/env bash\nexit 0\n' > "$LOCK_SHIM_BIN/touch"
cp "$LOCK_SHIM_BIN/touch" "$LOCK_SHIM_BIN/chmod"
cat > "$LOCK_SHIM_BIN/flock" <<'SHIM'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$FLOCK_LOG"
exit "${FLOCK_RC:?}"
SHIM
cat > "$LOCK_SHIM_BIN/systemctl" <<'SHIM'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$SYSTEMCTL_LOG"
exit 98
SHIM
chmod +x "$LOCK_SHIM_BIN/"*
for command in id touch chmod systemctl flock; do
    [[ $(PATH="/usr/local/bin:$LOCK_SHIM_BIN:$PATH" command -v "$command") == "$LOCK_SHIM_BIN/$command" ]] ||
        fail "$command lock-test shim would be bypassed by the provisioner PATH"
done
echo 'PASS: provisioner PATH resolves flock and provisioning guards to child shims'

run_lock_probe() {
    local flock_rc="$1"
    shift
    : > "$tmpdir/flock-log"
    : > "$tmpdir/systemctl-log"
    PATH="$LOCK_SHIM_BIN:$PATH" PROVISION_LOCK_FILE="$tmpdir/lock" \
        FLOCK_LOG="$tmpdir/flock-log" SYSTEMCTL_LOG="$tmpdir/systemctl-log" FLOCK_RC="$flock_rc" \
        bash "$SCRIPT_DIR/provision-runner.sh" ubuntu "$@" > "$tmpdir/lock-output" 2>&1
    lock_rc=$?
    [[ ! -s "$tmpdir/systemctl-log" ]] || fail 'lock probe reached real provisioning'
}

assert_flock_args() {
    [[ $(cat "$tmpdir/flock-log") == "$1" ]] ||
        fail "unexpected flock argv: $(cat "$tmpdir/flock-log") (expected $1)"
}

assert_plain_lock_failure() {
    [[ $lock_rc -eq 1 ]] || fail "flock failure must return 1, not 75 (got $lock_rc)"
    /usr/bin/grep -Fq 'not lock contention' "$tmpdir/lock-output" ||
        fail 'flock error was not identified as a plain failure'
    if /usr/bin/grep -Fq 'FATAL: could not acquire' "$tmpdir/lock-output"; then
        fail 'flock error was misreported as a lock-wait timeout'
    fi
}

run_lock_probe 75
[[ $lock_rc -eq 75 ]] || fail "default lock timeout must return 75 (got $lock_rc)"
assert_flock_args '-w 600 -E 75 9'
/usr/bin/grep -Fq "waiting for $tmpdir/lock (up to 600s)" "$tmpdir/lock-output" ||
    fail 'default bounded wait was not logged'
/usr/bin/grep -Fq 'FATAL: could not acquire' "$tmpdir/lock-output" || fail 'lock timeout lacks contention diagnostic'
/usr/bin/grep -Fq 'lock contention:' "$tmpdir/lock-output" || fail 'lock timeout does not identify contention'
echo 'PASS: default lock wait uses -w 600 -E 75 and reserves rc=75 for timeout'

for flock_rc in 71 75; do
    run_lock_probe "$flock_rc" --lock-wait unbounded
    assert_flock_args '9'
    assert_plain_lock_failure
    /usr/bin/grep -Fq "waiting for $tmpdir/lock (unbounded)" "$tmpdir/lock-output" ||
        fail 'unbounded wait was not logged'
    echo "PASS: unbounded flock rc=$flock_rc uses bare flock and returns plain failure rc=1"
done

run_lock_probe 71
assert_flock_args '-w 600 -E 75 9'
assert_plain_lock_failure
echo 'PASS: bounded flock rc=71 is a plain failure, not contention'

run_lock_probe 75 --lock-wait 5
assert_flock_args '-w 5 -E 75 9'
[[ $lock_rc -eq 75 ]] || fail "custom bounded timeout must return 75 (got $lock_rc)"
run_lock_probe 75 --lock-wait=5
assert_flock_args '-w 5 -E 75 9'
[[ $lock_rc -eq 75 ]] || fail "equals-form bounded timeout must return 75 (got $lock_rc)"
run_lock_probe 71 --lock-wait=unbounded
assert_flock_args '9'
assert_plain_lock_failure
echo 'PASS: custom lock waits accept separate and equals-form arguments'

for invalid_wait in bogus 0 -1 1.5 ''; do
    for flag in separate equals; do
        if [[ "$flag" == separate ]]; then
            run_lock_probe 75 --lock-wait "$invalid_wait"
        else
            run_lock_probe 75 "--lock-wait=$invalid_wait"
        fi
        [[ $lock_rc -eq 2 ]] || fail "invalid lock wait '$invalid_wait' ($flag) must return 2 (got $lock_rc)"
        [[ ! -s "$tmpdir/flock-log" ]] || fail 'invalid lock wait reached flock'
    done
done
run_lock_probe 75 --lock-wait
[[ $lock_rc -eq 2 ]] || fail "missing lock-wait value must return 2 (got $lock_rc)"
[[ ! -s "$tmpdir/flock-log" ]] || fail 'missing lock-wait value reached flock'
echo 'PASS: invalid or missing lock waits return usage rc=2 before flock'

for stage in mkdir rsync; do
    if [[ "$stage" == mkdir ]]; then
        run_launcher "$BOX_CHECK" ubuntu 0 0 1
    else
        run_launcher "$BOX_CHECK" ubuntu 0 1 0
    fi
    rc=$?
    [[ $rc -eq 1 ]] || fail "$stage failure must be fatal (got $rc)"
    if /usr/bin/grep -Eq 'sudo bash pgwt-provision/[^[:space:]]*-tests-provision-runner[.]sh |tests/run_all[.]sh --require-live' "$tmpdir/calls"; then
        fail "provision/build ran after $stage failure"
    fi
    echo "PASS: $stage failure stops before provision/build"
    /usr/bin/grep -Eq 'box-check: .* failed on persistent fixture-ubuntu \(see .+\)' "$tmpdir/output" ||
        fail "$stage failure did not surface its cause and log path on the terminal"
    echo "PASS: $stage failure announces its cause and log path on the terminal"
done
echo 'OK: persistent box provisioning behavioural checks'
