/* pgwt — "timeline" view: the per-session wait timeline (Gantt-style bars).
 *
 * Migrated to the { id, requests, build, mount, enter, leave } contract (B3
 * part 3). requests() fetches session_timeline (only when a pid/query filter is
 * set — otherwise returns a prompt sentinel) on a single-flight channel;
 * build() is PURE (lib/builders/timeline.js -> ECharts custom-series option);
 * mount() paints the optional truncation banner + feeds the option to the
 * view-owned ECharts instance. The view OWNS its ECharts instance: created
 * lazily in mount, disposed in leave() — no module-level chart global.
 *
 * U2 (review P5): mount reuses a stable shell (the histogram/concurrency
 * ensureShell pattern) — the ECharts instance and the wire-3 drag-select
 * overlay are created ONCE and fed setOption on every refresh. This view was
 * P5's first-named symptom: a live-followed session timeline ran
 * dispose/echarts.init/setOption(_, true) on EVERY refresh, so each 5 s tick
 * (and each of its own zoom refetches) flashed a teardown, dropped the open
 * tooltip and hover state, and rebuilt the selection overlay. The shell keeps
 * a banner slot above the chart so the truncation banner appearing or
 * disappearing does not rebuild the chart node — the banner is EMPTY (a
 * zero-height block, pixel-identical to the pre-U2 markup) when the server
 * did not truncate. Only the chart host's height still varies (it scales with
 * the PID count); a changed height is applied in place + chart.resize().
 *
 * U2 (review P3 wire 3): the timeline is ZOOMABLE — the shared
 * lib/selection.js drag-select overlay (its header says it is reusable; this
 * is the second consumer) rides the chart's x-axis and drives ctx.onZoom,
 * which REFETCHES the window. Refetch is the CORRECT mechanism here, not a
 * camera: the server coalesces/truncates timeline events per window
 * (truncated + total_count), so finer detail only exists server-side — the
 * roadmap explicitly says do not camera-ize the timeline. This turns the
 * long-standing "Zoom in for more detail" banner from a lie (a banner
 * instructing an action the surface could not perform) into an instruction.
 * The timeline x-axis is a value axis in RAW NANOSECONDS (builders/
 * timeline.js pins min/max to the window in ns), so the selection range is
 * already ns — no unit conversion at this boundary (contrast the AAS ms
 * axis).
 *
 * Behavior otherwise identical to the old legacy adapter in app.js:
 *   - No pid/query filter → "Select a session (PID) or query" prompt.
 *   - Empty result → "No events for selected session/range".
 *   - Truncated result → a yellow "Showing N of M events" banner above the chart.
 */

import { buildTimelineOption } from '../lib/builders/timeline.js';
import { attachSelection } from '../lib/selection.js';

export function createTimelineView() {
    let chart = null;       // ECharts instance — owned here, nowhere else
    let detachSel = null;   // drag-select overlay teardown (U2 wire 3)
    let ctxRef = null;      // last ctx (the once-attached overlay reads onZoom)

    function disposeChart() {
        if (detachSel) { detachSel(); detachSel = null; }
        if (chart) { chart.dispose(); chart = null; }
    }

    /* P5: build the banner + chart containers ONCE; refreshes only refill the
     * banner and setOption. Rebuilt lazily whenever another view (or a
     * prompt/no-data mount) replaced the container's content. */
    function ensureShell(el) {
        if (document.getElementById('timeline-chart')) return;
        disposeChart();   // shell is being rebuilt — any old chart DOM is gone
        el.innerHTML =
            '<div id="timeline-banner"></div>' +
            '<div id="timeline-chart" style="height:200px;padding:10px 20px"></div>';
    }

    return {
        id: 'timeline',

        async requests(ctx) {
            const f = ctx.filters.filters;
            if (!f.pid && !f.query_id) return { prompt: true };
            return ctx.transport.request(ctx.channel('table'), 'session_timeline', {
                from: ctx.timeRange.from, to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(),
            });
        },

        build(data, ctx) {
            if (data && data.prompt) return { prompt: true };
            return buildTimelineOption(data, { from: ctx.timeRange.from, to: ctx.timeRange.to });
        },

        mount(el, model, ctx) {
            ctxRef = ctx;
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';

            if (model.prompt) {
                disposeChart();
                el.innerHTML = '<div style="padding:40px;text-align:center;color:#888">' +
                    'Select a session (PID) or query to view timeline</div>';
                return;
            }
            if (!model.hasData) {
                disposeChart();
                el.innerHTML = '<div style="padding:40px;text-align:center;color:#888">' +
                    'No events for selected session/range</div>';
                return;
            }

            ensureShell(el);
            document.getElementById('timeline-banner').innerHTML = model.truncated
                ? '<div style="padding:8px 20px;font-size:12px;color:#ffd700;' +
                  'background:#3d3200;border-bottom:1px solid #555">Showing ' +
                  model.count + ' of ' + model.total_count +
                  ' events. Drag to zoom in for more detail.</div>'
                : '';

            const host = document.getElementById('timeline-chart');
            const h = model.chartHeight + 'px';
            const grew = host.style.height !== h;
            if (grew) host.style.height = h;
            if (!chart) {
                chart = ctx.echarts.init(host, 'dark');
                // U2 (wire 3): drag-select → zoomTo → refetch (server-side
                // coalescing means detail requires the round trip — see
                // header). The x axis is ns, so the range passes through
                // as-is. Attached ONCE with the shell (P5): it reads the
                // CURRENT ctx via ctxRef, so a refresh never rebuilds it.
                detachSel = attachSelection(host, chart, {
                    onSelect: (range) => {
                        if (ctxRef && ctxRef.onZoom) {
                            ctxRef.onZoom(range.from, range.to);
                        }
                    },
                });
            } else if (grew) {
                chart.resize();   // the PID count changed the host's height
            }
            chart.setOption(model.option, true);
        },

        enter(ctx) { ctxRef = ctx; /* chart created lazily in mount (P5) */ },
        leave() { disposeChart(); },
        resize() { if (chart) chart.resize(); },
    };
}
