#!/usr/bin/env bash
# The printed verdict is authoritative. A nonzero exit alone is NOT a verdict.
# Keep the gate executable and resolve relative drafts in the caller's directory.
set -uo pipefail
D="${1:?usage: audit-report.sh <draft-file>}"
SELFDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$SELFDIR/audit-report.py" "$D"
