#!/usr/bin/env python3
"""Point the bundled Dropbear at the rootless SFTP server alias."""

from __future__ import annotations

import sys
from pathlib import Path


STOCK_PATH = b"/usr/libexec/sftp-server\0"
ROOTLESS_PATH = b"/var/jb/sftp-server\0"


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {Path(sys.argv[0]).name} DROPBEAR", file=sys.stderr)
        return 2

    binary = Path(sys.argv[1])
    data = binary.read_bytes()
    padded_path = ROOTLESS_PATH.ljust(len(STOCK_PATH), b"\0")

    if padded_path in data:
        print(f"[*] Dropbear already uses {ROOTLESS_PATH[:-1].decode()}.")
        return 0
    if data.count(STOCK_PATH) != 1:
        raise RuntimeError(
            "Expected exactly one stock Dropbear SFTP server path, found "
            f"{data.count(STOCK_PATH)}"
        )

    binary.write_bytes(data.replace(STOCK_PATH, padded_path, 1))
    print(f"[*] Patched Dropbear SFTP path to {ROOTLESS_PATH[:-1].decode()}.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"[!] Failed to patch Dropbear SFTP path: {error}", file=sys.stderr)
        raise SystemExit(1)
