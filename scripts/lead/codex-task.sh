#!/bin/sh
# Usage: codex-task.sh <brief-file> <branch-name>
# The script's repository is the real repository, regardless of caller cwd.
# The caller needs permission to fetch into its
# .git; Codex runs only in the external clone. Never push from this wrapper.
# Slashes become %2F and literal percent signs become %25 (no name collisions).
# Exits: 2 usage, 10 BRIEF_MISSING, 11 INVALID_BRANCH, 12 UNSAFE_CLONE_PATH,
# 13 SETUP_FAILED, 14 CLONE_BUSY, 15 BRIEF_UNCHECKED, 20 BRANCH_MISSING, 21 BRANCH_UNCHANGED,
# 22 FETCH_FAILED, 23 COMMIT_UNREACHABLE, 24 CODEX_FAILED, 25 DIFF_FAILED,
# 26 AUTO_COMMIT_INCOMPLETE, 27 BUDGET_EXCEEDED.
# Codex cannot write under .git in its sandbox (#777), so THIS wrapper commits
# the round after Codex exits: whatever is in the clone's working tree goes onto
# the task branch as one commit and is fetched into the real repository. 21 now
# means the round produced nothing, or the commit itself failed (said loudly).
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
checked_brief=
cost_outcome=
cost_pid=
cost_tee=
cost_log="$lock/codex-output"
cost_pipe="$lock/codex-pipe"
append_cost() (
    tokens=unknown
    if [ -r "$cost_log" ]; then
        tokens=$(awk '
        pending { gsub(/,/, ""); if ($0 ~ /^[[:space:]]*[0-9]+[[:space:]]*$/) {
            value = $0; gsub(/[[:space:]]/, "", value)
        }; pending = 0 }
        /^[[:space:]]*tokens used[[:space:]]*$/ { pending = 1 }
        END { print (value == "" ? "unknown" : value) }
        ' "$cost_log") || return 1
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$cost_end" "$branch" "$brief" "$brief_sha" "$tokens" \
        "$cost_wall" "$codex_rc" "$cost_outcome" \
        >> "${PGWT_LEAD_STATE_DIR:-$repo/scripts/lead}/costs.tsv"
)
cleanup() {
    if [ "$cost_outcome" = INTERRUPTED ]; then
        cost_end=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
        cost_wall=$(( $(date +%s) - cost_start ))
    fi
    if [ -n "$cost_outcome" ]; then
        (append_cost) 2>/dev/null || printf 'WARNING: could not append cost record\n' >&2
    fi
    rm -f "$cost_log" "$cost_pipe"
    [ -z "$prompt" ] || rm -f "$prompt"
    [ -z "$checked_brief" ] || rm -f "$checked_brief"
    rm -f "$lock/budget-exceeded"
    rmdir "$lock"
}
interrupted() {
    codex_rc=$1
    if [ -n "$cost_pid" ]; then
        kill -TERM "$cost_pid" 2>/dev/null || :
        wait "$cost_pid" 2>/dev/null || :
    fi
    [ -z "$cost_tee" ] || wait "$cost_tee" 2>/dev/null || :
    exit "$codex_rc"
}
trap cleanup 0
trap 'interrupted 130' INT
trap 'interrupted 143' TERM HUP

# Freeze the bytes covered by the pass, so editing the source brief during a
# fetch cannot change either the dispatched scope or its budget.
checked_brief=$(mktemp "$lock/checked.XXXXXX") || die 13 'SETUP_FAILED: cannot snapshot brief'
cat "$brief" > "$checked_brief" || die 15 "$unchecked"
checked_sha=$(shasum -a 256 < "$checked_brief" 2>/dev/null) || die 15 "$unchecked"
[ "${checked_sha%% *}" = "$brief_sha" ] || die 15 "$unchecked"
budget_seconds=$(python3 "$repo/scripts/lead/brief_contract.py" "$checked_brief") || die 15 "$unchecked"

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
# The implementer hands its commit message over in this file. It is excluded in
# the clone so it is never committed, never in the salvage patch and never part
# of scripts/tree-hash.sh; a stale one from an earlier round must not be reused.
msgfile="$clone/.codex-commit-msg"
rm -f "$msgfile" || die 13 'SETUP_FAILED: cannot remove a stale commit message'
grep -Fqx '/.codex-commit-msg' "$clone/.git/info/exclude" 2>/dev/null ||
    { mkdir -p "$clone/.git/info" && printf '/.codex-commit-msg\n' >> "$clone/.git/info/exclude"; } ||
    die 13 'SETUP_FAILED: cannot exclude the commit message file'
prompt=$(mktemp "$lock/brief.XXXXXX") || die 13 'SETUP_FAILED: cannot create prompt'
{
    printf 'Working directory: %s\nBranch: %s\n' "$clone" "$branch"
    printf 'Work on this branch in this directory. Do not create your own clone.\n'
    printf 'Your sandbox forbids writes under .git, so do NOT run git add or git commit; they fail\n'
    printf 'with "Operation not permitted". The wrapper that launched you commits for you when you\n'
    printf 'exit: EVERYTHING in the working tree (tracked edits and new files) becomes one commit on\n'
    printf 'this branch. So before you finish: delete scratch files you do not want committed, and\n'
    printf 'write the commit message (subject line, blank line, body) to the file .codex-commit-msg\n'
    printf 'in this directory. That file itself is never committed.\n\n'
    cat "$checked_brief"
} > "$prompt" || die 13 'SETUP_FAILED: cannot read brief'
codex_rc=0
cost_start=$(date +%s)
cost_outcome=INTERRUPTED
# Keep streaming output and the supervisor's exact exit code: tee is outside
# the command's status path. If capture setup fails, run the original path.
if (mkfifo "$cost_pipe" && : > "$cost_log") 2>/dev/null; then
    tee "$cost_log" < "$cost_pipe" >&2 &
    cost_tee=$!
    (cd "$clone" && exec python3 "$repo/scripts/lead/run-budget.py" "$budget_seconds" "$lock/budget-exceeded" \
        codex exec --cd "$clone" --sandbox workspace-write - < "$prompt") > "$cost_pipe" 2>&1 &
    cost_pid=$!
    wait "$cost_pid" || codex_rc=$?
    wait "$cost_tee" || :
else
    (cd "$clone" && exec python3 "$repo/scripts/lead/run-budget.py" "$budget_seconds" "$lock/budget-exceeded" \
        codex exec --cd "$clone" --sandbox workspace-write - < "$prompt") >&2 &
    cost_pid=$!
    wait "$cost_pid" || codex_rc=$?
fi
cost_pid=
cost_tee=
cost_stop=$(date +%s)
cost_end=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
cost_wall=$((cost_stop - cost_start))
cost_outcome=BRANCH_UNCHANGED
budget_exceeded=0
[ ! -f "$lock/budget-exceeded" ] || budget_exceeded=1
[ "$budget_exceeded" -eq 0 ] || cost_outcome=BUDGET_EXCEEDED

# Salvage the working tree unconditionally: the implementer sandbox forbids
# writes under .git (issue #777), so a finished round normally leaves its work
# uncommitted and only here is it still reachable. Every failure in this block
# is LOUD: the one thing it must never do is report "clean" while the work is
# still only in the clone.
salvage="$(dirname "$brief")/$(basename "$brief" .md)-salvage.patch"
case $salvage in *-salvage.patch) ;; *) salvage="$salvage-salvage.patch" ;; esac
salvage_n=0
while [ -e "$salvage" ]; do
    salvage_n=$((salvage_n + 1))
    salvage="${salvage%-salvage.patch}-salvage.$salvage_n.patch"
done
# -N makes untracked files visible to `git diff HEAD`; its failure (a locked or
# unwritable index) would silently drop exactly those files from the patch.
add_err=$(git -C "$clone" add -A -N 2>&1) || {
    printf 'SALVAGE_FAILED: cannot stage the clone (%s); the work is still in %s only\n' \
        "$(printf '%s' "$add_err" | tr '\n' ' ')" "$clone" >&2
    salvage=''
}
if [ -n "$salvage" ]; then
    # --binary so a new binary file produces an appliable patch rather than
    # "Binary files differ".
    if ! git -C "$clone" diff --binary HEAD > "$salvage" 2>/dev/null; then
        printf 'SALVAGE_FAILED: cannot write %s; the work is still in %s only\n' \
            "$salvage" "$clone" >&2
    elif [ -s "$salvage" ]; then
        printf 'SALVAGE_PATCH: %s\n' "$salvage" >&2
    elif [ -n "$(git -C "$clone" status --porcelain 2>/dev/null)" ]; then
        printf 'SALVAGE_FAILED: empty patch but the clone is dirty; the work is still in %s only\n' \
            "$clone" >&2
    else
        rm -f "$salvage"
        printf 'SALVAGE_PATCH: none (working tree clean)\n' >&2
    fi
fi

# Commit the round here, outside the sandbox (#777). Runs even when Codex
# returned non-zero, so an interrupted round still lands on the branch. A
# failure is LOUD and leaves the salvage patch above as the only copy.
autocommit_failed=0
if [ -n "$(git -C "$clone" status --porcelain 2>/dev/null)" ]; then
    autocommit_failed=1
    head_branch=$(git -C "$clone" symbolic-ref --quiet --short HEAD 2>/dev/null) || head_branch=''
    if [ "$head_branch" != "$branch" ]; then
        printf 'AUTO_COMMIT_FAILED: clone HEAD is on "%s", not %s; the work is in %s only\n' \
            "$head_branch" "$branch" "$clone" >&2
    elif ! commit_err=$(git -C "$clone" add -A 2>&1); then
        printf 'AUTO_COMMIT_FAILED: cannot stage (%s); the work is in %s only\n' \
            "$(printf '%s' "$commit_err" | tr '\n' ' ')" "$clone" >&2
    else
        if [ -s "$msgfile" ]; then
            commit_err=$(git -C "$clone" commit -q --no-verify -F "$msgfile" \
                --trailer 'Co-authored-by: Codex <noreply@openai.com>' 2>&1) || commit_err="failed: $commit_err"
        else
            commit_err=$(git -C "$clone" commit -q --no-verify -m "codex round on $branch" \
                --trailer 'Co-authored-by: Codex <noreply@openai.com>' 2>&1) || commit_err="failed: $commit_err"
        fi
        case $commit_err in
            failed:*) printf 'AUTO_COMMIT_FAILED: git commit %s; the work is in %s only\n' \
                "$(printf '%s' "$commit_err" | tr '\n' ' ')" "$clone" >&2 ;;
            *) autocommit_failed=0
               printf 'AUTO_COMMIT: %s\n' "$(git -C "$clone" rev-parse --verify HEAD)" >&2
               # Name what went in: a reused clone commits its leftovers too.
               git -C "$clone" show --stat --format= HEAD >&2 || : ;;
        esac
    fi
fi

# Fetch committed work even when Codex itself returned a non-zero status.
tip=$(git -C "$clone" rev-parse --verify "refs/heads/$branch" 2>/dev/null) || die 20 "BRANCH_MISSING: branch does not exist in clone afterwards: $branch"
if [ "$budget_exceeded" -eq 0 ] && [ "$tip" != "$before" ]; then
    cost_outcome=AUTO_COMMIT
fi
[ "$budget_exceeded" -eq 1 ] || [ "$tip" != "$before" ] || die 21 "BRANCH_UNCHANGED: branch did not move: $branch (codex exit $codex_rc)"
git -C "$repo" fetch "$clone" "refs/heads/$branch:refs/heads/$branch" --force >&2 || die 22 'FETCH_FAILED: could not fetch task branch into real repository'
type=$(git -C "$repo" cat-file -t "$tip" 2>/dev/null) || die 23 'COMMIT_UNREACHABLE: fetched commit is unreachable'
landed=$(git -C "$repo" rev-parse --verify "refs/heads/$branch" 2>/dev/null) || die 23 'COMMIT_UNREACHABLE: fetched branch is missing'
[ "$type" = commit ] && [ "$landed" = "$tip" ] || die 23 'COMMIT_UNREACHABLE: fetched branch does not resolve to the expected commit'
# A branch that moved is not a landed round if later work could not be
# committed: the tip is fetched, but the rest exists only in the clone, so this
# must never print the success block.
[ "$autocommit_failed" -eq 0 ] || die 26 "AUTO_COMMIT_INCOMPLETE: $branch was fetched at $tip, but uncommitted work remains in $clone only"
if [ "$budget_exceeded" -eq 1 ]; then
    [ "$tip" != "$before" ] || die 27 "BUDGET_EXCEEDED: wall-clock budget ${budget_seconds}s; branch did not move; no new work was committed; unchanged tip fetched and verified at $tip"
    die 27 "BUDGET_EXCEEDED: wall-clock budget ${budget_seconds}s; work was fetched and verified at $tip"
fi
[ "$codex_rc" -eq 0 ] || die 24 "CODEX_FAILED: codex exit $codex_rc; committed work was fetched and verified"
diff=$(git -C "$repo" diff --name-only "refs/remotes/origin/master...refs/heads/$branch") || die 25 'DIFF_FAILED: cannot compare branch with origin/master'
# Only verified success emits this final block on stdout: path, branch, SHA,
# then the requested git diff --name-only origin/master...<branch> output.
printf '%s\n' "$clone" "$branch" "$tip"
[ -z "$diff" ] || printf '%s\n' "$diff"
