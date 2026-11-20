#!/usr/bin/env python3
"""#947 U1 guard: every block-type dispatch site handles trace v6 type 3.

Trace v6 adds block type 3 COVERAGE (src/coverage_record.h). Before the
bump, any data path that met an unknown block either counted it as damage
(decode returned -1) or sent it to the TRANSITIONS branch (every non-SAMPLES
block was a TRANSITIONS block, e.g. src/server.c cov_scan_file adding an
exact span at the block's write time).

This test ENUMERATES the sites. A function in src/ or a trace tool in tests/ that reads a block's type
or decodes / candidacy-tests / header-reads a trace block is a dispatch site.
Each one must either name PGWT_BLOCK_COVERAGE itself, or be listed in
COVERAGE_SAFE below with the reason it can never see a type-3 block. A NEW
site that forgets type 3 is neither, and fails here. A listed site that no
longer dispatches fails too, so the list cannot rot into a blanket waiver.

Pure Python, no build: runs in make check (Mac) and in the C unit tier.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
TOOLS = os.path.join(ROOT, "tests")

# Tokens that make a function a block-type dispatch site.
DISPATCH = re.compile(
    r"\bblock_type\b|\bis_sample\b|\bis_coverage\b|"
    r"\bpgwt_reader_decode_block(_info)?\s*\(|"
    r"\bpgwt_reader_block_info\s*\(|\bpgwt_reader_block_candidate\s*\(|"
    r"\bstruct\s+pgwt_trace_block_header\b")

# Files outside the trace-block world: the evidence-v1 codec (src/core/, its
# own v5 format), the coverage verdict, and the record codec itself.
SKIP_FILES = {"coverage.c", "coverage.h", "coverage_record.h"}

# function -> why it can never see a COVERAGE block. Keep each reason a
# checkable fact about the code, not a hope.
COVERAGE_SAFE = {
    ("tests/cross_validate.c", "main"):
        "ranges accept only TRANSITIONS/SAMPLES; decode <= 0 skips COVERAGE",
    ("tests/dump_markers.c", "main"):
        "decode <= 0 skips COVERAGE before examining records",
    ("tests/wait_cpu_canary.c", "main"):
        "decode <= 0 skips COVERAGE before examining records",
    ("event_writer.c", "write_typed_block"):
        "writes only the TRANSITIONS/SAMPLES blocks its callers pass",
    ("event_writer.c", "recover_current_trace"):
        "rebuilds the footer from EVERY committed block whatever its type "
        "(any type <= 15), so COVERAGE blocks survive recovery; pinned by "
        "tests/test_coverage_writer.c seal_only_clean",
    ("event_writer.h", "pgwt_trace_block_header_size"):
        "a sizeof, no type decision",
    ("event_reader.c", "pgwt_reader_open"):
        "indexes every block by header size and offset, type-blind; every "
        "consumer of the index is a listed or COVERAGE-naming site",
    ("event_reader.c", "read_block_header"):
        "copies the header; its callers decide on the type",
    ("event_reader.c", "pgwt_reader_block_info"):
        "returns the header; its callers decide on the type",
    ("event_reader.c", "pgwt_reader_decode_block"):
        "forwards to pgwt_reader_decode_block_info, which names COVERAGE",
    ("event_reader.h", None):
        "declarations only",
    ("block_agg.c", "pgwt_block_agg_build"):
        "refuses every block_type other than TRANSITIONS",
    ("block_agg.h", None):
        "declarations only",
    ("replay_accum.c", "pgwt_replay_events"):
        "receives records only; pgwt_replay_file skips COVERAGE before decode",
    ("server.c", "cov_decode_markers"):
        "decodes only pending_tb blocks, which cov_scan_file fills with "
        "TRANSITIONS blocks after skipping COVERAGE",
    ("server.c", None):
        "struct declarations at file scope",
}


def functions(text):
    """Yield (name or None, body) for each top-level function, and the
    file-scope remainder as (None, rest)."""
    out = []
    rest = []
    i = 0
    n = len(text)
    depth = 0
    start = 0
    name = None
    while i < n:
        c = text[i]
        if c == "/" and text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == "/" and text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c in "\"'":
            q = c
            i += 1
            while i < n and text[i] != q:
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c == "{":
            if depth == 0:
                head = text[start:i]
                name = func_name(strip_comments(head))
                if name is None:
                    rest.append(head)
                body_start = i
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                body = text[body_start:i + 1]
                if name:
                    out.append((name, body))
                else:
                    rest.append(body)
                start = i + 1
        elif c == ";" and depth == 0:
            rest.append(text[start:i + 1])
            start = i + 1
        i += 1
    rest.append(text[start:])
    out.append((None, "".join(rest)))
    return out


def func_name(head):
    """The function a `{` opens: the identifier before the balanced
    parameter list that ends the head. None for a struct/array/initializer."""
    h = head.rstrip()
    if not h.endswith(")"):
        return None
    depth = 0
    for k in range(len(h) - 1, -1, -1):
        if h[k] == ")":
            depth += 1
        elif h[k] == "(":
            depth -= 1
            if depth == 0:
                m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", h[:k])
                if not m or "=" in h[m.start():]:
                    return None
                return m.group(1)
    return None


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def main():
    sites = []
    failures = []
    seen_safe = set()
    # Unit fixtures intentionally construct malformed blocks; guard production
    # consumers plus all top-level C tools (including future tools), rather
    # than requiring those fixtures to skip what they are testing.
    files = [(fname, os.path.join(SRC, fname)) for fname in os.listdir(SRC)]
    files += [("tests/" + fname, os.path.join(TOOLS, fname))
              for fname in os.listdir(TOOLS) if not fname.startswith("test_")]
    for fname, path in sorted(files):
        if not fname.endswith((".c", ".h")) or fname in SKIP_FILES:
            continue
        text = open(path, encoding="utf-8").read()
        for func, body in functions(text):
            code = strip_comments(body)
            if not DISPATCH.search(code):
                continue
            key = (fname, func)
            sites.append(key)
            if "PGWT_BLOCK_COVERAGE" in code:
                continue
            if key in COVERAGE_SAFE:
                seen_safe.add(key)
                continue
            failures.append("%s:%s dispatches on block type but neither "
                            "names PGWT_BLOCK_COVERAGE nor is listed in "
                            "COVERAGE_SAFE" % (fname, func or "<file scope>"))
    for key in sorted(set(COVERAGE_SAFE) - seen_safe,
                      key=lambda k: (k[0], k[1] or "")):
        failures.append("COVERAGE_SAFE lists %s:%s, which is not a dispatch "
                        "site (or names COVERAGE now): remove it"
                        % (key[0], key[1] or "<file scope>"))
    print("block-type dispatch sites: %d" % len(sites))
    for fname, func in sites:
        print("  %s:%s" % (fname, func or "<file scope>"))
    # The sites the spec names (r2 §3 "Type 3 is non-data"), by function:
    # each must be found as a site here, or the scan itself is broken.
    must = [("server.c", "cur_cache_store"), ("server.c", "load_file_range_mono"),
            ("server.c", "cov_scan_file"), ("server.c", "get_cached_immutable"),
            ("event_reader.c", "pgwt_reader_decode_block_info"),
            ("event_reader.c", "pgwt_reader_block_candidate"),
            ("event_reader.c", "pgwt_reader_file_span"),
            ("block_agg.c", "pgwt_block_agg_window_from_reader"),
            ("replay_accum.c", "pgwt_replay_file"),
            ("tests/wait_cpu_scan.c", "main")]
    for key in must:
        if key not in sites:
            failures.append("expected dispatch site %s:%s not found: the "
                            "scan cannot see it" % key)
    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1
    print("PASS: every site handles COVERAGE or is listed with a reason")
    return 0


if __name__ == "__main__":
    sys.exit(main())
