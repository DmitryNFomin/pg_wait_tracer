#!/usr/bin/env bash
# Deterministic lead checks. State/ready.txt belongs to the lead, not this script.
set -u
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd) || {
  echo 'NEEDS-LEAD tick-error cannot locate tick.sh'; exit 10;
}
command -v python3 >/dev/null 2>&1 || {
  echo 'NEEDS-LEAD tick-error python3 unavailable'; exit 10;
}
exec python3 -B "$here/tick.py" "$@"
