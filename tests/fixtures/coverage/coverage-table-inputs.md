# Coverage verdict — example inputs (no answers)
Rules: m871/spec-coverage-label-r2.md §1 (record kinds, floors, one file = one boot) and §2 (model, holes, live, covered piece, taint, states, reason order, idle verdict, output). Use nothing else.
For every window give: verdict (complete / live_edge / incomplete / error), reasons[], covered_to (live_edge only), uncovered ranges with one reason each, restart_cause if any, excludes_in_flight, and the idle verdict when it differs from the DB verdict.

Notation: `B[S,W,state]` = a boot (S = start, W = last watermark); `wm W/fd/fi` = a WATERMARK record with W, floor_db fd, floor_idle fi (fi omitted = fd); `G(s,e)` = a closed gap from s to e; `GO(s)` = a gap opened at s and never closed. Gap records carry no taint; derive it. A listed wm is the last one at or before the next listed gap. `obs` = time of the last COVERAGE record written; `fN` = file_no N of the boot.
Defaults unless a row says otherwise: one generation, wall = mono, L = 3, now = 10000, sealed boots have closure CLEAN and their last wm is W/W, all boots share the server's kernel boot_id and the same postmaster start.

| # | setup | windows |
|---|---|---|
| 1 | B1[0,100,sealed] | a [10,50) · b [10,100) · c [90,110) |
| 2 | B1[0,100,sealed], B2[130,200,sealed] | a [50,150) · b [100,130) · c [130,200) |
| 3 | B1[0,100,sealed], B2[100,200,sealed] | [50,150) |
| 4 | as 2, but B2's postmaster start differs from B1's | [90,140) |
| 5 | B1[0,100,unsealed], last wm 100/95 | a [10,90) · b [90,96) |
| 6 | B1[0,100,unsealed], last wm 100/100 | a [50,100) · b [50,101) |
| 7 | B1 live [0,100], last wm 100/40, obs 100.5, now 101 | a [50,100) · b [40,101) · c [50,105) |
| 8 | as 7, but now 110 and no record after 100.5 | [50,100) |
| 9 | B1 live [0,60], wm 60/55 repeated every tick, GO(60), obs 79.5, now 80 | a [10,50) · b [50,58) · c [70,80) |
| 10 | B1[0,200,sealed], wm 58/55, G(60,70) | a [70,100) · b [20,55) · c [20,56) |
| 11 | B1[0,200,sealed], wm 78/75; W reached 150, and only then G(80,160) was appended | [90,100) |
| 12 | B1[0,200,sealed], wm 59/55, GO(60) | [100,150) |
| 13 | B1[0,1000,sealed]; the earliest retained file of B1 has BOOT obs 400 | a [300,500) · b [400,500) |
| 14 | B1 sealed: f0 (BOOT obs 0, last wm 100/90), f1 missing, f2 (BOOT obs 200, SEAL 300) | a [50,150) · b [210,250) · c [50,90) |
| 15 | only v4 trace files, data spanning [0,100) | [10,20) |
| 16 | v4 trace data spanning [0,100); B2[120,200,sealed] | [90,130) |
| 17 | B1 in generation 0 (wall = mono + 1000), mono [0,100), sealed; B2 in generation 1 (wall = mono + 5000), mono [10,50), sealed. Windows are wall time | a [1050,1080) · b [1090,5020) |
| 18 | B1[0,100]: SEAL present but header closure UNCLEAN, last wm 100/90 | [80,95) |
| 19 | B1 live [0,100], wm 97.5/97, G(98,99), last wm 100/100, obs 100.5, now 101 | [90,100) |
| 20 | master path: B1[0,100,sealed], wm 49/45; the counter vector changed on the next tick, writing G(49,50) | a [46,48) · b [40,44) |
| 21 | B1[0,100,sealed]; GO(0.5) written before any wm | [0,0.4) |
| 22 | B1[0,100,sealed] | [50,50) |
| 23 | B1[100,200,sealed] is the only boot; no legacy data | [50,150) |
| 24 | B1[0,100,sealed]; B2 live [130,200], last wm 200/180, obs 200.5, now 201 | a [140,170) · b [140,190) · c [90,150) · d [150,203) |
| 25 | B1 sealed: f0 (BOOT obs 0, wm 80/75, then a record with a bad CRC), f1 (BOOT obs 150, SEAL 200) | a [70,100) · b [160,190) · c [10,75) |
| 26 | B1 live [0,60], wm 60/58 written every tick (W stalled), obs 99.5, now 100 | a [10,50) · b [55,62) · c [50,70) |
| 27 | B1 sampled tier [0,100,unsealed], last wm 100/100 | [50,100) |
| 28 | B1 tiered mode [0,100,unsealed], escalated at 40, last wm 100/70 | a [20,70) · b [20,80) |
| 29 | B1[0,100,unsealed], last wm 100/95/20 | [50,90) |
| 30 | B1 live [0,100], last wm 100/100, obs 100.5, now 101 | [50,100) |
