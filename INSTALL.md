# pg_wait_tracer — Installation Guide

## Supported Platforms

- **Linux kernel** >= 5.8, **or** EL8 (RHEL/Rocky/Alma/Oracle Linux 8.10,
  kernel 4.18 with backported BPF ring buffer + BTF). BTF is always required;
  on kernels without the BPF ring buffer the tool degrades to sampled-only.
- **Architecture**: x86_64, aarch64
- **PostgreSQL**: 17, 18 (full); 13 (supported — query attribution requires
  `pg_stat_statements`); 14–16 not yet (see Troubleshooting)
- **Tested on**: Ubuntu 22.04/24.04, Rocky Linux 9, AlmaLinux 9,
  Oracle Linux 9, RHEL 9, Rocky Linux 8.10 / AlmaLinux 8 / Oracle Linux 8 / RHEL 8

## Prerequisites

### Main-loop real-time priority

The daemon enables **SCHED_FIFO priority 1 on its main event-loop thread** by
default. Every other thread uses SCHED_OTHER, including threads created later.
Startup first removes any inherited real-time policy (for example from `chrt`),
then enables FIFO only after initialization and reads back every thread's policy.
The reset-on-fork flag also prevents helper processes from inheriting FIFO.

This reduces missed short-lived PostgreSQL processes under load. RHEL-family
CFS schedulers can delay a freshly woken task; the default RHEL 8 `tuned`
`virtual-guest` profile raises the wakeup delay to 15 ms. Raising one process's
nice priority does not reliably help when processes are grouped by session or
service. The tracer does not change host scheduler settings. The background
and measurements are in [the scheduling decision](https://github.com/DmitryNFomin/pg_wait_tracer/issues/989).

In addition to the existing capture privileges, FIFO needs **CAP_SYS_NICE**.
When restricting a root service's capabilities, include CAP_SYS_NICE in its
`CapabilityBoundingSet` and `AmbientCapabilities` alongside its capture
capabilities. On EL8 with CONFIG_RT_GROUP_SCHED, a scope or service with
`CPUQuota` may refuse FIFO even for root: run outside that quota-limited CPU
group, or have the administrator allocate real-time runtime to the service.
Service `LimitRTPRIO`/`LimitRTTIME` restrictions can also prevent activation.

If scheduling, watchdog setup, or policy verification fails, capture continues
at normal priority with one warning naming the failed operation, its error,
the privileges/service configuration to check, and the consequence:
**short-lived processes may be missed under load**. Both control-socket `status`
and `metrics` report `rt_priority` as `active`, `refused`, `demoted`, or `disabled`.

The kernel's **RLIMIT_RTTIME** watchdog bounds real-time CPU consumed without a
blocking call. The defaults are **200,000 microseconds soft** and **1,000,000
microseconds hard**. At the soft limit, SIGXCPU immediately demotes the main
thread to SCHED_OTHER, prints one warning naming the runtime budget, increments
`rt_priority_demotions_total`, and continues capture at normal priority for the
rest of that process's life, including PostgreSQL reconnects. Accounting uses
kernel ticks: preemption or yielding does not reset it; a genuine blocking
sleep does. A sustained backlog can therefore trigger demotion across multiple
loop iterations. If demotion cannot execute or fails, the **hard limit kills
the process with SIGKILL**, without clean shutdown or a sealed capture.

Configure both budgets in microseconds with `--rt-soft-us US --rt-hard-us US`
(`0 < soft < hard`). The configured limits and a failed demotion's errno are
also exposed in status/metrics. Use **`--no-rt-priority`** to keep every thread
at normal priority, including when the program was launched under `chrt`.

Limit sizing (2026-11-20, PG18, four vCPUs):
`python3 tests/rt_runtime_measure.py --pid POSTMASTER --evidence-dir /tmp/rt-measure`
ran each mode for 30 seconds with 500 idle connections, 16 reconnecting pgbench
clients, and parallel aggregate scans of one million rows (two workers planned
per query; workers observed under load on both VMs). An independent filtered
`perf` scheduler trace sums CPU slices across runnable preemptions and CPU
migrations, ending a run only at an actual sleep. Both traces reported zero
lost events; captures stayed at FIFO 1 throughout the measurements.

| Kernel | Full: longest CPU run / completed runs | Sampled at 100 Hz: longest / runs |
| --- | --- | --- |
| EL8 4.18.0-553.157.1 | 27.216561 ms / 5,074 | 9.456643 ms / 4,419 |
| EL9 5.14.0-687.41.1 | 10.406600 ms / 5,256 | 4.061293 ms / 4,643 |

The 200 ms soft budget is 7.3 times the longest observed run; the 1 s hard
budget leaves a further fivefold margin for delivery and demotion. These are
measured defaults, not a bound on every workload. The live gate separately
forces a spin with a 20 ms soft budget, verifies actual kernel demotion (also
after dropping CAP_SYS_NICE), and requires capture to continue afterwards.

### 1. Build Dependencies

#### Rocky Linux 9 / RHEL 9 / AlmaLinux 9 / Oracle Linux 9

```bash
# Enable CRB (CodeReady Builder) repo — required for libbpf-devel
sudo dnf config-manager --set-enabled crb                        # Rocky/RHEL/AlmaLinux
# sudo dnf config-manager --set-enabled ol9_codeready_builder    # Oracle Linux

# Core build tools (tar may be missing on minimal installs)
sudo dnf install -y gcc clang llvm make tar

# BPF toolchain (libbpf-devel requires CRB repo enabled above)
sudo dnf install -y bpftool libbpf-devel

# ELF and compression libraries
sudo dnf install -y elfutils-libelf-devel zlib-devel lz4-devel

# For DWARF-based st_query_id offset detection (optional, for query_event view)
sudo dnf install -y elfutils    # provides readelf
```

#### Rocky Linux 8 / RHEL 8 / AlmaLinux 8

RHEL 8 ships `libbpf` 0.5.0 and an old `bpftool`, which predate the USDT
support pg_wait_tracer needs. The Makefile detects this automatically and, on
EL8 only, builds a pinned `libbpf` + `bpftool` from source into `build/`
(requires network access at build time). EL9 and Ubuntu are unaffected.

```bash
# powertools is EL8's equivalent of EL9's CRB repo
sudo dnf install -y dnf-plugins-core
sudo dnf config-manager --set-enabled powertools

# Core build tools + BPF toolchain + ELF/compression libs.
# (git is required: the Makefile clones pinned libbpf/bpftool on EL8.)
sudo dnf install -y gcc clang llvm make tar git \
    elfutils-libelf-devel zlib-devel lz4-devel bpftool libbpf-devel
```

PGDG packages a **non-PIE** postgres binary on EL8 (PIE on EL9); pg_wait_tracer
handles both. The kernel must still expose BTF (`/sys/kernel/btf/vmlinux`) —
present by default on Rocky 8.10 / RHEL 8.

#### Ubuntu 22.04 / 24.04 / Debian 12

```bash
# Core build tools
sudo apt install -y gcc clang llvm make

# BPF toolchain
sudo apt install -y linux-tools-common linux-tools-$(uname -r) libbpf-dev

# ELF and compression libraries
sudo apt install -y libelf-dev zlib1g-dev liblz4-dev

# For DWARF-based st_query_id offset detection (optional)
sudo apt install -y binutils    # provides readelf
```

### 2. Kernel BTF Support

The kernel must expose BTF (BPF Type Format) data. Verify:

```bash
ls /sys/kernel/btf/vmlinux
```

If missing, install the BTF-enabled kernel:
- **Rocky Linux 9 / RHEL 9 / AlmaLinux 9**: Enabled by default
- **Oracle Linux 9**: `sudo dnf install -y kernel-uek` (UEK7+ has BTF)
- **Ubuntu**: Enabled by default on 22.04+

### 3. PostgreSQL

#### Rocky Linux 9 / RHEL 9 / Oracle Linux 9

```bash
# Install PGDG repository
sudo dnf install -y https://download.postgresql.org/pub/repos/yum/reporpms/EL-9-x86_64/pgdg-redhat-repo-latest.noarch.rpm

# Install PostgreSQL 17 (or 18)
sudo dnf install -y postgresql17-server postgresql17-contrib

# Initialize and start
sudo /usr/pgsql-17/bin/postgresql-17-setup initdb
sudo systemctl enable --now postgresql-17

# (Optional) Debug symbols for query_event view, query text capture, plan_id
sudo dnf install -y postgresql17-debuginfo
```

##### PostgreSQL 13 (EOL — use the PGDG archive repo)

PostgreSQL 13 is end-of-life and has been removed from the live PGDG repo,
so `postgresql13-server` will not install from `pgdg-redhat-repo-latest`.
Add the PGDG **archive** repo instead (adjust `rhel-9` → `rhel-8` for EL8):

```bash
sudo tee /etc/yum.repos.d/pgdg13-archive.repo >/dev/null <<'EOF'
[pgdg13-archive]
name=PostgreSQL 13 (PGDG archive)
baseurl=https://yum-archive.postgresql.org/13/redhat/rhel-9-x86_64/
enabled=1
gpgcheck=0
EOF

sudo dnf install -y postgresql13-server postgresql13-contrib
sudo /usr/pgsql-13/bin/postgresql-13-setup initdb
sudo systemctl enable --now postgresql-13
```

PG13 has no in-core query id, so the **query_event view requires
`pg_stat_statements`** — add it to `shared_preload_libraries` and restart:

```bash
sudo -u postgres psql -c "ALTER SYSTEM SET shared_preload_libraries='pg_stat_statements'"
sudo systemctl restart postgresql-13
sudo -u postgres psql -c "CREATE EXTENSION pg_stat_statements"
```

Wait capture itself works without it. (No PG13 debuginfo exists anymore;
the tracer uses header-derived offsets, so debuginfo is not required.)

#### Ubuntu / Debian

```bash
# Install PostgreSQL 17 (or 18)
sudo apt install -y postgresql-17 postgresql-contrib-17

# (Optional) Debug symbols for query_event view, query text capture, plan_id
sudo apt install -y postgresql-17-dbgsym
```

### 4. Test Dependencies (optional)

Running the full test suite requires additional packages and PostgreSQL configuration.

#### Packages

```bash
# Rocky/RHEL/Oracle Linux 9:
sudo dnf install -y python3 python3-pip bc

# Ubuntu:
sudo apt install -y python3 python3-pip bc

# pgbench is included in postgresql-contrib (already installed above)

# Web UI tests (optional — skipped if not installed)
pip3 install playwright websockets
playwright install chromium

# On Rocky/RHEL, Chromium also needs:
sudo dnf install -y atk at-spi2-atk cups-libs libXcomposite libXdamage \
    libXrandr libgbm pango alsa-lib nss libxkbcommon
```

#### PostgreSQL Configuration

Tests connect as `postgres` user via `psql` and `pgbench`. On a fresh install
running tests as root, you need to allow local connections without a password:

```bash
# Find pg_hba.conf and switch local auth from peer to trust (test use only)
# Rocky/RHEL: /var/lib/pgsql/17/data/pg_hba.conf
# Ubuntu:     /etc/postgresql/17/main/pg_hba.conf
sudo sed -i 's/^local\s\+all\s\+all\s\+peer/local   all             all                                     trust/' \
    /var/lib/pgsql/17/data/pg_hba.conf    # adjust path for your OS/version
sudo systemctl reload postgresql-17
```

Enable extensions and settings used by tests:

```bash
psql -U postgres -c "ALTER SYSTEM SET compute_query_id = 'on'"
psql -U postgres -c "ALTER SYSTEM SET max_parallel_workers_per_gather = 4"
sudo systemctl restart postgresql-17
```

Initialize pgbench:

```bash
pgbench -U postgres -i postgres    # any scale works; -s 50 for more realistic IO
```

#### Optional: pg_wait_sampling (for cross-validation tests)

The `test_cross_pg_wait_sampling` and Extension event tests require pg_wait_sampling.
These tests are **gracefully skipped** if the extension is not installed.

```bash
# Rocky/RHEL/Oracle Linux 9 (PG17 example):
sudo dnf install -y pg_wait_sampling_17

# Ubuntu (PG17 example):
sudo apt install -y postgresql-17-pg-wait-sampling
```

Add to shared_preload_libraries and restart:

```bash
psql -U postgres -c "ALTER SYSTEM SET shared_preload_libraries = 'pg_stat_statements, pg_wait_sampling'"
sudo systemctl restart postgresql-17
```

### 5. Go Toolchain (for the web investigation client)

The web investigation client (`pgwt`) is written in Go. Install Go >= 1.21
if you want to build it. This runs on your **laptop** (macOS or Linux), not
the DB server.

```bash
# macOS (Homebrew)
brew install go

# Ubuntu / Debian
sudo apt install -y golang-go

# Rocky / RHEL / Fedora
sudo dnf install -y golang

# Or download from https://go.dev/dl/
```


## Build

### Daemon + Server (C) — runs on DB server

```bash
cd pg_wait_tracer
make clean
make
```

This produces two binaries in the project root:
- `pg_wait_tracer` — the BPF daemon (requires BPF toolchain, root)
- `pgwt-server` — the trace file server (no BPF dependencies)

To build only `pgwt-server` (e.g. on a machine without BPF toolchain):

```bash
make pgwt-server
```

Build steps performed automatically:
1. Generate `vmlinux.h` from kernel BTF via `bpftool`
2. Compile BPF program with `clang`
3. Generate BPF skeleton header via `bpftool gen skeleton`
4. Compile and link userspace C code with `gcc`

### Trace File Server (C) — runs on DB server

```bash
cd pg_wait_tracer
make pgwt-server
```

This produces the `pgwt-server` binary. It reads trace files and serves
aggregated data over stdin/stdout (JSON lines protocol). No BPF dependencies —
only requires `liblz4` and `zlib` at runtime.

Copy it to the DB server:

```bash
scp pgwt-server root@db-server:/usr/local/bin/
```

Or build directly on the DB server (no BPF toolchain needed):

```bash
# Only needs: gcc, make, lz4-devel, zlib-devel
make pgwt-server
```

### Web Investigation Client (Go) — runs on your laptop

From the repository root:

```bash
make pgwt-client
cd web
```

`make pgwt-client` passes version ldflags so client and server builds from the same checkout report matching versions without a false skew banner.

This produces the `web/pgwt` binary. Run it from your laptop after `cd web`:

```bash
# Connects to DB server over SSH, opens browser at localhost:8384
./pgwt root@db-server

# Custom trace directory
./pgwt --trace-dir /var/lib/pgwt/traces root@db-server

# Custom pgwt-server path on remote host
./pgwt --server-path /usr/local/bin/pgwt-server root@db-server
```

**Requirements:**

- Go >= 1.21 and Make on your laptop (build time only — the binary is self-contained)
- SSH access to the DB server (uses your default SSH key)
- `pgwt-server` binary on the DB server (either in PATH or specified with
  `--server-path`)
- Trace files on the DB server (produced by `pg_wait_tracer --daemon -T <dir>`)
- SSH user must be in the trace file group (default: `dba` if it exists,
  otherwise the group PostgreSQL runs under) — see below

**Setting up trace file access for non-root users:**

The daemon runs as root but creates trace files readable by a configurable Unix
group. The default is the first group that actually exists on the host:
`dba` if the site created one, otherwise the group PostgreSQL itself runs
under (usually `postgres`) — so a non-root reader such as the PostgreSQL
service account can open a trace without any extra setup. SSH users
connecting via `pgwt` need to be in that group:

```bash
# Create the group (once)
sudo groupadd dba

# Add your SSH user to the group
sudo usermod -aG dba youruser

# The user must log out and back in for the group change to take effect
```

The daemon sets permissions automatically (`<group>` = the resolved trace
group; `wait_event_names.json` gets the same treatment):
- Trace directory: `0750` (`rwxr-x---`), owned by `root:<group>`
- Trace files: `0640` (`rw-r-----`), owned by `root:<group>`

If the group named with `--trace-group` does not exist, the daemon says so
and leaves the files root-owned: nothing but root can read that trace.

To use a different group name:
```bash
sudo ./pg_wait_tracer --daemon -T /var/lib/pgwt/traces --trace-group mygroup
```

### Text Dump (pgwt-server --dump)

```bash
# Quick summary of trace files (no root needed)
pgwt-server --dump /var/lib/pgwt/traces
```

## Quick Test

```bash
# Must run as root. The default capture tier is "tiered" (low-overhead
# always-on sampler with on-demand escalation).
sudo ./pg_wait_tracer --pid $(pgrep -xo postgres) --interval 5 --duration 10 -v

# To exercise the exact hardware-watchpoint tier (requires CAP_SYS_ADMIN),
# force it with --mode full:
sudo ./pg_wait_tracer --mode full --pid $(pgrep -xo postgres) \
    --interval 5 --duration 10 -v
```

## Run Tests

```bash
# Build and run all tests (requires root + running PostgreSQL)
sudo tests/run_all.sh

# Target a specific PG version on multi-instance hosts
sudo tests/run_all.sh --pg-version 18

# Or specify the postmaster PID directly
sudo tests/run_all.sh --pid 12345

# Individual test layers:
make -C tests                             # Build C tests + generators
tests/test_wait_event                     # C unit: event ID parsing (75 checks)
tests/test_cmdline                        # C unit: CLI parser (36 checks)
tests/test_bucket                         # C unit: histogram buckets (25 checks)
python3 tests/test_data_time_model.py     # Synthetic: time model math
python3 tests/test_data_transitions.py    # Synthetic: transition matrix
python3 tests/test_data_lock_chains.py    # Synthetic: lock chains + interference
python3 tests/test_web_ui.py              # Playwright: 108 browser checks
make bench                                # Performance: 10.8M events/sec

# Memory safety checks:
make test-asan                            # Rebuild with AddressSanitizer
make test-valgrind                        # Run under Valgrind
```

## Troubleshooting

### "symbol 'my_wait_event_info' not found"

The postgres binary must export the `my_wait_event_info` symbol. Verify:

```bash
# Check dynamic symbol table (works even on stripped binaries)
readelf -s --dyn-syms /usr/pgsql-17/bin/postgres | grep my_wait_event_info

# Or via nm (requires non-stripped binary)
nm /usr/pgsql-17/bin/postgres | grep my_wait_event_info
```

PGDG-packaged binaries are typically stripped but retain dynamic symbols (`readelf`
will find them). If neither works, you may need a postgres build that preserves
the symbol table.

### "st_query_id offset not found"

The `query_event` view requires knowing the offset of `st_query_id` in
`PgBackendStatus`. Three detection methods are tried automatically:

1. **DWARF debug symbols** (preferred) — install `postgresql-XX-dbgsym` or
   `postgresql-XX-debuginfo`
2. **Known offset table** — built-in for PG17/18 on x86_64 (offset=424)
3. **Disabled** — other views (time_model, system_event, session_event, histogram)
   still work

The same DWARF-based discovery is used for `st_activity` (query text capture)
and `st_plan_id` (plan identifier, PG18+ only). Installing debug symbols
enables all three features.

### Plan identifier (st_plan_id) shows 0

PostgreSQL 18 added `st_plan_id` to `PgBackendStatus`, but core PG does not
compute it automatically. You need a `planner_hook` extension:

- **pg_store_plans** — stores actual plan text indexed by (queryid, planid)
- **pg_stat_sql_plans** — computes plan hash and stores plan-level stats

Without such an extension, `st_plan_id` remains 0 and the feature is silently
inactive. On PG17 and earlier, `st_plan_id` does not exist.

### "Cannot find load base" or "/proc permission denied"

Ensure running as root (`sudo`) or with `CAP_SYS_PTRACE` + `CAP_SYS_ADMIN`.

### "bpftool: command not found"

```bash
# Rocky / RHEL / Oracle Linux:
sudo dnf install -y bpftool

# Ubuntu (bpftool is in linux-tools):
sudo apt install -y linux-tools-$(uname -r)
```

### Kernel BTF missing (`/sys/kernel/btf/vmlinux` not found)

The kernel must be compiled with `CONFIG_DEBUG_INFO_BTF=y`. Rocky Linux 9,
RHEL 9, Oracle Linux UEK7+, and Ubuntu 22.04+ have this by default. Older
kernels may need an upgrade.

### PostgreSQL version compatibility

pg_wait_tracer observes each backend's `wait_event_info` field. How it reaches
that field depends on the PostgreSQL major version:

- **PG17, PG18 — full.** PostgreSQL writes wait events through the
  `my_wait_event_info` global pointer; the tracer resolves it directly. Wait
  event names are also discovered dynamically from `pg_wait_events` (PG17+).
- **PG13 — supported.** PostgreSQL 13 writes directly to
  `MyProc->wait_event_info`; the tracer resolves `MyProc` and adds the known
  PGPROC offset (684, verified live on 13.23), with a runtime validation guard
  that refuses to attach if the offset looks wrong. Both tiers (sampled and
  full) use this path. PG13 has no in-core query id, so the **query_event view
  requires `pg_stat_statements`** in `shared_preload_libraries` — without it,
  query views report unavailable (or the daemon exits if `--view query_event`
  was explicitly requested), but wait capture is unaffected.
- **PG14, PG15, PG16 — not yet.** These also write to
  `MyProc->wait_event_info`, but their verified PGPROC offsets and per-version
  name tables are not yet added, so the daemon fails fast at startup rather than
  capturing incorrect data. Support is planned.

### Operational contracts (run under a supervisor)

Two behaviors are by design and must be handled by your service manager:

- **The daemon exits when the postmaster dies.** All resolved addresses
  (load base, `MyProc`/`my_wait_event_info`, per-backend PGPROC slots) are
  specific to one postmaster instance; after a PostgreSQL restart they are
  meaningless, so the daemon exits with the reason
  `PostgreSQL (PID ...) stopped` instead of guessing. Run it under systemd
  so it re-attaches to the new postmaster automatically:

  ```ini
  [Service]
  ExecStart=/usr/local/bin/pg_wait_tracer --daemon --mode tiered -T /var/lib/pgwt
  Restart=always
  RestartSec=5
  # Watchpoint + bootstrap fds: 2 per backend + headroom (the daemon also
  # raises RLIMIT_NOFILE itself at startup, but the hard limit must allow it)
  LimitNOFILE=4096
  ```

  (`--daemon` already retries discovery in a loop; `Restart=always` covers
  daemon crashes as well.)

- **Same PID namespace as PostgreSQL.** The daemon matches BPF-reported
  kernel PIDs against `/proc` and `postmaster.pid`, which assumes it shares
  the PID namespace with PostgreSQL. Run it on the host for a host
  PostgreSQL. For a containerized PostgreSQL, run the daemon **inside that
  container** (or join its PID namespace, e.g.
  `nsenter -t <postmaster-pid> -p`); running it on the host against a
  container's PostgreSQL will mis-map PIDs.

### File descriptor limits (many backends)

Full/tiered capture holds up to two perf fds per backend. The daemon raises
`RLIMIT_NOFILE` at startup to cover its 1024-backend registry, but it cannot
exceed the hard limit granted to it. If you see
`perf_event_open: ... file descriptor limit reached` in the log (each such
failure also increments `wp_attach_failures_total` on the control socket),
raise the hard limit: `LimitNOFILE=` in the systemd unit or `ulimit -Hn`.

### Watchpoint retirement metrics

`wp_attach_exit_race_total` counts failed watchpoint operations only when errno
is ESRCH and the pinned original task is gone, Z/X, or has both PF_EXITING and
zero vsize. PF_EXITING proof is logged as `liveness=exiting` in
`WP-ATTACH-FAIL` (`exit_proof=1` for gone/Z/X, `2` for PF_EXITING).
ESRCH alone remains `wp_attach_failures_total`. Other errnos
remain failures even when the target has gone. The liveness subsets are
`wp_attach_failures_live_total`, `wp_attach_failures_gone_total`
and `wp_attach_failures_unknown_total`. Gone targets with a non-ESRCH errno
remain real failures in the gone subset.

In watchpoint capture, `lifecycle_exit_before_attach_total` counts retirements
without a completed real attachment or retained INIT evidence. It excludes
watchpoint failures already counted in `wp_attach_failures_total`, counted
enrollment refusals (including those without a registry entry), and counted
live FORK conflicts with a retained exit-race entry. Each backend has one
failure or retirement class. Its three once-per-retirement classes are:

- `lifecycle_exit_before_attach_with_opportunity_total`: missed attachment,
  opportunity observed (gated): a registry entry exists, bootstrap was armed,
  and the post-arm check ran.
- `lifecycle_exit_before_attach_unknown_total`: opportunity unknown (gated):
  the required entry/armed/post-arm evidence is incomplete and there is no
  positive first-handling exclusion. Bootstrap-open, startup-scan and direct
  attach exit races without that evidence belong here.
- `lifecycle_exit_before_attach_no_opportunity_total`: exited before the tracer
  could attach (reported): no registry entry because first handling positively
  found the target gone. Bounded refusal bookkeeping overflow denies this
  exclusion; a PID without retained evidence of a counted failure stays unknown.

Each counted PID emits `EXIT-BEFORE-ATTACH` with the class, entry/armed status,
post-arm pointer, shared-address result and errnos, plus global no-state and
wait-read counter deltas since enrollment (availability is explicit). These
are diagnostic deltas, not per-PID attribution. No INIT timestamp or duration
is invented. Retained INIT evidence keeps the existing INIT rule and counters.
An exit proof supplies the `wp_attach_exit_race_total` reason; it never proves
unavoidable loss or no opportunity. That reason may coexist with an INIT-loss
class or a retirement class. `wp_attach_failures_total`, observed-opportunity
retirements and unknown-opportunity retirements gate `DAEMON_INTEGRITY_COUNTERS`.
The no-opportunity retirement class is reported. A live FORK conflicting with
a retained startup exit-race entry is conservatively counted as an enrollment
failure, including possible PID reuse; a proven exit is not retried.
