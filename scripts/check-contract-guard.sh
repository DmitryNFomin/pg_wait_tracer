#!/usr/bin/env bash
# Check every commit in base..head, including side branches, not just the
# range's net diff. A contract commit must be isolated (rule 1); changing an
# existing contract also needs a Contract-Change trailer (rule 2).
#
# Merge exemption: compare each changed contracts/ path's resulting tree
# entry (including mode and absence for deletions) with every non-first
# parent. Exempt the merge only if each entry matches at least one such
# parent. This accepts plain merges, including master merges with unrelated
# implementation changes, but checks new conflict resolutions normally.
# Side-parent commits still get checked if they belong to base..head.
# Renames are deliberately represented as delete + add: this avoids rename
# heuristics and makes moves across the contracts/ boundary obey both rules.
# One diagnostic per violating commit; rule 1 takes precedence over rule 2.
# The numeric field after the SHA is the rule number; paths are shell-quoted
# so even names containing whitespace stay on one output line.
# Git plumbing reads history; temporary storage only holds NUL-delimited diffs.
# Requires bash 3.2+ (macOS and Linux), no build or other language runtime.
set -euo pipefail

die() { printf 'CONTRACT-GUARD ERROR %s\n' "$*" >&2; exit 1; }
[[ $# -eq 2 ]] || die 'usage: check-contract-guard.sh <base> <head>'
base=$(git rev-parse --verify --end-of-options "$1^{commit}") || die "cannot resolve base: $1"
head=$(git rev-parse --verify --end-of-options "$2^{commit}") || die "cannot resolve head: $2"
shallow=$(git rev-parse --is-shallow-repository) || die 'cannot inspect repository'
[[ $shallow == false ]] || die 'full history required; fetch without a depth limit'
commits=$(git rev-list --reverse "$base..$head") || die 'cannot enumerate range'
scratch=$(mktemp -d) || die 'cannot create temporary directory'
trap 'rm -rf "$scratch"' EXIT

count=0
failed=0
for commit in $commits; do
    count=$((count + 1))
    parent_line=$(git rev-list --parents -n 1 "$commit") || die "cannot read parents: $commit"
    read -r -a parents <<< "$parent_line"
    # parents[0] is the commit itself. Explicit first-parent diff for merges;
    # --root compares a root commit with the empty tree.
    if [[ ${#parents[@]} -gt 1 ]]; then
        git diff-tree --no-commit-id --raw -r -z --no-renames --no-ext-diff --no-textconv \
            "${parents[1]}" "$commit" > "$scratch/diff" || die "cannot diff: $commit"
    else
        git diff-tree --root --no-commit-id --raw -r -z --no-renames --no-ext-diff --no-textconv \
            "$commit" > "$scratch/diff" || die "cannot diff root: $commit"
    fi
    paths=()
    contract_paths=()
    existing_paths=()
    outside=0
    while IFS= read -r -d '' entry; do
        IFS= read -r -d '' path || die "incomplete diff: $commit"
        paths+=("$path")
        case "$path" in
            contracts/*)
                contract_paths+=("$path")
                [[ ${entry##* } == A ]] || existing_paths+=("$path")
                ;;
            *) outside=1 ;;
        esac
    done < "$scratch/diff"
    [[ ${#contract_paths[@]} -gt 0 ]] || continue

    if [[ ${#parents[@]} -gt 2 ]]; then
        inherited=1
        for path in "${contract_paths[@]}"; do
            matched=0
            for ((i=2; i<${#parents[@]}; i++)); do
                if git diff-tree --quiet -r --no-renames --no-ext-diff --no-textconv \
                    "${parents[i]}" "$commit" -- ":(literal)$path"; then
                    matched=1
                    break
                else
                    rc=$?
                    [[ $rc -eq 1 ]] || die "cannot compare merge parent: $commit"
                fi
            done
            [[ $matched -eq 1 ]] || inherited=0
        done
        [[ $inherited -eq 0 ]] || continue
    fi

    rule=0
    if [[ $outside -eq 1 ]]; then
        rule=1
    elif [[ ${#existing_paths[@]} -gt 0 ]]; then
        # Preserve the header/message separator even for an empty message;
        # command substitution would otherwise strip its trailing newlines.
        object=$(git cat-file commit "$commit" && printf '.') || die "cannot read commit: $commit"
        object=${object%.}
        [[ $object == *$'\n\n'* ]] || die "cannot read message: $commit"
        trailers=$(printf '%s\n' "${object#*$'\n\n'}" |
            git -c trailer.separators=: interpret-trailers --only-trailers --only-input) ||
            die "cannot read trailers: $commit"
        valid=0
        candidate=0
        while IFS= read -r line; do
            case "$line" in
                ' '*|$'\t'*) candidate=0 ;; # Reject folded, multi-line reasons.
                *)
                    [[ $candidate -eq 0 ]] || valid=1
                    candidate=0
                    if [[ $line =~ ^Contract-Change:[[:blank:]]+[^[:space:]].*$ ]]; then
                        candidate=1
                    fi
                    ;;
            esac
        done <<< "$trailers"
        [[ $candidate -eq 0 ]] || valid=1
        [[ $valid -eq 1 ]] || rule=2
    fi
    if [[ $rule -ne 0 ]]; then
        printf 'CONTRACT-GUARD %s %s' "$commit" "$rule"
        printf ' %q' "${paths[@]}"
        printf '\n'
        failed=1
    fi
done
[[ $failed -eq 0 ]] || exit 1
printf 'CONTRACT-GUARD OK %s commits\n' "$count"
