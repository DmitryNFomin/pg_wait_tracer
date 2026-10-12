# demos/

Recording tooling for the demo gifs in the project README.

## What's here

| File | Purpose |
|------|---------|
| `web.gif` | Browser UI demo, embedded in main README |
| `dump.gif` | Terminal `pgwt-server --dump` demo, embedded in main README |
| `dump.txt` | Raw text dump (101 lines) — reference output |
| `dump.tape` | VHS tape that renders `dump.gif` |
| `record-web.mjs` | Playwright script that drives + records the web UI |
| `record-web.sh` | Wrapper that boots `pgwt` locally and runs the Playwright script |
| `workload.sh` | The 60s mixed-wait workload — uploaded to the VM |
| `capture-trace.sh` | Provisions a Hetzner VM, builds, runs workload, downloads trace |
| `build-server-local.sh` | Builds `pgwt-server` on macOS / Linux for local replay |
| `replay-compat.h` | macOS API compatibility and capture CPU count for the demo replay build |
| `Makefile` | Orchestrator: `make all` does everything |
| `PROMPTS.md` | LLM prompts for re-driving the web demo via an MCP browser |

## Reproducing the gifs from scratch

Prerequisites:

- **Hetzner Cloud account** + API token in `HCLOUD_TOKEN` env var
- SSH public key already uploaded to your Hetzner account (script picks the
  one matching `~/.ssh/id_ed25519.pub` or `id_rsa.pub`)
- Local tools: `bash`, `rsync`, `ssh`, `scp`, `jq`, `curl`, `make`, `clang`/`gcc`
- For the `dump.gif`: [`vhs`](https://github.com/charmbracelet/vhs) (`brew install vhs`)
- For the `web.gif`: `node` **20+** + `npm`, `ffmpeg`, `go`; the wrapper installs
  the validated Playwright **1.64.0** and headed Chromium automatically
- macOS: `brew install lz4` (Linux usually has `liblz4-dev` packaged)

Then:

```bash
cd demos
export HCLOUD_TOKEN=...   # your Hetzner Cloud API token

make all                  # provision VM → capture trace → render both gifs
make clean-vm             # tear down the VM (saves ~€0.007/hr)
```

Or step by step:

```bash
make trace      # ~5 min including fresh provisioning + 60s workload
make dump-gif   # ~1 min including build + VHS encoding
make web-gif    # ~20 s warm; allow another ~45 s for first browser download
make clean-vm   # delete the VM
```

The trace files land in `demos/local-trace/` (gitignored, about 60 MB compressed).
`capture-cpus.txt` records the capture VM's CPU count: the local demo replay
uses it for the capacity line instead of showing the laptop's CPU count.
Older trace directories need this metadata before rendering; re-run `make trace`
or supply the verified capture-machine count in that file.

VM names are unique. Creation tries **one** EU location (`VM_LOCATION`, default
`fsn1`) and fails without retrying other locations. `VM_TYPE` defaults to `cpx22`.
An existing `.vm-id` blocks fresh provisioning until `make clean-vm` succeeds.
Deletion verifies the server returns HTTP 404 before removing the saved ID.
`make clean` preserves VM ownership files; it does **not** replace `clean-vm`.

Measured in the 2026-10-12 tooling dry run on master `2b2147f`: fresh capture
4m57s (including 3m27s provisioning), dump rendering about 58s, warm web
rendering about 20s. Allow **8–10 minutes** for a fresh run and teardown, plus
network variance. These are tooling measurements, not final demo evidence.
The shipped GIFs must still be cut from the final tagged tree after the last
merge and two clean rehearsals (see `docs/PAINT_LATENCY_PLAN.md` in the repo).

## Iterating on a gif without re-capturing

Once `demos/local-trace/` exists, you can re-render either gif freely without
spinning up a new VM:

```bash
# Tweak demos/dump.tape, then:
make dump-gif

# Tweak demos/record-web.mjs, then:
make web-gif
```

## How the trace was made

`workload.sh` runs on the VM in parallel with `pg_wait_tracer --daemon --mode full`:
(The default tiered mode cannot guarantee data for the Transitions demo.)

1. **pgbench TPC-B** — 8 clients, 60s, scale 10 → Lock (row), WAL, ClientRead, LWLock:BufferContent
2. **5× injected `LOCK TABLE pgbench_branches IN EXCLUSIVE MODE` + 2s sleep** — seeds Lock:relation and forces queue contention
3. **3× heavy reads** (`SELECT count(*) FROM pgbench_accounts WHERE aid % 7 = 0`) — surfaces IO + buffer pin activity

Original reference result: ~7M wait-event transitions, ~91 s wall clock, AAS 6.4. Wait class
breakdown (DB time): Client 32%, Lock 21%, Extension 16%, CPU 12%, LWLock 9%,
IO 7% (mostly WalSync).

## Why two recording mechanisms

- **VHS for `dump.gif`** — terminal output, no browser needed. Fast,
  deterministic, tiny dependency footprint.
- **Playwright for `web.gif`** — the web UI is canvas-heavy (uPlot + ECharts) and
  needs a real Chromium. Playwright drives clicks/drags and records video;
  the recorder waits for successful mounts, verifies real table rows and the
  zoomed time window, and requires the transition graph and 0% slider.
  `ffmpeg` denoises compression noise and uses a two-pass palette at 1200 px,
  8 fps for README-sized output. Screenshots + `evidence.json` remain in the
  gitignored `.video/` directory for review. Failed recordings close Chromium
  and stop both the local web client and its replay server.

## VM costs

Hetzner cpx22 = €0.0066/hr at the time of writing. A full `make all` run
followed by `make clean-vm` takes roughly 6–7 minutes with cached recording
dependencies. Don't forget `make clean-vm` afterwards.

## Troubleshooting

- **"Local SSH key not found in Hetzner"** — upload your pubkey to Hetzner first:
  `hcloud ssh-key create --name $(hostname) --public-key-from-file ~/.ssh/id_ed25519.pub`
- **`vhs` rendering blank** — the tape runs Bash with a simple prompt from the
  repo root and uses `TRACE_DIR`; no developer-specific checkout is needed.
  `make dump-gif` validates actual replay before starting VHS.
- **Local macOS build fails** — use `make dump-gif` / `make web-gif` so the
  demo's `replay-compat.h` and Homebrew lz4 flags are included.
- **Playwright fails to find tabs** — tab buttons are selected by `data-tab`.
  The recorder fits the offline capture before dragging; it fails if zooming
  did not shrink the selected window or a tab did not mount successfully.
- **`web-gif` startup fails** — inspect `demos/.pgwt.log`. The private SSH shim
  handles the client's SSH options and replays locally; HTTP readiness has a
  bounded wait and fails visibly.
- **`web.gif` too large** — lower `GIF_FPS` (env var, default 8) or shrink
  the `scale=` filter in the ffmpeg call inside `record-web.mjs`.
