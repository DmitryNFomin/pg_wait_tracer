#!/usr/bin/env bash
# Exercise the actual commit-msg invocation in a disposable repo, then check
# its committed messages with the board's actual attribution parser (#771).
# PGWT_TEST_COMMIT_MSG_HOOK substitutes a hook to prove regressions go red.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOOK_SRC="${PGWT_TEST_COMMIT_MSG_HOOK:-$REPO_ROOT/scripts/git-hooks/commit-msg}"

tests_run=0
tests_failed=0
report() {
    tests_run=$((tests_run + 1))
    if [[ "$1" == true ]]; then
        echo "  PASS: $2"
    else
        echo "  FAIL: $2"
        tests_failed=$((tests_failed + 1))
    fi
}
same() {
    if cmp -s "$1" "$2"; then report true "$3"; else report false "$3"; fi
}

# Check the source index before clearing overrides for the disposable repo.
# This also permits validation of untracked files with a temporary Git index.
if [[ ! -e "$REPO_ROOT/.git" ]]; then
    echo '  SKIP: tracked commit-msg mode (no .git; rsynced source tree)'
else
    tracked_entry=$(git -C "$REPO_ROOT" ls-files -s -- scripts/git-hooks/commit-msg)
    tracked_mode="${tracked_entry%% *}"
    report "$([[ "$tracked_mode" == 100755 ]] && echo true || echo false)" \
        "tracked commit-msg mode is 100755 (got ${tracked_mode:-untracked})"
fi
# Do not inherit repository/worktree overrides or a developer's Git config.
unset "${!GIT_@}" 2>/dev/null || true
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
# The squash template includes the author date; keep its exact text stable.
export GIT_AUTHOR_DATE='2000-01-01T00:00:00+0000'
export GIT_COMMITTER_DATE='2000-01-01T00:00:00+0000'
tmproot="$(mktemp -d "${TMPDIR:-/tmp}/pgwt-commit-msg-test.XXXXXX")" || exit 1
trap 'rm -rf "$tmproot"' EXIT
mkdir -p "$tmproot/repo" "$tmproot/hooks" "$tmproot/no-tools" || exit 1
cp "$HOOK_SRC" "$tmproot/hooks/commit-msg" || exit 1
chmod +x "$tmproot/hooks/commit-msg" || exit 1
hook="$tmproot/hooks/commit-msg"
repo="$tmproot/repo"
git -C "$repo" init -q -b main || exit 1
git -C "$repo" config user.name Test || exit 1
git -C "$repo" config user.email test@example.invalid || exit 1
git -C "$repo" config core.hooksPath "$tmproot/hooks" || exit 1
git -C "$repo" config commit.gpgSign false || exit 1
git -C "$repo" config tag.gpgSign false || exit 1

board_label() {
    python3 -c '
import json, pathlib, sys
message = pathlib.Path(sys.argv[1]).read_text()
headline, _, body = message.partition("\n")
print(json.dumps({"commits": [{"messageHeadline": headline, "messageBody": body}]}))
' "$1" | python3 "$REPO_ROOT/scripts/board-author.py"
}

check_commit() {
    local name="$1" expected_label="$2"; shift 2
    local label
    if env -u CLAUDECODE "$@" > "$tmproot/commit.log" 2>&1; then
        report true "$name: real commit succeeds"
    else
        report false "$name: real commit succeeds"
        cat "$tmproot/commit.log"
        return 1
    fi
    git -C "$repo" cat-file commit HEAD | python3 -c \
        'import sys; sys.stdout.buffer.write(sys.stdin.buffer.read().split(b"\n\n", 1)[1])' \
        > "$tmproot/result"
    same "$tmproot/expected" "$tmproot/result" "$name: exact committed message"
    if label=$(board_label "$tmproot/result") && [[ "$label" == "$expected_label" ]]; then
        report true "$name: board label $expected_label"
    else
        report false "$name: board label $expected_label (got $label)"
    fi
}

commit_case() {
    local name="$1" message="$2" expected="$3" expected_label="$4"
    local claude_env=(env -u CLAUDECODE)
    if [[ $# -ge 5 ]]; then claude_env=(env CLAUDECODE="$5"); fi
    printf '%s' "$message" > "$tmproot/input"
    printf '%s' "$expected" > "$tmproot/expected"
    # Only the disposable repo is committed. Verbatim cleanup lets us assert
    # byte preservation, including comments and existing trailer spelling.
    check_commit "$name" "$expected_label" "${claude_env[@]}" git -C "$repo" commit -q --allow-empty \
        --cleanup=verbatim -F "$tmproot/input" || return 1
    git -C "$repo" interpret-trailers --parse "$tmproot/input" > "$tmproot/original-trailers"
    git -C "$repo" interpret-trailers --parse "$tmproot/result" > "$tmproot/final-trailers"
    if python3 - "$tmproot/original-trailers" "$tmproot/final-trailers" <<'PY'
from pathlib import Path
import sys
original, result = (Path(arg).read_text().splitlines() for arg in sys.argv[1:])
remaining = iter(result)
sys.exit(0 if all(line in remaining for line in original) else 1)
PY
    then
        report true "$name: original trailers (including Co-Authored-By) still parse"
    else
        report false "$name: original trailers (including Co-Authored-By) still parse"
    fi
}

commit_case codex $'Task\n\nCo-Authored-By: codex <bot@example.invalid>\n' \
    $'Task\n\nCo-Authored-By: codex <bot@example.invalid>\nGenerated-with: codex\n' by:codex
commit_case opus $'Task\n\nCo-Authored-By: Claude Opus 5 <bot@example.invalid>\n' \
    $'Task\n\nCo-Authored-By: Claude Opus 5 <bot@example.invalid>\nGenerated-with: Claude Opus\n' by:claude-opus
commit_case sonnet $'Task\n\nCo-Authored-By: Claude Sonnet 4 <bot@example.invalid>\n' \
    $'Task\n\nCo-Authored-By: Claude Sonnet 4 <bot@example.invalid>\nGenerated-with: Claude Sonnet\n' by:claude-sonnet
commit_case joint $'Task\n\nCo-Authored-By: Claude Opus 5\nCo-Authored-By: codex\n' \
    $'Task\n\nCo-Authored-By: Claude Opus 5\nCo-Authored-By: codex\nGenerated-with: codex\n' by:codex
commit_case opus_precedence $'Task\n\nCo-Authored-By: Claude Sonnet 4\nCo-Authored-By: Claude Opus 5\n' \
    $'Task\n\nCo-Authored-By: Claude Sonnet 4\nCo-Authored-By: Claude Opus 5\nGenerated-with: Claude Opus\n' by:claude-opus
commit_case human $'Task\n\nAn ordinary explanation.\n' \
    $'Task\n\nAn ordinary explanation.\n\nGenerated-with: human\n' by:human
# Use this exact case again for the mutation control below.
claudecode_key_case() {
    commit_case claudecode_no_trailer $'Task\n\nAn ordinary explanation.\n' \
        $'Task\n\nAn ordinary explanation.\n' by:unknown 1
}
claudecode_key_case
commit_case claudecode_empty $'Task\n\nAn ordinary explanation.\n' \
    $'Task\n\nAn ordinary explanation.\n\nGenerated-with: human\n' by:human ""
for setting in unset set; do
    claude_args=()
    if [[ "$setting" == set ]]; then
        claude_args=(1)
    fi
    for model in codex 'GPT-6' 'Claude Opus 5' 'Claude Sonnet 4'; do
        case "$model" in
            codex|GPT-*) generated=codex; label=by:codex ;;
            'Claude Opus'*) generated='Claude Opus'; label=by:claude-opus ;;
            'Claude Sonnet'*) generated='Claude Sonnet'; label=by:claude-sonnet ;;
        esac
        commit_case "${setting}_$model" $'Task\n\nCo-Authored-By: '"$model"$'\n' \
            $'Task\n\nCo-Authored-By: '"$model"$'\nGenerated-with: '"$generated"$'\n' \
            "$label" "${claude_args[@]}"
    done
    commit_case "${setting}_explicit_human" $'Task\n\nGenerated-with: human\n' \
        $'Task\n\nGenerated-with: human\n' by:human "${claude_args[@]}"
    for evidence in claude Anthropic OpenAI GPT Fable Astra Haiku claude-opus; do
        commit_case "${setting}_unknown_$evidence" $'Task\n\nSigned-off-by: '"$evidence"$'\n' \
            $'Task\n\nSigned-off-by: '"$evidence"$'\n' by:unknown "${claude_args[@]}"
    done
    commit_case "${setting}_session" $'Task\n\nClaude-Session: abc123\n' \
        $'Task\n\nClaude-Session: abc123\n' by:unknown "${claude_args[@]}"
done
commit_case explicit $'Task\n\ngEnErAtEd-WiTh: Claude Sonnet\n' \
    $'Task\n\ngEnErAtEd-WiTh: Claude Sonnet\n' by:claude-sonnet
commit_case explicit_human $'Task\n\nGenerated-with: human\n' \
    $'Task\n\nGenerated-with: human\n' by:human
# The hook preserves the explicit human declaration; the board independently
# gives codex precedence when both trailers exist.
commit_case explicit_human_codex $'Task\n\nGenerated-with: human\nCo-Authored-By: codex\n' \
    $'Task\n\nGenerated-with: human\nCo-Authored-By: codex\n' by:codex
commit_case explicit_unknown $'Task\n\nGenerated-with: something else\n' \
    $'Task\n\nGenerated-with: something else\n' by:unknown
commit_case session $'Task\n\nClaude-Session: abc123\n' $'Task\n\nClaude-Session: abc123\n' by:unknown
commit_case fable $'Task\n\nCo-Authored-By: Claude Fable\n' $'Task\n\nCo-Authored-By: Claude Fable\n' by:unknown
commit_case hyphenated_opus $'Task\n\nCo-Authored-By: claude-opus\n' $'Task\n\nCo-Authored-By: claude-opus\n' by:unknown
commit_case prose $'Task\n\nExample:\n\nCo-Authored-By: codex\n\nThis is prose, outside the final trailer block.\n' \
    $'Task\n\nExample:\n\nCo-Authored-By: codex\n\nThis is prose, outside the final trailer block.\n\nGenerated-with: human\n' by:human
commit_case comments $'Task\n\nCo-Authored-By: codex\n# Keep this template comment.\n# And this one.\n' \
    $'Task\n\nCo-Authored-By: codex\nGenerated-with: codex\n# Keep this template comment.\n# And this one.\n' by:codex
commit_case scissors $'Task\n\nCo-Authored-By: codex\n\n# ------------------------ >8 ------------------------\n# Keep this verbose diff.\n+Co-Authored-By: Claude Opus\n' \
    $'Task\n\nCo-Authored-By: codex\nGenerated-with: codex\n\n# ------------------------ >8 ------------------------\n# Keep this verbose diff.\n+Co-Authored-By: Claude Opus\n' by:codex
commit_case folded $'Task\n\nco-AUTHORED-by: Claude\n Sonnet 4\n' \
    $'Task\n\nco-AUTHORED-by: Claude\n Sonnet 4\nGenerated-with: Claude Sonnet\n' by:claude-sonnet
commit_case gpt $'Task\n\nSigned-Off-By: GPT-6 <bot@example.invalid>\n' \
    $'Task\n\nSigned-Off-By: GPT-6 <bot@example.invalid>\nGenerated-with: codex\n' by:codex
commit_case session_classified $'Task\n\nClaude-Session: Claude Opus 5\n' \
    $'Task\n\nClaude-Session: Claude Opus 5\nGenerated-with: Claude Opus\n' by:claude-opus
commit_case human_signoff $'Task\n\nSigned-off-by: A Person <codex@example.invalid>\n' \
    $'Task\n\nSigned-off-by: A Person <codex@example.invalid>\nGenerated-with: human\n' by:human
commit_case unrelated_key $'Task\n\nReviewed-by: Claude Opus\n' \
    $'Task\n\nReviewed-by: Claude Opus\nGenerated-with: human\n' by:human
commit_case word_boundaries $'Task\n\nCo-Authored-By: codexian\n' \
    $'Task\n\nCo-Authored-By: codexian\nGenerated-with: human\n' by:human
for evidence in claude Anthropic OpenAI GPT Fable Astra Haiku; do
    commit_case "unknown_$evidence" $'Task\n\nSigned-off-by: '"$evidence"$'\n' \
        $'Task\n\nSigned-off-by: '"$evidence"$'\n' by:unknown
done

# Pin the messages Git actually leaves after each command's default cleanup.
# The editor's comments disappear, while both trailers remain in one block.
cat > "$tmproot/editor" <<'SH'
#!/bin/sh
cat > "$1" <<'MSG'
Editor task

Co-Authored-By: codex
# Keep this template comment until Git cleans up.
# Another comment.
MSG
SH
chmod +x "$tmproot/editor" || exit 1
printf 'Editor task\n\nCo-Authored-By: codex\nGenerated-with: codex\n' > "$tmproot/expected"
check_commit editor by:codex env GIT_EDITOR="$tmproot/editor" \
    git -C "$repo" commit -q --allow-empty

# A staged change makes this a real verbose commit. The editor supplies the
# scissors marker and a diff that must be removed from the committed message.
printf 'verbose change\n' > "$repo/verbose-file"
git -C "$repo" add verbose-file || exit 1
cat > "$tmproot/verbose-editor" <<'SH'
#!/bin/sh
cat > "$1" <<'MSG'
Verbose task

Co-Authored-By: codex

# ------------------------ >8 ------------------------
# Do not modify or remove the line above.
# Everything below it will be ignored.
diff --git a/verbose-file b/verbose-file
new file mode 100644
--- /dev/null
+++ b/verbose-file
@@ -0,0 +1 @@
+verbose change
MSG
SH
chmod +x "$tmproot/verbose-editor" || exit 1
printf 'Verbose task\n\nCo-Authored-By: codex\nGenerated-with: codex\n' > "$tmproot/expected"
check_commit verbose by:codex env GIT_EDITOR="$tmproot/verbose-editor" \
    git -C "$repo" commit -q -v

# The default merge message does not carry the source trailers.
git -C "$repo" checkout -q -b merge-source || exit 1
printf 'merge change\n' > "$repo/merge-file"
git -C "$repo" add merge-file || exit 1
env -u CLAUDECODE git -C "$repo" commit -q -m $'Merge feature\n\nCo-Authored-By: codex' || exit 1
git -C "$repo" checkout -q main || exit 1
printf "Merge branch 'merge-source'\n\nGenerated-with: human\n" > "$tmproot/expected"
check_commit merge_no_ff by:human env GIT_MERGE_AUTOEDIT=no \
    git -C "$repo" merge -q --no-ff merge-source

# Squash retains indented source trailers as prose. The current policy adds
# human to the outer message; do not reinterpret those indented trailers.
git -C "$repo" checkout -q -b squash-source || exit 1
printf 'squash change\n' > "$repo/squash-file"
git -C "$repo" add squash-file || exit 1
env -u CLAUDECODE git -C "$repo" commit -q -m $'Squash feature\n\nCo-Authored-By: codex' || exit 1
squash_source=$(git -C "$repo" rev-parse HEAD)
git -C "$repo" checkout -q main || exit 1
git -C "$repo" merge -q --squash squash-source > "$tmproot/squash.log" 2>&1 || exit 1
if [[ "$(cat "$repo/.git/SQUASH_MSG")" == *$'\n    Co-Authored-By: codex\n'* ]]; then
    report true 'squash: SQUASH_MSG contains indented source trailers'
else
    report false 'squash: SQUASH_MSG contains indented source trailers'
fi
cat > "$tmproot/expected" <<MSG
Squashed commit of the following:

commit $squash_source
Author: Test <test@example.invalid>
Date:   Sat Jan 1 00:00:00 2000 +0000

    Squash feature

    Co-Authored-By: codex
    Generated-with: codex

Generated-with: human
MSG
check_commit squash by:human git -C "$repo" commit -q --no-edit

reverted_commit=$(git -C "$repo" rev-parse HEAD)
# Git revert bypasses commit-msg, so no Generated-with is added and the board
# reports unknown. Pin that behavior rather than changing the hook's policy.
cat > "$tmproot/expected" <<MSG
Revert "Squashed commit of the following:"

This reverts commit $reverted_commit.
MSG
check_commit revert by:unknown git -C "$repo" revert --no-edit HEAD

# Repeat every cleanup path with no agent trailers under both environments.
# Start at the same commit and reuse branch names to keep the fixtures stable.
paths_base=$(git -C "$repo" rev-parse HEAD)
for setting in unset set; do
    claude_env=(env -u CLAUDECODE)
    label=by:human
    if [[ "$setting" == set ]]; then
        claude_env=(env CLAUDECODE=1)
        label=by:unknown
    fi
    git -C "$repo" checkout -q -B "paths-$setting" "$paths_base" || exit 1
    expected_path() {
        printf '%s' "$1" > "$tmproot/expected"
        if [[ "$setting" == unset ]]; then
            printf '\nGenerated-with: human\n' >> "$tmproot/expected"
        fi
    }
    cat > "$tmproot/plain-editor" <<'SH'
#!/bin/sh
cat > "$1" <<'MSG'
Editor fallback

An ordinary explanation.
# Git removes this comment after the hook.
MSG
SH
    chmod +x "$tmproot/plain-editor" || exit 1
    expected_path $'Editor fallback\n\nAn ordinary explanation.\n'
    check_commit "${setting}_editor_no_trailer" "$label" "${claude_env[@]}" \
        GIT_EDITOR="$tmproot/plain-editor" git -C "$repo" commit -q --allow-empty

    printf 'verbose fallback\n' > "$repo/verbose-fallback"
    git -C "$repo" add verbose-fallback || exit 1
    cat > "$tmproot/plain-verbose-editor" <<'SH'
#!/bin/sh
cat > "$1" <<'MSG'
Verbose fallback

An ordinary explanation.

# ------------------------ >8 ------------------------
# Git removes the following verbose diff after the hook.
diff --git a/verbose-fallback b/verbose-fallback
new file mode 100644
--- /dev/null
+++ b/verbose-fallback
@@ -0,0 +1 @@
+verbose fallback
MSG
SH
    chmod +x "$tmproot/plain-verbose-editor" || exit 1
    expected_path $'Verbose fallback\n\nAn ordinary explanation.\n'
    check_commit "${setting}_verbose_no_trailer" "$label" "${claude_env[@]}" \
        GIT_EDITOR="$tmproot/plain-verbose-editor" git -C "$repo" commit -q -v

    git -C "$repo" checkout -q -B paths-merge-source || exit 1
    printf 'merge fallback\n' > "$repo/merge-fallback"
    git -C "$repo" add merge-fallback || exit 1
    # Source fixtures have no trailers; only the resulting merge/squash runs
    # the hook under test, with its environment selected explicitly.
    env -u CLAUDECODE git -C "$repo" commit -q --no-verify -m 'Merge fallback' || exit 1
    git -C "$repo" checkout -q "paths-$setting" || exit 1
    expected_path "Merge branch 'paths-merge-source' into paths-$setting"$'\n'
    check_commit "${setting}_merge_no_trailer" "$label" "${claude_env[@]}" \
        GIT_MERGE_AUTOEDIT=no git -C "$repo" merge -q --no-ff paths-merge-source

    git -C "$repo" checkout -q -B paths-squash-source || exit 1
    printf 'squash fallback\n' > "$repo/squash-fallback"
    git -C "$repo" add squash-fallback || exit 1
    env -u CLAUDECODE git -C "$repo" commit -q --no-verify -m 'Squash fallback' || exit 1
    squash_source=$(git -C "$repo" rev-parse HEAD)
    git -C "$repo" checkout -q "paths-$setting" || exit 1
    env -u CLAUDECODE git -C "$repo" merge -q --squash paths-squash-source \
        > "$tmproot/squash.log" 2>&1 || exit 1
    cat > "$tmproot/expected" <<MSG
Squashed commit of the following:

commit $squash_source
Author: Test <test@example.invalid>
Date:   Sat Jan 1 00:00:00 2000 +0000

    Squash fallback
MSG
    if [[ "$setting" == unset ]]; then
        printf '\nGenerated-with: human\n' >> "$tmproot/expected"
    fi
    check_commit "${setting}_squash_no_trailer" "$label" "${claude_env[@]}" \
        git -C "$repo" commit -q --no-edit

    reverted_commit=$(git -C "$repo" rev-parse HEAD)
    cat > "$tmproot/expected" <<MSG
Revert "Squashed commit of the following:"

This reverts commit $reverted_commit.
MSG
    # Revert bypasses commit-msg in both environments.
    check_commit "${setting}_revert_no_trailer" by:unknown "${claude_env[@]}" \
        git -C "$repo" revert --no-edit HEAD
done

# Removing just the environment guard must break the very same key case.
# Isolate the expected assertion failures so they cannot mask suite failures.
mkdir "$tmproot/mutant-hooks" || exit 1
sed 's/agent or os.environ.get("CLAUDECODE")/agent/' "$HOOK_SRC" \
    > "$tmproot/mutant-hooks/commit-msg" || exit 1
chmod +x "$tmproot/mutant-hooks/commit-msg" || exit 1
if (
    tests_run=0
    tests_failed=0
    export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.hooksPath
    export GIT_CONFIG_VALUE_0="$tmproot/mutant-hooks"
    claudecode_key_case
    [[ $tests_run -eq 4 && $tests_failed -eq 2 ]]
) > "$tmproot/mutant.log" 2>&1 \
    && grep -Eq 'FAIL: claudecode_no_trailer: exact committed message' "$tmproot/mutant.log" \
    && grep -Eq 'FAIL: claudecode_no_trailer: board label by:unknown \(got by:human\)' "$tmproot/mutant.log"; then
    report true 'negative control: removing CLAUDECODE guard fails the key case'
else
    report false 'negative control: removing CLAUDECODE guard fails the key case'
    cat "$tmproot/mutant.log"
fi

# Every direct error case must exit zero AND preserve bytes, including inputs
# that Git itself would reject before/after invoking commit-msg.
direct_case() {
    local name="$1" file="$2"; shift 2
    cp "$file" "$tmproot/before" || exit 1
    if env -u CLAUDECODE "$@" > "$tmproot/direct.log" 2>&1; then
        report true "$name: exit zero"
    else
        report false "$name: exit zero"
        cat "$tmproot/direct.log"
    fi
    same "$tmproot/before" "$file" "$name: byte-identical input"
}

printf 'Task\n\nCo-Authored-By: codex\n' > "$tmproot/message"
direct_case missing_argument "$tmproot/message" "$hook"
direct_case nonexistent_file "$tmproot/message" "$hook" "$tmproot/absent"
if [[ ! -e "$tmproot/absent" ]]; then
    report true 'nonexistent file is not created'
else
    report false 'nonexistent file is not created'
fi
: > "$tmproot/empty"
direct_case empty "$tmproot/empty" "$hook" "$tmproot/empty"
printf ' \n\t\n' > "$tmproot/blank"
direct_case blank "$tmproot/blank" "$hook" "$tmproot/blank"
printf 'Task\n\000\377\001garbage\n' > "$tmproot/binary"
direct_case binary "$tmproot/binary" "$hook" "$tmproot/binary"
# A valid UTF-8 NUL must also be rejected, separately from invalid UTF-8.
printf 'Task\n\000garbage\n' > "$tmproot/nul"
direct_case nul "$tmproot/nul" "$hook" "$tmproot/nul"
chmod 444 "$tmproot/message"
direct_case unwritable "$tmproot/message" "$hook" "$tmproot/message"
chmod 644 "$tmproot/message"
cp "$tmproot/message" "$tmproot/before" || exit 1
chmod 000 "$tmproot/message"
if env -u CLAUDECODE "$hook" "$tmproot/message"; then
    report true 'unreadable: exit zero'
else
    report false 'unreadable: exit zero'
fi
chmod 644 "$tmproot/message"
same "$tmproot/before" "$tmproot/message" 'unreadable: byte-identical input'

# A private PATH contains a real Python executable but deliberately no git.
# Resolve symlinks/shims so Python does not itself require PATH lookup.
real_python=$(python3 -c 'import sys; print(sys.executable)')
ln -s "$real_python" "$tmproot/no-tools/python3" || exit 1
direct_case missing_git "$tmproot/message" env PATH="$tmproot/no-tools" "$hook" "$tmproot/message"
direct_case missing_python "$tmproot/message" env PATH="$tmproot/absent" "$hook" "$tmproot/message"

mkdir "$tmproot/failing-git"
cat > "$tmproot/failing-git/git" <<'SH'
#!/bin/sh
exit 1
SH
chmod +x "$tmproot/failing-git/git"
direct_case parser_failure "$tmproot/message" env PATH="$tmproot/failing-git:$PATH" "$hook" "$tmproot/message"

# Inject filesystem errors at the Python API boundary without relying on root
# permissions, disk exhaustion, or filesystem-specific behavior in fixtures.
mkdir "$tmproot/failing-io"
cat > "$tmproot/failing-io/python3" <<'SH'
#!/bin/sh
exec "$PGWT_REAL_PYTHON" -c '
import os, subprocess, sys, tempfile
from pathlib import Path
sys.argv = sys.argv[1:]
def fail(*args, **kwargs):
    raise OSError("injected filesystem failure")
if os.environ["PGWT_IO_FAILURE"] == "temp":
    tempfile.mkstemp = fail
elif os.environ["PGWT_IO_FAILURE"] == "replace":
    os.replace = fail
else:
    original_call = subprocess.check_call
    def broken_call(args, **kwargs):
        path = Path(args[-1])
        original = path.read_bytes()
        if os.environ["PGWT_IO_FAILURE"] == "write":
            path.write_bytes(b"truncated temp copy")
            fail()
        result = original_call(args, **kwargs)
        if os.environ["PGWT_IO_FAILURE"] == "corrupt":
            path.write_bytes(b"Generated-with: codex\n")
        elif os.environ["PGWT_IO_FAILURE"] == "trailers":
            # All original lines survive, but Co-Authored-By no longer parses.
            path.write_bytes(path.read_bytes().replace(
                b"\nGenerated-with:", b"\n\nGenerated-with:"))
        elif os.environ["PGWT_IO_FAILURE"] == "missing_generated":
            path.write_bytes(original)
        return result
    subprocess.check_call = broken_call
exec(compile(sys.stdin.read(), "commit-msg", "exec"))
' "$@"
SH
chmod +x "$tmproot/failing-io/python3"
for failure in temp replace write corrupt trailers missing_generated; do
    direct_case "${failure}_failure" "$tmproot/message" env \
        PATH="$tmproot/failing-io:$PATH" PGWT_REAL_PYTHON="$real_python" \
        PGWT_IO_FAILURE="$failure" "$hook" "$tmproot/message"
done

echo "test_commit_msg_hook: $((tests_run - tests_failed))/$tests_run passed"
[[ $tests_failed -eq 0 ]]
