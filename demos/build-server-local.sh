#!/usr/bin/env bash
# build-server-local.sh — Build pgwt-server on the local machine.
#
# pgwt-server is BPF-free — it just reads compressed trace files and
# computes aggregates. So it builds and runs anywhere C + lz4 + zlib do,
# including macOS. Used to render demo gifs locally without round-tripping
# to the Linux VM.
#
# Linux: assumes lz4 + zlib dev headers in standard locations.
# macOS: needs `brew install lz4` and uses Homebrew's prefix.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"
# Ask the Makefile for the stamp so both build entry points agree (#572).
BUILD_VERSION="$(make -s --no-print-directory print-build-version)"

case "$(uname -s)" in
    Darwin)
        BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
        [[ -d "$BREW_PREFIX/include" ]] || {
            echo "FATAL: Homebrew prefix not found. Install Homebrew + 'brew install lz4'." >&2
            exit 1
        }
        echo "==> Building pgwt-server (Darwin, brew prefix $BREW_PREFIX) ..."
        # Keep Linux APIs out of the macOS replay build. Force rebuilding so
        # compiler flags and the embedded version cannot be stale.
        make -B pgwt-server \
            CFLAGS="-g -O2 -Wall -Wextra -Wno-unused-parameter -include $REPO_ROOT/demos/replay-compat.h -DPGWT_BUILD_VERSION='\"$BUILD_VERSION\"' -I$BREW_PREFIX/include -Iinclude -Isrc" \
            SERVER_LDFLAGS="-L$BREW_PREFIX/lib -lz -llz4 -lm"
        ;;
    Linux)
        echo "==> Building pgwt-server (Linux) ..."
        make -B pgwt-server \
            CFLAGS="-g -O2 -Wall -Wextra -Wno-unused-parameter -include $REPO_ROOT/demos/replay-compat.h -DPGWT_BUILD_VERSION='\"$BUILD_VERSION\"' -Iinclude -Isrc"
        ;;
    *)
        echo "FATAL: unsupported OS: $(uname -s)" >&2
        exit 1
        ;;
esac

ls -la "$REPO_ROOT/pgwt-server"
echo "==> pgwt-server ready at $REPO_ROOT/pgwt-server"
