#!/usr/bin/env python3
"""Repack an Apple Archive to an exact size using an ignored padding entry."""

import argparse
import hashlib
import os
import subprocess
import sys


# Keep padding after the canonical files.
PADDING_NAME = "zzzz.surrealra1n-padding"


def padding_bytes(length: int) -> bytes:
    output = bytearray()
    counter = 0
    while len(output) < length:
        output.extend(hashlib.sha256(f"surrealra1n:{counter}".encode()).digest())
        counter += 1
    return bytes(output[:length])


def write_padding(path: str, length: int) -> None:
    if length == 0:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        return
    with open(path, "wb") as padding:
        padding.write(padding_bytes(length))


def archive(source: str, output: str) -> int:
    temporary = f"{output}.temporary"
    subprocess.run(
        [
            "/usr/bin/aa",
            "archive",
            "-d",
            source,
            "-o",
            temporary,
            "-a",
            "lzfse",
            "-b",
            "8m",
            "-exclude-field",
            "xat",
        ],
        check=True,
    )
    size = os.path.getsize(temporary)
    os.replace(temporary, output)
    return size


def repack(source: str, output: str, target_size: int) -> None:
    padding_path = os.path.join(source, PADDING_NAME)
    temporary_path = f"{output}.temporary"
    padding_length = 0
    try:
        for attempt in range(96):
            if attempt > 0 and attempt % 8 == 0:
                print(
                    f"[*] Canonical archive sizing attempt {attempt + 1}...",
                    file=sys.stderr,
                )
            write_padding(padding_path, padding_length)
            size = archive(source, output)
            if size == target_size:
                return
            padding_length = max(0, padding_length + target_size - size)
            if attempt >= 8 and abs(target_size - size) <= 8:
                distance = (attempt - 8) // 2 + 1
                padding_length = max(
                    0,
                    padding_length + (distance if attempt % 2 == 0 else -distance),
                )
    finally:
        try:
            os.unlink(padding_path)
        except FileNotFoundError:
            pass
        try:
            os.unlink(temporary_path)
        except FileNotFoundError:
            pass

    raise ValueError(f"could not produce an Apple Archive of {target_size} bytes")


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
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"repack_aa_exact: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
