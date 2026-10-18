/* Node unit tests for the pure backend-span builder
 * (lib/builders/backend-span.js) — the UI-side rendering rules for #455's
 * "Backend active span" feature and its wire envelope. The live Queries
 * and Waterfall mount gates are in their respective view test files.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    backendActivityStatus, backendActivityLine, backendActivityDrillModel,
    backendActivityDrillHtml, waterfallSpanReadout, backendActivityHtml,
} from '../../web/static/lib/builders/backend-span.js';

// ── Null/missing safety: never blank, never a crash ─────────────────────────

test('backendActivityLine(null/undefined): "Not measured", never blank or a crash', () => {
    assert.equal(backendActivityLine(null), 'Not measured');
    assert.equal(backendActivityLine(undefined), 'Not measured');
    assert.equal(backendActivityLine({}), 'Not measured');
});

test('backendActivityStatus defaults to the "nothing known" corner, never throws', () => {
    assert.deepEqual(backendActivityStatus(null), { measurement: 'unmeasured', completion: 'unknown' });
    assert.deepEqual(backendActivityStatus({}), { measurement: 'unmeasured', completion: 'unknown' });
    // Garbage field types are not trusted as the enum value.
    assert.deepEqual(backendActivityStatus({ measurement: 42, completion: {} }),
        { measurement: 'unmeasured', completion: 'unknown' });
});

// ── The two axes are independent -- never collapsed into one shared enum ───

test('measurement and completion survive on the status/drill model as '
    + 'SEPARATE fields, for every combination of the two axes', () => {
    const measurements = ['measured', 'partial', 'unmeasured'];
    const completions = ['closed', 'in_flight', 'unknown'];
    for (const measurement of measurements) {
        for (const completion of completions) {
            const span = { measurement, completion, observed_elapsed_ms: 1000,
                completed_duration_ms: 2000, executions_count: 3 };
            const status = backendActivityStatus(span);
            assert.equal(status.measurement, measurement);
            assert.equal(status.completion, completion);
            const model = backendActivityDrillModel(span);
            assert.equal(model.measurement, measurement);
            assert.equal(model.completion, completion);
        }
    }
});

// ── In-flight NEVER shows a completed duration ──────────────────────────────

test('in_flight: observed elapsed time only, completed_duration_ms is IGNORED '
    + 'even when present (never implies the span is over)', () => {
    const span = { measurement: 'measured', completion: 'in_flight',
        observed_elapsed_ms: 714000, completed_duration_ms: 999999 };
    const line = backendActivityLine(span);
    assert.ok(line.includes('In flight'), line);
    assert.ok(line.includes('714.0 s observed'), line);
    assert.ok(!line.includes('999999') && !line.includes('1000.0'),
        'completed_duration_ms must never leak into an in-flight line: ' + line);
});

test('#455 NEGATIVE CONTROL / bypass check: a naive implementation reading '
    + 'completed_duration_ms unconditionally (ignoring completion) WOULD '
    + 'leak it into the in-flight line -- demonstrates this gate can fail', () => {
    const span = { measurement: 'measured', completion: 'in_flight',
        observed_elapsed_ms: 714000, completed_duration_ms: 999999 };
    // The naive (wrong) composition this test guards against:
    const naiveLine = 'In flight · ' + (span.completed_duration_ms / 1000).toFixed(1) + ' s';
    assert.ok(naiveLine.includes('1000.0'), 'sanity: the naive line DOES leak the completed duration');
    // The real builder output must differ from the naive (buggy) one.
    assert.notEqual(backendActivityLine(span), naiveLine);
});

test('in_flight with no observed elapsed time yet: "In flight" alone, never a blank/zero elapsed', () => {
    const span = { measurement: 'measured', completion: 'in_flight', observed_elapsed_ms: null };
    const line = backendActivityLine(span);
    assert.equal(line, 'In flight');
    assert.ok(!line.includes('0.0 s') && !line.includes('null'));
});

// ── Closed: "Closed" names the activity-state transition, not statement '
// success -- the exact completed duration, never a bound derived elsewhere ─

test('closed: exact completed duration, never computed from DB Time / Execs '
    + '(this module has no such fields to divide in the first place)', () => {
    const span = { measurement: 'measured', completion: 'closed', completed_duration_ms: 714200 };
    assert.equal(backendActivityLine(span), 'Closed · 714.2 s');
});

test('closed with no completed_duration_ms (should not happen, but must not '
    + 'fabricate a number): "Not measured", never a NaN or blank', () => {
    const span = { measurement: 'measured', completion: 'closed', completed_duration_ms: null };
    const line = backendActivityLine(span);
    assert.equal(line, 'Not measured');
    assert.ok(!line.includes('NaN'));
});

// ── unmeasured / unknown corners ─────────────────────────────────────────────

test('measurement unmeasured: always "Not measured", regardless of completion', () => {
    for (const completion of ['closed', 'in_flight', 'unknown']) {
        assert.equal(backendActivityLine({ measurement: 'unmeasured', completion,
            completed_duration_ms: 5000, observed_elapsed_ms: 5000 }), 'Not measured');
    }
});

test('completion unknown (but something WAS measured): "Not measured" '
    + 'headline, with observed elapsed time attached if available -- never '
    + 'claims closed or in-flight when the completion state itself is unknown', () => {
    const withElapsed = backendActivityLine(
        { measurement: 'partial', completion: 'unknown', observed_elapsed_ms: 2000 });
    assert.ok(withElapsed.startsWith('Not measured'), withElapsed);
    assert.ok(withElapsed.includes('2.0 s observed'), withElapsed);
    const withoutElapsed = backendActivityLine(
        { measurement: 'partial', completion: 'unknown', observed_elapsed_ms: null });
    assert.equal(withoutElapsed, 'Not measured');
});

// ── Zero contained executions is a VALID, renderable state (brief item 0) ──

test('drill model with ZERO executions renders a complete, non-empty model '
    + '-- 0 is a real answer, never treated as missing data', () => {
    const span = { measurement: 'measured', completion: 'in_flight', pid: 1234,
        start_observed: true, end_observed: false, observed_elapsed_ms: 714000,
        executions_count: 0 };
    const model = backendActivityDrillModel(span);
    assert.equal(model.executionsCount, 0);
    assert.equal(model.executionsText, '0 observed executions');
    const html = backendActivityDrillHtml(model);
    assert.ok(html.includes('0 observed executions'), html);
    assert.ok(html.includes('PID 1234'), html);
    assert.ok(html.includes('In flight'), html);
});

test('#455 NEGATIVE CONTROL: a naive `if (executionsCount)` guard would '
    + 'DROP the executions line entirely on the zero-execution fixture -- '
    + 'demonstrates the gate is not vacuously true', () => {
    const span = { measurement: 'measured', completion: 'in_flight',
        executions_count: 0 };
    const model = backendActivityDrillModel(span);
    const naiveGateDrops = !model.executionsCount;   // truthy check on 0 -- wrong
    assert.equal(naiveGateDrops, true, 'sanity: a truthy check on 0 IS falsy');
    // The real model's executionsText is present regardless:
    assert.notEqual(model.executionsText, undefined);
    assert.ok(model.executionsText.length > 0);
});

test('drill model with unknown executions count (field absent): "Not measured", never 0', () => {
    const span = { measurement: 'partial', completion: 'unknown', executions_count: null };
    const model = backendActivityDrillModel(span);
    assert.equal(model.executionsCount, null);
    assert.equal(model.executionsText, 'Not measured');
    assert.notEqual(model.executionsText, '0 observed executions',
        'an UNKNOWN count must never render identically to a REAL zero');
});

// ── Window-edge boundaries (brief: left/right edge wording) ────────────────

test('right window edge: end not observed, PID and start still shown', () => {
    const span = { measurement: 'measured', completion: 'in_flight', pid: 42,
        start_observed: true, end_observed: false, observed_elapsed_ms: 100000,
        executions_count: 0 };
    const model = backendActivityDrillModel(span);
    assert.equal(model.startText, 'start observed');
    assert.equal(model.endText, 'end not observed');
});

test('left window edge: start not captured reads distinctly from "end not observed"', () => {
    const span = { measurement: 'partial', completion: 'closed', pid: 7,
        start_observed: false, end_observed: true, completed_duration_ms: 5000,
        executions_count: 2 };
    const model = backendActivityDrillModel(span);
    assert.equal(model.startText, 'Start not captured');
    assert.equal(model.endText, 'end observed');
});

test('a closed span always reads "end observed" for the end text, even if '
    + 'end_observed were somehow false -- completion:closed already implies it', () => {
    const span = { measurement: 'measured', completion: 'closed', end_observed: false,
        completed_duration_ms: 1000, executions_count: 1 };
    assert.equal(backendActivityDrillModel(span).endText, 'end observed');
});

// ── Extended protocol: multi-statement disclosure ───────────────────────────

test('multi_statement flag adds the "may include multiple statements" note; '
    + 'absent otherwise', () => {
    const withFlag = backendActivityDrillModel({ measurement: 'measured',
        completion: 'closed', completed_duration_ms: 1000, multi_statement: true });
    assert.equal(withFlag.multiStatementNote,
        'May include multiple statements/executions and client pauses.');
    const without = backendActivityDrillModel({ measurement: 'measured',
        completion: 'closed', completed_duration_ms: 1000 });
    assert.equal(without.multiStatementNote, null);
});

// ── Never an average: no field here is ever divided by another ─────────────

test('#455 FORBIDDEN check: db_time_ms / execs-shaped extra fields on the '
    + 'input are never read by this module at all -- adding them changes '
    + 'nothing about the output (no hidden division path exists)', () => {
    const base = { measurement: 'measured', completion: 'closed', completed_duration_ms: 714200,
        pid: 1, start_observed: true, end_observed: true, executions_count: 91 };
    const withHostileExtraFields = { ...base, db_time_ms: 720400, execs: 91, avg_exec_us: 33 };
    assert.equal(backendActivityLine(base), backendActivityLine(withHostileExtraFields));
    assert.deepEqual(backendActivityDrillModel(base), backendActivityDrillModel(withHostileExtraFields));
    // And the FORBIDDEN number itself (db_time_ms / execs = 7912.6 ms ~= "7.9 s")
    // must never appear anywhere in the rendered output.
    const html = backendActivityDrillHtml(backendActivityDrillModel(withHostileExtraFields));
    assert.ok(!html.includes('7.9'), html);
});

// ── Waterfall shares the SAME composition as the drill panel ───────────────

test('waterfallSpanReadout and the drill-panel HTML are IDENTICAL for the '
    + 'same span -- the two surfaces can never drift to different wording', () => {
    const span = { measurement: 'measured', completion: 'in_flight', pid: 55,
        start_observed: true, end_observed: false, observed_elapsed_ms: 60000,
        executions_count: 3 };
    assert.equal(waterfallSpanReadout(span),
        backendActivityDrillHtml(backendActivityDrillModel(span)));
});

test('waterfallSpanReadout(null) renders Not measured, not a crash', () => {
    assert.equal(waterfallSpanReadout(null), backendActivityDrillHtml(backendActivityDrillModel(null)));
    assert.ok(waterfallSpanReadout(null).includes('Not measured'));
});

test('backendActivityDrillHtml(null): empty string, no crash', () => {
    assert.equal(backendActivityDrillHtml(null), '');
});

test('#455 gate 5: rendered measurement × completion matrix has nine distinct states', () => {
    const states = new Set();
    for (const measurement of ['measured', 'partial', 'unmeasured']) {
        for (const completion of ['closed', 'in_flight', 'unknown']) {
            const html = waterfallSpanReadout({ measurement, completion,
                completed_duration_ms: 2000, observed_elapsed_ms: 1000 });
            assert.ok(html.trim());
            states.add(html);
        }
    }
    assert.equal(states.size, 9);
});

test('#455 wire envelope: null reason and scope are visible and escaped', () => {
    const html = backendActivityHtml({ backend_activity_span: null,
        backend_activity_unavailable_reason: 'refused <script>',
        backend_activity_span_scope: '<whole-query>' });
    assert.match(html, /Not measured/);
    assert.match(html, /refused &lt;script&gt;/);
    assert.match(html, /Scope: &lt;whole-query&gt;/);
});

test('#455 wire envelope: hostile totals leave compact and detail HTML byte-identical', () => {
    const span = { measurement: 'measured', completion: 'in_flight',
        observed_elapsed_ms: 714000, completed_duration_ms: 999999, executions_count: 0 };
    const base = { backend_activity_span: span };
    const hostile = { db_time_ms: 720400, execs: 91, backend_activity_span: {
        ...span, db_time_ms: 720400, execs: 91 } };
    for (const detail of [false, true]) {
        const html = backendActivityHtml(hostile, detail);
        assert.equal(html, backendActivityHtml(base, detail));
        assert.match(html, /714.0 s observed/);
        assert.doesNotMatch(html, /7\.9|1000.0|999999/);
    }
});
