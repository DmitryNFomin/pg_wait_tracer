#!/usr/bin/env bash
echo "MXSERVER pid=$$" >> /root/cx-matrix-regression/audit/browser-server-final-0.stderr
exec env PGWT_CURRENT_TRACE_CACHE_STATS=1 PGWT_TRANSITIONS_PROVENANCE=1 /root/cx-matrix-regression/final/pgwt-server "$@" 2>> /root/cx-matrix-regression/audit/browser-server-final-0.stderr
