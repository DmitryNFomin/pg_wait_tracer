#!/usr/bin/env python3
"""Recompute Git's SHA-1 tree from synced bytes, without a remote .git.

The manifest is `git ls-tree -rz <tree-hash>` from the exact tree produced by
tree-hash.sh (tracked + nonignored untracked files). Only its paths define the
file set: hashes and modes in the manifest are NOT trusted as content evidence.
Read the bytes, symlink targets and executable bits on the box, then encode
Git blob/tree objects with Git's ordering. This is the same tree hash, not a
different digest claiming equivalence. Extra ignored build outputs are outside
that set and clean-box removes them before compilation. The launcher overlays
the full announced file list, including tracked tests/results/ archives that
the ordinary source rsync excludes, so no manifest blob ID substitutes for
reading actual synced bytes.
"""
import hashlib
import os
from pathlib import Path
import stat
import sys


def object_hash(kind, content):
    return hashlib.sha1(kind + b" " + str(len(content)).encode() + b"\0" + content).digest()


def source_hash(manifest):
    root = {}
    for entry in Path(manifest).read_bytes().split(b"\0"):
        if not entry:
            continue
        metadata, path = entry.split(b"\t", 1)
        mode, kind, _ = metadata.split(b" ")
        parts = path.split(b"/")
        if (kind != b"blob" or mode not in (b"100644", b"100755", b"120000")
                or any(p in (b"", b".", b"..", b".git") for p in parts)):
            raise ValueError("unsupported manifest entry: %r" % entry)
        node = root
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        if parts[-1] in node:
            raise ValueError("duplicate manifest path: %r" % path)
        # Refuse directory symlinks, which could hide content outside the checkout.
        for i in range(1, len(parts)):
            if not stat.S_ISDIR(os.lstat(b"/".join(parts[:i])).st_mode):
                raise ValueError("non-directory parent: %r" % path)
        actual_mode = os.lstat(path).st_mode
        if stat.S_ISLNK(actual_mode):
            mode, content = b"120000", os.readlink(path)
        elif stat.S_ISREG(actual_mode):
            mode = b"100755" if actual_mode & stat.S_IXUSR else b"100644"
            with open(path, "rb") as source:
                content = source.read()
        else:
            raise ValueError("not a regular file or symlink: %r" % path)
        node[parts[-1]] = (mode, object_hash(b"blob", content))

    def tree_hash(node):
        entries = []
        for name in sorted(node, key=lambda n: n + (b"/" if isinstance(node[n], dict) else b"")):
            child = node[name]
            mode, digest = (b"40000", tree_hash(child)) if isinstance(child, dict) else child
            entries.append(mode + b" " + name + b"\0" + digest)
        return object_hash(b"tree", b"".join(entries))

    return tree_hash(root).hex()


if __name__ == "__main__":
    try:
        print(source_hash(sys.argv[1]))
    except (OSError, ValueError, IndexError, TypeError) as error:
        sys.exit("box-check: cannot hash synced sources: %s" % error)
