#!/usr/bin/env bash
# Verify every referenced issue/PR against comments fetched during this run.
# Exit 0: covered; 1: incomplete brief; 2: usage, IO, or live-query error.
set -u
if [ "$#" -ne 1 ]; then
  echo 'usage: check-brief.sh <brief-file>' >&2
  exit 2
fi
command -v python3 >/dev/null 2>&1 || { echo 'CHECK BRIEF ERROR: python3 unavailable' >&2; exit 2; }

python3 - "$1" <<'PY'
import fcntl
import hashlib
import json
import os
import re
import subprocess
import sys
import time

STATE = os.environ.get("PGWT_LEAD_STATE_DIR") or "/Users/dmitryfomin/work/git/pg_wait_tracer/scripts/lead"


def record_pass(data):
    with open(os.path.join(STATE, ".brief-gate.lock"), "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with open(os.path.join(STATE, ".briefs-passed"), "a") as records:
            records.write(f"{int(time.time())} {hashlib.sha256(data).hexdigest()} "
                          f"{os.path.abspath(sys.argv[1])}\n")


COMMENTS_COMMAND = re.compile(
    r"\bgh\s+api\s+repos/[^/\s]+/[^/\s]+/issues/([0-9]{1,5})\b"
    r"/comments(?=$|[\s\"'`])")


def fenced_blocks(text):
    """Collect closed fences with their start/end line indices."""
    blocks = []
    opening = None
    content = []
    for index, line in enumerate(text.splitlines()):
        if opening is None:
            match = re.match(r"^ {0,3}(`{3,}|~{3,})(.*)$", line)
            if match:
                opening = match.group(1)
                start = index
                content = []
        elif re.fullmatch(r" {0,3}" + re.escape(opening[0]) +
                          r"{" + str(len(opening)) + r",}\s*", line):
            blocks.append(("\n".join(content), start, index + 1))
            opening = None
        else:
            content.append(line)
    return blocks


def dispatched_numbers(text, blocks):
    lines = text.splitlines()
    for block, start, end in blocks:
        if COMMENTS_COMMAND.search(block):
            for index in range(start, end):
                # Pasted output is evidence, but a command's shell comment can
                # still assign work. Respect quotes around literal #s.
                line = lines[index]
                comment = ""
                command = COMMENTS_COMMAND.search(line)
                if command and re.match(r"^\s*(?:\$\s*)?gh\s+api\b", line):
                    # Find the first unquoted shell comment, preserving text.
                    quote = None
                    escaped = False
                    for offset, char in enumerate(line):
                        if escaped:
                            escaped = False
                        elif char == "\\" and quote != "'":
                            escaped = True
                        elif quote:
                            if char == quote:
                                quote = None
                        elif char in "\"'":
                            quote = char
                        elif char == "#" and offset and line[offset - 1].isspace():
                            comment = line[offset + 1:]
                            break
                    # The endpoint itself is a reference even if the brief
                    # consists only of a fetch (with no scope decision yet).
                    comment = f"issues/{command.group(1)} {comment}"
                lines[index] = comment
    dispatch_text = re.sub(
        r"^([ \t]*CONTROLLING SCOPE DECISION:[ \t]*#[0-9]{1,5}\b)[^\n]*",
        r"\1", "\n".join(lines), flags=re.MULTILINE)
    numbers = set()
    non_issue = re.compile(
        r"\b(?:build|run|step|line|job|attempt|case|item|test|rank|number|no\.|version|v)\s*$",
        re.IGNORECASE)
    for match in re.finditer(r"#([0-9]{1,5})\b", dispatch_text):
        if not non_issue.search(dispatch_text[:match.start()].split("\n")[-1]):
            numbers.add(int(match.group(1)))
    references = re.compile(
        r"\b(?:issues?/|pull/|issues?\s+|pull\s+request\s+|PR\s+|GH-)(?:#)?([0-9]{1,5})\b",
        re.IGNORECASE)
    numbers.update(int(match.group(1)) for match in references.finditer(dispatch_text))
    return sorted(numbers)


def live_comments(number):
    endpoint = f"repos/{{owner}}/{{repo}}/issues/{number}/comments"
    timeout = float(os.environ.get("PGWT_GH_TIMEOUT", "30"))
    if not 0 < timeout < float("inf"):
        raise ValueError("PGWT_GH_TIMEOUT must be a positive finite number")
    try:
        result = subprocess.run(
            [os.environ.get("PGWT_GH") or "gh", "api", endpoint, "--paginate",
             "--jq", ".[] | {id: .id, body: .body}"],
            capture_output=True, text=True, encoding="utf-8", timeout=timeout)
    except subprocess.TimeoutExpired:
        raise ValueError(f"gh timeout after {timeout:g}s") from None
    if result.returncode:
        raise ValueError(f"gh failed (exit {result.returncode}): {result.stderr.strip()}")
    decoder = json.JSONDecoder()
    stream = result.stdout
    comments = {}
    pos = 0
    while pos < len(stream):
        if stream[pos].isspace():
            pos += 1
            continue
        comment, pos = decoder.raw_decode(stream, pos)
        if (not isinstance(comment, dict) or type(comment.get("id")) is not int
                or comment["id"] <= 0 or not isinstance(comment.get("body"), str)):
            raise ValueError("unparseable comment object (expected numeric id and body string)")
        comments[comment["id"]] = comment["body"]
    return comments


def normalise(text):
    return " ".join(text.split())


def coverage_errors(number, comments, blocks, text):
    errors = []
    fetches = [block for block in blocks
               if any(int(match.group(1)) == number
                      for match in COMMENTS_COMMAND.finditer(block))]
    if not fetches:
        errors.append("fenced comments fetch command/output missing")
    elif comments:
        def pasted(block, cid, body):
            first_line = body.split("\n", 1)[0][:30]
            escaped = json.dumps(first_line, ensure_ascii=False)[1:-1]
            ascii_escaped = json.dumps(first_line)[1:-1]
            return (re.search(r"\b" + str(cid) + r"\b", block)
                    and (not first_line or any(part in block for part in
                                              (first_line, escaped, ascii_escaped))))

        if not any(all(pasted(block, cid, body) for cid, body in comments.items())
                   for block in fetches):
            errors.append("fetch block does not contain every live comment id and body prefix: " +
                          ", ".join(map(str, comments)))
    elif not any("[]" in block or "(no output)" in block for block in fetches):
        errors.append("zero-comment fetch block needs [] or (no output)")

    prefix = r"^[ \t]*CONTROLLING SCOPE DECISION:[ \t]*#0*" + str(number) + r"[ \t]+"
    none = re.search(prefix + r"none[ \t]+\(0 comments\)[ \t]*$", text, re.MULTILINE)
    decisions = re.findall(prefix + r'comment[ \t]+([0-9]+)[ \t]+"(.*)"[ \t]*$',
                           text, re.MULTILINE)
    if not comments:
        if not none:
            errors.append("CONTROLLING SCOPE DECISION must say none (0 comments)")
    else:
        valid = any(int(cid) in comments and len(normalise(quote)) >= 20
                    and normalise(quote) in normalise(comments[int(cid)])
                    for cid, quote in decisions)
        if none:
            errors.append("none (0 comments) scope decision is invalid while live comments exist")
        elif not valid:
            if not decisions:
                errors.append("CONTROLLING SCOPE DECISION comment line missing")
            else:
                errors.append("scope quote must be at least 20 characters and occur in the named live comment")
    return errors


def main():
    try:
        with open(sys.argv[1], "rb") as brief:
            data = brief.read()
        text = data.decode("utf-8")
        if "\n" in os.path.abspath(sys.argv[1]) or "\r" in os.path.abspath(sys.argv[1]):
            raise ValueError("brief path must fit on one record line")
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"CHECK BRIEF ERROR: cannot read brief: {exc}", file=sys.stderr)
        return 2
    fences = fenced_blocks(text)
    numbers = dispatched_numbers(text, fences)
    if not numbers:
        record_pass(data)
        print("no issue referenced")
        return 0
    blocks = [block for block, _, _ in fences]
    failed = False
    for number in numbers:
        try:
            comments = live_comments(number)
        except (OSError, UnicodeError, ValueError) as exc:
            print(f"CHECK BRIEF ERROR: #{number}: {exc}", file=sys.stderr)
            return 2
        errors = coverage_errors(number, comments, blocks, text)
        for error in errors:
            print(f"CHECK BRIEF BLOCK: #{number}: {error}")
        failed |= bool(errors)
    if failed:
        return 1
    record_pass(data)
    print("CHECK BRIEF: covered " + ", ".join(f"#{n}" for n in numbers))
    return 0


try:
    sys.exit(main())
except OSError as exc:
    print(f"CHECK BRIEF ERROR: cannot record pass: {exc}", file=sys.stderr)
    sys.exit(2)
PY
rc=$?
case "$rc" in
  0|1|2) exit "$rc" ;;
  *) echo "CHECK BRIEF ERROR: python3 failed (exit $rc)" >&2; exit 2 ;;
esac
