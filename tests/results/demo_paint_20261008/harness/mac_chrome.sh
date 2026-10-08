#!/usr/bin/env bash
# Mac side: launch the DEMO Chrome profile with the CDP port the walk needs.
# Same binary, same --user-data-dir and flags as ~/pgwt-demo-chrome.command;
# --remote-debugging-port is automation only and does not affect layout.
# Reuses an already-running instance if one answers on 9333 (one Chrome per
# measurement session, not one per walk).
set -uo pipefail
if curl -s -m 2 http://127.0.0.1:9333/json/version >/dev/null 2>&1; then
    echo "reusing the Chrome already on 9333:"
    curl -s -m 3 http://127.0.0.1:9333/json/version
    exit 0
fi
holder=$(lsof -nP -iTCP:9333 -sTCP:LISTEN -t 2>/dev/null)
if [ -n "$holder" ]; then
    echo "9333 held but not answering; holder:"; ps -p "$holder" -o pid=,args=
    exit 1
fi
nohup /Applications/Google\ Chrome.app/Contents/MacOS/Google\ Chrome \
  --user-data-dir="$HOME/.pgwt-demo-chrome-profile" --no-first-run \
  --no-default-browser-check --disable-session-crashed-bubble \
  --remote-debugging-port=9333 http://localhost:8384/ > /dev/null 2>&1 &
disown
for _ in $(seq 1 40); do
    curl -s -m 2 http://127.0.0.1:9333/json/version >/dev/null 2>&1 && break
    sleep 2
done
curl -s -m 3 http://127.0.0.1:9333/json/version || { echo "CHROME DID NOT COME UP"; exit 1; }
