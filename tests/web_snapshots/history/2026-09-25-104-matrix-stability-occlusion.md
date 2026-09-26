# 2026-09-25 — #104: matrix STABILITY/OCCLUSION fixes


2026-09-25 (#104, matrix STABILITY/OCCLUSION fixes): TWO baselines
regenerated — transition_matrix.png and gallery/matrix-dense-top20.png.
buildMatrixOption's grid/xAxis/yAxis geometry changed (axis names moved
off the tick labels: xAxis 'Source event' nameLocation 'middle' centered
below the rotated labels instead of the default axis-end position that
clipped it to "Sour" at 1280px; yAxis 'Target event' nameLocation
'middle' as a vertical strip inside the reserved left label margin
instead of overstriking the first x tick label "CPU*"; legend bucket max
now quantized to a fixed 1-2-5-10 log grid). `make ui-gallery` (local,
BASE=merge-base with master): 2 changed (gallery/matrix-dense-top20
9.56%, transition_matrix 3.74%), 0 added, 0 removed, 42 unchanged — both
expected from the geometry change, no unrelated cell moved. Regenerated
via CI run 36029149093 (workflow_dispatch update_snapshots=true,
ubuntu-latest, pins already recorded above); no local PNG committed.
