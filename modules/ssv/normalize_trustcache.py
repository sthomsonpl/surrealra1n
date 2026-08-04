#!/usr/bin/env python3
"""Normalize and validate Apple trust cache v1 and v2 record tables."""

import argparse
import os
import struct
import sys


HEADER_SIZE = 24
ENTRY_SIZES = {1: 22, 2: 24}


def read_cache(path: str) -> tuple[int, bytes, list[bytes]]:
    with open(path, "rb") as cache_file:
        contents = cache_file.read()
    if len(contents) < HEADER_SIZE:
        raise ValueError(f"{path}: truncated trust cache header")
    version, count = struct.unpack_from("<I16xI", contents)
    entry_size = ENTRY_SIZES.get(version)
    if entry_size is None:
        raise ValueError(f"{path}: unsupported trust cache version {version}")
    expected_size = HEADER_SIZE + count * entry_size
    if len(contents) != expected_size:
        raise ValueError(
            f"{path}: size {len(contents)} does not match {count} records"
        )
    header = contents[:HEADER_SIZE]
    entries = [
        contents[offset : offset + entry_size]
        for offset in range(HEADER_SIZE, len(contents), entry_size)
    ]
    return version, header, entries


def normalize(path: str, required_path: str | None) -> tuple[int, int, int]:
    version, header, entries = read_cache(path)
    original_count = len(entries)

    if required_path is not None:
        required_version, _, required = read_cache(required_path)
        if required_version != version:
            raise ValueError(
                f"{required_path}: trust cache version {required_version} "
                f"does not match base version {version}"
            )
        normalized = sorted(set(entries) | set(required))
    else:
        normalized = sorted(set(entries))

    output = bytearray(header)
    struct.pack_into("<I", output, 20, len(normalized))
    output.extend(b"".join(normalized))
    temporary = f"{path}.surrealra1n"
    with open(temporary, "wb") as cache_file:
        cache_file.write(output)
        cache_file.flush()
        os.fsync(cache_file.fileno())
    os.replace(temporary, path)

    verified_version, _, verified = read_cache(path)
    if verified_version != version:
        raise ValueError(f"{path}: trust cache version changed during normalization")
    if verified != sorted(verified):
        raise ValueError(f"{path}: normalized record table failed verification")
    if required_path is not None and not set(required).issubset(set(verified)):
        raise ValueError(f"{path}: required payload records failed verification")
    return version, original_count, len(verified)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cache")
    parser.add_argument("--require", metavar="PAYLOAD_CACHE")
    arguments = parser.parse_args()
    try:
        version, before, after = normalize(arguments.cache, arguments.require)
    except (OSError, ValueError) as error:
        print(f"normalize_trustcache: {error}", file=sys.stderr)
        return 1
    print(f"[*] TrustCache v{version} normalized: {before} -> {after} records")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
