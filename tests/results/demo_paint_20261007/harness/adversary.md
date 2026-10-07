# Three adversarial questions, answered BEFORE the harness was written

1. What input makes this check pass while the product is broken?
   An EMPTY or STALE panel. PANEL_CHECKS returning ok on a 0-row table, or a
   tab still showing the previous tab's chart, scores a fast paint on nothing;
   a dead daemon gives "No data for selected range" and a fake >15x margin
   (DEMO_REHEARSAL_CRITERIA's own retracted trap). Pins: every tab's detail
   string (row/cell/node counts) is recorded, the active range button is
   asserted "15m", daemon+pgbench+workload are checked alive after every walk,
   and the in-window record count is read server-side from the trace.

2. What single component failing makes this hang or skip rather than fail?
   The Go bridge. If it dies mid-walk every PANEL_CHECK times out at 15 s,
   paint_walk.py records samples=[] and then EXCLUDES that tab from `worst` --
   a broken tab makes the margin look BETTER. Fixed in paint_analyse.py:
   any errored tab, or any tab short of its expected sample count, makes the
   verdict REFUSE instead of reporting a number.

3. What here depends on timing or ordering, and what pins it?
   (a) window fullness -- >=900 s wall after daemon start on an EMPTY trace dir,
   capture age recorded; (b) viewport -- the demo Chrome profile persists zoom,
   so Cmd+0 then two agreeing reads asserted against 1710x981@2; (c) tab order
   -- timeline LAST (its drill-down sets a pid filter) and every navigation is
   a full page reload, so nothing is measured warm; (d) box contention --
   /tmp/pgwt-box-check.lock held for the whole session, load average recorded.
