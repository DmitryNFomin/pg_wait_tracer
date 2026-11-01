#!/usr/bin/env bash
# Rule 3 examines invocation operands; the other four rules deliberately
# remain raw substring tests. Feed JSON only: these commands never execute.
# PGWT_TEST_PUSH_GUARD_DENYLIST can supply an alternate hook script, to
# show these checks go red against a regressed one (same convention as
# tests/test_pre_push_hook.sh's PGWT_TEST_PRE_PUSH_HOOK).
#
# Usage: tests/test_push_guard_denylist.sh (run from anywhere)
# Commands below are literal test data, never shell-expanded.
# shellcheck disable=SC2016
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOOK="${PGWT_TEST_PUSH_GUARD_DENYLIST:-$REPO_ROOT/scripts/hooks/push-guard.sh}"

# Replay top-level project transcripts (the supplied *.jsonl corpus), accepting
# both hook records and Claude transcript tool_use records normalized to the
# same tool_name/tool_input shape. --replay PATH also accepts a saved snapshot.
if [[ "${1:-}" == "--replay" ]]; then
    exec python3 - "$HOOK" "${2:-$HOME/.claude/projects/-Users-dmitryfomin-work-git-pg-wait-tracer}" <<'PYREPLAY'
import concurrent.futures, glob, json, pathlib, subprocess, sys
hook, source = sys.argv[1:]
paths = sorted(glob.glob(str(pathlib.Path(source) / "*.jsonl"))) if pathlib.Path(source).is_dir() else [source]
if not paths:
    sys.exit("no transcript files found")
commands, malformed = [], 0
for path in paths:
    with open(path) as stream:
        for line_no, line in enumerate(stream, 1):
            try:
                record = json.loads(line)
            except ValueError:
                malformed += 1
                continue
            entries = [record] if record.get("tool_name") == "Bash" else []
            content = record.get("message", {}).get("content", [])
            if isinstance(content, list):
                entries += [{"tool_name": "Bash", "tool_input": c.get("input", {})}
                            for c in content if c.get("type") == "tool_use" and c.get("name") == "Bash"]
            for entry in entries:
                command = entry.get("tool_input", {}).get("command")
                if not isinstance(command, str):
                    sys.exit(f"missing Bash command at {path}:{line_no}")
                commands.append((record.get("source", path), record.get("line", line_no), command))

def replay(item):
    path, line_no, command = item
    result = subprocess.run(["bash", hook], input=json.dumps({"tool_name": "Bash", "tool_input": {"command": command}}), text=True, capture_output=True)
    if result.returncode not in (0, 2):
        raise RuntimeError(f"guard failed at {path}:{line_no}: {result.stderr}")
    return item, result

allowed = denied = 0
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    for (path, line_no, command), result in pool.map(replay, commands):
        if result.returncode == 0:
            allowed += 1
            continue
        denied += 1
        print(f"DENIAL {denied}: {path}:{line_no} [exit {result.returncode}]")
        print("COMMAND BEGIN\n" + command + "\nCOMMAND END")
        print(result.stdout + result.stderr, end="")
print(f"REPLAY files={len(paths)} commands={len(commands)} allowed={allowed} denied={denied} malformed_json_lines={malformed}")
PYREPLAY
fi

tests_run=0
tests_passed=0
tests_failed=0

report() {
    local ok="$1" msg="$2"
    tests_run=$((tests_run + 1))
    if [[ "$ok" == "true" ]]; then
        tests_passed=$((tests_passed + 1))
        echo "  PASS: $msg"
    else
        tests_failed=$((tests_failed + 1))
        echo "  FAIL: $msg"
    fi
}
check() { if [[ "$1" == "true" ]]; then report true "$2"; else report false "$2"; fi; }

# Builds the real Claude Code hook JSON ({"tool_input":{"command": ...}})
# for an arbitrary command string, including one containing newlines and
# apostrophes -- json.dumps handles the escaping, not this script.
hook_json() {
    python3 -c 'import json, sys; print(json.dumps({"tool_input": {"command": sys.argv[1]}}))' "$1"
}

run_hook() {
    printf '%s' "$(hook_json "$1")" | bash "$HOOK" 2>&1
}

denied() {
    local command="$1" label="$2"
    local out rc
    out=$(run_hook "$command"); rc=$?
    check "$([[ $rc -ne 0 ]] && echo true || echo false)" "$label: denied (nonzero exit)"
    check "$([[ -n "$out" ]] && echo true || echo false)" "$label: a message was printed, not a silent refusal"
}

allowed() {
    local command="$1" label="$2"
    local rc
    run_hook "$command" >/dev/null 2>&1; rc=$?
    check "$([[ $rc -eq 0 ]] && echo true || echo false)" "$label: allowed (exit 0)"
}

# ── Each of the five rules denies ────────────────────────────────────────
denied 'PGWT_SKIP_PUSH_GUARD=1 git push origin main' "PGWT_SKIP_PUSH_GUARD=1"
denied 'git config core.hooksPath /tmp/somewhere' "core.hooksPath"
denied 'chmod -x scripts/git-hooks/pre-push' "chmod + git-hooks together"
denied 'git push --no-verify origin main' "--no-verify"
denied 'git send-pack origin HEAD' "git send-pack bypasses pre-push entirely (#687)"

# ── Write-shaped operations together with git-hooks deny ────────────────
denied 'rm -f scripts/git-hooks/pre-push' "removal + git-hooks"
denied ': > scripts/git-hooks/pre-push' "truncating redirection + git-hooks"
denied 'install -m 000 /dev/null scripts/git-hooks/pre-push' "permission-stripping install + git-hooks"
denied 'echo exit 0 > scripts/git-hooks/pre-push' "overwrite via redirection + git-hooks"
allowed 'git commit -m "it'"'"'s fine, just discussing scripts/git-hooks/pre-push here"' \
    "apostrophe-bearing commit message mentioning git-hooks, no write-shaped operation: allowed"

# ── Write-shaped operations and git-hooks only deny TOGETHER ─────────────
allowed 'chmod +x my-script.sh' "chmod alone (no git-hooks mention)"
allowed 'ls -la scripts/git-hooks' "git-hooks alone (no chmod)"
allowed 'cat scripts/git-hooks/pre-push' "reading git-hooks"
allowed 'cat scripts/git-hooks/pre-push 2>&1' \
    "benign read with > in stderr redirection, without writing to the hook"
allowed 'echo "see scripts/git-hooks/pre-push -> it skips a non-executable hook"' \
    "benign prose with ->, without writing to the hook"
allowed 'diff <(cat a) <(cat scripts/git-hooks/pre-push) > /dev/null' \
    "benign comparison with > targeting /dev/null, without writing to the hook"
allowed 'git log -- scripts/git-hooks/pre-push' "history query for git-hooks"
allowed 'git commit -m "doc: explain git-hooks in README"' \
    "a commit message merely mentioning git-hooks with no write-shaped operation"

# ── A command with none of the five terms passes untouched ───────────────
allowed 'git push origin main' "an ordinary push with none of the five terms"
allowed 'git status && git log --oneline -5' "an unrelated command with none of the five terms"

# ── THE control that matters: odd apostrophe count, no push, none of the
#    five denylist terms -- must pass. The retired parser rejected exactly
#    this shape outright, blocking an unrelated command for a quoting
#    reason that had nothing to do with pushing or disarming anything. ──
IFS= read -r -d '' odd_apostrophe_heredoc <<'INNER'
cat > f.txt <<'EOF'
it can't be a push; neither can 'tests' alone
EOF
INNER
allowed "$odd_apostrophe_heredoc" \
    "odd-apostrophe heredoc, no push, no denylist term: passes (the shape the retired parser rejected)"

# ── The one accepted false positive: a MENTION, not a push ───────────────
denied 'git commit -m "doc: explain core.hooksPath in README"' \
    "a commit message merely mentioning core.hooksPath: denied too (the accepted tradeoff -- one blocked command, never a silent gap)"

# Required #719 controls: paste both the exact command and actual hook output.
control() {
    local label="$1" expected="$2" command="$3" out rc
    out=$(run_hook "$command"); rc=$?
    printf 'CONTROL %s: %s\nexit=%s output=%s\n' "$label" "$command" "$rc" "${out:-<empty>}"
    check "$([[ $rc -eq $expected ]] && echo true || echo false)" "control $label"
}
control 1 0 'git show origin/master:scripts/git-hooks/pre-push 2>/dev/null | grep -n stamp'
body_file=$(mktemp)
trap 'rm -f "$body_file"' EXIT
printf '%s\n' 'The guard denies rm scripts/git-hooks/pre-push.' > "$body_file"
control 2 0 "gh issue comment 719 --body-file $body_file"
# The guard does not open the file: tool payload files are data.
control 3 0 'gh issue close 719 --comment "The guard blocks rm scripts/git-hooks/pre-push and 2>/dev/null near it"'
control 4a 0 'ls -l scripts/git-hooks'
control 4b 0 'cat scripts/git-hooks/pre-push'
control 5 2 'rm scripts/git-hooks/pre-push'
control 6 2 ': > scripts/git-hooks/pre-push'
control 7 2 'install /dev/null scripts/git-hooks/pre-push'
control 8 2 'cp /tmp/x scripts/git-hooks/pre-push'
control 9 2 'chmod -x scripts/git-hooks/pre-push'
control 10 2 'echo x > scripts/git-hooks/pre-push'
control 11a 2 'PGWT_SKIP_PUSH_GUARD=1 git push'
control 11b 2 'git -c core.hooksPath=/dev/null push'
control 11c 2 'git push --no-verify'
control 11d 2 'git send-pack'

# Payloads and invocation boundaries, including commands following data.
allowed 'echo "rm scripts/git-hooks/pre-push"' "quoted mutation is data"
allowed "gh issue comment 719 --body 'chmod -x scripts/git-hooks/pre-push'" "inline body is data"
allowed 'git commit -m "rm scripts/git-hooks/pre-push"' "commit message is data"
allowed 'cat scripts/git-hooks/pre-push; rm /tmp/other' "unrelated mutation in separate invocation"
allowed 'cp scripts/git-hooks/pre-push /tmp/backup' "copying hook out only reads it"
allowed 'ln -s scripts/git-hooks/pre-push /tmp/link' "linking hook as source only reads it"
allowed 'echo ok # rm scripts/git-hooks/pre-push' "shell comment is data"
allowed 'cat "$(printf %s scripts/git-hooks/pre-push)"' "read with command substitution"
allowed 'diff <(cat scripts/git-hooks/pre-push) <(cat /tmp/backup)' "read with process substitution"
IFS= read -r -d '' hook_prose <<'INNER'
gh issue comment 719 --body-file - <<'EOF'
it can't rm scripts/git-hooks/pre-push; chmod -x is dangerous
EOF
INNER
allowed "$hook_prose" "heredoc hook discussion is data"
denied "$hook_prose"$'\nrm scripts/git-hooks/pre-push' "mutation following heredoc"
for verb in mv truncate tee ln unlink touch; do
    denied "$verb /tmp/x scripts/git-hooks/pre-push" "real $verb operand"
done
denied '/bin/rm -- "scripts/git-hooks/pre-push"' "absolute mutator and quoted path"
denied 'env X=1 command rm scripts/git-hooks/pre-push' "wrapped invocation"
denied 'echo "$(rm scripts/git-hooks/pre-push)"' "executed command substitution"
denied 'bash -c "rm scripts/git-hooks/pre-push"' "explicit shell command operand"
denied 'cp -t scripts/git-hooks /tmp/x' "target-directory operand"
denied 'echo x >>"scripts/git-hooks/pre-push"' "quoted append target"
denied 'dd if=/dev/null of=scripts/git-hooks/pre-push' "dd output operand"
denied 'eval "rm scripts/git-hooks/pre-push"' "ambiguous executable payload"
denied 'echo "scripts/git-hooks/pre-push' "ambiguous unfinished quote"

IFS= read -r -d '' quoted_heredoc <<'INNER'
gh issue close 719 --comment "$(cat <<'EOF'
it can't rm scripts/git-hooks/pre-push; a ) is prose here
EOF
)"
INNER
allowed "$quoted_heredoc" "heredoc in a quoted body substitution is data"
denied '2>/dev/null rm scripts/git-hooks/pre-push' "redirect before command preserves command position"
denied '{ rm scripts/git-hooks/pre-push; }' "mutation inside command group"
denied 'time rm scripts/git-hooks/pre-push' "mutation with time keyword"

allowed 'cp -t /tmp/backup scripts/git-hooks/pre-push' "target-directory copy reads hook"
allowed 'ln --target-directory=/tmp/backup scripts/git-hooks/pre-push' "target-directory link reads hook"
denied 'cp /tmp/x scripts/git-hooks/pre-push -f' "trailing copy option preserves destination"
denied 'install -d scripts/git-hooks /tmp/other' "install directory form mutates every operand"

IFS= read -r -d '' shell_heredoc <<'INNER'
bash <<'EOF'
rm scripts/git-hooks/pre-push
EOF
INNER
denied "$shell_heredoc" "shell interpreter executes its heredoc"

echo
echo "test_push_guard_denylist: $tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
