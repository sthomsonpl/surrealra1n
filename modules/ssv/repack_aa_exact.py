#!/usr/bin/env python3
"""Repack an Apple Archive to an exact size using an ignored padding entry."""

import argparse
import hashlib
import os
import subprocess
import sys


# Keep padding after the canonical files.
PADDING_NAME = "zzzz.surrealra1n-padding"
MAX_PADDING_VARIANTS = 32
MAX_ATTEMPTS_PER_VARIANT = 24


def padding_bytes(length: int, variant: int) -> bytes:
    output = bytearray()
    counter = 0
    while len(output) < length:
        output.extend(
            hashlib.sha256(
                f"surrealra1n:{variant}:{counter}".encode()
            ).digest()
        )
        counter += 1
    return bytes(output[:length])


def write_padding(path: str, length: int, variant: int = 0) -> None:
    if length == 0:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        return
    with open(path, "wb") as padding:
        padding.write(padding_bytes(length, variant))


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
            "-t",
            "1",
            "-exclude-field",
            "xat,ctm,mtm,btm",
        ],
        check=True,
    )
    size = os.path.getsize(temporary)
    os.replace(temporary, output)
    return size


def repack(source: str, output: str, target_size: int) -> None:
    padding_path = os.path.join(source, PADDING_NAME)
    temporary_path = f"{output}.temporary"
    total_attempts = 0
    try:
        write_padding(padding_path, 0)
        base_size = archive(source, output)
        total_attempts += 1
        if base_size == target_size:
            return
        if base_size > target_size:
            raise ValueError(
                "canonical archive without padding exceeds target size "
                f"({base_size} > {target_size})"
            )

        initial_padding = target_size - base_size
        for variant in range(MAX_PADDING_VARIANTS):
            padding_length = initial_padding
            seen: set[tuple[int, int]] = set()
            for attempt in range(MAX_ATTEMPTS_PER_VARIANT):
                write_padding(padding_path, padding_length, variant)
                size = archive(source, output)
                total_attempts += 1
                if size == target_size:
                    return

                state = (padding_length, size)
                if state in seen:
                    break
                seen.add(state)

                difference = target_size - size
                next_length = padding_length + difference
                if next_length <= 0:
                    break
                padding_length = next_length

                if total_attempts % 8 == 0:
                    print(
                        "[*] Canonical archive sizing "
                        f"attempt {total_attempts} "
                        f"(variant {variant + 1}, delta {difference:+d})...",
                        file=sys.stderr,
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

    raise ValueError(
        f"could not produce an Apple Archive of {target_size} bytes "
        f"after {total_attempts} deterministic attempts"
    )


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
