import { buildTimelineOption } from '../../../web/static/lib/builders/timeline.js';

// Demonstrates the latent "silent collapse" footgun noted in finding.md
// (second, unrelated problem -- NOT fixed here). If an event's pid is ever
// absent from the `pids` array the server sent (currently unreachable given
// the server's two-pass construction, but not defended against client-side),
// web/static/lib/builders/timeline.js:405's
// `pidIndexMap[ev.p] || 0` silently relocates that bar onto row 0 (PID A's
// lane) instead of refusing or dropping it -- the exact "silently losing
// lanes" shape case 3 of the brief describes, just dormant today.
const FROM = 0, TO = 1000;
const data = {
    pids: [1001, 1002],
    events: [
        { s: 10, d: 5, p: 1001, n: 'CPU*', c: 0, q: '1' },
        // 9999 is NOT in `pids` -- malformed/out-of-sync payload.
        { s: 20, d: 5, p: 9999, n: 'Lock:relation', c: 2, q: '1' },
    ],
    truncated: false, total_count: 2,
};
const model = buildTimelineOption(data, { from: FROM, to: TO, width: 1200 });
const bars = model.option.series[0].data;
console.log('lanes:', model.option.yAxis.data);
console.log('bar rows:', bars.map(b => ({ pidRowIdx: b[2], name: b[3] })));
console.log('--> pid 9999\'s Lock:relation bar landed on row',
    bars[1][2], '= lane "' + model.option.yAxis.data[bars[1][2]] + '" (PID 1001\'s lane), not refused.');
