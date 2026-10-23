#!/bin/bash
# Case 5, second set: 1.3-CPU limit (~72% average use, like gate-2's 1-CPU B1), single ring dump after the window.
cd /root/t
export REPORT_RATE=0.2
for r in Q0o:none:off Q13o:130%:off Q13t:130%:trace Q13o2:130%:off Q13t2:130%:trace Q0t:none:trace Q25t:250%:trace; do IFS=: read tag lim cap <<< "$r"; ./c5.sh $tag $lim 1400 $cap; done
echo C5B-ALLDONE
