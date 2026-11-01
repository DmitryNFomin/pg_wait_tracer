#!/usr/bin/env bash
# Compare CLAUDE.md with shipped agents, hooks and role invocations.
# Usage: scripts/check-agent-agreement.sh [repo-dir]
# Self-test: tests/test_check_agent_agreement.py (stdlib only).
set -uo pipefail

if ! command -v python3 >/dev/null 2>&1; then
    echo "REFUSING: check-agent-agreement: missing required tool 'python3'" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)" || exit 1
python3 "$SCRIPT_DIR/check-agent-agreement.py" "$@"
status=$?
if [[ $status -eq 126 || $status -eq 127 ]]; then
    echo "REFUSING: check-agent-agreement: could not run checker (python3 exited $status)" >&2
    exit 1
fi
exit "$status"
