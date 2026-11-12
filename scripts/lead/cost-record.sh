#!/usr/bin/env bash
# Draft a PR cost record from wrapper rounds; posting is explicitly opt-in.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
post=0
post_file=
pr=
while [[ "$#" -gt 0 ]]; do
    arg=$1
    shift
    case "$arg" in
        --post)
            [[ "$post" -eq 0 && "$#" -gt 0 ]] || {
                echo 'usage: cost-record.sh <pr> [--post <file>]' >&2; exit 2;
            }
            post=1
            post_file=$1
            shift ;;
        *)
            if [[ -n "$pr" || ! "$arg" =~ ^[1-9][0-9]*$ ]]; then
                echo 'usage: cost-record.sh <pr> [--post <file>]' >&2
                exit 2
            fi
            pr=$arg ;;
    esac
done
[[ -n "$pr" ]] || { echo 'usage: cost-record.sh <pr> [--post <file>]' >&2; exit 2; }
draft=$(mktemp "${TMPDIR:-/tmp}/pgwt-cost-record.XXXXXX")
trap 'rm -f "$draft"' EXIT
if [[ "$post" -eq 1 ]]; then
    cat -- "$post_file" > "$draft"
    if grep -Fq '[FILL IN' "$draft"; then
        echo 'cost-record: complete every [FILL IN placeholder before posting' >&2
        exit 1
    fi
    cat "$draft"
    gh pr comment "$pr" --body-file "$draft"
    exit
fi
head=$(gh pr view "$pr" --json headRefName --jq .headRefName)
python3 - "${PGWT_LEAD_STATE_DIR:-$here}/costs.tsv" "$head" "$pr" > "$draft" <<'PY'
import csv
from pathlib import Path
import sys

path, head, pr = sys.argv[1:]
branch = head.removeprefix('agent/')
aliases = {branch, head, head.replace('%', '%25').replace('/', '%2F')}
try:
    with Path(path).open(newline='') as stream:
        rows = [row for row in csv.reader(stream, delimiter='\t')
                if len(row) > 1 and row[1] in aliases]
    if not rows:
        raise ValueError(f'no cost rows for branch {branch}')
    if any(len(row) != 8 or not (row[4].isdigit() or row[4] == 'unknown') for row in rows):
        raise ValueError(f'invalid cost row for branch {branch}')
except (OSError, ValueError) as exc:
    sys.exit(f'cost-record: {exc}')

print(f'Cost record: PR #{pr} ({branch})\n')
for number, row in enumerate(rows, 1):
    print(f'- Codex round {number}: {row[4]} tokens; {row[7]} (ended {row[0]}).')
total = sum(int(row[4]) for row in rows if row[4] != 'unknown')
unknown = sum(row[4] == 'unknown' for row in rows)
if unknown:
    print(f'\nCodex total: unknown (known subtotal: {total} tokens; {unknown} rounds unknown).')
else:
    print(f'\nCodex total: {total} tokens ({len(rows)} rounds).')
print('\n- Reviewer tokens: [FILL IN reviewer, rounds, tokens, verdict]')
print('- Adviser tokens: [FILL IN advisers and tokens, or none]')
print('- Gate-box hours: [FILL IN hours]')
PY
cat "$draft"
