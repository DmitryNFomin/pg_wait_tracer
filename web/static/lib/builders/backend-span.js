/* pgwt — pure builder: backend active span rendering (issue #455's UI half).
 *
 * Shared by the Queries row, its bounded detail panel and Waterfall.
 * The response wraps each span in backend_activity_span, with a nullable
 * span, backend_activity_unavailable_reason and backend_activity_span_scope.
 *
 * SPAN INPUT (`span`, nullable/partial fields allowed throughout):
 *   {
 *     measurement: 'measured' | 'partial' | 'unmeasured',
 *     completion:  'closed' | 'in_flight' | 'unknown',
 *     pid: number | null,
 *     start_observed: boolean,            // false => "Start not captured"
 *     end_observed: boolean,              // false => "end not observed..."
 *     observed_elapsed_ms: number | null, // elapsed time actually seen
 *     completed_duration_ms: number | null, // ONLY meaningful when completion==='closed'
 *     executions_count: number | null,    // 0 is a real, valid count
 *     multi_statement: boolean,           // extended protocol: several statements/pauses in one span
 *   }
 *
 * MANDATORY RULES (both #455 briefs agree on these verbatim):
 *   - measurement and completion are two INDEPENDENT axes; this module never
 *     collapses them into one enum. backendActivityLine composes a single
 *     reader-facing line from both, explicitly, but the two fields survive
 *     unmerged on the model for any caller that wants them separately.
 *   - A missing/unmeasured value reads "Not measured" -- NEVER blank, NEVER
 *     a numeric zero standing in for "we don't know".
 *   - An in-flight span NEVER shows a completed duration: completed_duration_ms
 *     is read ONLY when completion === 'closed'. In-flight shows observed
 *     elapsed time instead, explicitly labeled "observed", never implying
 *     the span is over.
 *   - NEVER an average: nothing here divides a total by a count. (The
 *     backend half's FORBIDDEN list — DB Time / Execs — has no numerator or
 *     denominator available to this module at all, by construction.)
 *   - Zero contained executions is a valid, renderable state, not "no data"
 *     — the drill model always states a count (including 0), never omits
 *     the line because the count happens to be zero.
 */

import { esc } from '../format.js';

function fmtSpanSeconds(ms) {
    if (ms == null || !Number.isFinite(ms)) return null;
    return (ms / 1000).toFixed(1) + ' s';
}

/* PURE: the two-axis status, independent fields intact. Exported so a
 * caller that wants to branch on measurement/completion separately (e.g. a
 * future styling rule: color by measurement, icon by completion) never has
 * to re-parse a composed string. */
export function backendActivityStatus(span) {
    const measurement = span && typeof span.measurement === 'string'
        ? span.measurement : 'unmeasured';
    const completion = span && typeof span.completion === 'string'
        ? span.completion : 'unknown';
    return { measurement, completion };
}

/* PURE: the compact "Backend activity" line for the Queries row cell beside
 * DB Time, and the shared summary the drill panel / waterfall readout open
 * with. Composes measurement+completion into ONE reader-facing sentence
 * WITHOUT losing either fact — "Closed" names the activity-state transition
 * (not "successful statement completion", per the brief), and "In flight"
 * always pairs with observed (not completed) elapsed time. */
export function backendActivityLine(span) {
    if (!span) return 'Not measured';
    const { measurement, completion } = backendActivityStatus(span);
    if (measurement === 'unmeasured') return 'Not measured';

    if (completion === 'closed') {
        const dur = fmtSpanSeconds(span.completed_duration_ms);
        if (dur == null) return 'Not measured';
        return 'Closed · ' + dur;
    }
    if (completion === 'in_flight') {
        const elapsed = fmtSpanSeconds(span.observed_elapsed_ms);
        return 'In flight' + (elapsed != null ? ' · ' + elapsed + ' observed' : '');
    }
    // completion === 'unknown': we know SOMETHING was measured (measurement
    // is not 'unmeasured' here) but not whether the span closed.
    const elapsed = fmtSpanSeconds(span.observed_elapsed_ms);
    return 'Not measured' + (elapsed != null ? ' · ' + elapsed + ' observed' : '');
}

/* PURE: the bounded Queries-drill / Waterfall detail model — spans, PID,
 * observed boundaries, measurement/completion state, contained executions.
 * MUST produce a complete, renderable model with ZERO executions (brief
 * item 0): `executionsCount: 0` is a real answer, not an omitted field, so
 * the caller never has to special-case "no executions" as "no data". */
export function backendActivityDrillModel(span) {
    const { measurement, completion } = backendActivityStatus(span || {});
    const headline = backendActivityLine(span);
    const pid = span && typeof span.pid === 'number' ? span.pid : null;
    const startText = !span ? 'Not measured'
        : span.start_observed ? 'start observed'
        : 'Start not captured';
    const endText = !span ? 'Not measured'
        : completion === 'closed' ? 'end observed'
        : span.end_observed ? 'end observed'
        : 'end not observed';
    const executionsCount = span && typeof span.executions_count === 'number'
        ? span.executions_count : null;
    const executionsText = executionsCount == null ? 'Not measured'
        : executionsCount.toLocaleString() + ' observed execution' +
            (executionsCount === 1 ? '' : 's');
    const multiStatementNote = span && span.multi_statement === true
        ? 'May include multiple statements/executions and client pauses.' : null;
    return {
        headline, measurement, completion, pid, startText, endText,
        executionsCount, executionsText, multiStatementNote,
    };
}

export function backendActivityDrillHtml(model) {
    if (!model) return '';
    const parts = [model.headline, backendActivityAxesHtml(model)];
    const meta = [];
    if (model.pid != null) meta.push('PID ' + model.pid);
    meta.push(model.startText);
    meta.push(model.endText);
    meta.push(model.executionsText);
    parts.push(meta.join(' · '));
    if (model.multiStatementNote) parts.push(model.multiStatementNote);
    return parts.join('<br>');
}

/* PURE: the shared span readout the Waterfall view mounts (brief item 0,
 * "Waterfall: shared span readout") — same composition as the drill model
 * above, so the two surfaces can never drift to different wording for the
 * identical fact. */
export function waterfallSpanReadout(span) {
    return backendActivityDrillHtml(backendActivityDrillModel(span));
}

/* Keep both axes visible even when the duration headline is Not measured. */
function backendActivityAxesHtml(status) {
    const measurements = { measured: 'Measured', partial: 'Partial', unmeasured: 'Not measured' };
    const completions = { closed: 'Closed', in_flight: 'In flight', unknown: 'Unknown' };
    return 'Measurement: ' + (measurements[status.measurement] || 'Not measured') +
        ' · Completion: ' + (completions[status.completion] || 'Unknown');
}

/* Wire envelope shared by the table and detail readouts. Server prose is
 * escaped here; neither aggregate DB Time nor execution totals are read. */
export function backendActivityHtml(row, detail = false) {
    const span = row && row.backend_activity_span;
    const content = span
        ? (detail ? waterfallSpanReadout(span)
            : backendActivityLine(span) + '<br>' + backendActivityAxesHtml(backendActivityStatus(span)))
        : 'Not measured' + (row && row.backend_activity_unavailable_reason
            ? '<br>' + esc(row.backend_activity_unavailable_reason) : '');
    return content + (row && row.backend_activity_span_scope
        ? '<br><span class="backend-activity-scope">Scope: ' + esc(row.backend_activity_span_scope) + '</span>' : '');
}
