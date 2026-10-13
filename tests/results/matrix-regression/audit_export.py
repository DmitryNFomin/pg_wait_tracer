"""Export pristine historical checkpoints; reuse the audited diagnostic trees.

Run from the repository root. The earlier prepare.py produces the diagnostic
trees under /tmp/cx-matrix-regression-src; no repository source is modified.
Upload checkpoints.tar, source-manifest.json, and audit_*.py/sh to the remote
measurement root. audit_run.sh serializes builds and measurements under flock.
"""
from pathlib import Path
import hashlib
import io
import json
import subprocess
import tarfile

OUT = Path('/tmp/cx-matrix-audit')
OUT.mkdir(exist_ok=True)
COMMITS = dict(before329='c52a892', after329='b6cb1ec', after340='f78ab4c',
               after365='2b2147f', before_refusal='f5a856e', after_refusal='831102f')
manifest = {}
with tarfile.open(OUT / 'checkpoints.tar', 'w') as archive:
    for arm, sha in COMMITS.items():
        full = subprocess.check_output(['git', 'rev-parse', sha], text=True).strip()
        raw = subprocess.check_output(['git', 'archive', sha, 'src', 'scripts', 'Makefile'])
        with tarfile.open(fileobj=io.BytesIO(raw)) as tree:
            for entry in tree:
                stream = tree.extractfile(entry) if entry.isfile() else None
                entry.name = arm + '/' + entry.name
                archive.addfile(entry, stream)
        manifest[arm] = dict(commit=full, server_sha256=hashlib.sha256(
            subprocess.check_output(['git', 'show', sha + ':src/server.c'])).hexdigest())
for arm in ['pre', 'final', 'aggregate', 'oldsort']:
    source = Path('/tmp/cx-matrix-regression-src') / arm / 'src/server.c'
    manifest[arm] = dict(commit='b907ec0' if arm == 'pre' else '612bd98',
                         server_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
(OUT / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(OUT)
