/* pgwt — compare request coordination (impure transport edge only).
 * Delta arithmetic stays in builders/compare.js; this helper only classifies
 * a B-only failure so views can keep A visible. Superseded channel requests
 * remain cancellations and must be dropped by the owning refresh epoch. */

import { CancelledError } from './transport.js';

export async function settleBaseline(request) {
    try {
        return { ok: true, payload: await request };
    } catch (error) {
        if (error instanceof CancelledError) throw error;
        return { ok: false, error };
    }
}
