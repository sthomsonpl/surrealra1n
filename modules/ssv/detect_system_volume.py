#!/usr/bin/env python3
"""Detect whether an IPSW uses sealed or unsealed System volumes."""

from __future__ import annotations

import argparse
import plistlib
import sys
import zipfile
from pathlib import PurePosixPath


SEALED_COMPONENT = "SystemVolume"
CANONICAL_METADATA_COMPONENT = "Ap,SystemVolumeCanonicalMetadata"


class DetectionError(RuntimeError):
    """Raised when an IPSW has incomplete or inconsistent System metadata."""


def _component_path(manifest: dict, component: str) -> str | None:
    value = manifest.get(component)
    if not isinstance(value, dict):
        return None
    info = value.get("Info")
    if not isinstance(info, dict):
        return None
    path = info.get("Path")
    return path if isinstance(path, str) and path else None


def detect_system_volume_mode(ipsw_path: str) -> str:
    with zipfile.ZipFile(ipsw_path) as ipsw:
        archive_names = set(ipsw.namelist())
        try:
            build_manifest = plistlib.loads(ipsw.read("BuildManifest.plist"))
        except KeyError as error:
            raise DetectionError("IPSW is missing BuildManifest.plist") from error

    identities = build_manifest.get("BuildIdentities")
    if not isinstance(identities, list) or not identities:
        raise DetectionError("BuildManifest has no BuildIdentities")

    modes: set[str] = set()
    for index, identity in enumerate(identities):
        if not isinstance(identity, dict):
            raise DetectionError(f"BuildIdentity {index} is invalid")
        manifest = identity.get("Manifest")
        if not isinstance(manifest, dict):
            raise DetectionError(f"BuildIdentity {index} has no Manifest")

        root_hash = _component_path(manifest, SEALED_COMPONENT)
        canonical_mtree = _component_path(
            manifest, CANONICAL_METADATA_COMPONENT
        )
        if root_hash is None and canonical_mtree is None:
            modes.add("unsealed")
            continue
        if canonical_mtree is None and root_hash is not None:
            if PurePosixPath(root_hash).suffix == ".dmg":
                if root_hash not in archive_names:
                    raise DetectionError(
                        f"BuildIdentity {index} references missing System image: "
                        f"{root_hash}"
                    )
                modes.add("unsealed")
                continue
            raise DetectionError(
                f"BuildIdentity {index} has a root_hash without canonical mtree"
            )
        if root_hash is None:
            raise DetectionError(
                f"BuildIdentity {index} has incomplete sealed System metadata"
            )
        if PurePosixPath(root_hash).suffix != ".root_hash":
            raise DetectionError(
                f"BuildIdentity {index} SystemVolume is not a root_hash"
            )
        if PurePosixPath(canonical_mtree).suffix != ".mtree":
            raise DetectionError(
                f"BuildIdentity {index} canonical metadata is not an mtree"
            )
        missing = [
            path
            for path in (root_hash, canonical_mtree)
            if path not in archive_names
        ]
        if missing:
            raise DetectionError(
                f"BuildIdentity {index} references missing component(s): "
                + ", ".join(missing)
            )
        modes.add("sealed")

    if len(modes) != 1:
        raise DetectionError(
            "BuildManifest mixes sealed and unsealed System identities"
        )
    return modes.pop()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Print 'sealed' when BuildManifest pairs SystemVolume root_hash "
            "with canonical mtree metadata, otherwise print 'unsealed'."
        )
    )
    parser.add_argument("ipsw", help="Path to the IPSW archive")
    args = parser.parse_args()

    try:
        print(detect_system_volume_mode(args.ipsw))
    except (
        DetectionError,
        OSError,
        plistlib.InvalidFileException,
        zipfile.BadZipFile,
    ) as error:
        print(f"detect_system_volume: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
