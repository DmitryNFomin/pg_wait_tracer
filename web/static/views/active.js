/* pgwt — "active" view: the Average Active Sessions chart.
 *
 * This is the persistent top chart (Oracle ASH-style AAS). It is the FIRST
 * migrated view per the B3 plan order ("active → overview → ..."): the AAS
 * chart moves from ApexCharts to ECharts here, with the custom drag-select
 * overlay (lib/selection.js) replacing ApexCharts' built-in zoom.
 *
 * Unlike the tab views, this one is persistent — it does not tear down on tab
 * switches (the AAS chart sits above the tabs and reflects the current window
 * for every tab). So app.js drives it directly via build()/mount() on each
 * refresh rather than through the tab-switching view-manager. It still follows
 * the { id, requests, build, mount, enter, leave } contract: it owns its
 * ECharts instance (created in enter, disposed in leave — no module globals)
 * and its builder (lib/builders/aas.js) is pure and Node-tested.
 */

import { buildAasOption } from '../lib/builders/aas.js';
import {
    SAMPLED_BAND_COLOR, MIXED_BAND_COLOR, SAMPLED_BORDER, MIXED_BORDER,
} from '../lib/builders/fidelity.js';
import { attachSelection } from '../lib/selection.js';
import { esc, fmtDuration } from '../lib/format.js';

export function createActiveView() {
    let chart = null;          // ECharts instance — owned here, nowhere else
    let detachSel = null;      // selection-overlay teardown
    let el = null;             // chart container
    // Legend state, keyed by series NAME (U1, review P2 — an index Set turned
    // a CPU solo into an arbitrary-event solo once the server re-ranked the
    // event series). `selected` = visible names; `hovered` = the name being
    // hover-soloed (re-applied after a mid-hover live tick); `names` = the
    // last rendered series set, so a set change resets the selection.
    const legend = { selected: null, hovered: null, names: null };
    const resetLegend = () => {
        legend.selected = null; legend.hovered = null; legend.names = null;
    };

    return {
        id: 'active',

        /* Fetch the AAS data for the current window + filters. Uses a
         * single-flight channel so a new refresh supersedes a pending one. */
        async requests(ctx) {
            const t = ctx.timeRange;
            const params = {
                from: t.from,
                to: t.to,
                buckets: Math.min(Math.floor((el ? el.clientWidth : 800) / 4), 300),
                filters: ctx.filters.snapshot(),
            };
            // Class drill-down (no specific event): break down by events.
            const f = ctx.filters.filters;
            if (f.class && !f.event_id) params.detail = 'events';
            return ctx.transport.request(ctx.channel('aas'), 'aas', params);
        },

        /* PURE: data -> ECharts option + legend metadata. Threads the current
         * window (for full-extent fidelity bands) and the daemon escalation
         * status (for the active-window annotation) through to the builder. */
        build(data, ctx) {
            return buildAasOption(data, {
                numCpus: ctx.server.numCpus,
                win: { from: ctx.timeRange.from, to: ctx.timeRange.to },
                escalationStatus: ctx.getEscalationStatus ? ctx.getEscalationStatus() : null,
            });
        },

        mount(_el, model, ctx) {
            if (!chart) return;                     // disposed
            if (!model.hasData) {
                // UI-5: an empty window must not leave the PREVIOUS window's
                // paint on screen while the tables say "No data" — clear the
                // chart and say so explicitly.
                chart.clear();
                chart.setOption({
                    backgroundColor: 'transparent',
                    graphic: [{
                        type: 'text', left: 'center', top: 'middle',
                        style: { text: 'No data in selected range',
                                 fill: '#666', fontSize: 13 },
                    }],
                }, true);
                const legDiv = document.getElementById('aas-legend');
                if (legDiv) legDiv.innerHTML = '';
                const chip = document.getElementById('aas-fidelity-chip');
                if (chip) chip.remove();
                resetLegend();
                if (ctx.setStatus) {
                    ctx.setStatus(ctx.server.numCpus + ' CPUs · ' +
                        fmtDuration(ctx.timeRange.span()) + ' window · no data',
                        'connected');
                }
                return;
            }
            chart.setOption(model.option, true);
            renderLegend(model, chart, ctx, legend);
            renderFidelityChip(model, ctx);

            // Status line: window + peak AAS (kept identical to old behavior).
            if (ctx.setStatus) {
                const dur = fmtDuration(ctx.timeRange.span());
                const peak = (model.maxAas || 0).toFixed(1);
                ctx.setStatus(ctx.server.numCpus + ' CPUs · ' + dur +
                    ' window · peak ' + peak + ' AAS', 'connected');
            }
        },

        enter(ctx) {
            el = ctx.chartEl;
            if (!el) return;
            chart = ctx.echarts.init(el, 'dark');
            resetLegend();
            // Drag-select overlay → zoom the window.
            detachSel = attachSelection(el, chart, {
                onSelect: (range) => {
                    if (ctx.onZoom) ctx.onZoom(range.from, range.to);
                },
            });
        },

        leave() {
            if (detachSel) { detachSel(); detachSel = null; }
            if (chart) { chart.dispose(); chart = null; }
            const chip = document.getElementById('aas-fidelity-chip');
            if (chip) chip.remove();
            el = null;
            resetLegend();
        },

        resize() { if (chart) chart.resize(); },
    };
}

/* Fidelity legend chip: explains the sampled/exact distinction next to the AAS
 * legend. Rendered into a small host inserted after the series legend so the
 * shading band on the chart is never unexplained. Hidden for fully-exact
 * windows (the common case) to avoid noise. */
function renderFidelityChip(model, ctx) {
    const host = ctx.chartEl;
    if (!host || !host.parentNode) return;

    let chip = document.getElementById('aas-fidelity-chip');
    const fid = model.fidelity;
    const showBand = model.shading && model.shading.showLegend;

    if (!showBand) {
        if (chip) chip.remove();
        return;
    }
    if (!chip) {
        chip = document.createElement('div');
        chip.id = 'aas-fidelity-chip';
        chip.style.cssText =
            'display:flex;flex-wrap:wrap;gap:10px;justify-content:center;' +
            'padding:0 20px 8px;background:#1e1e3a;font-size:11px;color:#aaa;';
        const legDiv = document.getElementById('aas-legend');
        if (legDiv && legDiv.parentNode) legDiv.parentNode.insertBefore(chip, legDiv.nextSibling);
        else host.parentNode.insertBefore(chip, host.nextSibling);
    }

    const sampledSwatch =
        '<span style="width:14px;height:10px;display:inline-block;border-radius:2px;' +
        'background:' + SAMPLED_BAND_COLOR + ';border:1px dashed ' + SAMPLED_BORDER + '"></span>';
    const mixedSwatch =
        '<span style="width:14px;height:10px;display:inline-block;border-radius:2px;' +
        'background:' + MIXED_BAND_COLOR + ';border:1px dashed ' + MIXED_BORDER + '"></span>';

    let html = '<span>' + esc(model.fidelityLabel) + '</span>';
    if (fid === 'sampled') {
        html += '<span style="display:inline-flex;align-items:center;gap:5px">' +
            sampledSwatch + 'shaded = sampled (estimated, not exact)</span>';
    } else if (fid === 'mixed') {
        html += '<span style="display:inline-flex;align-items:center;gap:5px">' +
            sampledSwatch + 'sampled</span>' +
            '<span style="display:inline-flex;align-items:center;gap:5px">' +
            mixedSwatch + 'mixed window</span>';
    }
    if (model.escalation) {
        html += '<span style="color:' +
            (model.escalation.isAnomaly ? '#E53935' : '#4fc3f7') + '">● ' +
            esc(model.escalation.label) + '</span>';
    }
    chip.innerHTML = html;
}

/* External HTML legend with solo / multi-select / hover — kept out of the chart
 * for full control (matches the old ApexCharts custom legend behavior).
 *
 * U1 (review P2): selection is keyed by series NAME and lives in `leg`
 * ({selected, hovered, names}, owned by the view). It resets whenever the
 * series set changes — class<->event mode switch, or the top-N event set
 * shifting between ticks — so a CPU solo can never silently become a solo of
 * whatever series inherited the index. Visibility is applied as ONE batched
 * setOption({legend:{selected}}) per gesture (the per-name dispatchAction
 * storm is gone), and an in-progress hover-solo is tracked in leg.hovered and
 * RE-APPLIED after a live-tick rebuild — the old trailing apply(sel) used to
 * cancel the solo under a stationary cursor. */
function renderLegend(model, chart, ctx, leg) {
    const names = model.seriesNames;
    const colors = model.seriesColors;
    const host = ctx.chartEl;
    if (!host) return;

    const sameSet = !!leg.names && leg.names.length === names.length &&
        leg.names.every((n, i) => n === names[i]);
    if (!sameSet) {
        leg.selected = new Set(names);
        leg.hovered = null;
    }
    leg.names = names.slice();

    let legDiv = document.getElementById('aas-legend');
    if (!legDiv) {
        legDiv = document.createElement('div');
        legDiv.id = 'aas-legend';
        legDiv.style.cssText = 'display:flex;flex-wrap:wrap;gap:4px;justify-content:center;padding:8px 20px;background:#1e1e3a;';
        host.parentNode.insertBefore(legDiv, host.nextSibling);
    }

    legDiv.innerHTML = names.map((name, i) =>
        `<span class="aleg" data-i="${i}" style="display:inline-flex;align-items:center;gap:4px;` +
        `cursor:pointer;padding:3px 10px;border-radius:4px;font-size:11px;color:#ccc;` +
        `border:1px solid ${colors[i]};border-left:3px solid ${colors[i]};` +
        `user-select:none;transition:opacity 0.1s">` +
        `<span style="width:8px;height:8px;border-radius:2px;background:${colors[i]}"></span>` +
        `${esc(name)}</span>`
    ).join('');

    // One batched legend update per gesture. The annotation series is not in
    // `names`, so its trust marks can never be switched off from here.
    function applyChart(visible) {
        const sel = {};
        names.forEach(n => { sel[n] = visible.has(n); });
        chart.setOption({ legend: { selected: sel } });
    }

    // Chip opacity always reflects the persistent selection, not a hover.
    function paintChips() {
        legDiv.querySelectorAll('.aleg').forEach(elx => {
            elx.style.opacity = leg.selected.has(names[+elx.dataset.i]) ? '1' : '0.3';
        });
    }

    legDiv.querySelectorAll('.aleg').forEach(elx => {
        const name = names[+elx.dataset.i];
        elx.addEventListener('mouseenter', () => {
            leg.hovered = name;
            applyChart(new Set([name]));
        });
        elx.addEventListener('mouseleave', () => {
            leg.hovered = null;
            applyChart(leg.selected);
        });
        elx.addEventListener('click', (e) => {
            let set = leg.selected;
            if (e.metaKey || e.ctrlKey) {
                if (set.has(name)) { if (set.size > 1) set.delete(name); }
                else set.add(name);
            } else {
                if (set.size === 1 && set.has(name)) set = new Set(names);
                else set = new Set([name]);
            }
            leg.selected = set;
            // The click states the user's new intent; a later refresh must
            // show this selection, not resurrect the pre-click hover solo.
            leg.hovered = null;
            paintChips();
            applyChart(set);
        });
    });

    // A live tick replaces the chips mid-hover; the detached chip never fires
    // mouseleave, so clear a stuck hover when the cursor leaves the legend
    // itself. Plain assignment (not addEventListener): legDiv persists across
    // refreshes and must carry exactly one, current-closure handler.
    legDiv.onmouseleave = () => {
        if (leg.hovered != null) {
            leg.hovered = null;
            applyChart(leg.selected);
        }
    };

    paintChips();
    // Re-apply the CURRENT state after the rebuild: the in-progress
    // hover-solo if there is one, else the persistent selection.
    applyChart(leg.hovered != null ? new Set([leg.hovered]) : leg.selected);
}
