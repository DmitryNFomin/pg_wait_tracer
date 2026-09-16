#!/usr/bin/env bash
# provision-runner.sh — idempotent provisioning for a pg_wait_tracer gate/CI
# box. Runs ON the box, as root.
#
#   tests/provision-runner.sh ubuntu
#   tests/provision-runner.sh el8    # stub — step 4 of docs/DEV_LOOP_PLAN.md
#   tests/provision-runner.sh el9    # stub — step 4 of docs/DEV_LOOP_PLAN.md
#
# ubuntu installs everything `make box-check` needs for the LIVE tier
# (tests/run_all.sh --require-live): build deps for the daemon + pgwt-server
# (same recipe as .github/workflows/ci.yml / nightly.yml — the known-good
# apt + bpftool-fallback steps), the PGDG repo, and PostgreSQL 13/16/17/18,
# one cluster each, on ports 5413/5416/5417/5418 (pg_stat_statements
# preloaded, compute_query_id on for 14+). It deliberately does NOT install
# node/Playwright/Pillow/numpy: those back the Deterministic tier, which
# `make check` already runs natively on the Mac (see CLAUDE.md's test-tier
# table) — tests/run_all.sh degrades those sections to a plain (non-live)
# skip when the interpreter/browser deps are absent and $CI is unset, so
# --require-live still passes cleanly without them.
#
# Idempotent: every step is guarded so a second run is a fast no-op modulo
# apt/PGDG metadata refresh.
set -uo pipefail

OS="${1:-}"
if [[ -z "$OS" ]]; then
    echo "Usage: $0 <ubuntu|el8|el9>" >&2
    exit 2
fi

if [[ $(id -u) -ne 0 ]]; then
    echo "provision-runner.sh must run as root" >&2
    exit 1
fi

case "$OS" in
    el8)
        echo "STUB: el8 provisioning is not implemented yet — it is step 4" >&2
        echo "of docs/DEV_LOOP_PLAN.md (ephemeral VMs + real OS matrix)." >&2
        exit 1
        ;;
    el9)
        echo "STUB: el9 provisioning is not implemented yet — it is step 4" >&2
        echo "of docs/DEV_LOOP_PLAN.md (ephemeral VMs + real OS matrix)." >&2
        exit 1
        ;;
    ubuntu)
        ;;
    *)
        echo "Unknown OS: $OS (expected ubuntu|el8|el9)" >&2
        exit 2
        ;;
esac

log() { echo "[provision-runner] $*"; }

export DEBIAN_FRONTEND=noninteractive

# ---------------------------------------------------------------------------
# 1. Build dependencies for the daemon + pgwt-server (ci.yml "Install build
#    dependencies"), plus rsync/git/python3/procps for box-check + run_all.sh.
# ---------------------------------------------------------------------------
log "apt-get update"
apt-get update -qq

log "installing build + runtime dependencies"
apt-get install -y -qq \
    clang llvm libbpf-dev libelf-dev zlib1g-dev liblz4-dev \
    linux-tools-common gcc make git \
    rsync python3 procps findutils diffutils util-linux \
    sudo curl gnupg lsb-release ca-certificates \
    postgresql-common >/dev/null

# perf: try the exact-kernel package first (perf's ABI is kernel-version
# pinned), fall back to -generic. Neither failing is fatal — capture-smoke's
# hardware-watchpoint probe degrades loudly without perf; the gate box is
# expected to have it, so a missing perf is reported, not silently ignored.
log "installing perf (linux-tools for $(uname -r))"
apt-get install -y -qq "linux-tools-$(uname -r)" 2>/dev/null \
    || apt-get install -y -qq linux-tools-generic 2>/dev/null \
    || log "WARNING: no linux-tools package matched — perf may be unavailable"

# ---------------------------------------------------------------------------
# 2. bpftool: use it if a working one is already on PATH (same probe ci.yml
#    uses); otherwise build the pinned version from source and install it to
#    /usr/local/bin so bare `bpftool` (box-check.sh does not set $BPFTOOL)
#    resolves to a working binary.
# ---------------------------------------------------------------------------
BPFTOOL_VERSION="v7.5.0"
if bpftool version >/dev/null 2>&1; then
    log "system bpftool OK ($(command -v bpftool))"
elif [[ -x /usr/local/bin/bpftool ]] && /usr/local/bin/bpftool version >/dev/null 2>&1; then
    log "previously built bpftool OK (/usr/local/bin/bpftool)"
else
    log "building bpftool $BPFTOOL_VERSION from source"
    rm -rf /tmp/bpftool
    git clone --depth 1 --branch "$BPFTOOL_VERSION" --recurse-submodules \
        https://github.com/libbpf/bpftool.git /tmp/bpftool
    make -C /tmp/bpftool/src -j"$(nproc)"
    install -m 0755 /tmp/bpftool/src/bpftool /usr/local/bin/bpftool
    rm -rf /tmp/bpftool
fi
bpftool version >/dev/null || { echo "bpftool still not working" >&2; exit 1; }
test -r /sys/kernel/btf/vmlinux || {
    echo "FATAL: /sys/kernel/btf/vmlinux not readable — BPF build needs BTF" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# 3. PGDG apt repo.
# ---------------------------------------------------------------------------
if [[ ! -f /etc/apt/sources.list.d/pgdg.list ]]; then
    log "adding PGDG apt repo"
    apt-get install -y -qq postgresql-common >/dev/null
    yes | /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y >/dev/null
else
    log "PGDG apt repo already present"
fi
apt-get update -qq

# ---------------------------------------------------------------------------
# 4. PostgreSQL 13/16/17/18, one cluster each, on port 54<major>.
#    pg_stat_statements preloaded everywhere (PG13 query attribution has no
#    in-core query_id; PG14+ also gets compute_query_id=on for st_query_id).
#    PG13 is EOL and may have left the live PGDG repo — same apt-archive
#    fallback ci.yml/nightly.yml use.
# ---------------------------------------------------------------------------
for V in 13 16 17 18; do
    PORT=$((5400 + V))
    log "=== PostgreSQL $V (port $PORT) ==="

    if ! dpkg -s "postgresql-$V" >/dev/null 2>&1; then
        if ! apt-cache show "postgresql-$V" >/dev/null 2>&1; then
            log "postgresql-$V not in the live PGDG repo — adding apt-archive"
            echo "deb https://apt-archive.postgresql.org/pub/repos/apt $(lsb_release -cs)-pgdg main" \
                > /etc/apt/sources.list.d/pgdg-archive.list
            apt-get update -qq
        fi
        log "installing postgresql-$V postgresql-contrib-$V"
        apt-get install -y -qq "postgresql-$V" "postgresql-contrib-$V" >/dev/null
    else
        log "postgresql-$V already installed"
    fi

    if ! pg_lsclusters -h | awk -v v="$V" '$1 == v && $2 == "main"' | grep -q .; then
        log "creating cluster $V/main"
        pg_createcluster "$V" main
    fi

    pg_conftool "$V" main set port "$PORT"
    pg_conftool "$V" main set shared_preload_libraries pg_stat_statements
    if [[ "$V" -ge 14 ]]; then
        pg_conftool "$V" main set compute_query_id on
    fi
    # Trust local auth: tests connect as "psql -U postgres" from root.
    sed -i -E 's/(peer|scram-sha-256|md5)$/trust/' \
        "/etc/postgresql/$V/main/pg_hba.conf"

    # (Re)start so port/preload/hba changes take effect. pg_ctlcluster start
    # on an already-running cluster is a no-op with the OLD config, so
    # restart unconditionally — cheap, and this script is not on any hot
    # path.
    if pg_lsclusters -h | awk -v v="$V" '$1 == v && $2 == "main" && $4 == "online"' | grep -q .; then
        pg_ctlcluster "$V" main restart
    else
        pg_ctlcluster "$V" main start
    fi

    for _ in $(seq 1 30); do
        pg_isready -p "$PORT" -h /var/run/postgresql >/dev/null 2>&1 && break
        sleep 1
    done

    sudo -u postgres psql -p "$PORT" -d postgres -c \
        "CREATE EXTENSION IF NOT EXISTS pg_stat_statements" >/dev/null

    # Several live tests (test_accuracy.py, test_query_event.py,
    # test_cross_validate.py) document "Requires: ... pgbench initialized"
    # but do not initialize it themselves — the same convention the old
    # tests/cloud-init-rocky9-pg18.yaml used (`pgbench -i -s 10 postgres`).
    # Idempotent: only initializes when pgbench_accounts is missing/empty.
    ROWS=$(sudo -u postgres psql -p "$PORT" -tAc \
        "SELECT count(*) FROM pgbench_accounts" 2>/dev/null || echo 0)
    if [[ "${ROWS:-0}" -lt 1 ]]; then
        log "initializing pgbench tables (scale 10) on port $PORT"
        sudo -u postgres pgbench -p "$PORT" -i -s 10 -q postgres >/dev/null
    else
        log "pgbench tables already present on port $PORT ($ROWS rows)"
    fi

    log "PG $V: $(sudo -u postgres psql -p "$PORT" -tAc 'SELECT version()')"
done

log "clusters:"
pg_lsclusters

log "provisioning complete (ubuntu)"
