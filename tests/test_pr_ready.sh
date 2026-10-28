#!/usr/bin/env bash
# Hermetic bypass suite for scripts/pr-ready.sh, registered in scripts/check.sh.
# Real scratch Git repositories exercise history/index/untracked detection;
# fake make targets replace remote work and the expensive local check. Nothing
# touches the caller's index, boxes, stamps, hooks, or branch.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
REAL_GIT=$(command -v git)
REAL_BASH=$(command -v bash)
export REAL_GIT
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
export GIT_AUTHOR_NAME=Test GIT_AUTHOR_EMAIL=test@example.invalid
export GIT_COMMITTER_NAME=Test GIT_COMMITTER_EMAIL=test@example.invalid
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR
unset OS PG EPHEMERAL KEEP PGWT_BOX PGWT_BOX_EL8 PGWT_BOX_EL9
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
mkdir -p "$tmpdir/bin" "$tmpdir/logs"
export PR_FORBIDDEN="$tmpdir/forbidden"

cat > "$tmpdir/bin/git" <<'EOF'
#!/usr/bin/env bash
case "${PR_GIT_FAIL:-}:$1" in
    root:rev-parse) printf '%s\n' "$PR_STATE/nonexistent"; exit 0 ;;
    diff:diff|ls-files:ls-files) exit 2 ;;
esac
case "$1" in
    rev-parse|symbolic-ref|merge-base|diff|ls-files|add|write-tree) ;;
    *) echo "git $*" >> "$PR_FORBIDDEN"; exit 98 ;;
esac
exec "$REAL_GIT" "$@"
EOF
cat > "$tmpdir/bin/make" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$PR_STATE/calls"
wait_for() {
    local i
    for ((i=0; i<250; i++)); do
        [[ ! -f "$PR_STATE/$1" ]] || return 0
        sleep 0.02
    done
    echo "timed out waiting for $1" >&2
    return 97
}
case "$1" in
    box-check)
        echo box-start >> "$PR_STATE/events"
        touch "$PR_STATE/box-started"
        if [[ "${PR_ORDER:-0}" == 1 ]]; then
            sleep 2
            # Serial box-before-check ordering must go red too.
            [[ -f "$PR_STATE/check-finished" ]] || exit 96
        fi
        if [[ "${PR_BOX_MUTATE:-0}" == 1 ]]; then
            wait_for check-finished
            echo changed > final-tree-change
        fi
        echo box-finish >> "$PR_STATE/events"
        touch "$PR_STATE/box-finished"
        exit "${PR_BOX_RC:-0}" ;;
    check)
        if [[ "${PR_ORDER:-0}" == 1 ]]; then
            wait_for box-started
            echo check-start >> "$PR_STATE/events"
            sleep 0.2
            [[ ! -f "$PR_STATE/box-finished" ]] || exit 95
        fi
        case "${PR_STAMP:-good}" in
            good) scripts/tree-hash.sh > .pgwt-check.stamp ;;
            wrong) echo fabricated-wrong-stamp > .pgwt-check.stamp ;;
            empty) : > .pgwt-check.stamp ;;
            none) ;;
        esac
        echo check-finish >> "$PR_STATE/events"
        touch "$PR_STATE/check-finished"
        exit "${PR_CHECK_RC:-0}" ;;
    ui-gallery)
        echo gallery >> "$PR_STATE/events"
        [[ "${PR_GALLERY_MUTATE:-0}" != 1 ]] || echo changed > gallery-tree-change
        exit "${PR_GALLERY_RC:-0}" ;;
    *) echo "unexpected make $*" >> "$PR_FORBIDDEN"; exit 98 ;;
esac
EOF
cat > "$tmpdir/bin/gh" <<'EOF'
#!/usr/bin/env bash
echo "gh $*" >> "$PR_FORBIDDEN"
exit 98
EOF
chmod +x "$tmpdir/bin/"*
test_path="$tmpdir/bin:$PATH"

tests_run=0
tests_failed=0
case_number=0
report() {
    local label=$1
    shift
    tests_run=$((tests_run + 1))
    if "$@"; then
        echo "  PASS: $label"
    else
        tests_failed=$((tests_failed + 1))
        echo "  FAIL: $label (exit $rc)"
        cat "$PR_STATE/output" 2>/dev/null || true
    fi
}
g() { "$REAL_GIT" -C "$fixture" "$@"; }
new_case() {
    case_number=$((case_number + 1))
    fixture="$tmpdir/case-$case_number/tree"
    export PR_STATE="$tmpdir/case-$case_number/state"
    mkdir -p "$fixture/scripts" "$fixture/web/static" \
        "$fixture/tests/web_snapshots" "$PR_STATE"
    cp "$REPO_ROOT/scripts/pr-ready.sh" "$fixture/scripts/"
    cp "$REPO_ROOT/scripts/tree-hash.sh" "$fixture/scripts/"
    printf '.pgwt-check.stamp\n*.ignored\n' > "$fixture/.gitignore"
    printf '.PHONY: box-check check ui-gallery\nbox-check:\ncheck:\nui-gallery:\n' > "$fixture/Makefile"
    echo baseline > "$fixture/web/static/app.js"
    echo baseline > "$fixture/tests/web_snapshots/baseline.png"
    echo baseline > "$fixture/web/main.go"
    g init -q
    g symbolic-ref HEAD refs/heads/master
    g add .
    g commit -qm baseline
    g checkout -qb agent/test
    run_dir="$fixture"
    export PGWT_BOX=fake-box TMPDIR="$tmpdir/logs"
    unset OS PG EPHEMERAL KEEP PGWT_BOX_EL8 PGWT_BOX_EL9
    unset PR_GIT_FAIL PR_ORDER PR_BOX_MUTATE PR_GALLERY_MUTATE
    unset PR_STAMP PR_BOX_RC PR_CHECK_RC PR_GALLERY_RC
    PATH="$test_path"
    export PATH
}
run_ready() {
    if (cd "$run_dir" && "$REAL_BASH" "$fixture/scripts/pr-ready.sh" "$@") \
        > "$PR_STATE/output" 2>&1; then rc=0; else rc=$?; fi
    output=$(< "$PR_STATE/output")
}
passed() { [[ "$rc" == 0 && "$output" == *'check PASSED:'* ]]; }
failed_with() { [[ "$rc" != 0 && "$output" == *"$1"* ]]; }
has_call() { grep -Fxq -- "$1" "$PR_STATE/calls"; }
no_gallery() { ! has_call ui-gallery; }
refused() {
    failed_with "REFUSED: $1" && [[ ! -s "$PR_STATE/calls" ]]
}
refusal_case() {
    local label=$1 message=$2
    shift 2
    run_ready "$@"
    report "$label refuses before any make target starts" refused "$message"
}

new_case
export PR_ORDER=1
TIMEFORMAT=%R
{ time run_ready OS=ubuntu PG=16 EPHEMERAL=0 KEEP=1; } 2> "$PR_STATE/elapsed"
elapsed=$(< "$PR_STATE/elapsed")
report "concurrent box (2 s) and check (0.2 s), wall ${elapsed}s" passed
report "box starts before check; check finishes while box is running" \
    python3 - "$PR_STATE/events" <<'PY'
import sys
events = open(sys.argv[1]).read().splitlines()
assert events == ["box-start", "check-start", "check-finish", "box-finish"], events
PY
report "OS/PG/EPHEMERAL/KEEP forwarded to box-check" has_call 'box-check OS=ubuntu PG=16 EPHEMERAL=0 KEEP=1'
report "make check receives no box arguments or --fast" has_call check
report "unchanged UI skips gallery" no_gallery

new_case
export PR_CHECK_RC=7
run_ready
report "valid final stamp wins over check exit 7" passed
report "nonzero check exit is reported honestly" grep -Fq 'check make exit 7' "$PR_STATE/output"
for stamp_mode in wrong empty none; do
    new_case
    export PR_STAMP=$stamp_mode
    # In the 'none' case this would falsely pass if the old stamp survived.
    (cd "$fixture" && scripts/tree-hash.sh > .pgwt-check.stamp)
    run_ready
    report "$stamp_mode stamp fails despite make check exiting zero" failed_with 'check FAILED: stamp missing/mismatch'
done
for target in box gallery; do
    new_case
    if [[ "$target" == box ]]; then
        export PR_BOX_RC=7
    else
        echo changed >> "$fixture/web/static/app.js"
        export PR_GALLERY_RC=7
    fi
    run_ready
    report "$target target failure makes the run fail" failed_with 'make exit 7'
done
for target in box gallery; do
    new_case
    if [[ "$target" == box ]]; then export PR_BOX_MUTATE=1
    else
        echo changed >> "$fixture/web/static/app.js"
        export PR_GALLERY_MUTATE=1
    fi
    run_ready
    report "$target changes after check invalidate the final stamp" failed_with 'check FAILED: stamp missing/mismatch'
done
new_case
echo 'exit 4' > "$fixture/scripts/tree-hash.sh"
run_ready
report "unavailable final tree hash fails closed" failed_with 'hash exit=4'
new_case
cat > "$tmpdir/bin/tee" <<'EOF'
#!/usr/bin/env bash
cat > /dev/null
exit 8
EOF
chmod +x "$tmpdir/bin/tee"
run_ready
report "check log write failure fails even with a valid stamp" failed_with 'FAILED to record check log'
rm "$tmpdir/bin/tee"

# Gate matrix: changes only in each directory, never a coincident web/ edit.
for path in web/static/app.js tests/web_snapshots/baseline.png; do
    for kind in unstaged staged committed untracked deleted ignored; do
        new_case
        case "$kind" in
            unstaged) echo changed >> "$fixture/$path" ;;
            staged) echo changed >> "$fixture/$path"; g add "$path" ;;
            committed) echo changed >> "$fixture/$path"; g add "$path"; g commit -qm changed ;;
            untracked) echo new > "$fixture/$(dirname "$path")/new-file" ;;
            deleted) rm "$fixture/$path" ;;
            ignored) echo ignored > "$fixture/$(dirname "$path")/generated.ignored" ;;
        esac
        run_ready
        report "$path $kind run passes" passed
        if [[ "$kind" == ignored ]]; then
            report "$path ignored file skips gallery" no_gallery
        else
            report "$path $kind triggers gallery" has_call ui-gallery
        fi
    done
done
for kind in unstaged committed untracked; do
    new_case
    case "$kind" in
        unstaged) echo changed >> "$fixture/web/main.go" ;;
        committed) echo changed >> "$fixture/web/main.go"; g add web/main.go; g commit -qm bridge ;;
        untracked) echo new > "$fixture/web/bridge.go" ;;
    esac
    # A missing gallery target must not block bridge-only work either.
    sed '/^ui-gallery:/d' "$fixture/Makefile" > "$fixture/Makefile.new"
    mv "$fixture/Makefile.new" "$fixture/Makefile"
    run_ready
    report "Go-bridge-only $kind run needs no gallery target" passed
    report "Go-bridge-only $kind change skips gallery" no_gallery
done

new_case
unset PGWT_BOX
run_ready EPHEMERAL=1 KEEP=1 OS=ubuntu PG=17
report "EPHEMERAL=1 works without PGWT_BOX" passed
report "ephemeral arguments reach box-check" has_call 'box-check EPHEMERAL=1 KEEP=1 OS=ubuntu PG=17'
report "ephemeral arguments stay out of make check" has_call check
new_case
unset PGWT_BOX
export EPHEMERAL=1 KEEP=1
run_ready
report "environment EPHEMERAL=1 also works without PGWT_BOX" passed
new_case
g update-ref refs/remotes/origin/master master
g branch -D master > /dev/null
echo changed >> "$fixture/tests/web_snapshots/baseline.png"
run_ready
report "origin/master fallback passes" passed
report "origin/master fallback still detects snapshot changes" has_call ui-gallery
for os in el8 el9; do
    new_case
    export PGWT_BOX_EL8=fake-el8 PGWT_BOX_EL9=fake-el9
    run_ready "OS=$os" PG=15
    report "$os explicit box passes" passed
    report "$os args stay on box-check" has_call "box-check OS=$os PG=15"
    report "$os check is full and gets no arguments" has_call check
done

# Every explicit REFUSED path, including Git/filesystem/tool failures.
for pg in '' sixteen 16.1; do
    new_case
    refusal_case "invalid PG='$pg'" 'PG must be a major version number' "PG=$pg"
done
new_case
refusal_case 'unsupported OS' 'unsupported OS=plan9' OS=plan9
new_case
refusal_case 'known but unsupported flag' '--fast is not supported here' --fast
new_case
refusal_case 'unknown flag' "unrecognized argument '--not-a-real-flag'" --not-a-real-flag
new_case
refusal_case 'unknown assignment' "unrecognized argument 'BOGUS=1'" BOGUS=1
for flag in EPHEMERAL KEEP; do
    new_case
    refusal_case "invalid $flag" "$flag must be 0 or 1" "$flag=yes"
done
new_case
mkdir "$tmpdir/nonrepo"
run_dir="$tmpdir/nonrepo"
refusal_case 'outside Git' 'not inside a git repository'
new_case
export PR_GIT_FAIL=root
refusal_case 'inaccessible repository root' 'cannot enter repository root'
new_case
g checkout -q --detach
refusal_case 'detached HEAD' 'not on an agent/ branch (detached HEAD)'
new_case
g checkout -qb feature/test
refusal_case 'non-agent branch' 'not on an agent/ branch: feature/test'
new_case
unset PGWT_BOX
refusal_case 'no box (ephemeral is not implicit)' 'PGWT_BOX is unset or empty'
new_case
unset PGWT_BOX
export EPHEMERAL=1
refusal_case 'explicit EPHEMERAL=0 overrides environment' 'PGWT_BOX is unset or empty' EPHEMERAL=0
for os in el8 el9; do
    new_case
    if [[ "$os" == el8 ]]; then box_var=PGWT_BOX_EL8; else box_var=PGWT_BOX_EL9; fi
    refusal_case "missing $os box" "OS=$os requires $box_var" "OS=$os"
done
new_case
mkdir "$tmpdir/no-make"
ln -s "$REAL_BASH" "$tmpdir/no-make/bash"
ln -s "$tmpdir/bin/git" "$tmpdir/no-make/git"
# Intentional restricted PATH: make must really be absent for this refusal.
# shellcheck disable=SC2123
PATH="$tmpdir/no-make"
refusal_case 'missing make' 'make is unavailable'
PATH="$test_path"
new_case
rm "$fixture/Makefile"
refusal_case 'missing Makefile' 'Makefile is missing'
for kind in missing not-executable; do
    new_case
    if [[ "$kind" == missing ]]; then rm "$fixture/scripts/tree-hash.sh"
    else chmod -x "$fixture/scripts/tree-hash.sh"; fi
    refusal_case "$kind hash tool" 'scripts/tree-hash.sh is missing or not executable'
done
new_case
g branch -D master > /dev/null
refusal_case 'missing comparison base' 'cannot compare changes against master or origin/master'
for fault in diff ls-files; do
    new_case
    export PR_GIT_FAIL=$fault
    if [[ "$fault" == diff ]]; then message='cannot inspect gallery changes'
    else message='cannot inspect untracked gallery files'; fi
    refusal_case "Git $fault failure" "$message"
done
for target in box-check check ui-gallery; do
    new_case
    echo changed >> "$fixture/tests/web_snapshots/baseline.png"
    # Leave the .PHONY name in place: it is not a usable target rule.
    sed "/^$target:/d" "$fixture/Makefile" > "$fixture/Makefile.new"
    mv "$fixture/Makefile.new" "$fixture/Makefile"
    refusal_case "missing $target rule" "required target '$target' is absent from Makefile"
done
new_case
TMPDIR="$tmpdir/nonexistent"
refusal_case 'log directory creation failure' 'cannot create log directory'
new_case
mkdir "$fixture/.pgwt-check.stamp"
echo debris > "$fixture/.pgwt-check.stamp/file"
refusal_case 'unremovable previous stamp' 'cannot remove previous check stamp'

report 'no gh, Git commit/push, or unexpected commands from pr-ready' test ! -e "$PR_FORBIDDEN"
echo "$((tests_run - tests_failed))/$tests_run passed"
[[ "$tests_failed" == 0 ]]
