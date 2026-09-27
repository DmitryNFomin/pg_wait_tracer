#!/usr/bin/env python3
"""orphan_guard_decide.py -- stdin/stdout adapter for orphan-guard.sh.

Reads a Claude Code PreToolUse hook payload (JSON) on stdin, calls
orphan_guard_lib.decide(), and prints one tab-separated line:
    <kind>\t<session_id>\t<code>\t<reason with newlines stripped>

kind is one of "allow", "mark", "refuse". Any parse/import failure raises
(non-zero exit, no output) so the caller (orphan-guard.sh) can fail closed
instead of treating a blank line as "allow".
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orphan_guard_lib as lib

data = json.load(sys.stdin)
tool_name = data.get("tool_name", "")
tool_input = data.get("tool_input", {}) or {}
command = tool_input.get("command", "")
rib = tool_input.get("run_in_background", False)
session_id = data.get("session_id", "unknown") or "unknown"

d = lib.decide(tool_name, command, rib, session_id)
print("\t".join([d.kind, session_id, d.code or "", (d.reason or "").replace("\n", " ")]))
