#!/usr/bin/env bash
# Shared remote phase for make box-check and CI. The caller holds flock 9
# on /tmp/pgwt-box-check.lock for this ENTIRE script, including live UI smoke
# (registered in run_all.sh). Source manifest must describe this checkout.
set -euo pipefail
tree_hash="${1:?usage: box-tier.sh <tree-hash> <commit> [run_all arguments]}"
commit="${2:?usage: box-tier.sh <tree-hash> <commit> [run_all arguments]}"
shift 2

# Check capability rather than a kernel-specific package name: provisioning
# can supply either the distro bpftool or a source-built fallback.
if ! bpftool version >/dev/null 2>&1 || [ ! -r /sys/kernel/btf/vmlinux ]; then
    echo "gate box needs re-provisioning after a kernel change: run tests/provision-runner.sh ${OS:-ubuntu} on the box" >&2
    exit 2
fi
bash scripts/box-build.sh "$tree_hash" "$commit"

# sudo's env_reset must not drop the box-check port/phase overrides. Keep
# unset variables unset; CI_CELL lets cancellation cleanup target this run.
test_env=()
for name in PGWT_EXPECT_PHASE_PROBES PGWT_TEST_FORCE_NO_PHASE_PROBES \
            PGWT_PGPORT PGWT_PG_PORT_BASE PGWT_CI_CELL; do
    if [[ -n "${!name:-}" ]]; then
        test_env+=("$name=${!name}")
    fi
done
sudo env "${test_env[@]}" tests/run_all.sh --require-live "$@"
