#!/usr/bin/env bash
# record-web.sh — Record demos/web.gif end-to-end.
#
# Boots a local pgwt-server pointed at demos/local-trace, opens it in a real
# Chromium via Playwright, drives the demo flow (zoom + tab switching),
# stops the server, converts video to gif.
#
# Why a real browser instead of headless: ECharts canvas drawing renders
# correctly and animations look right.
#
# Requires:
#   pgwt-server already built (run demos/build-server-local.sh)
#   demos/local-trace/ populated (run demos/capture-trace.sh)
#   node + npm (for Playwright)
#   ffmpeg on PATH

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
TRACE_DIR="${TRACE_DIR:-$HERE/local-trace}"
PORT="${PORT:-8384}"

[[ -x "$REPO_ROOT/pgwt-server" ]] || { echo "FATAL: build pgwt-server first (demos/build-server-local.sh)"; exit 1; }
[[ -d "$TRACE_DIR" ]] || { echo "FATAL: no trace dir at $TRACE_DIR (run demos/capture-trace.sh)"; exit 1; }
grep -Eq '^[1-9][0-9]*$' "$TRACE_DIR/capture-cpus.txt" || {
    echo "FATAL: capture CPU metadata missing — run 'make trace'" >&2
    exit 1
}
PGWT_DEMO_CPUS="$(cat "$TRACE_DIR/capture-cpus.txt")"
export PGWT_DEMO_CPUS

# Pin the browser driver validated by this pipeline.
PLAYWRIGHT_VERSION=1.64.0
if [[ "$(cd "$HERE" && node -p "require('./node_modules/playwright/package.json').version" 2>/dev/null || true)" != "$PLAYWRIGHT_VERSION" ]]; then
    echo "==> Installing Playwright (one-time) ..."
    (cd "$HERE" && npm install --no-fund --no-audit --save-exact "playwright@$PLAYWRIGHT_VERSION")
fi
echo "==> Checking Playwright Chromium ..."
(cd "$HERE" && npx playwright install chromium --no-shell)

# Build the web client if missing
if ! command -v pgwt >/dev/null 2>&1 && [[ ! -x "$REPO_ROOT/web/pgwt" ]]; then
    echo "==> Building pgwt web client ..."
    (cd "$REPO_ROOT" && make pgwt-client)
fi
PGWT_BIN="${PGWT_BIN:-$REPO_ROOT/web/pgwt}"
[[ -x "$PGWT_BIN" ]] || PGWT_BIN="$(command -v pgwt)"

# The demo's private SSH shim replays locally and ignores all SSH options.
# Current pgwt passes -o options BEFORE the hostname.
SSH_SHIM_DIR="$(mktemp -d)"
cleanup() {
    if [[ -n "${PGWT_PID:-}" ]]; then
        kill -INT "$PGWT_PID" 2>/dev/null || true
        wait "$PGWT_PID" 2>/dev/null || true
    fi
    rm -rf "$SSH_SHIM_DIR"
}
trap cleanup EXIT
cat > "$SSH_SHIM_DIR/ssh" <<EOF
#!/usr/bin/env bash
exec "$REPO_ROOT/pgwt-server" "$TRACE_DIR"
EOF
chmod +x "$SSH_SHIM_DIR/ssh"
# Playwright opens the recording browser; avoid an extra browser window.
printf '#!/bin/sh\nexit 0\n' > "$SSH_SHIM_DIR/open"
cp "$SSH_SHIM_DIR/open" "$SSH_SHIM_DIR/xdg-open"
chmod +x "$SSH_SHIM_DIR/open" "$SSH_SHIM_DIR/xdg-open"

echo "==> Starting pgwt web client on port $PORT ..."
PATH="$SSH_SHIM_DIR:$PATH" "$PGWT_BIN" \
    --port "$PORT" \
    --server-path "$REPO_ROOT/pgwt-server" \
    --trace-dir "$TRACE_DIR" \
    fake-user@localhost > "$HERE/.pgwt.log" 2>&1 &
PGWT_PID=$!

# Wait for HTTP to come up
HTTP_READY=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 "$PGWT_PID" 2>/dev/null || break
    if curl -sf "http://localhost:$PORT/" > /dev/null 2>&1; then
        echo "==> pgwt web up at http://localhost:$PORT"
        HTTP_READY=1
        break
    fi
    sleep 1
done

[[ "$HTTP_READY" -eq 1 ]] || {
    echo "FATAL: pgwt failed to start — see demos/.pgwt.log" >&2
    cat "$HERE/.pgwt.log" >&2
    exit 1
}

# Run Playwright to drive + record
PGWT_URL="http://localhost:$PORT/" node "$HERE/record-web.mjs"

echo "==> demos/web.gif ready"
ls -la "$HERE/web.gif"
