#!/bin/bash
for r in 1 2 3; do
  ./trw.sh v12t$r trace on
  ./trw.sh v12o$r off on
done
echo ALLDONE
