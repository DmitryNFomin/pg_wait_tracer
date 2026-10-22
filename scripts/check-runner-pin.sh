#!/usr/bin/env bash
# check-runner-pin.sh — refuse a tree whose CI has drifted back to a floating
# runner image, or whose C test build has lost the flag that makes a missing
# POSIX #include fatal.
#
# Why this exists (2026-10-22): GitHub rolled `ubuntu-latest` to Ubuntu 26.04
# and four gating jobs went red at once (run 39765852420) — Playwright 1.60.0
# has no chromium for ubuntu26.04-x64, actions/setup-python handed the
# v1.60.0-noble container a Python with no working `ssl` module, and the newer
# GCC turned a long-standing implicit declaration in tests/gen_test_traces.c
# into an error. The Playwright version cannot move without regenerating all
# of tests/web_snapshots/ (the baselines are byte-compared against that
# version's glyph rendering), so every hosted job is pinned to `ubuntu-24.04`.
# A "modernisation" back to `ubuntu-latest` would silently re-arm the same
# breakage; and pinning the image also makes the implicit-declaration class
# invisible again (that GCC only warns) — hence the second half of this guard,
# which keeps -Werror=implicit-function-declaration in tests/Makefile's CFLAGS
# AND verifies every recipe there still passes $(CFLAGS). A recipe that drops
# $(CFLAGS) is how the flag would stop applying without anyone touching it.
#
# FAIL-SAFE: this guard REFUSES (exit 1, "REFUSE:") whenever it cannot see
# what it is supposed to check — no workflow directory, no workflow files, a
# workflow with no `runs-on:` at all, a `runs-on:` value it cannot resolve
# (block/list form, or an expression indirected through
# matrix/env/vars/inputs), no tests/Makefile, no or multiple CFLAGS
# definitions, no compile recipes, or a `grep` that errors out (exit >= 2,
# which includes a 126/127 shim). A pin check that found nothing to check
# must never report success.
#
# Usage: scripts/check-runner-pin.sh [repo-dir]       (default: this repo)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="${1:-$SCRIPT_DIR/..}"

rc=0
refuse() { printf 'REFUSE: %s\n' "$1" >&2; rc=1; }
fail()   { printf 'FAIL: %s\n' "$1" >&2; rc=1; }

# ── dependencies ────────────────────────────────────────────────────────────
for tool in grep sed; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        refuse "required tool '$tool' not found in PATH — cannot verify anything"
        exit 1
    fi
done

# grep_checked <args...> — run grep; an exit status >= 2 means grep itself
# failed (bad invocation, or a 126/127 shim) and is a refusal, never treated
# as "no matches". Output lands in GREP_OUT.
GREP_OUT=""
grep_checked() {
    local out grc
    out="$(grep "$@" 2>/dev/null)"; grc=$?
    if (( grc >= 2 )); then
        refuse "grep exited $grc for: grep $* — dependency broken, refusing"
        return 2
    fi
    GREP_OUT="$out"
    return 0
}

# ── part 1: every runs-on in every workflow names a pinned image ────────────
WF_DIR="$REPO/.github/workflows"
if [[ ! -d "$WF_DIR" ]]; then
    refuse "no $WF_DIR — cannot verify runner pins"
else
    shopt -s nullglob
    wf_files=("$WF_DIR"/*.yml "$WF_DIR"/*.yaml)
    shopt -u nullglob
    if (( ${#wf_files[@]} == 0 )); then
        refuse "$WF_DIR contains no workflow files — nothing to check, refusing"
    fi
    values_checked=0
    for f in "${wf_files[@]}"; do
        grep_checked -nE '^[[:space:]]*runs-on:' "$f" || continue
        if [[ -z "$GREP_OUT" ]]; then
            refuse "$f declares no 'runs-on:' — a workflow whose runner cannot be seen is not verified"
            continue
        fi
        while IFS= read -r hit; do
            [[ -z "$hit" ]] && continue
            lineno="${hit%%:*}"
            value="$(printf '%s' "${hit#*:}" | sed -E 's/^[[:space:]]*runs-on:[[:space:]]*//; s/[[:space:]]*$//')"
            # Strip a trailing YAML comment so the pin's own "not ubuntu-latest"
            # note does not look like a floating label. Only outside a `${{ }}`
            # expression — a comment cannot change which runner is used, but a
            # value inside the expression can.
            if [[ "$value" != *'${{'* ]]; then
                value="$(printf '%s' "$value" | sed -E 's/[[:space:]]+#.*$//; s/[[:space:]]*$//')"
            fi
            values_checked=$((values_checked + 1))
            if [[ -z "$value" ]]; then
                refuse "$f:$lineno runs-on has no inline value (block/list form) — cannot resolve the image, refusing"
                continue
            fi
            if [[ "$value" == *'${{'* ]] \
               && printf '%s' "$value" | grep -qE '(matrix|env|vars|inputs|secrets)\.'; then
                refuse "$f:$lineno runs-on is indirected through an unresolvable context ($value) — refusing"
                continue
            fi
            if printf '%s' "$value" | grep -qE '(ubuntu|macos|windows)-latest'; then
                fail "$f:$lineno runs-on uses a FLOATING image ($value). Pin it (ubuntu-24.04) — see the RUNNER IMAGE PIN header in .github/workflows/ci.yml for what must be true before the pin moves."
                continue
            fi
        done <<< "$GREP_OUT"
    done
    if (( values_checked == 0 )) && (( ${#wf_files[@]} > 0 )); then
        refuse "no runs-on values were checked at all — refusing rather than approving"
    fi
    printf 'runner pin: %d runs-on value(s) across %d workflow file(s) checked\n' \
        "$values_checked" "${#wf_files[@]}"
fi

# ── part 2: the implicit-declaration flag is present AND reaches every recipe ─
MK="$REPO/tests/Makefile"
FLAG='-Werror=implicit-function-declaration'
if [[ ! -f "$MK" ]]; then
    refuse "no $MK — cannot verify $FLAG"
else
    if grep_checked -cE '^CFLAGS[[:space:]]*=' "$MK"; then
        n_cflags="${GREP_OUT:-0}"
        if [[ "$n_cflags" != "1" ]]; then
            refuse "$MK defines CFLAGS $n_cflags time(s) (expected exactly 1) — cannot tell which definition is in force"
        elif grep_checked -E '^CFLAGS[[:space:]]*=' "$MK"; then
            if ! printf '%s' "$GREP_OUT" | grep -qF -- "$FLAG"; then
                fail "$MK CFLAGS is missing $FLAG — an implicit declaration (a missing #include) would be a warning again, which is exactly how tests/gen_test_traces.c:694 reached CI"
            fi
        fi
    fi
    if grep_checked -nE '[$][(]CC[)]' "$MK"; then
        if [[ -z "$GREP_OUT" ]]; then
            refuse "$MK has no \$(CC) compile recipes — nothing for $FLAG to apply to, refusing"
        else
            n_cc=$(printf '%s\n' "$GREP_OUT" | grep -c . || true)
            bad="$(printf '%s\n' "$GREP_OUT" | grep -v 'CFLAGS' || true)"
            if [[ -n "$bad" ]]; then
                fail "$MK has \$(CC) recipe(s) that do not pass \$(CFLAGS), so $FLAG does not apply to them:"
                printf '%s\n' "$bad" >&2
            else
                printf 'implicit-decl flag: %d $(CC) recipe(s) checked, all pass $(CFLAGS)\n' "$n_cc"
            fi
        fi
    fi
fi

if (( rc != 0 )); then
    printf 'runner-pin guard: NOT OK\n' >&2
    exit 1
fi
printf 'runner-pin guard: OK\n'
