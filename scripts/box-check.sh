#!/usr/bin/env bash
# box-check.sh — run the LIVE tier (Linux build + C units + synthetic + protocol
# drift + real-PostgreSQL capture) on an x86 Linux box over ssh. Nothing eBPF
# runs on the Mac; this is the definition of done for any capture change.
#
#   make box-check                 # default box ($PGWT_BOX), all PG versions
#   make box-check PG=13           # one PG major (--pg-version)
#   make box-check OS=el8          # $PGWT_BOX_EL8 (Rocky 8 / kernel 4.18)
#
# Env:
#   PGWT_BOX          ssh target for the default (Ubuntu) box, e.g. root@1.2.3.4
#   PGWT_BOX_EL8 / PGWT_BOX_EL9 / PGWT_BOX_UBUNTU   per-OS targets
#   PGWT_BOX_DIR      remote checkout root (default ~/pgwt-check)
#
# Each run gets its own remote directory named after the local branch, and a
# box-wide flock serialises concurrent runs (timing tests must not overlap).
# The full log lands in tests/results/box-check-<os>-<ts>.log; the last lines
# (run_all.sh summary) are echoed so an agent can paste them into a PR.
set -uo pipefail
cd "$(dirname "$0")/.."

OS="${OS:-ubuntu}"
PG="${PG:-}"
case "$OS" in
    ubuntu) target="${PGWT_BOX_UBUNTU:-${PGWT_BOX:-}}" ;;
    el8)    target="${PGWT_BOX_EL8:-}" ;;
    el9)    target="${PGWT_BOX_EL9:-}" ;;
    *) echo "OS must be ubuntu|el8|el9"; exit 2 ;;
esac
if [[ -z "$target" ]]; then
    echo "no box for OS=$OS: set PGWT_BOX (ubuntu) or PGWT_BOX_EL8/EL9 to an ssh target." >&2
    echo "Provisioning: tests/provision-runner.sh (step 4 of the dev-loop plan) / tests/hetzner-vm.sh" >&2
    exit 2
fi

branch=$(git rev-parse --abbrev-ref HEAD | tr '/' '_')
remote_root="${PGWT_BOX_DIR:-pgwt-check}"
remote_dir="$remote_root/$branch"
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p tests/results
log="tests/results/box-check-$OS-$ts.log"

echo "box-check: $target  OS=$OS PG=${PG:-all}  -> $remote_dir  (log: $log)"
ssh -o BatchMode=yes "$target" "mkdir -p '$remote_dir'" || exit 1
rsync -az --delete \
    --exclude .git --exclude build --exclude 'pgwt-server*' --exclude 'pg_wait_tracer*' \
    --exclude 'tests/results' --exclude 'web/pgwt' --exclude '__pycache__' \
    --exclude '.pgwt-check.stamp' \
    ./ "$target:$remote_dir/" || exit 1

pgarg=""; [[ -n "$PG" ]] && pgarg="--pg-version $PG"
# shellcheck disable=SC2029
ssh -o BatchMode=yes "$target" \
    "cd '$remote_dir' && flock /tmp/pgwt-box-check.lock bash -c 'make -j\$(nproc) && make -C tests && sudo tests/run_all.sh --require-live $pgarg'" \
    2>&1 | tee "$log"
rc=${PIPESTATUS[0]}

echo
echo "box-check ($OS, PG=${PG:-all}) exit=$rc — summary:"
tail -n 25 "$log" | sed 's/^/  /'
exit "$rc"
