# Motion pass over the rehearsal recordings — findings

Material: `tests/results/demo_rehearsal_907e070_{1,2}_*/demo_rehearsal/<phase>/<tab>/<tab>.webm`,
11 tabs x 3 phases x 2 runs = **66 recordings, all 66 scanned** (run 2 first, then run 1, per the
brief's priority order — both finished inside the time budget).

## Negative control (mandatory, done first)

The brief's known defect: run 2, `late/waterfall`, a grey chart-only frame at ~00:10 where the
table and disclosure vanish.

**First attempt failed it.** Extracting at 4 fps (as the brief's method section suggested) and
running the spike-return detector produced **zero** transients between t=8.5s and t=11.75s in
that recording — every sampled frame in that window was byte-for-byte static:

```
34 8.5   0.0256
35 8.75  0.0
36 9.0   0.0
...
40 10.0  0.0
...
47 11.75 0.0
```

Decoding the same window at the source's native 25 fps showed why: the real defect is a rapid
good/blank alternation (period ≈ 4-5 native frames, ≈160-200 ms) that happens to fall almost
exactly on the 4 fps grid's blind spots in that particular window — an aliasing/moiré miss, not
an absent defect (native-rate diffs at t=9.96s and t=10.08s are 0.85, i.e. the defect is there).
**Conclusion: 4 fps was the wrong sampling rate and the brief's own warning applied — "your
threshold is wrong and everything else is untrustworthy."** Re-extracted at **8 fps** and the
same detector now flags it cleanly:

```
peak_t=10.0s  diff_in=0.8516  diff_return=0.0  flat_colour=(127,125,129) grey
```

Saved at `candidates/waterfall-late-run2-t10.0-NEGATIVE-CONTROL{,-prev,-next}.png` — frame -prev
is the normal full UI, the flagged frame is the grey chart-only state, frame -next is back to
normal. **All subsequent extraction in this pass used 8 fps**, not the 4 fps the brief's method
section proposed as an example; that one parameter choice is the difference between confirming
and missing the known defect, so every number below depends on it.

## Headline finding

This is not "one grey frame, found by accident." The same failure mode — a page region
flashing to a flat colour and recovering within 1-2 frames, then returning to the
pre-flicker content — recurs constantly across the whole demo, on both runs:

- **961 raw frame-to-frame spike-return events, clustering into 378 distinct flicker episodes**
  (gap ≤ 1.0 s merges consecutive raw hits from the same episode into one; see Method).
- **56 of 66 recordings (85%) show at least one episode.** The only clean tabs are **Sessions**
  (0 of 6 recordings, 0 episodes) and **Events** outside the `early` phase (0 of 4; `early` shows
  a related but visually different glitch, see below). Every other tab — Overview, Queries,
  Histogram, Timeline, Transitions, Concurrency, Waterfall, Scatter, Matrix — shows it in nearly
  every recording.
- Two visually distinct sub-patterns, both confirmed by direct frame inspection, not just the
  pixel-diff number:
  - **`grey-chart-blank`** (204 of 378 episodes): the already-known shape — a content/table
    region goes a flat mid-grey (`~rgb(126,126,126)`) for 1-2 sampled frames while a sibling
    region keeps rendering. Seen in Waterfall (below the gantt chart), Histogram and Scatter
    (surrounding chrome), Timeline, Concurrency.
  - **`navy-chrome-blank`** (171 of 378 episodes): the inverse — the page header, CPU-class
    chips and tab bar vanish into the plain navy page background while the tab's own content
    (e.g. the Overview stat table, or the Events wait-event table) stays rendered. Confirmed
    visually on `overview-late-run2` (t=28.4s) and `events-early-run1` (multiple, see below).
  - 3 further episodes (all on `scatter`) show the same chrome-blanking but the tab's own content
    is itself high-variance (scatter dots), so the simple modal-colour classifier doesn't call them
    "flat" — visual check confirms the identical chrome-blank pattern around the dot cloud.
- Both sub-patterns are the same underlying failure (a page region clearing to its background
  colour and repainting within roughly one 8 fps frame, ≤250ms), observed on two different
  layout regions. I'm reporting them together for that reason rather than as unrelated bugs.

### A distinct, separate, single-frame finding

`waterfall-late-run1`, t=27.5s (`candidates/waterfall-late-run1-t27.5.png`, the single worst
frame in the whole pass, diff 0.986): one frame where almost the whole 1710x980 viewport
collapses to a small thumbnail of the chart in the top-left corner over black, with the rest of
the frame flat grey — then the very next sampled frame (125ms later) is back to the full,
correctly laid-out page. This looks like a viewport-resize event, not the same chrome-blank
pattern (different shape: the chart content shrinks rather than a region blanking). I cannot
tell from the recording alone whether this is a product bug or a Playwright/recorder viewport
hiccup — flagging it with that uncertainty rather than asserting either.

### The `events-early` outlier

`events-early-run1` (17 episodes) and `events-early-run2` (8 episodes) are the only `events-*`
recordings with any hits, all confined to the `early` phase, all lower magnitude (~0.26 vs ~0.85
for the chrome-blank pattern) than the main finding. Visual check of the worst one
(`events-early-run1`, t=6.6s) shows the page header/tab-bar region missing **and** the table
reflowing upward to fill the gap, with ghosted/doubled column-header text — consistent with the
same chrome-blank defect but caught mid-CSS-transition rather than as a clean before/blank/after
triple, which is why the diffs are smaller and messier. Not a separate bug by my reading, but
visually different enough from the other two sub-patterns that it is called out on its own.

## Ranked table — top 40 of 378 episodes by peak magnitude

(Full 378-row data: `events.json`. Full per-recording counts: `summary.json`.)

| tab | phase | run | t | peak diff (frac pixels changed) | pattern (raw samples in episode) |
|---|---|---|---|---|---|
| waterfall | late | run1 | 27.500s | 0.9863 | distinct single-frame viewport-collapse (see above), not the chrome-blank pattern (4) |
| concurrency | late | run2 | 14.500s | 0.9662 | grey-chart-blank (2) |
| timeline | late | run2 | 14.000s | 0.9648 | grey-chart-blank (2) |
| timeline | late | run1 | 18.375s | 0.9647 | grey-chart-blank (5) |
| timeline | middle | run2 | 28.625s | 0.9646 | grey-chart-blank (1) |
| timeline | middle | run1 | 18.875s | 0.9645 | grey-chart-blank (2) |
| timeline | early | run2 | 22.250s | 0.9619 | grey-chart-blank (2) |
| timeline | late | run1 | 25.125s | 0.9607 | grey-chart-blank (2) |
| timeline | middle | run2 | 24.875s | 0.9607 | grey-chart-blank (1) |
| timeline | early | run2 | 34.000s | 0.9601 | grey-chart-blank (4) |
| histogram | middle | run1 | 36.250s | 0.9070 | grey-chart-blank (3) |
| scatter | late | run1 | 20.500s | 0.9057 | grey-chart-blank (3) |
| histogram | middle | run2 | 24.500s | 0.9052 | grey-chart-blank (1) |
| histogram | late | run2 | 25.250s | 0.9045 | grey-chart-blank (1) |
| histogram | late | run2 | 35.125s | 0.9045 | navy-chrome-blank (3) |
| histogram | late | run1 | 35.125s | 0.9044 | grey-chart-blank (4) |
| histogram | middle | run1 | 14.000s | 0.9044 | grey-chart-blank (1) |
| histogram | middle | run2 | 7.875s | 0.9044 | grey-chart-blank (4) |
| histogram | middle | run1 | 7.500s | 0.9042 | grey-chart-blank (3) |
| histogram | middle | run1 | 29.250s | 0.9041 | grey-chart-blank (5) |
| histogram | late | run2 | 18.125s | 0.9040 | grey-chart-blank (3) |
| histogram | late | run2 | 6.750s | 0.9038 | grey-chart-blank (2) |
| histogram | middle | run1 | 24.750s | 0.9037 | grey-chart-blank (1) |
| histogram | late | run2 | 23.750s | 0.9036 | grey-chart-blank (2) |
| histogram | middle | run1 | 18.500s | 0.9036 | grey-chart-blank (4) |
| histogram | middle | run2 | 36.125s | 0.9004 | grey-chart-blank (3) |
| histogram | middle | run2 | 18.875s | 0.8986 | grey-chart-blank (4) |
| histogram | late | run1 | 24.125s | 0.8979 | grey-chart-blank (3) |
| histogram | late | run2 | 30.125s | 0.8977 | grey-chart-blank (4) |
| histogram | late | run1 | 19.500s | 0.8974 | grey-chart-blank (2) |
| histogram | middle | run2 | 13.875s | 0.8972 | grey-chart-blank (4) |
| histogram | middle | run2 | 28.750s | 0.8971 | grey-chart-blank (2) |
| histogram | middle | run2 | 23.250s | 0.8970 | grey-chart-blank (2) |
| histogram | middle | run2 | 30.000s | 0.8970 | grey-chart-blank (2) |
| histogram | late | run1 | 12.500s | 0.8968 | grey-chart-blank (3) |
| histogram | late | run1 | 29.000s | 0.8966 | grey-chart-blank (4) |
| histogram | late | run2 | 12.500s | 0.8963 | grey-chart-blank (5) |
| histogram | late | run1 | 8.375s | 0.8961 | grey-chart-blank (4) |
| timeline | late | run2 | 9.750s | 0.8728 | grey-chart-blank (5) |
| timeline | middle | run1 | 10.125s | 0.8728 | grey-chart-blank (4) |

**`waterfall-late-run2` t=10.0s (diff 0.8516) is the brief's known defect; it ranks 73rd of 378 by
magnitude** — well below this top-40, i.e. it is a middling, not even the worst, instance of a
defect that happens far more often and sometimes far more severely than the one instance anyone
had previously seen. Its candidate images are saved regardless of rank (see Method).

## Method

1. `ffmpeg -vf fps=8` per recording (not 4 fps — see Negative control above for why).
2. Per consecutive frame pair: `frac = count(max-channel abs diff > 24) / total_pixels`.
3. Transient test at frame i: `frac(i-1,i) >= 0.06` (spike in) AND
   `frac(i-1,i+1) < max(0.02, 0.4 * frac(i-1,i))` (frame i+1 resembles the pre-spike baseline,
   i.e. it returned). This is what rejects "live data updating" — a monotonically evolving
   chart never returns to an earlier visual state, so it never satisfies the return clause
   regardless of how large its frame-to-frame diff is.
4. Flat-frame check: median colour over a 4x4-downsampled frame; frame counted "flat" if ≥90% of
   pixels are within 8 per channel of that median. Used only for classification/description
   above, not for the transient gate itself.
5. **Clustering**: raw transient hits within the same recording whose timestamps are ≤1.0s apart
   are merged into one episode (this is what turns 961 raw hits into 378 reported episodes — see
   Honesty below for why that matters).
6. Candidates saved: the mandatory negative control (always, regardless of rank) plus the top 60
   episodes by peak magnitude, each as flagged + immediate 8fps-neighbour PNGs.

## Honesty / what this pass did not do

- **I did not visually inspect all 378 episodes.** I spot-checked ~10 across different tabs,
  magnitudes and both sub-patterns (waterfall, histogram, overview, scatter, events) to confirm
  the pixel-diff signal corresponds to a real visual defect and not a codec artifact, and all
  matched one of the two chrome-blank sub-patterns described above. The full raw data
  (`events.json`, `summary.json`) is there for anyone who wants to check a specific one; I am not
  claiming each of the 378 individually verified.
- **The 961-to-378 clustering is a judgement call**, not a brief requirement. Reporting 961
  near-duplicate rows (many are the same ~1s flicker sampled 2-5 times at 8fps) would have
  padded the table without adding information; reporting only 378 could be read as hiding volume,
  so both the raw and clustered totals are stated above and the full raw list is derivable from
  `events.json`'s `n_raw_samples` field per episode.
- **Did not determine root cause.** This is a video analysis pass only — no product code was
  read or changed. The pattern (a page region clearing to background colour and repainting
  within ~1 sampled frame, specifically on chart-bearing tabs, never on Sessions) is consistent
  with a live-refresh re-render that briefly unmounts/clears a region before repopulating it, but
  that is a hypothesis for whoever picks this up, not a finding I verified against source.
- **Scatter's 3 "other"-colour episodes** were classified by eye, not by the automatic
  flat-colour heuristic (their own tab content is too high-variance for the modal-colour check to
  call them "flat") — flagged as a known gap in the automatic classifier, not in the underlying
  detector.
