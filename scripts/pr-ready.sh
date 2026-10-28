#!/usr/bin/env bash
# Mechanical definition-of-done gates. Run from inside the agent worktree.
# Usage: scripts/pr-ready.sh [OS=ubuntu|el8|el9] [PG=major]
set -uo pipefail

die() { printf 'pr-ready: REFUSED: %s\n' "$*" >&2; exit 1; }
event() { printf '[%s] %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$*" >&3; }

box_args=(box-check)
os=${OS:-ubuntu}
for arg in "$@"; do
    case "$arg" in
        OS=*) os=${arg#OS=}; box_args+=("$arg") ;;
        PG=*)
            pg=${arg#PG=}
            [[ -n "$pg" && "$pg" != *[!0-9]* ]] || die "PG must be a major version number"
            box_args+=("$arg") ;;
        *) die "usage: scripts/pr-ready.sh [OS=ubuntu|el8|el9] [PG=major]" ;;
    esac
done
case "$os" in ubuntu|el8|el9) ;; *) die "unsupported OS=$os" ;; esac

root=$(git rev-parse --show-toplevel 2>/dev/null) || die "not inside a git repository"
cd "$root" || die "cannot enter repository root"
branch=$(git symbolic-ref --quiet --short HEAD) || die "not on an agent/ branch (detached HEAD)"
[[ "$branch" == agent/?* ]] || die "not on an agent/ branch: $branch"
[[ -n "${PGWT_BOX:-}" ]] || die 'PGWT_BOX is unset or empty; configure a live box'
case "$os" in
    el8) [[ -n "${PGWT_BOX_EL8:-}" ]] || die 'OS=el8 requires PGWT_BOX_EL8' ;;
    el9) [[ -n "${PGWT_BOX_EL9:-}" ]] || die 'OS=el9 requires PGWT_BOX_EL9' ;;
esac
command -v make >/dev/null 2>&1 || die "make is unavailable"
[[ -f Makefile ]] || die "Makefile is missing"
[[ -x scripts/tree-hash.sh ]] || die "scripts/tree-hash.sh is missing or not executable"

base=master
git rev-parse --verify --quiet "$base^{commit}" >/dev/null || base=origin/master
merge_base=$(git merge-base "$base" HEAD) || die "cannot compare changes against master or origin/master"
web_changed=0
git diff --quiet "$merge_base" -- web/
case $? in
    0) ;;
    1) web_changed=1 ;;
    *) die "cannot inspect web/ changes against $base" ;;
esac
untracked_web=$(git ls-files --others --exclude-standard -- web/) || die "cannot inspect untracked web/ files"
[[ -z "$untracked_web" ]] || web_changed=1

# Require actual explicit rules, not merely names in a .PHONY declaration.
targets=(box-check check)
[[ "$web_changed" -eq 0 ]] || targets+=(ui-gallery)
for target in "${targets[@]}"; do
    awk -v target="$target" '
        /^[^# \t][^=]*:/ {
            sub(/:.*/, "")
            for (i = 1; i <= NF; i++) if ($i == target) found = 1
        }
        END { exit !found }
    ' Makefile || die "required target '$target' is absent from Makefile"
done

log_dir=$(mktemp -d "${TMPDIR:-/tmp}/pgwt-pr-ready.XXXXXX") || die "cannot create log directory"
exec 3>&1
printf 'pr-ready: logs: %s\n' "$log_dir"
printf 'pr-ready: web/ changed versus %s: %s\n' "$base" "$web_changed"
printf '%s\n' 'pr-ready: UNCHECKED: required OS/PG coverage, box log provenance and live-UI-smoke verdict, gallery cell justifications, and fresh reviewer approval remain with the caller (CLAUDE.md).'

# A stamp from a previous run must not turn an unavailable check into a pass.
rm -f .pgwt-check.stamp || die "cannot remove previous check stamp"
finish_target() {
    local target=$1 rc
    shift
    make "$target" "$@"
    rc=$?
    event "$target finish (make exit $rc)"
    return "$rc"
}

event "box-check start (output: $log_dir/box-check.log)"
finish_target "${box_args[@]}" >"$log_dir/box-check.log" 2>&1 &
box_pid=$!
event "check start (full; output: $log_dir/check.log)"
finish_target check 2>&1 | tee "$log_dir/check.log"
check_status=("${PIPESTATUS[@]}")
wait "$box_pid"
box_rc=$?

gallery_rc=0
if [[ "$web_changed" -eq 1 ]]; then
    event "ui-gallery start (web/ changed)"
    finish_target ui-gallery 2>&1 | tee "$log_dir/ui-gallery.log"
    gallery_rc=$?
fi

failed=0
printf '\npr-ready: box-check: make exit %s; log: %s/box-check.log\n' "$box_rc" "$log_dir"
tail -n 22 "$log_dir/box-check.log"
if [[ "$box_rc" -eq 0 ]]; then
    printf 'pr-ready: box-check PASSED\n'
else
    printf 'pr-ready: box-check FAILED\n'
    failed=1
fi

# Check the final tree, including any changes made by box-check or the gallery.
# check.sh can write a valid success stamp and still exit non-zero.
printf 'pr-ready: check make exit %s (stamp is authoritative)\n' "${check_status[0]}"
tree_hash=$(scripts/tree-hash.sh)
hash_rc=$?
stamp=$(cat .pgwt-check.stamp 2>/dev/null) || stamp=''
if [[ "$hash_rc" -eq 0 && -n "$tree_hash" && "$stamp" == "$tree_hash" ]]; then
    printf 'pr-ready: check PASSED: .pgwt-check.stamp matches scripts/tree-hash.sh (%s)\n' "$tree_hash"
else
    printf 'pr-ready: check FAILED: stamp missing/mismatch or tree hash unavailable (stamp=%s, tree=%s, hash exit=%s)\n' "${stamp:-missing}" "${tree_hash:-unavailable}" "$hash_rc" >&2
    failed=1
fi
if [[ "${check_status[1]}" -ne 0 ]]; then
    printf 'pr-ready: FAILED to record check log\n' >&2
    failed=1
fi
if [[ "$web_changed" -eq 1 ]]; then
    printf 'pr-ready: ui-gallery ran (exit %s); log: %s/ui-gallery.log\n' "$gallery_rc" "$log_dir"
    [[ "$gallery_rc" -eq 0 ]] || failed=1
fi
exit "$failed"
