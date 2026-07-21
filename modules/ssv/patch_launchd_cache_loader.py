#!/usr/bin/env python3
"""Force launchd_cache_loader to use its built-in unsecure path."""

import argparse
import os
import stat
import sys


# Branch to the known stock unsecure-cache path.
ORIGINAL = bytes.fromhex(
    "ff1300f9"  # str xzr, [sp, #0x20]
    "00c50050"  # adr x0, \"kern.bootargs\"
    "1f2003d5"  # nop
    "e1830091"  # add x1, sp, #0x20
    "29020094"  # bl boot_args_helper
    "600300b4"  # cbz x0, no_boot_args
)
PATCHED = ORIGINAL[:4] + bytes.fromhex("0c000014") + ORIGINAL[8:]


def patch(source: str, destination: str) -> None:
    metadata = os.stat(source)
    with open(source, "rb") as executable:
        contents = executable.read()

    original_matches = [
        index
        for index in range(len(contents))
        if contents.startswith(ORIGINAL, index)
    ]
    patched_matches = [
        index
        for index in range(len(contents))
        if contents.startswith(PATCHED, index)
    ]
    if len(original_matches) == 1 and not patched_matches:
        offset = original_matches[0]
        contents = contents[:offset] + PATCHED + contents[offset + len(PATCHED) :]
    elif len(patched_matches) == 1 and not original_matches:
        offset = patched_matches[0]
    else:
        raise ValueError(
            "expected exactly one stock or patched launchd cache-loader sequence"
        )

    if b"launchd_unsecure_cache=" not in contents:
        raise ValueError("the built-in unsecure cache path is missing")
    if contents[offset + 4 : offset + 8] != bytes.fromhex("0c000014"):
        raise ValueError("unsecure cache branch verification failed")

    temporary = f"{destination}.surrealra1n"
    with open(temporary, "wb") as executable:
        executable.write(contents)
        executable.flush()
        os.fsync(executable.fileno())
    os.chmod(temporary, stat.S_IMODE(metadata.st_mode))
    os.replace(temporary, destination)
    print(f"[*] launchd_cache_loader unsecure branch patched at 0x{offset + 4:x}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("destination")
    arguments = parser.parse_args()
    try:
        patch(arguments.source, arguments.destination)
    except (OSError, ValueError) as error:
        print(f"patch_launchd_cache_loader: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
