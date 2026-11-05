#!/bin/sh
# Usage: codex-task.sh <brief-file> <branch-name>
# The script's repository is the real repository, regardless of caller cwd.
# The caller needs permission to fetch into its
# .git; Codex runs only in the external clone. Never push from this wrapper.
# Slashes become %2F and literal percent signs become %25 (no name collisions).
# Exits: 2 usage, 10 BRIEF_MISSING, 11 INVALID_BRANCH, 12 UNSAFE_CLONE_PATH,
# 13 SETUP_FAILED, 14 CLONE_BUSY, 15 BRIEF_UNCHECKED,
# 20 BRANCH_MISSING, 21 BRANCH_UNCHANGED,
# 22 FETCH_FAILED, 23 COMMIT_UNREACHABLE, 24 CODEX_FAILED, 25 DIFF_FAILED.
set -u

die() { code=$1; shift; printf '%s\n' "$*" >&2; exit "$code"; }
[ "$#" -eq 2 ] || die 2 'USAGE: codex-task.sh <brief-file> <branch-name>'
brief=$1
branch=$2
[ -f "$brief" ] && [ -r "$brief" ] || die 10 "BRIEF_MISSING: brief file does not exist or is unreadable: $brief"
unchecked='BRIEF_UNCHECKED: run scripts/lead/check-brief.sh <brief> first (no passing record for this exact brief content)'
brief_sha=$(shasum -a 256 < "$brief" 2>/dev/null) || die 15 "$unchecked"
brief_sha=${brief_sha%% *}
now=$(date +%s) || die 15 "$unchecked"
passes="${PGWT_LEAD_STATE_DIR:-/Users/dmitryfomin/work/git/pg_wait_tracer/scripts/lead}/.briefs-passed"
[ -r "$passes" ] && awk -v sha="$brief_sha" -v now="$now" '
    $1 ~ /^[0-9]+$/ && $2 == sha && NF >= 3 && $1 >= now - 86400 && $1 <= now { found = 1 }
    END { exit !found }
' "$passes" 2>/dev/null || die 15 "$unchecked"
git check-ref-format "refs/heads/$branch" >/dev/null 2>&1 || die 11 "INVALID_BRANCH: $branch"
git check-ref-format --branch "$branch" >/dev/null 2>&1 || die 11 "INVALID_BRANCH: $branch"
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel 2>/dev/null) || die 13 'SETUP_FAILED: script must live inside the real repository'
repo=$(CDPATH='' cd -P "$repo" && pwd -P) || die 13 'SETUP_FAILED: cannot resolve repository'

# Resolve existing ancestors, including symlinks, before creating anything.
# A dangling symlink or non-directory is a refusal, never a lexical fallback.
resolve_dir() (
    path=$1
    suffix=
    while [ ! -e "$path" ] && [ ! -L "$path" ]; do
        suffix="/${path##*/}$suffix"
        path=${path%/*}
    done
    [ -d "$path" ] || exit 1
    CDPATH='' cd -P "$path" || exit 1
    printf '%s%s\n' "$(pwd -P)" "$suffix"
)
outside_repo() {
    case "$1" in
        "$repo"|"$repo"/*) die 12 "UNSAFE_CLONE_PATH: clone path resolves inside the repository: $1" ;;
    esac
}
flat=$(printf '%s' "$branch" | sed 's/%/%25/g; s,/,%2F,g')
root=$(resolve_dir /private/tmp/pgwt-codex) || die 12 'UNSAFE_CLONE_PATH: cannot resolve clone root'
outside_repo "$root"
clone=$(resolve_dir "$root/$flat") || die 12 'UNSAFE_CLONE_PATH: cannot resolve clone path'
outside_repo "$clone"
mkdir -p "$root" || die 13 'SETUP_FAILED: cannot create clone root'
# Serialize a branch without ever putting a lock in the source repository.
lock="$root/$flat.lock"
mkdir "$lock" 2>/dev/null || die 14 "CLONE_BUSY: lock exists: $lock"
prompt=
cleanup() {
    [ -z "$prompt" ] || rm -f "$prompt"
    rmdir "$lock"
}
trap cleanup 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP

upstream=$(git -C "$repo" remote get-url origin) || die 13 'SETUP_FAILED: origin is missing'
# Resolve local relative origin URLs against the source worktree, not the clone.
case "$upstream" in
    /*|*:* ) ;;
    *) upstream="$repo/$upstream" ;;
esac
created=0
if [ -e "$clone" ]; then
    top=$(git -C "$clone" rev-parse --show-toplevel 2>/dev/null) || die 13 'SETUP_FAILED: existing path is not a clone'
    top=$(resolve_dir "$top") || die 13 'SETUP_FAILED: cannot resolve existing clone'
    [ "$top" = "$clone" ] || die 13 'SETUP_FAILED: existing path is not a clone root'
    origin=$(git -C "$clone" remote get-url origin) || die 13 'SETUP_FAILED: clone origin is missing'
    [ "$origin" = "$upstream" ] || die 13 'SETUP_FAILED: existing clone belongs to a different origin'
    [ -d "$clone/.git" ] && [ ! -L "$clone/.git" ] || die 13 'SETUP_FAILED: existing path must be a standalone clone'
    printf 'Reusing clone: %s (preserving existing work)\n' "$clone" >&2
else
    printf 'Creating clone: %s\n' "$clone" >&2
    git clone --shared --no-checkout "$repo" "$clone" >&2 || die 13 'SETUP_FAILED: clone failed'
    git -C "$clone" remote set-url origin "$upstream" || die 13 'SETUP_FAILED: cannot configure origin'
    created=1
fi
git -C "$clone" fetch --no-tags origin '+refs/heads/master:refs/remotes/origin/master' >&2 || die 13 'SETUP_FAILED: origin/master fetch failed'
if [ "$created" -eq 1 ]; then
    # A clone may already have a local branch with the source's HEAD name.
    # Only a NEW clone may reset it to the freshly fetched upstream base.
    git -C "$clone" checkout -B "$branch" refs/remotes/origin/master >&2 || die 13 'SETUP_FAILED: cannot create task branch'
elif git -C "$clone" show-ref --verify --quiet "refs/heads/$branch"; then
    git -C "$clone" checkout "$branch" >&2 || die 13 'SETUP_FAILED: cannot check out existing branch'
else
    git -C "$clone" checkout -b "$branch" refs/remotes/origin/master >&2 || die 13 'SETUP_FAILED: cannot create branch'
fi
before=$(git -C "$clone" rev-parse --verify "refs/heads/$branch") || die 13 'SETUP_FAILED: cannot read initial branch tip'
prompt=$(mktemp "$lock/brief.XXXXXX") || die 13 'SETUP_FAILED: cannot create prompt'
{
    printf 'Working directory: %s\nBranch: %s\n' "$clone" "$branch"
    printf 'Work and commit on this branch in this directory. Do not create your own clone.\n\n'
    cat "$brief"
} > "$prompt" || die 13 'SETUP_FAILED: cannot read brief'
codex_rc=0
(cd "$clone" && codex exec --cd "$clone" --sandbox workspace-write - < "$prompt") >&2 || codex_rc=$?

# Salvage committed work even when Codex itself returned a non-zero status.
tip=$(git -C "$clone" rev-parse --verify "refs/heads/$branch" 2>/dev/null) || die 20 "BRANCH_MISSING: branch does not exist in clone afterwards: $branch"
[ "$tip" != "$before" ] || die 21 "BRANCH_UNCHANGED: branch did not move: $branch (codex exit $codex_rc)"
git -C "$repo" fetch "$clone" "refs/heads/$branch:refs/heads/$branch" --force >&2 || die 22 'FETCH_FAILED: could not fetch task branch into real repository'
type=$(git -C "$repo" cat-file -t "$tip" 2>/dev/null) || die 23 'COMMIT_UNREACHABLE: fetched commit is unreachable'
landed=$(git -C "$repo" rev-parse --verify "refs/heads/$branch" 2>/dev/null) || die 23 'COMMIT_UNREACHABLE: fetched branch is missing'
[ "$type" = commit ] && [ "$landed" = "$tip" ] || die 23 'COMMIT_UNREACHABLE: fetched branch does not resolve to the expected commit'
[ "$codex_rc" -eq 0 ] || die 24 "CODEX_FAILED: codex exit $codex_rc; committed work was fetched and verified"
diff=$(git -C "$repo" diff --name-only "refs/remotes/origin/master...refs/heads/$branch") || die 25 'DIFF_FAILED: cannot compare branch with origin/master'
# Only verified success emits this final block on stdout: path, branch, SHA,
# then the requested git diff --name-only origin/master...<branch> output.
printf '%s\n' "$clone" "$branch" "$tip"
[ -z "$diff" ] || printf '%s\n' "$diff"
