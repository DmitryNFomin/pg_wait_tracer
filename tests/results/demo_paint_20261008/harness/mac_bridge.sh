#!/usr/bin/env bash
# Mac side: (re)start the Go bridge pointed at ONE run's trace dir on gate-2,
# and prove the one now listening is the one we started and points at this box.
# A stale bridge from an earlier session once pointed at a different box and a
# pre-fix binary, and nothing noticed.
set -uo pipefail
WT=/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-afa9ff9486d78f624
BOX=2.28.45.47
RUN="${1:?usage: mac_bridge.sh RUNLABEL}"
PORT=8384
LOG="/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/258d73f0-4867-4c95-9aca-9a7966648e2b/scratchpad/paint1/bridge-$RUN.log"

# Never pkill -f: that pattern matches this script's own command line.
# Find the bridge by the port it holds, inspect it, then kill by pid.
holder=$(lsof -nP -iTCP:$PORT -sTCP:LISTEN -t 2>/dev/null)
if [ -n "$holder" ]; then
    echo "port $PORT held by:"; ps -p "$holder" -o pid=,args=
    kill "$holder" 2>/dev/null
    for _ in $(seq 1 20); do
        lsof -nP -iTCP:$PORT -sTCP:LISTEN -t >/dev/null 2>&1 || break
        sleep 1
    done
fi
lsof -nP -iTCP:$PORT -sTCP:LISTEN -t >/dev/null 2>&1 && { echo "port $PORT still held"; exit 1; }

cd "$WT/web" || exit 2
nohup ./pgwt --trace-dir "/root/paint1_trace_$RUN" \
    --server-path /root/pgwt-paint-20261008/pgwt-server --port $PORT "root@$BOX" \
    > "$LOG" 2>&1 &
disown
for _ in $(seq 1 30); do
    curl -sf -m 3 "http://localhost:$PORT/" >/dev/null 2>&1 && break
    sleep 1
done
code=$(curl -s -m 5 -o /dev/null -w '%{http_code}' "http://localhost:$PORT/")
pid=$(lsof -nP -iTCP:$PORT -sTCP:LISTEN -t 2>/dev/null | head -1)
echo "bridge http $code, pid $pid"
args=$(ps -p "$pid" -o args= 2>/dev/null)     # -o args=, never -o command= (truncates)
echo "bridge args: $args"
case "$args" in
    *"paint1_trace_$RUN"*"$BOX"*) echo "BRIDGE_OK" ;;
    *) echo "BRIDGE IS NOT POINTING AT $BOX:/root/paint1_trace_$RUN"; exit 1 ;;
esac
