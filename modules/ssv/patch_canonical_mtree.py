#!/usr/bin/env python3
"""Synchronize the static SurrealLoader launchd-cache patch in canonical mtree."""

import argparse
import os
import re
import stat
import sys


LIBEXEC_SECTION = "# ./usr/libexec\n"
XPC_SECTION = "# ./System/Library/xpc\n"


def section_bounds(contents: str, marker: str):
    start = contents.find(marker)
    if start < 0:
        raise ValueError(f"mtree section is missing: {marker.strip()}")
    end = contents.find(marker, start + len(marker))
    if end < 0:
        raise ValueError(f"mtree section terminator is missing: {marker.strip()}")
    return start, end


def entry_bounds(contents: str, section_start: int, section_end: int, name: str):
    prefix = f"    {name} "
    cursor = section_start
    while True:
        start = contents.find(prefix, cursor, section_end)
        if start < 0:
            return None
        if start == 0 or contents[start - 1] == "\n":
            break
        cursor = start + len(prefix)
    line_start = start
    while True:
        newline = contents.find("\n", line_start, section_end)
        if newline < 0:
            raise ValueError(f"unterminated mtree entry for {name}")
        if not contents[line_start:newline].rstrip().endswith("\\"):
            return start, newline + 1
        line_start = newline + 1


def token(entry: str, key: str) -> str:
    match = re.search(rf"{re.escape(key)}([^\s\\]+)", entry)
    if match is None:
        raise ValueError(f"missing {key} metadata")
    return match.group(1)


def optional_token(entry: str, key: str):
    match = re.search(rf"{re.escape(key)}([^\s\\]+)", entry)
    return None if match is None else match.group(1)


def format_entry(name: str, digest: str, inode: int, sibling: str) -> str:
    return (
        f"    {name} \\\n"
        f"                xattrsdigest={digest} inode={inode} \\\n"
        f"                siblingid={sibling}\n"
    )


def patch_group(
    contents: str,
    marker: str,
    stock_name: str,
    original_name: str,
    patched_inode: int,
    original_inode: int,
    extra=None,
) -> str:
    section_start, section_end = section_bounds(contents, marker)
    stock = entry_bounds(contents, section_start, section_end, stock_name)
    original = entry_bounds(contents, section_start, section_end, original_name)
    extra_bounds = None if extra is None else entry_bounds(
        contents, section_start, section_end, extra[0]
    )
    if stock is None:
        raise ValueError(f"stock {stock_name} entry is missing")
    expected_extra_start = (original or stock)[1]
    if extra_bounds is not None and extra_bounds[0] != expected_extra_start:
        contents = contents[: extra_bounds[0]] + contents[extra_bounds[1] :]
        return patch_group(
            contents,
            marker,
            stock_name,
            original_name,
            patched_inode,
            original_inode,
            extra,
        )

    metadata_bounds = original or stock
    metadata_entry = contents[metadata_bounds[0] : metadata_bounds[1]]
    digest = optional_token(metadata_entry, "xattrsdigest=") or "none.0"
    sibling = token(metadata_entry, "siblingid=")
    manifest_original_inode = int(token(metadata_entry, "inode="))
    if manifest_original_inode != original_inode:
        raise ValueError(
            f"original {stock_name} inode differs between device and canonical "
            f"mtree ({original_inode} != {manifest_original_inode})"
        )

    replace_end = max(
        bounds[1]
        for bounds in (stock, original, extra_bounds)
        if bounds is not None
    )
    replacement = (
        format_entry(stock_name, "none.0", patched_inode, "0")
        + format_entry(original_name, digest, original_inode, sibling)
    )
    if extra is not None:
        replacement += format_entry(extra[0], "none.0", extra[1], "0")
    return contents[: stock[0]] + replacement + contents[replace_end:]


def patch_manifest(
    path: str,
    cache_loader: int,
    cache_loader_original: int,
    loader: int,
    launchd_cache: int,
    launchd_cache_original: int,
) -> None:
    metadata = os.stat(path)
    with open(path, "r", encoding="utf-8") as manifest:
        contents = manifest.read()

    contents = patch_group(
        contents,
        LIBEXEC_SECTION,
        "launchd_cache_loader",
        "launchd_cache_loader.srr",
        cache_loader,
        cache_loader_original,
        extra=("surreal_loader", loader),
    )
    contents = patch_group(
        contents,
        XPC_SECTION,
        "launchd.plist",
        "launchd.plist.srr",
        launchd_cache,
        launchd_cache_original,
    )

    temporary = f"{path}.surrealra1n"
    with open(temporary, "w", encoding="utf-8", newline="") as manifest:
        manifest.write(contents)
        manifest.flush()
        os.fsync(manifest.fileno())
    os.chmod(temporary, stat.S_IMODE(metadata.st_mode))
    os.utime(temporary, ns=(metadata.st_atime_ns, metadata.st_mtime_ns))
    os.replace(temporary, path)

    with open(path, "r", encoding="utf-8") as manifest:
        verified = manifest.read()
    expected = (
        (LIBEXEC_SECTION, "launchd_cache_loader", cache_loader),
        (
            LIBEXEC_SECTION,
            "launchd_cache_loader.srr",
            cache_loader_original,
        ),
        (LIBEXEC_SECTION, "surreal_loader", loader),
        (XPC_SECTION, "launchd.plist", launchd_cache),
        (
            XPC_SECTION,
            "launchd.plist.srr",
            launchd_cache_original,
        ),
    )
    for marker, name, expected_inode in expected:
        section_start, section_end = section_bounds(verified, marker)
        bounds = entry_bounds(verified, section_start, section_end, name)
        if bounds is None or int(
            token(verified[bounds[0] : bounds[1]], "inode=")
        ) != expected_inode:
            raise ValueError(f"canonical mtree verification failed for {name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest")
    parser.add_argument("cache_loader_inode", type=int)
    parser.add_argument("cache_loader_original_inode", type=int)
    parser.add_argument("loader_inode", type=int)
    parser.add_argument("launchd_cache_inode", type=int)
    parser.add_argument("launchd_cache_original_inode", type=int)
    arguments = parser.parse_args()
    inodes = (
        arguments.cache_loader_inode,
        arguments.cache_loader_original_inode,
        arguments.loader_inode,
        arguments.launchd_cache_inode,
        arguments.launchd_cache_original_inode,
    )
    if any(inode <= 0 for inode in inodes):
        parser.error("inodes must be positive")
    try:
        patch_manifest(arguments.manifest, *inodes)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"patch_canonical_mtree: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
