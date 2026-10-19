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

/* ── The shape the server ACTUALLY sends (integration finding, #455) ────────
 * The two halves of #455 were briefed from the issue text, not from each
 * other, so the field list above is the UI half's placeholder shape. What
 * src/backend_span.c's add_span_object() emits differs in four names, and
 * the Queries row wraps the span one level deeper than execution_detail
 * does:
 *
 *   boundaries        start_ns / end_ns -- ns as JSON STRINGS, or null
 *                     (never a boolean `start_observed`/`end_observed`)
 *   executions        exec_starts / exec_ends  (not `executions_count`)
 *   multi-statement   multi_exec / multi_query (not `multi_statement`)
 *   Queries row       backend_activity: { source, span, leader_spans, workers }
 *   execution_detail  backend_activity_span: { ... } (directly on the root)
 *
 * Read literally, the renderers found none of their fields on a real row and
 * drew the string "Not measured" over a measured, closed, 420 ms span -- the
 * silent dead cell piece 1's provisional note warned about. These two
 * adapters translate; they never INVENT. A field the server did not send
 * stays absent, so "Not measured" still means not measured. */
export function normalizeBackendSpan(span) {
    if (!span || typeof span !== 'object') return null;
    const out = { ...span };
    if (typeof out.start_observed !== 'boolean') out.start_observed = out.start_ns != null;
    if (typeof out.end_observed !== 'boolean') out.end_observed = out.end_ns != null;
    if (typeof out.executions_count !== 'number' && typeof out.exec_starts === 'number') {
        out.executions_count = out.exec_starts;
    }
    if (typeof out.multi_statement !== 'boolean' &&
        (typeof out.multi_exec === 'boolean' || typeof out.multi_query === 'boolean')) {
        out.multi_statement = out.multi_exec === true || out.multi_query === true;
    }
    return out;
}

/* The span plus its absence reason and scope, from EITHER envelope. `present`
 * separates "this object carries no backend-activity information at all"
 * (a response from before #455, or an unrelated row) from "it carries the
 * information and the answer is null" -- the caller must not render a panel
 * for the first and must render the stated absence for the second. */
export function backendSpanEnvelope(src) {
    const o = (src && typeof src === 'object') ? src : null;
    const hasDirect = !!o && Object.hasOwn(o, 'backend_activity_span');
    const hasWrapped = !!o && Object.hasOwn(o, 'backend_activity');
    const raw = hasDirect ? o.backend_activity_span
        : hasWrapped ? (o.backend_activity && o.backend_activity.span) || null
        : null;
    const str = (v) => (typeof v === 'string' && v ? v : null);
    return {
        present: hasDirect || hasWrapped,
        span: normalizeBackendSpan(raw),
        source: hasWrapped && o.backend_activity ? str(o.backend_activity.source) : null,
        reason: o ? str(o.backend_activity_unavailable_reason) : null,
        scope: o ? (str(o.backend_activity_span_scope) || str(o.backend_activity_scope)) : null,
    };
}

function fmtSpanSeconds(ms) {
    if (ms == null || !Number.isFinite(ms)) return null;
    return (ms / 1000).toFixed(1) + ' s';
}

/* Queries selects the longest in-flight span first, then the longest fully
 * measured closed span. Waterfall selects a CONTAINING span: keep this
 * wording local to Queries, never in the shared drill renderer. */
export function queryBackendSpanLine(span) {
    const { measurement, completion } = backendActivityStatus(span);
    const boundary = { closed: 'closed', in_flight: 'in flight', unknown: 'unknown boundary' }[completion]
        || 'unknown boundary';
    if (!span || measurement === 'unmeasured') {
        return 'Backend span unavailable: Not measured' + (span ? ' · ' + boundary : '');
    }
    const elapsed = fmtSpanSeconds(span.observed_elapsed_ms);
    if (measurement === 'partial') {
        return 'Partial backend span (' + boundary + '): ' +
            (elapsed == null ? 'elapsed not measured' : elapsed + ' observed');
    }
    if (completion === 'in_flight') {
        return 'Longest observed in-flight backend span: ' +
            (elapsed == null ? 'elapsed not measured' : elapsed + ' so far');
    }
    if (completion === 'closed') {
        const duration = fmtSpanSeconds(span.completed_duration_ms);
        return 'Longest measured closed backend span: ' + (duration || 'Not measured');
    }
    return 'Backend span (unknown boundary): ' +
        (elapsed == null ? 'Not measured' : elapsed + ' observed');
}

export function queryBackendSpanHtml(row) {
    const { span } = backendSpanEnvelope(row);
    return '<span class="query-backend-span"><span>' + esc(queryBackendSpanLine(span)) +
        '</span><span>' + (span && span.pid != null ? 'PID ' + esc(String(span.pid)) : 'PID unavailable') +
        '</span></span>';
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
export function backendActivityDrillModel(rawSpan) {
    // Accepts either envelope's span, in the server's names or the
    // placeholder ones -- see normalizeBackendSpan.
    const span = normalizeBackendSpan(rawSpan);
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
        ' · Backend activity boundary: ' + (completions[status.completion] || 'Unknown');
}

/* Wire envelope shared by the table and detail readouts. Server prose is
 * escaped here; neither aggregate DB Time nor execution totals are read. */
export function backendActivityHtml(row, detail = false) {
    const { span, reason, scope } = backendSpanEnvelope(row);
    const content = span
        ? (detail ? waterfallSpanReadout(span)
            : backendActivityLine(span) + '<br>' + backendActivityAxesHtml(backendActivityStatus(span)))
        : 'Not measured' + (reason ? '<br>' + esc(reason) : '');
    return content + (scope
        ? '<br><span class="backend-activity-scope">Scope: ' + esc(scope) + '</span>' : '');
}
