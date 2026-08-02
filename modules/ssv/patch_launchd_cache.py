#!/usr/bin/env python3
"""Add the persistent SurrealLoader job to the iOS launchd service cache."""

import argparse
import os
import plistlib
import stat
import sys


JOB_PATH = "/System/Library/LaunchDaemons/com.surrealra1n.loader.plist"
JOB = {
    "KeepAlive": True,
    "Label": "com.surrealra1n.loader",
    "ProcessType": "Interactive",
    "ProgramArguments": ["/usr/libexec/surreal_loader"],
    "RunAtLoad": True,
    "ThrottleInterval": 10,
}


def patch(source: str, destination: str) -> None:
    metadata = os.stat(source)
    with open(source, "rb") as cache_file:
        cache = plistlib.load(cache_file)
    if not isinstance(cache, dict) or not isinstance(cache.get("LaunchDaemons"), dict):
        raise ValueError("unsupported launchd cache structure")

    cache["LaunchDaemons"][JOB_PATH] = JOB
    temporary = f"{destination}.surrealra1n"
    with open(temporary, "wb") as cache_file:
        plistlib.dump(cache, cache_file, fmt=plistlib.FMT_BINARY, sort_keys=False)
        cache_file.flush()
        os.fsync(cache_file.fileno())
    os.chmod(temporary, stat.S_IMODE(metadata.st_mode))
    os.replace(temporary, destination)

    with open(destination, "rb") as cache_file:
        verified = plistlib.load(cache_file)
    if verified.get("LaunchDaemons", {}).get(JOB_PATH) != JOB:
        raise ValueError("SurrealLoader job verification failed")
    print("[*] Added com.surrealra1n.loader to the launchd cache")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("destination")
    arguments = parser.parse_args()
    try:
        patch(arguments.source, arguments.destination)
    except (OSError, ValueError, plistlib.InvalidFileException) as error:
        print(f"patch_launchd_cache: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
