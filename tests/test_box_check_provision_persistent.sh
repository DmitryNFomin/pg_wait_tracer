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

# Exercise the actual provisioner BEFORE installing any command shims. The
# supported OS must reach the privilege check, while the unsupported OS
# must return its reserved code even without root. Never provision this host
# when this registered unit test is run by the live tier as root.
if [[ $(id -u) -ne 0 ]]; then
    "$SCRIPT_DIR/provision-runner.sh" el9 > "$tmpdir/real-el9" 2>&1
    rc=$?
    [[ $rc -eq 3 ]] || fail "real el9 provisioner must return 3 (got $rc): $(cat "$tmpdir/real-el9")"
    echo 'PASS: real non-shimmed el9 provisioner exits 3 without root'
    "$SCRIPT_DIR/provision-runner.sh" ubuntu > "$tmpdir/real-ubuntu" 2>&1
    rc=$?
    [[ $rc -eq 1 ]] || fail "real ubuntu provisioner must return 1, not 3 (got $rc): $(cat "$tmpdir/real-ubuntu")"
    grep -Fq 'provision-runner.sh must run as root' "$tmpdir/real-ubuntu" ||
        fail 'real ubuntu provisioner did not reach the root check'
    echo 'PASS: real non-shimmed ubuntu provisioner reaches root check and exits 1, not 3'
else
    echo 'SKIP: real provisioner privilege checks require a non-root caller'
fi

mkdir -p "$tmpdir/bin" "$tmpdir/repo/scripts" "$tmpdir/repo/tests"

# Log argv as tab-separated fields (these fixture arguments contain no tabs).
# Never execute a remote command. Even accidental cloud/ephemeral calls are
# intercepted, so inherited credentials cannot turn this into a network test.
cat > "$tmpdir/bin/shim" <<'SHIM'
#!/usr/bin/env bash
name=${0##*/}
printf '%s' "$name" >> "$CALL_LOG"
printf '\t%s' "$@" >> "$CALL_LOG"
printf '\n' >> "$CALL_LOG"
case "$name" in
    git) printf 'fixture-head\n' ;;
    ssh)
        case "${*: -1}" in
            'mkdir -p pgwt-provision') exit "${MKDIR_RC:-0}" ;;
            *tests-provision-runner.sh*) exit "${PROVISION_RC:-0}" ;;
        esac ;;
    rsync)
        case "${*: -1}" in
            *:pgwt-provision/tests-provision-runner.sh) exit "${STAGE_RC:-0}" ;;
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
    command=$(awk -F '\t' '$1 == "ssh" && $NF ~ /tests-provision-runner[.]sh/ { print $NF }' "$1")
    if [[ -z "$command" ]]; then
        echo 'FAIL: persistent provisioner invocation missing (probe refuses rc=2)' >&2
        return 2
    fi
    printf '%s\n' "$command"
}

assert_provisioned() {
    local os="$1" command
    command=$(provision_command "$tmpdir/calls") || return $?
    [[ "$command" == "sudo bash pgwt-provision/tests-provision-runner.sh $os" ]] || {
        echo "FAIL: persistent $os provisioner must receive OS without an outer flock: $command" >&2
        return 1
    }
    # Check the emitted remote command, not a comment in the source. This
    # also catches a provisioner appended to the existing locked build SSH.
    if [[ "$command" == *flock* || "$command" == */tmp/pgwt-box-check.lock* ]]; then
        echo 'FAIL: persistent provisioner nested inside outer box-check flock' >&2
        return 1
    fi
    # Verify real staging argv, correct per-OS target, and stage -> provision
    # -> build ordering. Finding none of any required operation refuses too.
    awk -F '\t' -v target="fixture-$os" '
        $1 == "rsync" && $(NF-1) == "./tests/provision-runner.sh" &&
            $NF == target ":pgwt-provision/tests-provision-runner.sh" { stage = NR }
        $1 == "ssh" && $(NF-1) == target &&
            $NF ~ /^sudo bash pgwt-provision\/tests-provision-runner[.]sh / { provision = NR }
        $1 == "ssh" && $NF ~ /tests\/run_all[.]sh --require-live/ { build = NR }
        END { exit !(stage && provision && build && stage < provision && provision < build) }
    ' "$tmpdir/calls" || { echo "FAIL: persistent $os must stage to its target before provisioning and build" >&2; return 1; }
}

for os in ubuntu el8 el9; do
    run_launcher "$BOX_CHECK" "$os" || fail "persistent $os launcher failed: $(cat "$tmpdir/output")"
    assert_provisioned "$os" || exit $?
    echo "PASS: persistent $os stages and provisions with $os before build, without outer flock"
done

run_launcher "$BOX_CHECK" el9 3 || fail "unsupported el9 launcher failed: $(cat "$tmpdir/output")"
assert_provisioned el9 || exit $?
grep -Fq 'WARNING: provisioner does not support OS=el9 (rc=3); provisioning skipped for that reason; using persistent fixture-el9 as-is' "$tmpdir/output" ||
    fail 'unsupported el9 provisioning skip was not announced'
echo 'PASS: persistent el9 rc=3 announces unsupported-OS skip and reaches build'

# Negative control: delete exactly the inclusive BEGIN/END persistent
# provisioning block from a copy of the real script using awk. Verify both
# markers occur once; an absent/changed block must refuse, not create a
# no-op mutant and silently approve an untested assertion.
mutant="$tmpdir/box-check-mutant.sh"
awk '
    /^    # BEGIN persistent provisioning$/ { begin++; dropping = 1 }
    !dropping { print }
    /^    # END persistent provisioning$/ { end++; dropping = 0 }
    END { if (begin != 1 || end != 1 || dropping) exit 2 }
' "$BOX_CHECK" > "$mutant" || fail "cannot construct provision-block deletion mutant (rc=2)"
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
    if grep -q 'tests/run_all.sh --require-live' "$tmpdir/calls"; then
        fail "build ran after provision failure rc=$provision_rc"
    fi
    if [[ $provision_rc -eq 75 ]]; then
        grep -Fq "persistent fixture-ubuntu is busy (rc=75): contention, not a provisioning defect; CI or another box-check held /tmp/pgwt-box-check.lock past the provisioner's 600 s wait; caller should retry this run later" "$tmpdir/output" ||
            fail 'rc=75 did not announce lock contention and caller retry guidance'
        echo 'PASS: rc=75 announces contention and remains fatal before build'
    fi
    echo "PASS: provision failure rc=$provision_rc is fatal before build"
done
for stage in mkdir rsync; do
    if [[ "$stage" == mkdir ]]; then
        run_launcher "$BOX_CHECK" ubuntu 0 0 1
    else
        run_launcher "$BOX_CHECK" ubuntu 0 1 0
    fi
    rc=$?
    [[ $rc -eq 1 ]] || fail "$stage failure must be fatal (got $rc)"
    if grep -q 'sudo bash pgwt-provision/tests-provision-runner.sh\|tests/run_all.sh --require-live' "$tmpdir/calls"; then
        fail "provision/build ran after $stage failure"
    fi
    echo "PASS: $stage failure stops before provision/build"
done
echo 'OK: persistent box provisioning behavioural checks'
