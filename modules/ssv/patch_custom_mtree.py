#!/usr/bin/env python3
"""Synchronize canonical mtree metadata for custom binary patches."""

import argparse
import hashlib
import json
import os
import posixpath
import re
import stat
import sys

from patch_canonical_mtree import entry_bounds, section_bounds, token
from xattr_utils import get_xattr, list_xattrs


UF_COMPRESSED = getattr(stat, "UF_COMPRESSED", 0x00000020)
PROVENANCE_XATTR = "com.apple.provenance"


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
        compressed = item.get("compressed")
        xattrs = item.get("xattrs")
        canonical_xattrs = item.get("canonical_xattrs")
        if (
            not isinstance(target, str)
            or not target.startswith("/")
            or not isinstance(inode, int)
            or isinstance(inode, bool)
            or inode <= 0
            or not isinstance(compressed, bool)
            or not isinstance(xattrs, list)
        ):
            raise ValueError("invalid target or filesystem metadata")

        normalized = "/" + posixpath.normpath(target).lstrip("/")
        if normalized != target or target == "/" or target in seen:
            raise ValueError(f"invalid or duplicate target: {target}")

        parsed_xattrs = []
        xattr_names = set()
        for xattr in xattrs:
            if not isinstance(xattr, dict):
                raise ValueError(f"invalid xattr metadata: {target}")
            name = xattr.get("name")
            digest = xattr.get("sha256")
            if (
                not isinstance(name, str)
                or not name
                or name in xattr_names
                or not isinstance(digest, str)
                or re.fullmatch(r"[0-9a-f]{64}", digest) is None
            ):
                raise ValueError(f"invalid xattr metadata: {target}")
            xattr_names.add(name)
            parsed_xattrs.append((name, digest))
        parsed_xattrs.sort()

        if canonical_xattrs is not None:
            canonical_pair = None
            if isinstance(canonical_xattrs, dict) and set(canonical_xattrs) == {
                "digest",
                "count",
            }:
                canonical_pair = (
                    canonical_xattrs.get("digest"),
                    canonical_xattrs.get("count"),
                )
            expected_xattr_names = [name for name, _ in parsed_xattrs]
            valid_normalization = (
                canonical_pair == ("none.0", 0) and not parsed_xattrs
            ) or (
                canonical_pair == ("authapfs.0", 1)
                and expected_xattr_names == [PROVENANCE_XATTR]
            )
            if compressed or not valid_normalization:
                raise ValueError(
                    f"invalid canonical xattr normalization: {target}"
                )

        seen.add(target)
        entries.append(
            {
                "target": target,
                "inode": inode,
                "compressed": compressed,
                "xattrs": parsed_xattrs,
                "canonical_xattrs": canonical_xattrs,
            }
        )
    return entries


def entry_location(contents, target):
    relative = target.lstrip("/")
    directory, name = posixpath.split(relative)
    marker = f"# ./{directory}\n" if directory else "# .\n"
    section_start, section_end = section_bounds(contents, marker)
    bounds = entry_bounds(contents, section_start, section_end, name)
    if bounds is None:
        raise ValueError(f"canonical mtree entry is missing: {target}")
    return bounds


def replace_metadata(contents, metadata):
    target = metadata["target"]
    bounds = entry_location(contents, target)
    entry = contents[bounds[0] : bounds[1]]
    updated, inode_count = re.subn(
        r"\binode=\d+\b", f"inode={metadata['inode']}", entry
    )
    if inode_count != 1:
        raise ValueError(f"canonical mtree inode is ambiguous: {target}")

    canonical_xattrs = metadata["canonical_xattrs"]
    if canonical_xattrs is not None:
        updated, digest_count = re.subn(
            r"\bxattrsdigest=[^\s\\]+",
            f"xattrsdigest={canonical_xattrs['digest']}",
            updated,
        )
        updated, xattr_count = re.subn(
            r"\bnxattr=\d+\b",
            f"nxattr={canonical_xattrs['count']}",
            updated,
        )
        if digest_count != 1 or xattr_count != 1:
            raise ValueError(
                f"canonical mtree xattr metadata is ambiguous: {target}"
            )
    return contents[: bounds[0]] + updated + contents[bounds[1] :]


def verify_entries(contents, entries):
    for metadata in entries:
        target = metadata["target"]
        bounds = entry_location(contents, target)
        entry = contents[bounds[0] : bounds[1]]
        if int(token(entry, "inode=")) != metadata["inode"]:
            raise ValueError(f"canonical mtree inode verification failed: {target}")
        canonical_xattrs = metadata["canonical_xattrs"]
        if canonical_xattrs is not None and (
            token(entry, "xattrsdigest=") != canonical_xattrs["digest"]
            or int(token(entry, "nxattr=")) != canonical_xattrs["count"]
        ):
            raise ValueError(
                f"canonical mtree xattr verification failed: {target}"
            )


def verify_filesystem(system_root, entries):
    for metadata in entries:
        target = metadata["target"]
        path = os.path.join(system_root, target.lstrip("/"))
        path_stat = os.stat(path)
        if path_stat.st_ino != metadata["inode"]:
            raise ValueError(f"filesystem inode verification failed: {target}")
        compressed = bool(getattr(path_stat, "st_flags", 0) & UF_COMPRESSED)
        if compressed != metadata["compressed"]:
            raise ValueError(f"filesystem compression verification failed: {target}")

        actual_xattrs = [
            (name, hashlib.sha256(get_xattr(path, name)).hexdigest())
            for name in list_xattrs(path)
        ]
        actual_xattrs.sort()
        if actual_xattrs != metadata["xattrs"]:
            raise ValueError(f"filesystem xattr verification failed: {target}")


def patch_manifest(path, entries):
    metadata = os.stat(path)
    with open(path, encoding="utf-8") as source:
        contents = source.read()
    for entry in entries:
        contents = replace_metadata(contents, entry)
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
    parser.add_argument(
        "--system-root",
        help="also verify patched filesystem metadata under this mounted root",
    )
    arguments = parser.parse_args()
    try:
        entries = load_metadata(arguments.metadata)
        if arguments.verify:
            with open(arguments.manifest, encoding="utf-8") as source:
                verify_entries(source.read(), entries)
        else:
            patch_manifest(arguments.manifest, entries)
        if arguments.system_root:
            verify_filesystem(arguments.system_root, entries)
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        print(f"patch_custom_mtree: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
