#!/bin/sh
# Generated tag board; all GitHub writes are explicit set/publish operations.
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$D/board.py" "$@"
