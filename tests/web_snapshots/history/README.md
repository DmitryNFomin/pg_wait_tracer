# tests/web_snapshots/history/

One file per baseline-regeneration event: `<YYYY-MM-DD>-<issue-or-track>-<slug>.md`.
See `../VERSION` for the rule this directory satisfies (RULE 3: a commit
touching `tests/web_snapshots/*.png` must add or modify a file here) and for
the current playwright/chromium pins and the CI-only regeneration policy.

Splitting one file per event, instead of appending to a single prose log, is
the whole point: two branches that each regenerate a different baseline and
each add their own history file never touch the same file, so they can never
conflict on this bookkeeping the way two branches appending to one `VERSION`
always did before 2026-09-26.

Read every entry here, in order, before regenerating anything — particularly
the flexbox neighbour-drift mechanism (issue #122: any gallery cell
insertion re-wraps its row and re-rasterizes unrelated row-mates' glyphs —
see the 2026-09-18 and 2026-09-19 entries) and the cross-session font-drift
fix that pinned the snapshots job to a container image (issue #159: see the
2026-09-26 entry).

## A note on the migrated entries

The entries dated 2026-09-26 and earlier were migrated verbatim, one event
per file, out of a single append-only `VERSION` file that used to hold all
of this prose (see git history before 2026-09-26 for that layout). They were
not reworded or summarized in the split, so a cross-reference inside one of
them — "the pins already recorded at the top of this file", "see VERSION",
"entry above" — refers to that original single-file layout: "the top of this
file" / "above" means `VERSION`'s pins section (RULE 1), not this directory.
Later entries, once this directory exists, should reference other files here
by name.
