#!/usr/bin/env bash
set -euo pipefail
cd /root/cx-executions-356
exec 9>/tmp/pgwt-box-check.lock
flock -x 9
make -C after -j4 pgwt-server PGWT_BUILD_VERSION=measurement >after-build.log 2>&1
python3 measure.py "$PWD" /tmp/pgwt_cx_rehearsal_tqWPfm after >after.jsonl
make -C before pgwt-client PGWT_BUILD_VERSION=measurement >client-build.log 2>&1
python3 browser.py "$PWD" /tmp/pgwt_cx_rehearsal_tqWPfm before >browser-before.jsonl
python3 browser.py "$PWD" /tmp/pgwt_cx_rehearsal_tqWPfm after >browser-after.jsonl
make -C final -j4 >final-build.log 2>&1
make -C final/tests -j4 >unit-build.log 2>&1
(
    cd final/tests
    while read -r t; do
        case "$t" in ''|\#*) continue ;; esac
        echo "RUN $t"
        case "$t" in *.py) python3 "$t" ;; *) ./"$t" ;; esac
    done < unit_tests.list
) >unit-results.log 2>&1
(cd final/tests && python3 test_data_executions.py && python3 test_data_exec_prefix.py) >data-results.log 2>&1
sha256sum before/pgwt-server after/pgwt-server final/pgwt-server >binaries.sha256
date -u >>evidence-host.txt
