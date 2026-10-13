#!/usr/bin/env bash
set -euo pipefail
cd /root/cx-matrix-regression
mkdir -p audit
exec 9>/tmp/pgwt-box-check.lock
echo 'Waiting for exclusive measurement lock'
flock -x 9
echo 'Acquired exclusive measurement lock'
date -u > audit/host.txt
uname -a >> audit/host.txt
lscpu >> audit/host.txt
gcc --version >> audit/host.txt
tar xf checkpoints.tar
python3 - <<'PY'
import hashlib,json
from pathlib import Path
for arm, expected in json.loads(Path('source-manifest.json').read_text()).items():
    actual=hashlib.sha256((Path(arm)/'src/server.c').read_bytes()).hexdigest()
    assert actual == expected['server_sha256'], (arm, actual, expected)
print('Reused sources and historical checkpoints match the manifest')
PY
for arm in before329 after329 after340 after365 before_refusal after_refusal; do
    make -C "$arm" -j4 pgwt-server PGWT_BUILD_VERSION=unknown >"audit/$arm-build.log" 2>&1 9>&-
done
make -C final pgwt-client PGWT_BUILD_VERSION=unknown >audit/client-build.log 2>&1 9>&-
sha256sum */pgwt-server final/web/pgwt >audit/binaries.sha256
python3 audit_measure.py >audit/measure.jsonl 9>&-
python3 audit_sequence.py >audit/sequence.jsonl 9>&-
python3 audit_browser.py >audit/browser.jsonl 9>&-
date -u >> audit/host.txt
echo 'Measurements complete'
