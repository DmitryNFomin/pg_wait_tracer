#!/usr/bin/env python3
"""Small JSON adapter for hetzner-sweep.sh; safety decisions stay in Bash."""

import json
import sys


def jq_text(value):
    if isinstance(value, str):
        return value
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def default_empty(value):
    # jq's `// ""` treats false and null as absent.
    return "" if value is None or value is False else value


def main():
    mode = sys.argv[1]
    try:
        data = json.load(sys.stdin)
        if mode == "validate":
            servers = data.get("servers")
            return 0 if servers is not None and servers is not False else 1
        if mode == "delete-error":
            error = data.get("error") or {}
            message = default_empty(error.get("message"))
            if message != "":
                print(jq_text(message))
            return 0
        if mode == "servers":
            servers = data["servers"]
            if isinstance(servers, dict):
                servers = servers.values()
            for server in servers:
                labels = server.get("labels") or {}
                fields = (default_empty(server.get("id")),
                          default_empty(server.get("name")),
                          default_empty(labels.get("pgwt")),
                          default_empty(labels.get("created")))
                # Match jq @tsv's escaping so values cannot shift columns.
                print("\t".join(jq_text(field).replace("\\", "\\\\")
                                .replace("\t", "\\t").replace("\n", "\\n")
                                .replace("\r", "\\r") for field in fields))
            return 0
    except (ValueError, TypeError, KeyError, AttributeError):
        return 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
