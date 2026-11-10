#!/bin/sh
# Offline black-box tests; --negative-controls also mutates isolated code copies.
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$D/board-selftest.py" "$@"
