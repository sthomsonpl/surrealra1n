#!/usr/bin/env python3
"""Print one component path from a matching IPSW BuildIdentity."""

from __future__ import annotations

import plistlib
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print(
            f"usage: {sys.argv[0]} <BuildManifest.plist> <component> <behavior>",
            file=sys.stderr,
        )
        return 64
    manifest_path, component, behavior = sys.argv[1:]
    with open(manifest_path, "rb") as handle:
        manifest = plistlib.load(handle)
    paths = {
        identity.get("Manifest", {})
        .get(component, {})
        .get("Info", {})
        .get("Path")
        for identity in manifest.get("BuildIdentities", [])
        if identity.get("Info", {}).get("RestoreBehavior") == behavior
    }
    paths.discard(None)
    if len(paths) != 1:
        print(
            f"expected one {behavior} {component} path, found {sorted(paths)}",
            file=sys.stderr,
        )
        return 1
    print(paths.pop())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
