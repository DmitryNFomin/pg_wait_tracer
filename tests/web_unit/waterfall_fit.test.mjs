/* #415: the drilled Waterfall readout must be on screen at the pinned demo
 * viewport (1710x981) without a scroll. Every geometry below is the one
 * MEASURED in Chrome at that viewport (see the PR body's fold measurement):
 *   list top            435 px
 *   pane height         304 px  (banner 30 + chart 240 + readout 34)
 *   trailing below pane  20 px  (#table-container's padding-bottom)
 * With the old fixed 310 px cap those add up to 1069 px of document in a
 * 981 px viewport and the readout's bottom lands at 1049 -- 68 px below the
 * fold, which is the defect.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    executionListMaxHeight, waterfallChartFitHeight,
    EXECUTION_LIST_MAX_PX, EXECUTION_LIST_MIN_PX, EXECUTION_LIST_FOLD_MARGIN_PX,
} from '../../web/static/lib/builders/waterfall.js';

const DEMO = { viewportHeight: 981, listTop: 435, paneHeight: 304, trailingBelowPane: 20 };

test('the chart absorbs the 19px drilled overflow without changing the list floor', () => {
    const geometry = { viewportHeight: 981, listTop: 468, paneHeight: 401,
        trailingBelowPane: 20, contextHeight: 58, rowListHeights: [110.5, 139.5] };
    const listHeight = executionListMaxHeight(geometry);
    const pageHeight = geometry.listTop + listHeight + geometry.paneHeight + 20;
    const chartHeight = waterfallChartFitHeight(262, 240, pageHeight - 981);
    assert.equal(listHeight, 111);
    assert.equal(chartHeight, 243);
    assert.equal(pageHeight - (262 - chartHeight), 981);
});

test('chart fit preserves its minimum and leaves an impossible fold visible', () => {
    assert.equal(waterfallChartFitHeight(262, 240, 23), 240);
    assert.equal(1004 - (262 - waterfallChartFitHeight(262, 240, 23)), 982);
    assert.equal(waterfallChartFitHeight(240, 240, 171), 240);
});

test('chart fit restores its natural height with spare room and rounds overflow up', () => {
    assert.equal(waterfallChartFitHeight(262, 240, 0), 262);
    assert.equal(waterfallChartFitHeight(262, 240, -100), 262);
    assert.equal(waterfallChartFitHeight(262, 240, 18.5), 243);
});

function readoutBottom(geometry, listHeight) {
    // The pane sits directly under the list, and the readout is its last
    // 34 px -- i.e. the pane's own bottom edge.
    return geometry.listTop + listHeight + geometry.paneHeight;
}

test('the measured demo geometry keeps the readout above the fold', () => {
    const height = executionListMaxHeight(DEMO);
    assert.equal(height, 981 - 435 - 304 - 20 - EXECUTION_LIST_FOLD_MARGIN_PX);  // 214
    assert.ok(readoutBottom(DEMO, height) <= DEMO.viewportHeight,
        `readout bottom ${readoutBottom(DEMO, height)} must be within 981`);
    // And the whole page fits, so there is no scrollbar to scroll away from
    // the readout with.
    assert.ok(readoutBottom(DEMO, height) + DEMO.trailingBelowPane <= DEMO.viewportHeight);
});

test('the OLD fixed cap is what put the readout 68px below the fold', () => {
    // The regression this fix exists for, stated as a number: the fixed cap
    // is strictly worse than the computed one at the demo viewport.
    assert.ok(EXECUTION_LIST_MAX_PX > executionListMaxHeight(DEMO));
    assert.equal(readoutBottom(DEMO, EXECUTION_LIST_MAX_PX) - DEMO.viewportHeight, 68);
});

test('a taller multi-lane chart yields further, not less', () => {
    // chartHeight = max(240, lanes*54+100): three lanes -> 262, so the pane
    // is 326 px. Issue #415 measured that case at 101 px below the fold.
    const threeLanes = Object.assign({}, DEMO, { paneHeight: 326 });
    assert.equal(readoutBottom(threeLanes, EXECUTION_LIST_MAX_PX) - 981, 90);
    const height = executionListMaxHeight(threeLanes);
    assert.ok(height < executionListMaxHeight(DEMO));
    assert.ok(readoutBottom(threeLanes, height) <= 981);
});

test('a viewport with room to spare renders exactly as before', () => {
    // Nothing shrinks when the fixed cap already fits: no snapshot cell with
    // a short table or a tall window may move because of this change.
    const roomy = { viewportHeight: 1400, listTop: 435, paneHeight: 304,
        trailingBelowPane: 20 };
    assert.equal(executionListMaxHeight(roomy), EXECUTION_LIST_MAX_PX);
});

test('a viewport that genuinely cannot fit both keeps the list usable', () => {
    // 600 px tall: 600-435-304-20-8 is negative. The list must not collapse
    // to nothing (or to a negative height) -- it stays at the minimum, and
    // the layout honestly does not fit, which the recorder's own fold check
    // then refuses rather than scrolling around.
    const tiny = Object.assign({}, DEMO, { viewportHeight: 600 });
    assert.equal(executionListMaxHeight(tiny), EXECUTION_LIST_MIN_PX);
    assert.ok(readoutBottom(tiny, EXECUTION_LIST_MIN_PX) > tiny.viewportHeight);
});

test('visible context shares the minimum list budget without being counted twice', () => {
    // Context can share the minimum budget only while a header and full
    // execution row remain visible. Prefer a usable list when both panes
    // cannot fit, even if the available budget is smaller than the floor.
    const geometry = { viewportHeight: 981, listTop: 468, paneHeight: 401,
        trailingBelowPane: 20, contextHeight: 58, rowListHeights: [110.5, 139.5] };
    const height = executionListMaxHeight(geometry);
    assert.equal(height, Math.ceil(geometry.rowListHeights[0]));
    assert.equal(geometry.listTop + height + geometry.paneHeight + 20, 1000);
    assert.equal(executionListMaxHeight({ ...geometry, contextHeight: 0 }), 120);
    assert.equal(executionListMaxHeight({ ...geometry, viewportHeight: 1500 }), 310);
    assert.equal(executionListMaxHeight({ ...geometry, viewportHeight: 600 }), 111);
    assert.equal(executionListMaxHeight({ ...geometry, contextHeight: 200 }), 111);
});

test('snapshot floor rounds the measured first row and border up', () => {
    const geometry = { viewportHeight: 800, listTop: 435, paneHeight: 379,
        trailingBelowPane: 20, contextHeight: 58, rowListHeights: [544.5 - 435 + 1] };
    assert.equal(executionListMaxHeight(geometry), 111);
    // A changed font or wrapped title must change the floor too.
    assert.equal(executionListMaxHeight({ ...geometry, rowListHeights: [127.25] }), 128);
});

test('demo finishes its second row within the fold cushion', () => {
    const geometry = { viewportHeight: 981, listTop: 435, paneHeight: 379,
        trailingBelowPane: 20, contextHeight: 58,
        rowListHeights: [544.5 - 435 + 1, 573.5 - 435 + 1] };
    const height = executionListMaxHeight(geometry);
    assert.equal(height, 140);
    assert.ok(geometry.listTop + height + geometry.paneHeight + geometry.trailingBelowPane
        <= geometry.viewportHeight);
    // The second row cannot be completed if that would break the fold.
    assert.equal(executionListMaxHeight({ ...geometry, paneHeight: 386.5 }), 131);
});

test('missing row measurements retain the old usable minimum', () => {
    assert.equal(executionListMaxHeight({ ...DEMO, viewportHeight: 600,
        contextHeight: 58 }), EXECUTION_LIST_MIN_PX);
});

/* ── unreachable-detection suite: every way the computation can be made
 * blind must produce null (leave the CSS cap alone), never a number. A
 * made-up height is worse than the old fixed one: it would look like the
 * fix ran. */
for (const [name, geometry] of [
    ['no geometry at all', undefined],
    ['empty object', {}],
    ['missing viewportHeight', { listTop: 435, paneHeight: 304 }],
    ['missing listTop', { viewportHeight: 981, paneHeight: 304 }],
    ['missing paneHeight', { viewportHeight: 981, listTop: 435 }],
    ['null paneHeight (element absent)', { viewportHeight: 981, listTop: 435, paneHeight: null }],
    ['NaN listTop', { viewportHeight: 981, listTop: NaN, paneHeight: 304 }],
    ['Infinite viewportHeight', { viewportHeight: Infinity, listTop: 435, paneHeight: 304 }],
    ['string numbers', { viewportHeight: '981', listTop: '435', paneHeight: '304' }],
    ['zero viewportHeight (detached document)', { viewportHeight: 0, listTop: 0, paneHeight: 304 }],
    ['zero paneHeight (pane not laid out yet)', { viewportHeight: 981, listTop: 435, paneHeight: 0 }],
    ['negative listTop (scrolled out of view)', { viewportHeight: 981, listTop: -40, paneHeight: 304 }],
]) {
    test(`unreadable geometry refuses to guess: ${name}`, () => {
        assert.equal(executionListMaxHeight(geometry), null);
    });
}

test('an unreadable trailing space degrades to the previous behaviour, not a guess', () => {
    // trailingBelowPane is the only optional input: absent or nonsense means
    // "count nothing", which is exactly how this behaved before it existed --
    // it must never be read as a negative that GROWS the list past the fold.
    const base = { viewportHeight: 981, listTop: 435, paneHeight: 304 };
    const expected = 981 - 435 - 304 - EXECUTION_LIST_FOLD_MARGIN_PX;
    assert.equal(executionListMaxHeight(base), expected);
    assert.equal(executionListMaxHeight(Object.assign({}, base, { trailingBelowPane: NaN })), expected);
    assert.equal(executionListMaxHeight(Object.assign({}, base, { trailingBelowPane: -200 })), expected);
    assert.equal(executionListMaxHeight(Object.assign({}, base, { trailingBelowPane: '20' })), expected);
});

test('the returned height is an integer px value', () => {
    const odd = { viewportHeight: 981.6, listTop: 435.4, paneHeight: 304.9,
        trailingBelowPane: 20.3 };
    const height = executionListMaxHeight(odd);
    assert.equal(height, Math.floor(height));
    // Floor, not round: a half pixel must come off the list, never off the
    // margin that keeps the pane above the fold.
    assert.ok(odd.listTop + height + odd.paneHeight + odd.trailingBelowPane
        <= odd.viewportHeight);
});
