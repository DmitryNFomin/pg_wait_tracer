#!/bin/sh
# After the normal project build, prove the regression test detects #355.
# Temporarily mutates server.c; always restores it, including on interruption.
set -eu
cd "$(dirname "$0")/../../.."
mkdir -p build/issue355-redgreen
backup=$(mktemp)
cp src/server.c "$backup"
trap 'cp "$backup" src/server.c; rm -f "$backup"' EXIT
trap 'exit 1' HUP INT TERM

python3 - <<'PY'
from pathlib import Path
p = Path("src/server.c")
text = p.read_text()
charged = """           (size_t)a->key_cap * sizeof(*a->keys) +
           (size_t)a->ev_cap * sizeof(*a->events);"""
assert text.count(charged) == 1, "mutation anchor must occur exactly once"
p.write_text(text.replace(charged, "           (size_t)a->key_cap * sizeof(*a->keys);"))
PY
make -C tests test_bagg_cache > build/issue355-redgreen/red-build.log 2>&1
rc=0
tests/test_bagg_cache > build/issue355-redgreen/red.txt 2>&1 || rc=$?
cat build/issue355-redgreen/red.txt
printf 'Dropped event-capacity term: exit %d (required: 1)\n' "$rc"
[ "$rc" -eq 1 ]

cp "$backup" src/server.c
rm -f "$backup"
trap - EXIT HUP INT TERM
make -C tests test_bagg_cache > build/issue355-redgreen/green-build.log 2>&1
tests/test_bagg_cache > build/issue355-redgreen/green.txt 2>&1
cat build/issue355-redgreen/green.txt
printf 'Restored event-capacity term: exit 0\n'
