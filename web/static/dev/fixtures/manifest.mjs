/* pgwt — the fixture manifest: every (builder × state) the gallery renders.
 *
 * MANIFEST is derived from the fixture modules (never hand-listed) so it can
 * not drift from them. Deterministic order: BUILDER_ORDER × each module's
 * insertion order.
 *
 * Contracts other code relies on:
 *   - manifest id      = '<builder>/<state>'            (bug reports cite this)
 *   - gallery cell id  = 'gallery-<builder>-<state>'    (cellId() — the
 *     snapshot suite screenshots #<cellId>; tick cells expose data-tick)
 *   - entry fields     = { id, builder, state, cellId, description, tags,
 *                          ticks }  (ticks = replay length, 0 = static cell)
 *   - tags: UPPERCASE = docs/VISUAL_CHECKLIST.md vocabulary words; lowercase =
 *     free-form facets (fidelity / injection / i18n / replay)
 *
 * Node-side importers use tests/fixtures/manifest.mjs (a re-export of this
 * file); the canonical copy lives under web/static/ so the gallery page can
 * fetch it from the mock's static root.
 */

import { states as aas, uplotStates as uplotAas } from './aas.mjs';
import { states as fidelity } from './fidelity.mjs';
import { states as timeline } from './timeline.mjs';
import { states as histogram } from './histogram.mjs';
import { states as transitions } from './transitions.mjs';
import { states as concurrency } from './concurrency.mjs';
import { states as tableConfigs } from './table-configs.mjs';

// 'uplot-aas' (U2b): the SAME aas fixture states rendered through
// buildUplotSpec + a real uPlot mount — both AAS renderers stay eyeballable
// while the ?renderer= seam exists (aas.mjs uplotStates shares objects with
// states, so the two cell sets can never drift apart).
export const BUILDER_ORDER = ['aas', 'uplot-aas', 'fidelity', 'timeline',
    'histogram', 'transitions', 'concurrency', 'table-configs'];

export const FIXTURES = {
    'aas': aas,
    'uplot-aas': uplotAas,
    'fidelity': fidelity,
    'timeline': timeline,
    'histogram': histogram,
    'transitions': transitions,
    'concurrency': concurrency,
    'table-configs': tableConfigs,
};

export function cellId(builder, state) {
    return 'gallery-' + builder + '-' + state;
}

export const MANIFEST = BUILDER_ORDER.flatMap(builder =>
    Object.keys(FIXTURES[builder]).map(state => {
        const s = FIXTURES[builder][state];
        return {
            id: builder + '/' + state,
            builder,
            state,
            cellId: cellId(builder, state),
            description: s.description,
            tags: s.tags || [],
            ticks: s.ticks ? s.ticks.length : 0,
        };
    }));
