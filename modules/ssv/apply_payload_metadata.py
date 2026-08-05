#!/usr/bin/env python3
"""Apply and verify generated Dropbear payload ownership and file modes."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import stat
import sys
from pathlib import Path


def read_metadata(path: Path) -> dict[str, object]:
    with path.open(encoding="utf-8") as source:
        metadata = json.load(source)
    if metadata.get("schema_version") != 1:
        raise ValueError("unsupported payload metadata schema")
    if not isinstance(metadata.get("rootfs"), list) or not isinstance(
        metadata.get("ramdisk"), list
    ):
        raise ValueError("invalid payload metadata")
    return metadata


def safe_path(root: Path, relative: str) -> Path:
    candidate = (root / relative).resolve(strict=False)
    if os.path.commonpath((str(root.resolve()), str(candidate))) != str(root.resolve()):
        raise ValueError(f"unsafe payload path: {relative}")
    return candidate


def metadata_mode(entry: dict[str, object]) -> int:
    value = entry.get("mode")
    if not isinstance(value, str) or len(value) != 4 or not value.isdigit():
        raise ValueError(f"invalid mode for {entry}")
    return int(value, 8)


def metadata_ids(entry: dict[str, object]) -> tuple[int, int]:
    uid = entry.get("uid")
    gid = entry.get("gid")
    if not isinstance(uid, int) or not isinstance(gid, int):
        raise ValueError(f"invalid owner for {entry}")
    return uid, gid


def apply_rootfs(entries: list[object], destination: Path) -> None:
    for raw_entry in entries:
        if not isinstance(raw_entry, dict):
            raise ValueError("invalid rootfs metadata entry")
        relative = raw_entry.get("path")
        expected_type = raw_entry.get("type")
        if not isinstance(relative, str) or expected_type not in {"file", "directory"}:
            raise ValueError(f"invalid rootfs metadata entry: {raw_entry}")
        target = safe_path(destination, relative)
        information = target.lstat()
        if stat.S_ISLNK(information.st_mode) or (
            expected_type == "directory" and not stat.S_ISDIR(information.st_mode)
        ) or (expected_type == "file" and not stat.S_ISREG(information.st_mode)):
            raise ValueError(f"unexpected payload entry type: {relative}")
        uid, gid = metadata_ids(raw_entry)
        os.chown(target, uid, gid)
        os.chmod(target, metadata_mode(raw_entry))


def apply_ramdisk(entries: list[object], source_root: Path, destination_root: Path) -> None:
    for raw_entry in entries:
        if not isinstance(raw_entry, dict):
            raise ValueError("invalid ramdisk metadata entry")
        source = raw_entry.get("source")
        destination = raw_entry.get("destination")
        if not isinstance(source, str) or not isinstance(destination, str):
            raise ValueError(f"invalid ramdisk metadata entry: {raw_entry}")
        source_path = safe_path(source_root, source)
        target = safe_path(destination_root, destination)
        if not source_path.is_file():
            raise ValueError(f"missing ramdisk source: {source}")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_path, target)
        uid, gid = metadata_ids(raw_entry)
        os.chown(target, uid, gid)
        os.chmod(target, metadata_mode(raw_entry))


def verify_entry(entry: dict[str, object], target: Path, label: str) -> None:
    information = target.lstat()
    uid, gid = metadata_ids(entry)
    mode = metadata_mode(entry)
    if information.st_uid != uid or information.st_gid != gid or (
        stat.S_IMODE(information.st_mode) != mode
    ):
        raise ValueError(
            f"{label}: expected {uid}:{gid} {mode:04o}, got "
            f"{information.st_uid}:{information.st_gid} "
            f"{stat.S_IMODE(information.st_mode):04o}"
        )


def verify(metadata: dict[str, object], rootfs_destination: Path, ramdisk_destination: Path) -> None:
    for raw_entry in metadata["rootfs"]:
        if not isinstance(raw_entry, dict) or not isinstance(raw_entry.get("path"), str):
            raise ValueError("invalid rootfs metadata entry")
        verify_entry(
            raw_entry,
            safe_path(rootfs_destination, raw_entry["path"]),
            raw_entry["path"],
        )
    for raw_entry in metadata["ramdisk"]:
        if not isinstance(raw_entry, dict) or not isinstance(
            raw_entry.get("destination"), str
        ):
            raise ValueError("invalid ramdisk metadata entry")
        verify_entry(
            raw_entry,
            safe_path(ramdisk_destination, raw_entry["destination"]),
            raw_entry["destination"],
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("apply", "verify"))
    parser.add_argument("metadata", type=Path)
    parser.add_argument("rootfs_destination", type=Path)
    parser.add_argument("ramdisk_destination", type=Path)
    parser.add_argument("--source-root", type=Path)
    arguments = parser.parse_args()
    try:
        metadata = read_metadata(arguments.metadata)
        if arguments.action == "apply":
            if arguments.source_root is None:
                raise ValueError("--source-root is required for apply")
            apply_rootfs(metadata["rootfs"], arguments.rootfs_destination)
            apply_ramdisk(
                metadata["ramdisk"],
                arguments.source_root,
                arguments.ramdisk_destination,
            )
        else:
            verify(metadata, arguments.rootfs_destination, arguments.ramdisk_destination)
    except (OSError, ValueError) as error:
        print(f"payload_metadata: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
