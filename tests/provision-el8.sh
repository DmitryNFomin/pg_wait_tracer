#!/usr/bin/env bash
# Rocky Linux 8 implementation of provision-runner.sh. Run via that entry point.
# Keep the end state in sync with its Ubuntu recipe, including LIVE UI fixtures.
# No VM creation, kernel replacement, daemon build, or test-suite invocation here.
# Cheap local structural checks (no root/host/network required):
#   bash -n tests/provision-runner.sh
#   bash -n tests/provision-el8.sh
# Lint with: shellcheck tests/provision-el8.sh
# Ubuntu has no dry-run mode; these use the existing Bash script mechanism.
set -Eeuo pipefail

STEP="host preflight"
log() { echo "[provision-runner/el8] $*"; }
die() { echo "FATAL: EL8 $STEP: $*" >&2; exit 1; }
# Do not print BASH_COMMAND: credentials passed in the environment must not leak.
on_error() {
    local rc=$?
    echo "FATAL: EL8 $STEP failed at line ${BASH_LINENO[0]} (exit $rc); provisioning incomplete" >&2
    exit "$rc"
}
trap on_error ERR
WORK_DIR=""
trap '[[ -z "$WORK_DIR" ]] || rm -rf -- "$WORK_DIR"' EXIT

[[ $(id -u) -eq 0 ]] || die "must run as root (sudo -H)"
[[ $# -eq 0 ]] || die "use tests/provision-runner.sh el8"
[[ -r /etc/os-release ]] || die "missing /etc/os-release; Rocky Linux 8.10 required"
# shellcheck disable=SC1091
source /etc/os-release
[[ "$ID" == rocky && "$VERSION_ID" == 8.10 ]] || die "expected Rocky Linux 8.10, found $ID $VERSION_ID"
[[ $(uname -m) == x86_64 ]] || die "box-check OS=el8 requires x86_64"
[[ $(uname -r) == 4.18.*.el8* ]] || die "expected the real EL8 4.18 kernel, found $(uname -r); boot the Rocky 8.10 kernel"
[[ -r /sys/kernel/btf/vmlinux ]] || die "kernel $(uname -r) has no readable /sys/kernel/btf/vmlinux; boot a BTF-enabled Rocky 8.10 kernel"
for cmd in dnf rpm systemctl flock getent; do
    command -v "$cmd" >/dev/null || die "missing host command: $cmd"
done
systemctl show --property=Version --value >/dev/null
ROOT_HOME=$(getent passwd root | cut -d: -f6)
[[ "$HOME" == "$ROOT_HOME" ]] || die "HOME must be root's $ROOT_HOME; invoke with sudo -H"
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

STEP="box-check lock (600s timeout)"
touch /tmp/pgwt-box-check.lock
chmod 0666 /tmp/pgwt-box-check.lock
exec 9>/tmp/pgwt-box-check.lock
flock -w 600 9 || die "could not acquire /tmp/pgwt-box-check.lock; another build/provisioner holds it"
WORK_DIR=$(mktemp -d /tmp/pgwt-provision-el8.XXXXXX)
# A failed re-provision must not leave the CI 'fully provisioned' marker live.
rm -f /etc/pgwt-gate-box

STEP="disable unattended DNF upgrades"
# Mask even absent units so a later package install cannot enable them. DNF's
# metadata-only makecache timer is harmless and is deliberately left alone.
for unit in dnf-automatic.timer dnf-automatic-download.timer \
    dnf-automatic-install.timer dnf-automatic-notifyonly.timer \
    dnf-automatic.service dnf-automatic-download.service \
    dnf-automatic-install.service dnf-automatic-notifyonly.service; do
    # Unlike Ubuntu's best-effort guard, a failure to stop/mask an existing
    # updater is fatal. Missing units need no stop/disable, but can be masked.
    load_state=$(systemctl show "$unit" --property=LoadState --value)
    if [[ "$load_state" != not-found && "$load_state" != masked ]]; then
        systemctl disable --now "$unit"
    elif [[ "$load_state" == masked ]]; then
        systemctl stop "$unit"
    fi
    systemctl mask "$unit"
    [[ $(systemctl is-enabled "$unit") == masked ]] || die "$unit was not masked"
    if systemctl is-active --quiet "$unit"; then die "$unit is still active"; fi
done

STEP="Rocky repositories: BaseOS, AppStream, PowerTools, EPEL"
# skip_if_unavailable=0 prevents DNF's usual successful partial metadata refresh.
dnf_required() { dnf -y --setopt='*.skip_if_unavailable=0' "$@"; }
dnf_required -y install dnf-plugins-core epel-release
dnf_required config-manager --set-enabled baseos appstream powertools epel
dnf_required --refresh makecache

BUILD_PACKAGES=(
    gcc clang llvm make tar git libbpf-devel elfutils-libelf-devel
    zlib-devel lz4-devel perf binutils elfutils
    rsync python3.11 python3.11-pip procps-ng findutils diffutils util-linux jq
    sudo curl gnupg2 ca-certificates which glibc-langpack-en
    openssh-clients openssh-server policycoreutils policycoreutils-python-utils golang
)
STEP="install build/runtime packages: ${BUILD_PACKAGES[*]}"
dnf_required -y install "${BUILD_PACKAGES[@]}"
for package in "${BUILD_PACKAGES[@]}"; do
    rpm -q "$package" >/dev/null || die "missing installed RPM: $package"
done
# EL8's /usr/bin/python3 is 3.6. Do not change it (DNF uses system Python).
ln -sfn /usr/bin/python3.11 /usr/local/bin/python3
# Rocky's sudo secure_path omits /usr/local/bin, hiding both this Python and
# the PG client links below from sudo tests/run_all.sh. Validate before install.
STEP="sudo PATH configuration (Python 3.11 and PostgreSQL clients)"
cat > "$WORK_DIR/sudoers" <<'EOF'
Defaults secure_path = /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
EOF
visudo -cf "$WORK_DIR/sudoers"
install -m 0440 "$WORK_DIR/sudoers" /etc/sudoers.d/90-pgwt-path
STEP="verify clang BPF backend, Python 3.11, Go >=1.21, perf"
printf 'int probe(void *ctx) { return 0; }\n' > "$WORK_DIR/probe.c"
clang -target bpf -O2 -g -c "$WORK_DIR/probe.c" -o "$WORK_DIR/probe.o"
llvm-strip --version >/dev/null
sudo python3 -c 'import sys; assert sys.version_info >= (3, 11), sys.version'
GO_VERSION=$(go env GOVERSION)
[[ "$GO_VERSION" =~ ^go([0-9]+)\.([0-9]+) ]] || die "cannot parse Go version: $GO_VERSION"
(( BASH_REMATCH[1] > 1 || BASH_REMATCH[2] >= 21 )) || die "Go >=1.21 required, found $GO_VERSION"
perf version

# Bare bpftool is required by box-check even though the EL8 Makefile always
# builds its own pinned static libbpf/bpftool. Reuse a working tool, try both
# distro package names, then use Ubuntu's source fallback for this host tool.
# This function is used in conditionals: explicitly propagate command failures
# because Bash suppresses errexit/ERR within a conditional function call.
bpftool_works() {
    bpftool version >/dev/null 2>&1 || return 1
    bpftool -j feature probe kernel > "$WORK_DIR/features.json" || return 1
    python3 - "$WORK_DIR/features.json" <<'PY'
import json
import sys
with open(sys.argv[1]) as f:
    features = json.load(f)
required = {
    "syscall_config": ["have_bpf_syscall"],
    "map_types": ["have_ringbuf_map_type"],
    "program_types": ["have_tracing_prog_type", "have_tracepoint_prog_type",
                      "have_perf_event_prog_type", "have_kprobe_prog_type"],
}
missing = [f"{group}.{name}" for group, names in required.items()
           for name in names if features.get(group, {}).get(name) is not True]
if missing:
    sys.exit("FATAL: EL8 kernel/bpftool lacks required features: " + ", ".join(missing)
             + "; boot a BTF/ringbuf-enabled Rocky 8.10 kernel; do not skip live tiers")
PY
}

STEP="host bpftool: version and required kernel features"
if ! bpftool_works; then
    for package in bpftool kernel-tools; do
        log "trying $package for host bpftool"
        if dnf_required -y install "$package"; then
            hash -r
            if bpftool_works; then break; fi
            log "$package did not provide a working bpftool; trying fallback"
        else
            log "$package installation failed; trying fallback"
        fi
    done
    if ! bpftool_works; then
        BPFTOOL_VERSION="v7.5.0"
        STEP="build host bpftool $BPFTOOL_VERSION from source"
        log "building host bpftool $BPFTOOL_VERSION from source"
        git clone --depth 1 --branch "$BPFTOOL_VERSION" --recurse-submodules \
            https://github.com/libbpf/bpftool.git "$WORK_DIR/bpftool"
        make -C "$WORK_DIR/bpftool/src" -j"$(nproc)"
        install -m 0755 "$WORK_DIR/bpftool/src/bpftool" /usr/local/bin/bpftool
        hash -r
    fi
fi
STEP="verify host bpftool and EL8 kernel BPF features (BTF, ringbuf, tracing, perf events, uprobes)"
bpftool_works || die "no working host bpftool with required kernel features after package/source fallback; provisioning incomplete"
bpftool version
log "host bpftool OK ($(command -v bpftool))"
# EL8 exposes scheduler event IDs through debugfs/tracing. Persist the mount
# for subsequent boots, and fail on missing events instead of sampling-only.
if ! mountpoint -q /sys/kernel/debug; then
    mount -t debugfs debugfs /sys/kernel/debug
fi
if ! awk '$1 !~ /^#/ && $2 == "/sys/kernel/debug" { found=1 } END { exit !found }' /etc/fstab; then
    echo 'debugfs /sys/kernel/debug debugfs defaults 0 0' >> /etc/fstab
fi
for event in sched_switch sched_process_fork sched_process_exit; do
    [[ -r "/sys/kernel/debug/tracing/events/sched/$event/id" ]] || die "missing sched/$event tracepoint ID under debugfs/tracing"
done
[[ -r /proc/self/schedstat ]] || die "missing /proc/self/schedstat (CONFIG_SCHED_INFO)"
STEP="EL8 hardware watchpoint availability"
cat > "$WORK_DIR/watchpoint.c" <<'EOF'
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
static volatile int target;
int main(void) {
    struct perf_event_attr attr = {0};
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_type = HW_BREAKPOINT_W;
    attr.bp_addr = (unsigned long)&target;
    attr.bp_len = HW_BREAKPOINT_LEN_4;
    attr.sample_period = 1;
    attr.wakeup_events = 1;
    int fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
    if (fd < 0) { perror("FATAL: perf_event_open hardware watchpoint (kernel/hypervisor permissions)"); return 1; }
    close(fd);
    return 0;
}
EOF
gcc -Wall -Werror "$WORK_DIR/watchpoint.c" -o "$WORK_DIR/watchpoint"
"$WORK_DIR/watchpoint"

STEP="disable distro PostgreSQL module before installing PGDG packages"
dnf_required -qy module disable postgresql
dnf_required -q module list --disabled postgresql > "$WORK_DIR/modules"
grep -Eq '^postgresql[[:space:]].*\[x\]' "$WORK_DIR/modules" || die "AppStream postgresql module is not disabled"
STEP="PGDG EL8 repository RPM"
dnf_required -y install https://download.postgresql.org/pub/repos/yum/reporpms/EL-8-x86_64/pgdg-redhat-repo-latest.noarch.rpm
[[ -r /etc/yum.repos.d/pgdg-redhat-all.repo ]] || die "PGDG RPM did not install pgdg-redhat-all.repo"
[[ -r /etc/pki/rpm-gpg/PGDG-RPM-GPG-KEY-RHEL ]] || die "PGDG RPM signing key missing"
STEP="PGDG PostgreSQL 13 archive repository"
cat > /etc/yum.repos.d/pgdg13-archive.repo <<'EOF'
[pgdg13-archive]
name=PostgreSQL 13 (PGDG archive, EL8)
baseurl=https://yum-archive.postgresql.org/13/redhat/rhel-8-x86_64/
enabled=1
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/PGDG-RPM-GPG-KEY-RHEL
skip_if_unavailable=0
EOF
dnf_required config-manager --set-enabled pgdg-common pgdg16 pgdg17 pgdg18
dnf_required --refresh makecache

allow_pg_port() {
    # Custom ports are denied by SELinux even though initdb/service setup works.
    # Keep enforcing enabled and refuse to steal a port labelled for another app.
    if selinuxenabled; then
        semanage port -l > "$WORK_DIR/selinux-ports"
        local port_type
        port_type=$(python3 - "$WORK_DIR/selinux-ports" "$PORT" <<'PY'
import sys
port = int(sys.argv[2])
with open(sys.argv[1]) as f:
    for line in f:
        fields = line.split(None, 2)
        if len(fields) != 3 or fields[1] != "tcp":
            continue
        for value in fields[2].strip().split(","):
            bounds = [int(n) for n in value.strip().split("-")]
            if bounds[0] <= port <= bounds[-1]:
                # Generic unreserved_port_t ranges do not preclude a specific
                # PostgreSQL mapping added with semanage port -a.
                if fields[0] not in ("unreserved_port_t", "port_t"):
                    print(fields[0])
PY
        )
        case "$port_type" in
            postgresql_port_t) ;;
            "") semanage port -a -t postgresql_port_t -p tcp "$PORT" ;;
            *) die "TCP $PORT is labelled $port_type, expected postgresql_port_t" ;;
        esac
    fi
}

# Use explicit versioned clients and sockets during provisioning: an inherited
# PGPORT/PGHOST must never populate or verify a different cluster by accident.
pg_sql() {
    sudo -u postgres env -u PGHOST -u PGPORT -u PGSERVICE -u PGOPTIONS \
        "$BIN/psql" -X -h /var/run/postgresql -p "$PORT" -U postgres \
        -d postgres -v ON_ERROR_STOP=1 "$@"
}
for V in 13 16 17 18; do
    PORT=$((5400 + V))
    BIN="/usr/pgsql-$V/bin"
    DATA="/var/lib/pgsql/$V/data"
    STEP="PG$V packages: postgresql$V, postgresql$V-server, postgresql$V-contrib"
    dnf_required -y install "postgresql$V" "postgresql$V-server" "postgresql$V-contrib"
    for tool in initdb postgres psql pgbench pg_isready; do
        [[ -x "$BIN/$tool" ]] || die "missing $BIN/$tool"
    done
    if [[ "$V" == 13 ]]; then
        # The first server RPM creates the postgres user; only now can tmpfiles
        # assign ownership on a fresh host. Recreate the socket dir after reboot.
        STEP="PostgreSQL socket directory (including reboot persistence)"
        cat > /etc/tmpfiles.d/pgwt-postgresql.conf <<'EOF'
d /run/postgresql 2775 postgres postgres -
EOF
        systemd-tmpfiles --create /etc/tmpfiles.d/pgwt-postgresql.conf
        restorecon -RF /run/postgresql
    fi
    STEP="PG$V cluster initialization at $DATA"
    if [[ ! -f "$DATA/PG_VERSION" ]]; then
        # setup refuses a nonempty/half-initialized directory; that must be fatal.
        "$BIN/postgresql-$V-setup" initdb
    fi
    [[ $(cat "$DATA/PG_VERSION") == "$V" ]] || die "$DATA contains a different PostgreSQL major"
    # A separate managed include avoids duplicate appended settings on reruns.
    # Read last; refuse conflicting ALTER SYSTEM values via SHOW checks below.
    sed -i '/^include = '\''pgwt-gate.conf'\''$/d' "$DATA/postgresql.conf"
    echo "include = 'pgwt-gate.conf'" >> "$DATA/postgresql.conf"
    cat > "$DATA/pgwt-gate.conf" <<EOF
port = $PORT
listen_addresses = 'localhost'
unix_socket_directories = '/var/run/postgresql, /tmp'
shared_preload_libraries = 'pg_stat_statements'
EOF
    if [[ "$V" -ge 14 ]]; then
        echo 'compute_query_id = on' >> "$DATA/pgwt-gate.conf"
    fi
    chown postgres:postgres "$DATA/pgwt-gate.conf"
    chmod 0600 "$DATA/pgwt-gate.conf"
    # Match Ubuntu's test-only trust for local socket/loopback connections.
    sed -i -E 's/(ident|peer|scram-sha-256|md5)([[:space:]]*(#.*)?)$/trust\2/' "$DATA/pg_hba.conf"
    STEP="PG$V service postgresql-$V on port $PORT (SELinux labels included)"
    restorecon -RF "$DATA"
    allow_pg_port
    systemctl enable "postgresql-$V"
    systemctl restart "postgresql-$V"
    READY=0
    for ((attempt=0; attempt<30; attempt++)); do
        if "$BIN/pg_isready" -h /var/run/postgresql -p "$PORT" -U postgres >/dev/null 2>&1; then
            READY=1
            break
        fi
        sleep 1
    done
    [[ "$READY" == 1 ]] || die "PG$V did not become ready at /var/run/postgresql:$PORT in 30s; inspect journalctl -u postgresql-$V"
    [[ $(pg_sql -Atc 'SHOW port') == "$PORT" ]] || die "PG$V port mismatch"
    [[ $(pg_sql -Atc 'SHOW data_directory') == "$DATA" ]] || die "port $PORT belongs to another cluster"
    [[ $(pg_sql -Atc 'SHOW shared_preload_libraries') == pg_stat_statements ]] || die "PG$V pg_stat_statements preload missing/overridden"
    if [[ "$V" -ge 14 ]]; then
        [[ $(pg_sql -Atc 'SHOW compute_query_id') == on ]] || die "PG$V compute_query_id is not on"
    fi
    pg_sql -c 'CREATE EXTENSION IF NOT EXISTS pg_stat_statements'
    pg_sql -c 'SELECT count(*) FROM pg_stat_statements' >/dev/null

    STEP="PG$V pgbench scale-10 fixture (1,000,000 accounts)"
    # Only absence/shortness is repairable; SQL/auth/server errors are fatal.
    EXISTS=$(pg_sql -Atc "SELECT to_regclass('public.pgbench_accounts') IS NOT NULL")
    ROWS=0
    if [[ "$EXISTS" == t ]]; then ROWS=$(pg_sql -Atc 'SELECT count(*) FROM pgbench_accounts'); fi
    if (( ROWS < 1000000 )); then
        sudo -u postgres "$BIN/pgbench" -h /var/run/postgresql -p "$PORT" -U postgres -i -s 10 -q postgres
    fi
    (( $(pg_sql -Atc 'SELECT count(*) FROM pgbench_accounts') >= 1000000 )) || die "PG$V pgbench initialization is short"

    STEP="PG$V _smoke_io_load fixture (3,000,000 rows, 200-byte filler)"
    EXISTS=$(pg_sql -Atc "SELECT to_regclass('public._smoke_io_load') IS NOT NULL")
    ROWS=0
    if [[ "$EXISTS" == t ]]; then ROWS=$(pg_sql -Atc 'SELECT count(*) FROM _smoke_io_load'); fi
    if (( ROWS < 3000000 )); then
        START=$(date +%s)
        pg_sql -c "
            DROP TABLE IF EXISTS _smoke_io_load;
            CREATE TABLE _smoke_io_load (id bigint PRIMARY KEY, filler text, ctr int NOT NULL DEFAULT 0);
            INSERT INTO _smoke_io_load (id, filler, ctr)
                SELECT g, repeat('x', 200), 0 FROM generate_series(1, 3000000) g;"
        SIZE=$(pg_sql -Atc "SELECT pg_size_pretty(pg_total_relation_size('_smoke_io_load'))")
        log "PG$V _smoke_io_load built in $(( $(date +%s) - START ))s; size $SIZE"
    fi
    (( $(pg_sql -Atc 'SELECT count(*) FROM _smoke_io_load') >= 3000000 )) || die "PG$V IO fixture is short"
    STEP="PG$V _smoke_lockmgr_fanout fixture (200 range partitions)"
    pg_sql -c "
        CREATE TABLE IF NOT EXISTS _smoke_lockmgr_fanout (id bigint, val int) PARTITION BY RANGE (id);
        DO \$\$
        DECLARE i int;
        BEGIN
            FOR i IN 0..199 LOOP
                EXECUTE format(
                    'CREATE TABLE IF NOT EXISTS %I PARTITION OF _smoke_lockmgr_fanout '
                    'FOR VALUES FROM (%s) TO (%s)',
                    '_smoke_lockmgr_fanout_p' || i, i * 1000, (i + 1) * 1000);
            END LOOP;
        END \$\$;"
    [[ $(pg_sql -Atc "SELECT count(*) FROM pg_inherits WHERE inhparent = '_smoke_lockmgr_fanout'::regclass") == 200 ]] || die "PG$V lock-manager fixture does not have 200 partitions"
    log "PG$V ready: $(pg_sql -Atc 'SELECT version()')"
done

STEP="default PG18 clients, PGPORT=5418, socket directory"
for tool in psql pgbench pg_isready; do
    ln -sfn "/usr/pgsql-18/bin/$tool" "/usr/local/bin/$tool"
done
touch /etc/environment
sed -i '/^PGPORT=/d' /etc/environment
echo 'PGPORT=5418' >> /etc/environment
sudo env PGPORT=5418 psql -X -U postgres -d postgres -Atc 'SELECT version()'

STEP="Python LIVE UI dependencies: Playwright 1.60.0, websockets, Pillow, numpy"
python3 -m pip install playwright==1.60.0 websockets pillow numpy
# --with-deps runs apt-get (unsupported on EL8). Install the native equivalents
# from INSTALL.md plus the rest of Chromium's runtime/font dependencies instead.
BROWSER_PACKAGES=(
    alsa-lib atk at-spi2-atk at-spi2-core cups-libs dbus-libs glib2 gtk3
    libX11 libXcomposite libXdamage libXext libXfixes libXrandr libxcb
    libdrm mesa-libgbm libxkbcommon nss nspr pango cairo expat
    liberation-fonts dejavu-sans-fonts
)
STEP="Chromium runtime RPMs: ${BROWSER_PACKAGES[*]}"
dnf_required -y install "${BROWSER_PACKAGES[@]}"
STEP="Playwright Chromium and ffmpeg download (root's cache)"
python3 -m playwright install chromium
STEP="Chromium compatibility: launch, screenshot, video/ffmpeg under sudo"
# EL8 is not an officially supported Playwright OS. Prove the actual binaries
# work with this host's glibc/libraries; a download alone is not provisioning.
sudo python3 - <<'PY'
import pathlib
import tempfile
import numpy
import websockets
from PIL import Image
from playwright.sync_api import sync_playwright

with tempfile.TemporaryDirectory(prefix="pgwt-browser-") as tmp:
    with sync_playwright() as p:
        browser = p.chromium.launch()
        context = browser.new_context(record_video_dir=tmp)
        page = context.new_page()
        page.set_content("<h1>pg_wait_tracer EL8 provisioning</h1>")
        image = pathlib.Path(tmp) / "probe.png"
        page.screenshot(path=str(image))
        with Image.open(image) as screenshot:
            assert numpy.asarray(screenshot).size, "empty Chromium screenshot"
        page.wait_for_timeout(250)
        video = page.video
        context.close()
        assert pathlib.Path(video.path()).stat().st_size > 0, "empty Chromium video"
        print("Chromium screenshot/video OK:", browser.version)
        browser.close()
PY

STEP="root localhost SSH self-trust"
ssh-keygen -A
systemctl enable --now sshd
mkdir -p "$ROOT_HOME/.ssh"
chmod 0700 "$ROOT_HOME/.ssh"
SSH_KEY="$ROOT_HOME/.ssh/id_ed25519"
if [[ ! -f "$SSH_KEY" ]]; then
    ssh-keygen -t ed25519 -N '' -f "$SSH_KEY" -C pgwt-gate-box-self
fi
[[ -s "$SSH_KEY.pub" ]] || die "missing public key $SSH_KEY.pub"
touch "$ROOT_HOME/.ssh/authorized_keys" "$ROOT_HOME/.ssh/known_hosts"
if ! grep -qxF "$(cat "$SSH_KEY.pub")" "$ROOT_HOME/.ssh/authorized_keys"; then
    cat "$SSH_KEY.pub" >> "$ROOT_HOME/.ssh/authorized_keys"
fi
chmod 0600 "$ROOT_HOME/.ssh/authorized_keys" "$ROOT_HOME/.ssh/known_hosts"
restorecon -RF "$ROOT_HOME/.ssh"
for host in localhost 127.0.0.1; do
    ssh-keyscan -H "$host" > "$WORK_DIR/hostkeys"
    [[ -s "$WORK_DIR/hostkeys" ]] || die "sshd returned no host keys for $host"
    ssh-keygen -R "$host" -f "$ROOT_HOME/.ssh/known_hosts" >/dev/null
    cat "$WORK_DIR/hostkeys" >> "$ROOT_HOME/.ssh/known_hosts"
done
ssh -o BatchMode=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=5 root@localhost true

STEP="completion marker"
cat > /etc/pgwt-gate-box <<EOF
# Written only after successful tests/provision-runner.sh el8 provisioning.
# Build dependencies, PG 13/16/17/18, fixtures, Chromium and localhost SSH ready.
provisioned_at=$(date -u +%FT%TZ)
os=el8
EOF
log "provisioning complete (el8); no GitHub runner registered (Ubuntu-only option)"
