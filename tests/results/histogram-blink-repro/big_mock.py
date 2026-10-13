#!/usr/bin/env python3
"""tests/mock_server.py with a REALISTIC heatmap payload.

The canned heatmap is 60 time buckets (20 populated columns) x 16 bands.
A real live window asks for BUCKETS() = min(innerWidth/6, 300) = 285
buckets at the pinned 1710px demo viewport, and a loaded capture fills
nearly all of them. PGWT_HM_COLS / PGWT_HM_DENSE control the grid.
"""
import os
import sys

sys.path.insert(0, os.environ["PGWT_TESTS_DIR"])
import mock_server as ms  # noqa: E402

cols = int(os.environ.get("PGWT_HM_COLS", "285"))
step = int(os.environ.get("PGWT_HM_STEP", "1"))
labels = ms._CANNED["heatmap"]["labels"]
rows = len(labels)
cells = []
for i in range(0, cols, step):
    for j in range(rows):
        v = max(0, 5000 - abs(j - 3) * 300 - abs(i - cols // 2) * 10)
        if v > 0:
            cells.append([i, j, v])
ms._CANNED["heatmap"] = {
    "bucket_ns": ms._BUCKET_NS,
    "max_count": 5000,
    "total_events": sum(c[2] for c in cells),
    "times": [ms._FROM_NS + i * ms._BUCKET_NS for i in range(cols)],
    "labels": labels,
    "cells": cells,
}
print("big_mock: heatmap %d cols x %d rows, %d cells"
      % (cols, rows, len(cells)), flush=True)

ms.main()
