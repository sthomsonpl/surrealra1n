#!/usr/bin/env python3
"""Repack a gzip payload to an exact size using an ignored header field."""

import argparse
import gzip
import os
import struct
import sys


GZIP_MAGIC = b"\x1f\x8b\x08"
FEXTRA = 0x04
FNAME = 0x08
FCOMMENT = 0x10


def add_header_padding(archive: bytes, padding: int) -> bytes:
    if padding == 0:
        return archive
    if not archive.startswith(GZIP_MAGIC) or archive[3] != 0:
        raise ValueError("generated gzip has an unexpected header")

    header = bytearray(archive[:10])
    body = archive[10:]

    if padding == 1:
        header[3] |= FNAME
        optional = b"\0"
    elif padding <= 65537:
        header[3] |= FEXTRA
        optional = struct.pack("<H", padding - 2) + bytes(padding - 2)
    else:
        header[3] |= FCOMMENT
        optional = b"P" * (padding - 1) + b"\0"
    return bytes(header) + optional + body


def repack(source: str, output: str, target_size: int) -> None:
    with open(source, "rb") as input_file:
        contents = input_file.read()

    archive = gzip.compress(contents, compresslevel=9, mtime=0)
    if len(archive) > target_size:
        raise ValueError(
            "canonical gzip without padding exceeds target size "
            f"({len(archive)} > {target_size})"
        )

    archive = add_header_padding(archive, target_size - len(archive))
    if len(archive) != target_size:
        raise ValueError(
            f"canonical gzip size mismatch ({len(archive)} != {target_size})"
        )
    if gzip.decompress(archive) != contents:
        raise ValueError("canonical gzip verification failed")

    temporary = f"{output}.temporary"
    try:
        with open(temporary, "wb") as output_file:
            output_file.write(archive)
            output_file.flush()
            os.fsync(output_file.fileno())
        os.replace(temporary, output)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("output")
    parser.add_argument("target_size", type=int)
    arguments = parser.parse_args()
    if arguments.target_size <= 0:
        parser.error("target_size must be positive")
    try:
        repack(arguments.source, arguments.output, arguments.target_size)
    except (OSError, ValueError) as error:
        print(f"repack_gzip_exact: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
