#!/usr/bin/env python3
"""Update canonical mtree inode entries for custom binary patches."""

import argparse
import json
import os
import posixpath
import re
import stat
import sys

from patch_canonical_mtree import entry_bounds, section_bounds, token


def load_metadata(path):
    with open(path, encoding="utf-8") as source:
        metadata = json.load(source)
    if not isinstance(metadata, list) or not metadata:
        raise ValueError("binpatcher metadata must be a non-empty list")

    entries = []
    seen = set()
    for item in metadata:
        if not isinstance(item, dict):
            raise ValueError("binpatcher metadata entry must be an object")
        target = item.get("target")
        inode = item.get("inode")
        if (
            not isinstance(target, str)
            or not target.startswith("/")
            or not isinstance(inode, int)
            or isinstance(inode, bool)
            or inode <= 0
        ):
            raise ValueError("invalid target or inode in binpatcher metadata")
        normalized = "/" + posixpath.normpath(target).lstrip("/")
        if normalized != target or target == "/" or target in seen:
            raise ValueError(f"invalid or duplicate target: {target}")
        seen.add(target)
        entries.append((target, inode))
    return entries


def replace_inode(contents, target, inode):
    relative = target.lstrip("/")
    directory, name = posixpath.split(relative)
    marker = f"# ./{directory}\n" if directory else "# .\n"
    section_start, section_end = section_bounds(contents, marker)
    bounds = entry_bounds(contents, section_start, section_end, name)
    if bounds is None:
        raise ValueError(f"canonical mtree entry is missing: {target}")
    entry = contents[bounds[0] : bounds[1]]
    updated, count = re.subn(r"\binode=\d+\b", f"inode={inode}", entry)
    if count != 1:
        raise ValueError(f"canonical mtree inode is ambiguous: {target}")
    return contents[: bounds[0]] + updated + contents[bounds[1] :]


def verify_entries(contents, entries):
    for target, expected_inode in entries:
        relative = target.lstrip("/")
        directory, name = posixpath.split(relative)
        marker = f"# ./{directory}\n" if directory else "# .\n"
        section_start, section_end = section_bounds(contents, marker)
        bounds = entry_bounds(contents, section_start, section_end, name)
        if bounds is None:
            raise ValueError(f"canonical mtree entry is missing: {target}")
        entry = contents[bounds[0] : bounds[1]]
        if int(token(entry, "inode=")) != expected_inode:
            raise ValueError(f"canonical mtree inode verification failed: {target}")


def patch_manifest(path, entries):
    metadata = os.stat(path)
    with open(path, encoding="utf-8") as source:
        contents = source.read()
    for target, inode in entries:
        contents = replace_inode(contents, target, inode)
    verify_entries(contents, entries)

    temporary = f"{path}.surrealra1n"
    with open(temporary, "w", encoding="utf-8", newline="") as output:
        output.write(contents)
        output.flush()
        os.fsync(output.fileno())
    os.chmod(temporary, stat.S_IMODE(metadata.st_mode))
    os.utime(temporary, ns=(metadata.st_atime_ns, metadata.st_mtime_ns))
    os.replace(temporary, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest")
    parser.add_argument("metadata")
    parser.add_argument("--verify", action="store_true")
    arguments = parser.parse_args()
    try:
        entries = load_metadata(arguments.metadata)
        if arguments.verify:
            with open(arguments.manifest, encoding="utf-8") as source:
                verify_entries(source.read(), entries)
        else:
            patch_manifest(arguments.manifest, entries)
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        print(f"patch_custom_mtree: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
