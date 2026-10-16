// Diagnosis evidence for the Timeline single-lane finding
// (#timeline-lane-diag). Feeds the REAL pgwt-server response captured in
// real_server_response.json (produced by repro_server_probe.py against a
// locally-built pgwt-server, not tests/mock_server.py) through the actual
// client-side pure builder, web/static/lib/builders/timeline.js, to show
// what the UI would draw for it -- no DOM, no ECharts runtime needed.
//
// Run from the repo root:
//   node tests/results/timeline-lane-diag/builder_probe.mjs
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { buildTimelineOption } from '../../../web/static/lib/builders/timeline.js';

const here = dirname(fileURLToPath(import.meta.url));
const data = JSON.parse(readFileSync(join(here, 'real_server_response.json'), 'utf8'));

// BASE_TS .. WINDOW_END from tests/test_data_timeline.py (the window the
// probe queried the server with).
const FROM = 10_000_000_000_000;
const TO   = 10_000_900_000_000;
const model = buildTimelineOption(data, { from: FROM, to: TO, width: 1200 });

console.log('distinct pids in real server payload:', data.pids.length, data.pids);
console.log('yAxis categories (lanes) the builder emits:', model.option.yAxis.data);
console.log('lane count:', model.option.yAxis.data.length);
console.log('aggregated (density collapse)?', model.aggregated);
console.log('bar rows emitted:', model.option.series[0].data.length);
const rowsUsed = new Set(model.option.series[0].data.map(d => d[2]));
console.log('distinct row indices actually drawn:', [...rowsUsed].sort((a, b) => a - b));
