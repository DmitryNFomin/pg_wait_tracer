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
import json
import os
import re
import subprocess
import sys


COMMENTS_COMMAND = re.compile(
    r"\bgh\s+api\s+repos/[^/\s]+/[^/\s]+/issues/([0-9]+)"
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
                lines[index] = ""
    dispatch_text = re.sub(
        r"^([ \t]*CONTROLLING SCOPE DECISION:[ \t]*#[0-9]+)[^\n]*",
        r"\1", "\n".join(lines), flags=re.MULTILINE)
    return sorted({int(n) for n in re.findall(r"#([0-9]+)", dispatch_text)})


def live_comments(number):
    endpoint = f"repos/{{owner}}/{{repo}}/issues/{number}/comments"
    result = subprocess.run(
        [os.environ.get("PGWT_GH") or "gh", "api", endpoint, "--paginate",
         "--jq", ".[] | {id: .id, body: .body}"],
        capture_output=True, text=True, encoding="utf-8")
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
        if not any(all(re.search(r"\b" + str(cid) + r"\b", block)
                       for cid in comments) for block in fetches):
            errors.append("fetch block does not contain every live comment id: " +
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
        with open(sys.argv[1], encoding="utf-8") as brief:
            text = brief.read()
    except (OSError, UnicodeError) as exc:
        print(f"CHECK BRIEF ERROR: cannot read brief: {exc}", file=sys.stderr)
        return 2
    fences = fenced_blocks(text)
    numbers = dispatched_numbers(text, fences)
    if not numbers:
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
    print("CHECK BRIEF: covered " + ", ".join(f"#{n}" for n in numbers))
    return 0


sys.exit(main())
PY
rc=$?
case "$rc" in
  0|1|2) exit "$rc" ;;
  *) echo "CHECK BRIEF ERROR: python3 failed (exit $rc)" >&2; exit 2 ;;
esac
