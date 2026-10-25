#!/usr/bin/env bash
# Gate-2 only. Hold the common lock for the bridge, server and browser lifetime.
# Example: bash tests/replay_ui.sh --trace-dir /path/to/trace --out-dir /tmp/replay
set -euo pipefail
cd "$(dirname "$0")/.."
exec flock /tmp/pgwt-box-check.lock python3 tests/replay_ui.py "$@"
