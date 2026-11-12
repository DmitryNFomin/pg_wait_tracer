# Repository context and earlier handover

PostgreSQL eBPF tracer: C/BPF daemon, `pgwt-server`, Go WebSocket bridge,
ECharts/uPlot UI. Plan/status: `docs/ROADMAP_AND_STATUS.md`. Run nothing eBPF-related on macOS.
> **Picking this work up after 2026-11-10?** Start at
> [`docs/HANDOVER_2026-11-10.md`](../HANDOVER_2026-11-10.md): the open release
> blockers with what is measured and what is only read from source, the state of
> every open PR, three decisions nobody made, and where the measurement evidence
> lives. Then
> [`docs/OPERATING_MODEL.md`](../OPERATING_MODEL.md) for how the work was
> organised and [`docs/LEAD_RETROSPECTIVE_2026-11-10.md`](../LEAD_RETROSPECTIVE_2026-11-10.md)
> for how its reporting failed, which is worth reading before trusting either.

## Layout

- `src/` daemon + BPF (`src/bpf/`), `src/server.c` = `pgwt-server`
- `web/` Go bridge; `web/static/` UI — `views/` per-tab, `dev/gallery.html` fixture gallery
- `tests/` everything: `run_all.sh` (Linux master runner), `ci_smoke.sh` (real-PG
  capture), `test_web_ui*.py` (Playwright), `web_unit/` (Node), `web_snapshots/` (baselines)
- `docs/VISUAL_CHECKLIST.md` — the seven words a UI reviewer grades against
- `.github/workflows/ci.yml` gating; `nightly.yml` OS matrix
