#!/usr/bin/env bash
# provision-runner.sh — idempotent provisioning for a pg_wait_tracer gate/CI
# box. Runs ON the box, as root.
#
#   tests/provision-runner.sh ubuntu
#   tests/provision-runner.sh el8    # stub — step 4 of docs/DEV_LOOP_PLAN.md
#   tests/provision-runner.sh el9    # stub — step 4 of docs/DEV_LOOP_PLAN.md
#
#   tests/provision-runner.sh ubuntu --runner-token TOKEN
#     Also installs + registers the persistent GitHub Actions self-hosted
#     runner ("GitHub Actions runner" block near the bottom of this script).
#     TOKEN can also be given via PGWT_RUNNER_TOKEN instead of the flag (mint
#     it right before calling this script — see that block's own comment for
#     the exact command; never echo/log/commit it).
#
#   tests/provision-runner.sh ubuntu --runner-token TOKEN --runner-name pgwt-gate-2
#     --runner-name (default: pgwt-gate) names the registered runner —
#     needed when provisioning a SIBLING gate box (a second persistent
#     machine, same labels, so CI's `[self-hosted, gate-box]` targeting sees
#     it as just another box in the pool): GitHub runner names must be
#     unique per repo, so a second box registering as "pgwt-gate" again
#     would collide with the first. Labels are deliberately NOT
#     parameterized here — every gate box, however many there are, carries
#     the exact same `self-hosted,linux,x64,gate-box` set so CI never has to
#     know how many boxes are in the pool.
#
# ubuntu installs everything `make box-check` needs for the LIVE tier
# (tests/run_all.sh --require-live): build deps for the daemon + pgwt-server
# (same recipe as .github/workflows/ci.yml / nightly.yml — the known-good
# apt + bpftool-fallback steps), the PGDG repo, PostgreSQL 13/16/17/18 (one
# cluster each, on ports 5413/5416/5417/5418, pg_stat_statements preloaded,
# compute_query_id on for 14+), Go (to build web/pgwt), and Playwright +
# Chromium + Pillow + numpy for root's python3 (issue #93's live UI smoke,
# tests/ui_live_smoke.sh, is a LIVE-tier test and needs a real browser on
# the box). It deliberately does NOT install node: nothing in the LIVE tier
# needs it (the Node builder-unit tests are Deterministic-tier, Mac-only —
# see CLAUDE.md's test-tier table); tests/run_all.sh's Step 5 (the
# Playwright UI suite against mock_server.py, redundant with `make check`
# and ci.yml's own web-ui job) additionally stays opt-in
# (PGWT_RUN_WEB_UI=1) even though the deps are now present, so a plain
# `make box-check` does not also carry a full Chromium walk on the shared
# box for every run.
#
# Idempotent: every step is guarded so a second run is a fast no-op modulo
# apt/PGDG metadata refresh. MUST be re-run after any kernel change (a
# reboot onto a new kernel, an apt/unattended upgrade that pulls one in,
# etc.): bpftool's package (and its BTF-dependent vmlinux.h generation
# step) is tied to the exact running `uname -r`, and a stale
# linux-tools-<old-kernel> package makes `make` fail with an opaque
# "bpftool not found for kernel <new>" error -- scripts/box-check.sh's own
# preflight check now catches this and tells you to come back here.
#
# -e: fail loudly. A failed apt-get/pg_createcluster/pg_conftool/CREATE
# EXTENSION/pgbench step must stop the script, not let it print
# "provisioning complete" over a half-provisioned box. Every command below
# that is allowed to fail (an optional/best-effort step, or one whose
# failure this script itself handles) has its own explicit `|| true` or
# `||` fallback guard -- see the Playwright/Chromium/Go and ssh
# self-trust blocks below.
#
# Takes the same lock scripts/box-check.sh uses (see the flock block below):
# every run unconditionally restarts all four clusters (pg_ctlcluster ...
# restart), which would otherwise race with an in-flight box-check's live
# tests from another agent's invocation.
set -euo pipefail

OS="${1:-}"
shift || true

# --runner-token / PGWT_RUNNER_TOKEN: optional, only consumed by the "GitHub
# Actions runner" block near the bottom of this script (OS=ubuntu only).
RUNNER_TOKEN="${PGWT_RUNNER_TOKEN:-}"
# --runner-name / PGWT_RUNNER_NAME: optional, defaults to "pgwt-gate" (the
# original box's name) below — see the "GitHub Actions runner" block.
RUNNER_NAME="${PGWT_RUNNER_NAME:-}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --runner-token)
            RUNNER_TOKEN="${2:-}"
            shift 2
            ;;
        --runner-token=*)
            RUNNER_TOKEN="${1#*=}"
            shift
            ;;
        --runner-name)
            RUNNER_NAME="${2:-}"
            shift 2
            ;;
        --runner-name=*)
            RUNNER_NAME="${1#*=}"
            shift
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done
RUNNER_NAME="${RUNNER_NAME:-pgwt-gate}"

if [[ -z "$OS" ]]; then
    echo "Usage: $0 <ubuntu|el8|el9> [--runner-token TOKEN] [--runner-name NAME]" >&2
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

# The lock file must be writable by the gate-box GitHub Actions runner's
# unprivileged 'runner' user too (ci.yml's gate-box jobs each wrap their own
# build/test steps in this same flock; the runner is registered by section 7
# below) -- create it 0666
# up front so a non-root `flock 9` doesn't fail with "Permission denied"
# against a file this script (or scripts/box-check.sh) previously created
# 0644 as root. Real bug found running ci.yml against this box for the
# first time: `exec 9>/tmp/pgwt-box-check.lock` as the unprivileged runner
# user failed outright on the root-owned, 0644 file.
touch /tmp/pgwt-box-check.lock
chmod 0666 /tmp/pgwt-box-check.lock

# Same lock make box-check (scripts/box-check.sh) uses: the cluster restarts
# below must not race with an in-flight box-check's live tests. Bounded wait
# (not a bare `flock 9`, which blocks forever): a nested/concurrent call
# fails loudly with a clear message instead of hanging silently until
# whatever holds the lock finishes (a full box-check run can itself take
# several minutes per PG version).
log "waiting for /tmp/pgwt-box-check.lock (up to 600s)"
exec 9>/tmp/pgwt-box-check.lock
if ! flock -w 600 9; then
    echo "FATAL: could not acquire /tmp/pgwt-box-check.lock within 600s -- another box-check or provisioning run holds it" >&2
    exit 1
fi
log "lock acquired"

# ---------------------------------------------------------------------------
# 0. Disable unattended OS upgrades. A CI gate box must NEVER change kernel
#    or libc -- or bounce a running service -- out from under a live test:
#    Ubuntu's unattended-upgrades fired mid `make box-check` run, installed
#    a new kernel, and needrestart auto-restarted all four PostgreSQL
#    clusters, producing empty captures (2026-09-17). Runs FIRST, before any
#    apt-get call, so a freshly rebooted box has the smallest possible
#    window where the stock timers could fire again. Idempotent: `disable`/
#    `mask` are no-ops on an already-masked unit; the needrestart conf.d
#    snippet is (re)written every run, not appended. `|| true` on both: under
#    `set -e`, a second run (units already masked) or an image that never
#    shipped unattended-upgrades.service at all makes systemctl exit
#    non-zero, which would otherwise abort provisioning at step one --
#    exactly the idempotence this comment promises.
# ---------------------------------------------------------------------------
log "disabling + masking unattended-upgrade timers/service"
systemctl disable --now apt-daily.timer apt-daily-upgrade.timer unattended-upgrades.service || true
systemctl mask apt-daily.timer apt-daily-upgrade.timer unattended-upgrades.service || true

log "setting needrestart to list-only mode (never auto-restart services)"
mkdir -p /etc/needrestart/conf.d
cat > /etc/needrestart/conf.d/99-pgwt-gate-box.conf <<'EOF'
# pg_wait_tracer gate box: list-only, never auto-restart services.
# needrestart auto-restarting PostgreSQL mid-capture (after an
# unattended-upgrades kernel bump) produced empty test traces (2026-09-17).
$nrconf{restart} = 'l';
EOF

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
# The installed apt.postgresql.org.sh (Ubuntu 24.04 / Debian's postgresql-
# common) writes the deb822-format /etc/apt/sources.list.d/pgdg.sources, not
# the legacy pgdg.list it actively deletes -- checking for pgdg.list here
# (found while proving idempotence this round) always missed, so this
# "idempotent" step actually re-ran the script's apt-get update + repo
# rewrite on EVERY provisioning run rather than skipping when already done.
# Harmless in effect, but not what the comment/idempotence design claimed.
if [[ ! -f /etc/apt/sources.list.d/pgdg.sources ]]; then
    log "adding PGDG apt repo"
    apt-get install -y -qq postgresql-common >/dev/null
    # `-y` already makes the upstream script non-interactive; `yes` feeding
    # its stdin is belt-and-suspenders, but under pipefail (added this
    # round) that pipe's `yes` side gets a real SIGPIPE (rc 141) the moment
    # the script stops reading stdin, which made pipefail fail this whole
    # line even though the script itself succeeded. `|| true` swallows
    # that, then the actual result is checked explicitly below, same
    # idempotent-verification style as every other step in this script --
    # a genuine failure (repo file never appears) is still caught, loudly.
    yes | /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y >/dev/null || true
    [[ -f /etc/apt/sources.list.d/pgdg.sources ]] || {
        echo "FATAL: PGDG apt repo setup did not produce /etc/apt/sources.list.d/pgdg.sources" >&2
        exit 1
    }
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

    CLUSTER_LINE=$(pg_lsclusters -h | awk -v v="$V" '$1 == v && $2 == "main"' || true)
    if [[ -z "$CLUSTER_LINE" ]]; then
        log "creating cluster $V/main"
        pg_createcluster "$V" main
    fi

    pg_conftool "$V" main set port "$PORT"
    pg_conftool "$V" main set shared_preload_libraries pg_stat_statements
    if [[ "$V" -ge 14 ]]; then
        pg_conftool "$V" main set compute_query_id on
    fi
    # Trust local auth: tests connect as "psql -U postgres" from root.
    sed -i -E 's/(ident|peer|scram-sha-256|md5)$/trust/' \
        "/etc/postgresql/$V/main/pg_hba.conf"

    # (Re)start so port/preload/hba changes take effect. pg_ctlcluster start
    # on an already-running cluster is a no-op with the OLD config, so
    # restart unconditionally — cheap, and this script is not on any hot
    # path.
    CLUSTER_ONLINE=$(pg_lsclusters -h | awk -v v="$V" '$1 == v && $2 == "main" && $4 == "online"' || true)
    if [[ -n "$CLUSTER_ONLINE" ]]; then
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
    # Idempotent: only (re)initializes when pgbench_accounts is missing,
    # empty, or below scale-10's 1,000,000-row expectation. Issue #129: a
    # test that ran `pgbench -i -s 1` directly against this box's "postgres"
    # database silently shrank the dataset to scale 1 on every CI run,
    # breaking test_query_event.py Test 2's large-table (138 MB) scan
    # assumption; that hazard is now fixed at its source (the test uses its
    # own scratch database), but this check also repairs an already-shrunk
    # box on the next provisioning run instead of requiring a manual fix.
    SCALE=10
    EXPECTED_ROWS=$((SCALE * 100000))
    ROWS=$(sudo -u postgres psql -p "$PORT" -tAc \
        "SELECT count(*) FROM pgbench_accounts" 2>/dev/null || echo 0)
    if [[ "${ROWS:-0}" -lt "$EXPECTED_ROWS" ]]; then
        log "pgbench tables missing/at a smaller scale on port $PORT" \
            "(found ${ROWS:-0} rows, want $EXPECTED_ROWS) --" \
            "(re)initializing at scale $SCALE"
        sudo -u postgres pgbench -p "$PORT" -i -s "$SCALE" -q postgres >/dev/null
    else
        log "pgbench tables already present on port $PORT ($ROWS rows)"
    fi

    log "PG $V: $(sudo -u postgres psql -p "$PORT" -tAc 'SELECT version()')"
done

log "clusters:"
pg_lsclusters

# ---------------------------------------------------------------------------
# 5. issue #93 (live UI smoke): Go (to build web/pgwt, the LIVE tier's ONLY
#    consumer of the Go bridge -- the Deterministic tier's `make check` runs
#    web/'s `go vet`/`go test` on the Mac, but nothing on this box needed an
#    actual `web/pgwt` binary until this test), Playwright + Chromium for
#    root's python3, and a self-trust ssh loop so the Go bridge can `ssh
#    root@localhost pgwt-server ...` exactly like a real deployment
#    (web/bridge.go's NewSSHBridge runs ssh with -o BatchMode=yes, which
#    REFUSES rather than prompts on an unknown host key or missing key auth
#    -- both must already be in place before tests/ui_live_smoke.sh runs).
# ---------------------------------------------------------------------------
if ! command -v go >/dev/null 2>&1; then
    log "installing Go (golang-go, needs go.mod's 1.21+)"
    apt-get install -y -qq golang-go >/dev/null
fi
log "go: $(go version)"

log "installing python3-pip"
apt-get install -y -qq python3-pip >/dev/null

log "installing Playwright + websockets + Pillow + numpy for root's python3"
# --break-system-packages: Ubuntu 24.04's python3 is PEP-668
# externally-managed; root installing system-wide for its own test runs
# (never --user -- LIVE_TESTS run under sudo, i.e. as root) is the
# equivalent of the Mac setup's `pip install --user` for a single-user box.
# pillow + numpy: tests/ui_live_smoke_lib.py decodes/diffs the tick
# screenshots with them (same pair CLAUDE.md's Mac "Local setup" lists).
python3 -m pip install --break-system-packages -q \
    playwright==1.60.0 websockets pillow numpy

log "installing Chromium + its OS dependencies"
# --with-deps: also apt-get installs the shared libraries (fonts, libnss,
# etc.) headless Chromium needs, which a minimal server image lacks.
python3 -m playwright install --with-deps chromium >/dev/null

log "setting up root's localhost ssh self-trust loop"
SSH_KEY="$HOME/.ssh/id_ed25519"
# mkdir BEFORE ssh-keygen: on a freshly rebooted/never-provisioned box
# ~/.ssh does not exist yet, and ssh-keygen -f into a missing parent
# directory fails outright.
mkdir -p "$HOME/.ssh"
chmod 700 "$HOME/.ssh"
if [[ ! -f "$SSH_KEY" ]]; then
    log "generating root's ssh keypair"
    ssh-keygen -t ed25519 -N '' -f "$SSH_KEY" -C "pgwt-gate-box-self" >/dev/null
fi
touch "$HOME/.ssh/authorized_keys"
if ! grep -qxF "$(cat "${SSH_KEY}.pub")" "$HOME/.ssh/authorized_keys" 2>/dev/null; then
    log "adding root's own pubkey to authorized_keys"
    cat "${SSH_KEY}.pub" >> "$HOME/.ssh/authorized_keys"
fi
chmod 600 "$HOME/.ssh/authorized_keys"
touch "$HOME/.ssh/known_hosts"
# issue #141 (ephemeral VMs from a gate-box snapshot): a real failure found
# running this against a fresh VM booted from the pgwt=gate-snapshot image
# -- the snapshot's disk carries the ORIGINAL gate box's known_hosts entry
# for localhost/127.0.0.1, but cloud-init on the new instance (a different
# Hetzner server id) regenerates sshd's own host keys on first boot
# (ssh_deletekeys, the standard "don't reuse the imaged host's keys"
# behaviour), so that inherited known_hosts entry no longer matches this
# VM's actual, freshly-generated host key. The old "add only if entirely
# missing" check below left that STALE, now-mismatched entry in place, and
# `ssh -o BatchMode=yes` then refuses to connect ("REMOTE HOST
# IDENTIFICATION HAS CHANGED"), failing this whole (otherwise idempotent,
# fast no-op) re-provisioning run. A persistent, never-reimaged box's host
# key never changes, so unconditionally purging + rescanning here is a
# harmless few extra milliseconds there, and the actual fix for a
# snapshot-booted VM.
for h in localhost 127.0.0.1; do
    # Scan into a temp file FIRST and only remove+replace the existing
    # known_hosts entry (via ssh-keygen -R) once the scan actually produced
    # something. Scanning straight into known_hosts after an unconditional
    # -R (the previous version) would, on a hiccup (sshd not answering
    # yet), leave known_hosts with NO entry at all for $h -- strictly worse
    # than the stale-but-present entry it started with. The BatchMode ssh
    # check right below this loop is still the real guard either way.
    tmp_scan=$(mktemp)
    if ssh-keyscan -H "$h" >"$tmp_scan" 2>/dev/null && [[ -s "$tmp_scan" ]]; then
        ssh-keygen -R "$h" -f "$HOME/.ssh/known_hosts" >/dev/null 2>&1 || true
        cat "$tmp_scan" >> "$HOME/.ssh/known_hosts"
        log "(re)scanned $h into known_hosts"
    else
        log "WARNING: ssh-keyscan $h produced nothing -- leaving any existing known_hosts entry for $h untouched"
    fi
    rm -f "$tmp_scan"
done
chmod 600 "$HOME/.ssh/known_hosts"

# Prove the loop actually works now, loudly, rather than have
# tests/ui_live_smoke.sh fail later with an opaque "bridge never answered
# /session".
if ssh -o BatchMode=yes -o ConnectTimeout=5 root@localhost true; then
    log "root@localhost ssh self-trust loop OK"
else
    echo "FATAL: ssh -o BatchMode=yes root@localhost failed after setup" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 6. Marker file: /etc/pgwt-gate-box. Presence (not content) tells
#    .github/workflows/ci.yml's gate-box jobs that they are running on this
#    already-provisioned, persistent box (build deps + bpftool + PostgreSQL
#    13/16/17/18 on ports 5413/5416/5417/5418 already present), so they can
#    skip their apt/PGDG/PG-install steps and use the existing cluster
#    directly (PGPORT=54<major>) instead of reinstalling on every run.
#    Written unconditionally at the end of a successful ubuntu provisioning
#    run, independent of whether this particular invocation also registered
#    a runner (section 7) — a box can be (re)provisioned without touching an
#    already-registered runner.
# ---------------------------------------------------------------------------
log "writing /etc/pgwt-gate-box marker"
cat > /etc/pgwt-gate-box <<EOF
# Written by tests/provision-runner.sh. Marks this host as pg_wait_tracer's
# persistent gate box (docs/DEV_LOOP_PLAN.md step 1): fully provisioned with
# build deps, bpftool, and PostgreSQL 13/16/17/18 on ports
# 5413/5416/5417/5418. .github/workflows/ci.yml checks only for this file's
# presence, not its content.
provisioned_at=$(date -u +%FT%TZ)
os=ubuntu
EOF

# ---------------------------------------------------------------------------
# 7. GitHub Actions self-hosted runner — only when a registration token was
#    given (--runner-token TOKEN or PGWT_RUNNER_TOKEN). Mint the token
#    immediately before calling this script, and only pass it via this flag
#    or env var — never echo it, never write it to a file in the repo, never
#    put it in a commit:
#      gh api -X POST repos/<owner>/<repo>/actions/runners/registration-token \
#        --jq .token
#    (it expires in ~1 hour).
#
#    - dedicated, unprivileged 'runner' system user with passwordless sudo
#      (the gate-box CI jobs run `sudo tests/ci_smoke.sh` /
#      `sudo tests/run_all.sh`, the same as an agent's `make box-check`).
#    - PERSISTENT: no --ephemeral. This is the one gate box, registered once
#      and reused by every job — not a throwaway VM (that is step 4's
#      ephemeral-VM runner, a separate, --ephemeral registration).
#    - labels self-hosted,linux,x64,gate-box ; name $RUNNER_NAME (default
#      pgwt-gate, override with --runner-name/PGWT_RUNNER_NAME for a
#      sibling box).
#    - one job at a time: config.sh's default (no extra flag needed) — the
#      box is shared with agents running `make box-check`, and every timing
#      test step already serialises on /tmp/pgwt-box-check.lock, but a
#      second concurrent *job* would still contend for the same CPU/PG
#      clusters outside that lock's reach.
#    - idempotent: with no token, this whole block is skipped and an
#      existing install/registration/service is left completely alone; with
#      a token, the binaries are (re)installed if missing, but config.sh
#      (the actual GitHub registration) only runs if this box has never
#      registered before (no .runner file) — re-running config.sh against an
#      already-registered runner fails loudly asking to remove it first, so
#      re-registering is a deliberate, separate step, not something a bare
#      re-run of this script does.
# ---------------------------------------------------------------------------
if [[ "$OS" == "ubuntu" && -n "$RUNNER_TOKEN" ]]; then
    RUNNER_HOME=/opt/actions-runner
    RUNNER_VERSION=2.337.0
    RUNNER_TARBALL="actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz"
    # Pinned SHA256 of the release asset (from GitHub's release API "digest"
    # field for actions/runner v2.337.0), verified before extraction.
    RUNNER_SHA256="70920811a4f8ad4328818682bca5c6469c1c942fab52448868071d0063816613"
    RUNNER_REPO_URL="https://github.com/DmitryNFomin/pg_wait_tracer"

    log "=== GitHub Actions runner ($RUNNER_NAME) ==="

    if ! id runner >/dev/null 2>&1; then
        log "creating dedicated 'runner' system user"
        useradd --system --create-home --home-dir "$RUNNER_HOME" --shell /bin/bash runner
    fi
    mkdir -p "$RUNNER_HOME"
    chown runner:runner "$RUNNER_HOME"

    # Write-validate-install rather than write-then-validate-in-place: a
    # syntax error must never leave a broken (or half-written, if the
    # script died between `cat` and `visudo -cf`) file live in
    # /etc/sudoers.d -- validate a private temp copy first, only `install`
    # it once visudo has approved it.
    SUDOERS_TMP=$(mktemp)
    cat > "$SUDOERS_TMP" <<'EOF'
runner ALL=(ALL) NOPASSWD:ALL
EOF
    visudo -cf "$SUDOERS_TMP"
    install -m 0440 -o root -g root "$SUDOERS_TMP" /etc/sudoers.d/90-runner
    rm -f "$SUDOERS_TMP"

    if [[ ! -x "$RUNNER_HOME/config.sh" ]]; then
        log "installing actions-runner $RUNNER_VERSION binaries"
        curl -fsSL -o "/tmp/$RUNNER_TARBALL" \
            "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/${RUNNER_TARBALL}"
        echo "$RUNNER_SHA256  /tmp/$RUNNER_TARBALL" | sha256sum -c -
        sudo -u runner tar xzf "/tmp/$RUNNER_TARBALL" -C "$RUNNER_HOME"
        rm -f "/tmp/$RUNNER_TARBALL"
        "$RUNNER_HOME/bin/installdependencies.sh"
    else
        log "actions-runner binaries already installed"
    fi

    if [[ -f "$RUNNER_HOME/.runner" ]]; then
        log "runner already registered — leaving registration alone (remove $RUNNER_HOME/.runner and re-run with a fresh token to re-register)"
    else
        log "registering runner '$RUNNER_NAME' with GitHub (labels: self-hosted,linux,x64,gate-box)"
        # Note: GitHub's documented config.sh invocation takes --token on
        # the command line, so the registration token is briefly visible to
        # anyone who can read /proc/*/cmdline on this box while config.sh
        # runs (root and the 'runner' user itself, given the NOPASSWD sudo
        # above). This is GitHub's own documented mechanism, not something
        # this script can avoid; the token is single-use and expires in
        # ~1h (minted just before this script is invoked — see the comment
        # at the top of this block).
        sudo -u runner "$RUNNER_HOME/config.sh" \
            --unattended \
            --url "$RUNNER_REPO_URL" \
            --token "$RUNNER_TOKEN" \
            --name "$RUNNER_NAME" \
            --labels self-hosted,linux,x64,gate-box \
            --work _work
    fi

    # `|| true`: under set -e/pipefail, `systemctl list-unit-files` exits 1
    # (not 0-with-empty-output) when NOTHING matches the glob -- exactly the
    # first-ever registration on a fresh box, before the service has been
    # installed at all. Without this guard, that exit code propagates
    # through the pipeline (pipefail: the pipeline's status is the last
    # non-zero among all stages, even one that isn't rightmost) into this
    # command substitution and aborts the whole script right after a
    # successful `config.sh` registration -- reproduced provisioning
    # pgwt-gate-2 from scratch: registration succeeded but the script died
    # here, leaving a registered-but-not-running runner (never appears
    # online to GitHub) with no indication beyond a bare "exited with
    # code 1".
    RUNNER_UNIT=$(systemctl list-unit-files 'actions.runner.*.service' --no-legend 2>/dev/null | awk '{print $1}' | head -1) || true
    if [[ -n "$RUNNER_UNIT" ]]; then
        if systemctl is-active --quiet "$RUNNER_UNIT"; then
            log "runner systemd service ($RUNNER_UNIT) already installed and active"
        else
            log "runner systemd service ($RUNNER_UNIT) installed but inactive — starting it"
            systemctl start "$RUNNER_UNIT"
        fi
    else
        log "installing + starting the runner systemd service"
        (cd "$RUNNER_HOME" && ./svc.sh install runner && ./svc.sh start)
    fi
    systemctl --no-pager status 'actions.runner.*.service' 2>/dev/null || true
else
    log "no --runner-token / PGWT_RUNNER_TOKEN given — skipping GitHub Actions runner install/registration"
fi

log "provisioning complete (ubuntu)"
