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

# Do not inherit repository/worktree overrides or a developer's Git config.
unset "${!GIT_@}" 2>/dev/null || true
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
tmproot="$(mktemp -d "${TMPDIR:-/tmp}/pgwt-commit-msg-test.XXXXXX")" || exit 1
trap 'rm -rf "$tmproot"' EXIT
mkdir -p "$tmproot/repo" "$tmproot/hooks" "$tmproot/no-tools" || exit 1
cp "$HOOK_SRC" "$tmproot/hooks/commit-msg" || exit 1
chmod +x "$tmproot/hooks/commit-msg" || exit 1
hook="$tmproot/hooks/commit-msg"
repo="$tmproot/repo"
git -C "$repo" init -q || exit 1
git -C "$repo" config user.name Test || exit 1
git -C "$repo" config user.email test@example.invalid || exit 1
git -C "$repo" config core.hooksPath "$tmproot/hooks" || exit 1

board_label() {
    python3 -c '
import json, pathlib, sys
message = pathlib.Path(sys.argv[1]).read_text()
headline, _, body = message.partition("\n")
print(json.dumps({"commits": [{"messageHeadline": headline, "messageBody": body}]}))
' "$1" | python3 "$REPO_ROOT/scripts/board-author.py"
}

commit_case() {
    local name="$1" message="$2" addition="$3" expected_label="$4" label
    printf '%s' "$message" > "$tmproot/input"
    cp "$tmproot/input" "$tmproot/expected"
    if [[ -n "$addition" ]]; then
        printf '\n\nGenerated-with: %s\n' "$addition" >> "$tmproot/expected"
    fi
    # Only the disposable repo is committed. Verbatim cleanup lets us assert
    # byte preservation, including comments and existing trailer spelling.
    if git -C "$repo" -c commit.gpgSign=false commit -q --allow-empty \
        --cleanup=verbatim -F "$tmproot/input" > "$tmproot/commit.log" 2>&1; then
        report true "$name: real commit succeeds"
    else
        report false "$name: real commit succeeds"
        cat "$tmproot/commit.log"
        return
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

commit_case codex $'Task\n\nCo-Authored-By: codex <bot@example.invalid>\n' codex by:codex
commit_case opus $'Task\n\nCo-Authored-By: Claude Opus 5 <bot@example.invalid>\n' 'Claude Opus' by:claude-opus
commit_case sonnet $'Task\n\nCo-Authored-By: Claude Sonnet 4 <bot@example.invalid>\n' 'Claude Sonnet' by:claude-sonnet
commit_case joint $'Task\n\nCo-Authored-By: Claude Opus 5\nCo-Authored-By: codex\n' codex by:codex
commit_case opus_precedence $'Task\n\nCo-Authored-By: Claude Sonnet 4\nCo-Authored-By: Claude Opus 5\n' 'Claude Opus' by:claude-opus
commit_case human $'Task\n\nAn ordinary explanation.\n' human by:human
commit_case explicit $'Task\n\ngEnErAtEd-WiTh: Claude Sonnet\n' '' by:claude-sonnet
commit_case explicit_human $'Task\n\nGenerated-with: human\n' '' by:human
# The hook preserves the explicit human declaration; the board independently
# gives codex precedence when both trailers exist.
commit_case explicit_human_codex $'Task\n\nGenerated-with: human\nCo-Authored-By: codex\n' '' by:codex
commit_case explicit_unknown $'Task\n\nGenerated-with: something else\n' '' by:unknown
commit_case session $'Task\n\nClaude-Session: abc123\n' '' by:unknown
commit_case fable $'Task\n\nCo-Authored-By: Claude Fable\n' '' by:unknown
commit_case hyphenated_opus $'Task\n\nCo-Authored-By: claude-opus\n' '' by:unknown
commit_case prose $'Task\n\nExample:\n\nCo-Authored-By: codex\n\nThis is prose, outside the final trailer block.\n' human by:human
commit_case comments $'Task\n\nCo-Authored-By: codex\n# Keep this template comment.\n# And this one.\n' codex by:codex
commit_case folded $'Task\n\nco-AUTHORED-by: Claude\n Sonnet 4\n' 'Claude Sonnet' by:claude-sonnet
commit_case gpt $'Task\n\nSigned-Off-By: GPT-6 <bot@example.invalid>\n' codex by:codex
commit_case session_classified $'Task\n\nClaude-Session: Claude Opus 5\n' 'Claude Opus' by:claude-opus
commit_case human_signoff $'Task\n\nSigned-off-by: A Person <codex@example.invalid>\n' human by:human
commit_case unrelated_key $'Task\n\nReviewed-by: Claude Opus\n' human by:human
commit_case word_boundaries $'Task\n\nCo-Authored-By: codexian\n' human by:human
for evidence in claude Anthropic OpenAI GPT Fable Astra Haiku; do
    commit_case "unknown_$evidence" $'Task\n\nSigned-off-by: '"$evidence"$'\n' '' by:unknown
done

# Every direct error case must exit zero AND preserve bytes, including inputs
# that Git itself would reject before/after invoking commit-msg.
direct_case() {
    local name="$1" file="$2"; shift 2
    cp "$file" "$tmproot/before" || exit 1
    if "$@" > "$tmproot/direct.log" 2>&1; then
        report true "$name: exit zero"
    else
        report false "$name: exit zero"
        cat "$tmproot/direct.log"
    fi
    same "$tmproot/before" "$file" "$name: byte-identical input"
}

printf 'Task\n' > "$tmproot/message"
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
if "$hook" "$tmproot/message"; then
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
import os, sys, tempfile
from pathlib import Path
sys.argv = sys.argv[1:]
def fail(*args, **kwargs):
    raise OSError("injected filesystem failure")
if os.environ["PGWT_IO_FAILURE"] == "temp":
    tempfile.mkstemp = fail
elif os.environ["PGWT_IO_FAILURE"] == "replace":
    os.replace = fail
else:
    original_open = Path.open
    def broken_open(self, mode="r", *args, **kwargs):
        if mode == "ab":
            with original_open(self, "wb") as stream:
                stream.write(b"truncated temp copy")
            if os.environ["PGWT_IO_FAILURE"] == "write":
                fail()
        return original_open(self, mode, *args, **kwargs)
    Path.open = broken_open
exec(compile(sys.stdin.read(), "commit-msg", "exec"))
' "$@"
SH
chmod +x "$tmproot/failing-io/python3"
for failure in temp replace write corrupt; do
    direct_case "${failure}_failure" "$tmproot/message" env \
        PATH="$tmproot/failing-io:$PATH" PGWT_REAL_PYTHON="$real_python" \
        PGWT_IO_FAILURE="$failure" "$hook" "$tmproot/message"
done

echo "test_commit_msg_hook: $((tests_run - tests_failed))/$tests_run passed"
[[ $tests_failed -eq 0 ]]
