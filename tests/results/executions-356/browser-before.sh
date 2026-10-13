#!/usr/bin/env bash
echo "EXSERVER pid=$$" >> /root/cx-executions-356/evidence/browser-before.stderr
exec env PGWT_CURRENT_TRACE_CACHE_STATS=1 /root/cx-executions-356/before/pgwt-server "$@" 2>> /root/cx-executions-356/evidence/browser-before.stderr
