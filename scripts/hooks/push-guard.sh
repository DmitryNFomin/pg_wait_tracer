#!/usr/bin/env bash
# Claude Code PreToolUse hook (Bash matcher only): a narrow, four-term
# substring denylist -- no shell parsing, no repository resolution, no
# quoting logic (#230). scripts/git-hooks/pre-push (#332, already on
# master) is the SOLE enforcement for an actual push's stamp/tree check;
# it runs at the real git invocation with the real refs and worktree, so
# it cannot be fooled by shell shape the way a text parser can.
#
# What a git hook structurally cannot see is its OWN disarming. So this
# hook denies any Bash tool command that merely CONTAINS one of four
# strings, whether or not the command is a push at all:
#
#   1. PGWT_SKIP_PUSH_GUARD=1 -- the documented humans-only escape hatch;
#      nothing else stops an agent from setting it itself.
#   2. core.hooksPath         -- the git config that arms the real hook.
#   3. chmod together with git-hooks -- could strip the real hook's +x
#      bit; git silently SKIPS a non-executable pre-push hook (see
#      tests/test_pre_push_hook.sh's documented residual for that fact).
#   4. --no-verify            -- the one git flag that skips a pre-push
#      hook outright; nothing server-side can see it.
#
# Deliberately dumb, on purpose. The retired scripts/hooks/push-guard.sh
# (through #230) parsed shell grammar to decide whether a command was a
# push and against which repository, and every round found a new way to
# be wrong (#219: five rounds; #241: two more after that, including
# refusing an unrelated command for merely quoting the word "push" inside
# a heredoc). A substring test has no repository to resolve, no quoting to
# track, no `cd` to follow -- it is either present in the raw text or it
# is not, so it cannot be "ambiguous".
#
# The one accepted false positive: a commit message, doc edit, or code
# comment (such as this file) that merely MENTIONS one of the four
# strings. That costs one blocked command and a visible stderr line,
# never a silent gap -- the tradeoff this file exists to make.
#
# Fed the Claude Code hook JSON on stdin ({"tool_input":{"command":...}}).
# Deliberately matched against the RAW stdin text rather than a
# JSON-extracted field: there is no parsing step here to get wrong, no
# JSON library call that can throw on malformed input and no field lookup
# that can silently come back empty. A substring is a substring.
set -uo pipefail
input=$(cat)

deny() {
    echo "blocked: command contains '$1' (push-guard denylist, #230) -- $2" >&2
    exit 2
}

case "$input" in
    *'PGWT_SKIP_PUSH_GUARD=1'*)
        deny "PGWT_SKIP_PUSH_GUARD=1" \
            "humans-only escape hatch (CLAUDE.md); an agent must not set it" ;;
esac

case "$input" in
    *'core.hooksPath'*)
        deny "core.hooksPath" \
            "this is the git config that arms scripts/git-hooks/pre-push" ;;
esac

case "$input" in
    *'chmod'*)
        case "$input" in
            *'git-hooks'*)
                deny "chmod ... git-hooks" \
                    "could strip the pre-push hook's executable bit, which git then silently skips" ;;
        esac
        ;;
esac

case "$input" in
    *'--no-verify'*)
        deny "--no-verify" \
            "the one flag that bypasses a pre-push hook outright" ;;
esac

exit 0
