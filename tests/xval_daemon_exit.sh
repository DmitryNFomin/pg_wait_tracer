#!/bin/sh
# ExecStopPost receives the service's real exit status, before --collect unloads
# it. A synchronous systemd-run start lets the caller's signal trap stop a unit
# whose creation has completed, without racing an asynchronous launch client.
printf '%s:%s\n' "${EXIT_CODE:-unknown}" "${EXIT_STATUS:-unknown}" > "$1"
