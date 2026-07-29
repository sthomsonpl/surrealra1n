#!/usr/bin/env python3
"""Apply configured binary patches to an unpacked system root."""

import argparse
import fnmatch
import glob
import json
import os
import subprocess
import sys
import tempfile


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PATCH_DIR = os.path.join(SCRIPT_DIR, "patches")
DEFAULT_CONFIG = os.path.join(SCRIPT_DIR, "patches.json")
DEFAULT_SIGN_TOOL = os.path.join(os.path.dirname(SCRIPT_DIR), "bin", "ldid")
TEMPLATE_PATCH = "template_patch.json"


def load_json(path):
    try:
        with open(path, encoding="utf-8") as file:
            return json.load(file)
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"{path}: {error}") from error


def load_patches(patch_dir):
    if not os.path.isdir(patch_dir):
        raise ValueError(f"patch directory does not exist: {patch_dir}")
    patches = {}
    for path in sorted(glob.glob(os.path.join(patch_dir, "*.json"))):
        if os.path.basename(path) == TEMPLATE_PATCH:
            continue
        patch = load_json(path)
        if not isinstance(patch, dict):
            raise ValueError(f"{path}: patch must be a JSON object")
        patch_id = patch.get("id")
        if not isinstance(patch_id, str) or not patch_id:
            raise ValueError(f"{path}: missing id")
        if patch_id in patches:
            raise ValueError(f"duplicate patch id: {patch_id}")
        if not isinstance(patch.get("target"), str) or not patch["target"]:
            raise ValueError(f"{path}: missing target")
        versions = patch.get("versions")
        if not isinstance(versions, list) or not versions:
            raise ValueError(f"{path}: missing versions")
        patches[patch_id] = patch
    return patches


def load_config(path):
    config = load_json(path)
    if not isinstance(config, dict):
        raise ValueError(f"{path}: config must be a JSON object")
    for patch_id, enabled in config.items():
        if not isinstance(patch_id, str) or not isinstance(enabled, bool):
            raise ValueError(f"{path}: patch states must be true or false")
    return config


def has_wildcard(value):
    return isinstance(value, str) and any(char in value for char in "*?[")


def selector_matches(pattern, value):
    return isinstance(pattern, str) and value is not None and fnmatch.fnmatchcase(value, pattern)


def ios_selector_matches(pattern, value):
    if value is None:
        return False
    expected, wildcard = parse_ios_version(pattern, "variant ios", True)
    current, _ = parse_ios_version(value, "--ios", False)
    if wildcard:
        return current[: len(expected)] == expected
    size = max(len(expected), len(current))
    return expected + (0,) * (size - len(expected)) == current + (0,) * (
        size - len(current)
    )


def parse_ios_version(value, field, allow_wildcard):
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a version string")
    wildcard = value.endswith(".*")
    if "*" in value and (not allow_wildcard or not wildcard or value.count("*") != 1):
        raise ValueError(f"{field} has an invalid wildcard: {value}")
    base = value[:-2] if wildcard else value
    parts = base.split(".")
    if not parts or any(not part.isdigit() for part in parts):
        raise ValueError(f"{field} is not a numeric iOS version: {value}")
    return tuple(int(part) for part in parts), wildcard


def compare_versions(left, right):
    size = max(len(left), len(right))
    return (left + (0,) * (size - len(left))) < (
        right + (0,) * (size - len(right))
    )


def range_matches(variant, ios):
    min_version = (
        parse_ios_version(variant["ios_min"], "ios_min", True)
        if "ios_min" in variant
        else None
    )
    max_version = (
        parse_ios_version(variant["ios_max"], "ios_max", True)
        if "ios_max" in variant
        else None
    )

    if min_version and max_version:
        min_parts = min_version[0]
        max_parts, max_wildcard = max_version
        if max_wildcard:
            prefix = min_parts[: len(max_parts)]
            prefix += (0,) * (len(max_parts) - len(prefix))
            if prefix > max_parts:
                raise ValueError("ios_min cannot be greater than ios_max")
        elif compare_versions(max_parts, min_parts):
            raise ValueError("ios_min cannot be greater than ios_max")

    if ios is None:
        return False
    current, _ = parse_ios_version(ios, "--ios", False)
    if min_version and compare_versions(current, min_version[0]):
        return False
    if max_version:
        max_parts, max_wildcard = max_version
        if max_wildcard:
            prefix = current[: len(max_parts)]
            prefix += (0,) * (len(max_parts) - len(prefix))
            if prefix > max_parts:
                return False
        elif compare_versions(max_parts, current):
            return False
    return True


def variant_rank(variant, ios, build):
    if not isinstance(variant, dict):
        raise ValueError("version variant must be a JSON object")
    if variant.get("default") is True:
        return 5

    ios_pattern = variant.get("ios")
    build_pattern = variant.get("build")
    has_range = "ios_min" in variant or "ios_max" in variant
    if ios_pattern is not None and not isinstance(ios_pattern, str):
        raise ValueError("variant ios must be a string")
    if build_pattern is not None and not isinstance(build_pattern, str):
        raise ValueError("variant build must be a string")
    if ios_pattern is not None and has_range:
        raise ValueError("variant cannot combine ios with ios_min or ios_max")
    if ios_pattern is None and build_pattern is None and not has_range:
        raise ValueError("variant needs ios, ios_min, ios_max, build, or default")
    if ios_pattern is not None and not ios_selector_matches(ios_pattern, ios):
        return None
    if build_pattern is not None and not selector_matches(build_pattern, build):
        return None
    if has_range:
        if not range_matches(variant, ios):
            return None
        return 4 if has_wildcard(build_pattern) else 3
    if has_wildcard(ios_pattern) or has_wildcard(build_pattern):
        return 4
    if ios_pattern is not None and build_pattern is not None:
        return 0
    if build_pattern is not None:
        return 1
    return 2


def select_variant(patch, ios, build):
    matches = []
    for index, variant in enumerate(patch["versions"]):
        rank = variant_rank(variant, ios, build)
        if rank is not None:
            matches.append((rank, index, variant))
    return min(matches, default=(None, None, None))[2]


def variant_label(variant):
    if variant.get("default") is True:
        return "default"
    parts = []
    if "ios" in variant:
        parts.append(f"iOS {variant['ios']}")
    elif "ios_min" in variant and "ios_max" in variant:
        parts.append(f"iOS {variant['ios_min']}-{variant['ios_max']}")
    elif "ios_min" in variant:
        parts.append(f"iOS >= {variant['ios_min']}")
    elif "ios_max" in variant:
        parts.append(f"iOS <= {variant['ios_max']}")
    if "build" in variant:
        parts.append(str(variant["build"]))
    return " / ".join(parts)


def parse_hex(value, field, patch_id):
    if not isinstance(value, str):
        raise ValueError(f"{patch_id}: {field} must be a hex string")
    try:
        result = bytes.fromhex(value)
    except ValueError as error:
        raise ValueError(f"{patch_id}: invalid {field}: {error}") from error
    if not result:
        raise ValueError(f"{patch_id}: {field} cannot be empty")
    return result


def parse_offset(value, patch_id):
    try:
        if isinstance(value, bool):
            raise ValueError
        offset = int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{patch_id}: invalid offset: {value!r}") from error
    if offset < 0:
        raise ValueError(f"{patch_id}: offset cannot be negative")
    return offset


def target_paths(system_root, target):
    if not target.startswith("/"):
        raise ValueError(f"target must be an absolute system path: {target}")
    root = os.path.abspath(system_root)
    actual = os.path.abspath(os.path.join(root, target.lstrip("/")))
    if os.path.commonpath((root, actual)) != root:
        raise ValueError(f"target escapes system root: {target}")
    display = system_root.rstrip("/") + target
    return actual, display


def collect_groups(patches, config, system_root, ios, build):
    groups = {}
    for patch_id, enabled in config.items():
        if not enabled:
            continue
        if patch_id not in patches:
            raise ValueError(f"enabled patch has no definition: {patch_id}")
        patch = patches[patch_id]
        variant = select_variant(patch, ios, build)
        if variant is None:
            raise ValueError(f"{patch_id}: no matching version")
        operations = variant.get("operations")
        if not isinstance(operations, list) or not operations:
            raise ValueError(f"{patch_id}: variant has no operations")

        actual, display = target_paths(system_root, patch["target"])
        group = groups.setdefault(
            actual,
            {
                "target": patch["target"],
                "display": display,
                "operations": [],
                "reports": [],
            },
        )
        for operation in operations:
            if not isinstance(operation, dict):
                raise ValueError(f"{patch_id}: operation must be a JSON object")
            offset = parse_offset(operation.get("offset"), patch_id)
            expected = parse_hex(operation.get("expected"), "expected", patch_id)
            replace = parse_hex(operation.get("replace"), "replace", patch_id)
            if len(expected) != len(replace):
                raise ValueError(
                    f"{patch_id}: expected and replace have different lengths"
                )
            item = {
                "patch_id": patch_id,
                "name": patch.get("name", patch_id),
                "variant": variant_label(variant),
                "offset": offset,
                "expected": expected,
                "replace": replace,
            }
            group["operations"].append(item)
            group["reports"].append(item)
    return groups


def validate_groups(groups):
    for path, group in groups.items():
        if not os.path.isfile(path):
            raise ValueError(f"target does not exist: {group['display']}")
        metadata = os.stat(path, follow_symlinks=True)
        with open(path, "rb") as file:
            original = file.read()
        occupied = []
        for operation in group["operations"]:
            start = operation["offset"]
            end = start + len(operation["expected"])
            if end > len(original):
                raise ValueError(
                    f"{operation['patch_id']}: offset 0x{start:x} is outside target"
                )
            for other_start, other_end in occupied:
                if start < other_end and end > other_start:
                    raise ValueError(
                        f"{operation['patch_id']}: overlapping operations at 0x{start:x}"
                    )
            occupied.append((start, end))
            before = original[start:end]
            if before != operation["expected"]:
                raise ValueError(
                    f"{operation['patch_id']} ({operation['variant']}): "
                    f"expected mismatch at 0x{start:x} "
                    f"(found {before.hex(' ')})"
                )
        group["original"] = original
        group["stat"] = metadata


def write_backup(backup, original, metadata):
    if os.path.exists(backup):
        return
    os.makedirs(os.path.dirname(backup), exist_ok=True)
    fd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, metadata.st_mode & 0o7777)
    try:
        with os.fdopen(fd, "wb") as file:
            file.write(original)
            file.flush()
            os.fsync(file.fileno())
        os.chown(backup, metadata.st_uid, metadata.st_gid)
        os.chmod(backup, metadata.st_mode & 0o7777)
        os.utime(backup, ns=(metadata.st_atime_ns, metadata.st_mtime_ns))
    except Exception:
        try:
            os.unlink(backup)
        except OSError:
            pass
        raise


def command_error(result):
    output = result.stderr.decode(errors="replace").strip()
    return output or f"exit code {result.returncode}"


def sign_file(sign_tool, source, staged, directory):
    try:
        extracted = subprocess.run(
            [sign_tool, "-e", source], stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
    except OSError as error:
        raise ValueError(f"cannot run signing tool {sign_tool}: {error}") from error
    if extracted.returncode != 0:
        raise ValueError(f"entitlements extraction failed: {command_error(extracted)}")

    entitlements_path = None
    try:
        command = [sign_tool, "-S", staged]
        if extracted.stdout.strip():
            fd, entitlements_path = tempfile.mkstemp(
                prefix=".entitlements.", suffix=".plist", dir=directory
            )
            with os.fdopen(fd, "wb") as file:
                file.write(extracted.stdout)
            command = [sign_tool, f"-S{entitlements_path}", staged]
        signed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if signed.returncode != 0:
            raise ValueError(f"signing failed: {command_error(signed)}")
    finally:
        if entitlements_path:
            try:
                os.unlink(entitlements_path)
            except OSError:
                pass


def copy_extended_metadata(source, destination):
    if hasattr(os, "listxattr"):
        for name in os.listxattr(source):
            os.setxattr(destination, name, os.getxattr(source, name))


def stage_group(path, group, sign_tool):
    patched = bytearray(group["original"])
    for operation in group["operations"]:
        start = operation["offset"]
        patched[start : start + len(operation["replace"])] = operation["replace"]

    directory = os.path.dirname(path)
    fd, staged = tempfile.mkstemp(
        prefix=f".{os.path.basename(path)}.", suffix=".patch", dir=directory
    )
    try:
        with os.fdopen(fd, "wb") as file:
            file.write(patched)
            file.flush()
            os.fsync(file.fileno())
        metadata = group["stat"]
        os.chmod(staged, metadata.st_mode & 0o7777)
        sign_file(sign_tool, path, staged, directory)
        copy_extended_metadata(path, staged)
        os.chown(staged, metadata.st_uid, metadata.st_gid)
        os.chmod(staged, metadata.st_mode & 0o7777)
        os.utime(staged, ns=(metadata.st_atime_ns, metadata.st_mtime_ns))
        if hasattr(os, "chflags") and hasattr(metadata, "st_flags"):
            os.chflags(staged, metadata.st_flags)
        return staged
    except Exception:
        try:
            os.unlink(staged)
        except OSError:
            pass
        raise


def apply_groups(groups, sign_tool, backup_dir=None):
    staged_files = {}
    try:
        for path, group in groups.items():
            staged_files[path] = stage_group(path, group, sign_tool)
        for path, group in groups.items():
            backup = (
                os.path.join(
                    os.path.abspath(backup_dir), group["target"].lstrip("/") + ".bak"
                )
                if backup_dir
                else path + ".bak"
            )
            write_backup(backup, group["original"], group["stat"])
        for path, staged in staged_files.items():
            os.replace(staged, path)
            directory_fd = os.open(os.path.dirname(path), os.O_RDONLY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
            staged_files[path] = None
    finally:
        for staged in staged_files.values():
            if staged:
                try:
                    os.unlink(staged)
                except OSError:
                    pass


def write_metadata(path, groups):
    metadata = [
        {"target": group["target"], "inode": os.stat(target).st_ino}
        for target, group in groups.items()
    ]
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(
        prefix=".binpatcher-metadata.", suffix=".json", dir=directory
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as file:
            json.dump(metadata, file, indent=2)
            file.write("\n")
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def print_reports(groups, signing, status):
    for group in groups.values():
        for item in group["reports"]:
            print(f"Patch: {item['name']}")
            print(f"Target: {group['display']}")
            print(f"Variant: {item['variant']}")
            print(f"Offset: 0x{item['offset']:x}")
            print(f"Before: {item['expected'].hex(' ')}")
            print(f"After:  {item['replace'].hex(' ')}")
            print(f"Signing: {signing}")
            print(f"Status: {status}")
            print()


def list_patches(patches, config):
    for patch_id, patch in patches.items():
        state = "ON " if config.get(patch_id, False) else "OFF"
        print(f"[{state}] {patch_id}: {patch.get('name', patch_id)}")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--system-root", help="root of the unpacked iOS system")
    parser.add_argument("--ios", help="target iOS version")
    parser.add_argument("--build", help="target iOS build identifier")
    parser.add_argument(
        "--patch-dir", default=DEFAULT_PATCH_DIR, help="patch definition directory"
    )
    parser.add_argument(
        "--config", default=DEFAULT_CONFIG, help="enabled patch state file"
    )
    parser.add_argument(
        "--sign-tool", default=DEFAULT_SIGN_TOOL, help="ldid-compatible signing tool"
    )
    parser.add_argument("--backup-dir", help="optional external backup directory")
    parser.add_argument(
        "--metadata-output", help="write changed targets and inode numbers as JSON"
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="validate without changing files"
    )
    parser.add_argument(
        "--list", action="store_true", help="list patches and their enabled state"
    )
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        patches = load_patches(args.patch_dir)
        config = load_config(args.config)
        if args.list:
            list_patches(patches, config)
            return 0
        if not args.system_root:
            raise ValueError("--system-root is required unless --list is used")
        groups = collect_groups(patches, config, args.system_root, args.ios, args.build)
        if not groups:
            print("No enabled patches.")
            return 0
        validate_groups(groups)
        if args.dry_run:
            print_reports(groups, "skipped (dry-run)", "validated")
            return 0
        apply_groups(groups, args.sign_tool, args.backup_dir)
        if args.metadata_output:
            write_metadata(args.metadata_output, groups)
        print_reports(groups, "success", "applied")
        return 0
    except (OSError, ValueError) as error:
        print(f"custom_bin_patcher: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
