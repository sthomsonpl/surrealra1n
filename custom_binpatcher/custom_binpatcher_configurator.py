#!/usr/bin/env python3
"""Configure binary patches in a simple CLI menu."""

import argparse
import glob
import json
import os
import sys
import tempfile


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PATCH_DIR = os.path.join(SCRIPT_DIR, "patches")
DEFAULT_CONFIG = os.path.join(SCRIPT_DIR, "patches.json")
TEMPLATE_PATCHES = {
    "template_patch.json",
    "template_multi_target_patch.json",
    "template_patchfind.json",
}


def load_json(path, missing=None):
    try:
        with open(path, encoding="utf-8") as file:
            return json.load(file)
    except FileNotFoundError:
        if missing is not None:
            return missing
        raise
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"{path}: {error}") from error


def scan_patches(patch_dir):
    if not os.path.isdir(patch_dir):
        raise ValueError(f"patch directory does not exist: {patch_dir}")
    patches = {}
    for path in sorted(glob.glob(os.path.join(patch_dir, "*.json"))):
        if os.path.basename(path) in TEMPLATE_PATCHES:
            continue
        patch = load_json(path)
        if not isinstance(patch, dict):
            raise ValueError(f"{path}: patch must be a JSON object")
        values = {}
        for field in ("id", "name", "description"):
            value = patch.get(field)
            if not isinstance(value, str) or not value:
                raise ValueError(f"{path}: missing {field}")
            values[field] = value
        if values["id"] in patches:
            raise ValueError(f"duplicate patch id: {values['id']}")
        patches[values["id"]] = values
    return patches


def load_config(path):
    config = load_json(path, {})
    if not isinstance(config, dict):
        raise ValueError(f"{path}: config must be a JSON object")
    for patch_id, enabled in config.items():
        if not isinstance(patch_id, str) or not isinstance(enabled, bool):
            raise ValueError(f"{path}: patch states must be true or false")
    return config


def sync_config(config, patches):
    return {patch_id: config.get(patch_id, False) for patch_id in patches}


def save_config(path, config):
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".patches.", suffix=".json", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as file:
            json.dump(config, file, indent=2)
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


def show_menu(patches, config, dirty):
    print("\nCustom Binpatcher\n")
    if patches:
        for number, (patch_id, patch) in enumerate(patches.items(), 1):
            state = "ON " if config[patch_id] else "OFF"
            print(f"[{number}] [{state}] {patch['name']}")
            print(f"    {patch['description']}")
    else:
        print("No custom patch definitions found.")
        print("Copy template_patch.json to a new JSON file to create one.")
    print("\n[S] Save")
    print("[R] Rescan")
    print("[Q] Quit")
    if dirty:
        print("\n* Unsaved changes")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--patch-dir", default=DEFAULT_PATCH_DIR, help="patch definition directory"
    )
    parser.add_argument(
        "--config", default=DEFAULT_CONFIG, help="enabled patch state file"
    )
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        patches = scan_patches(args.patch_dir)
        original = load_config(args.config)
        config = sync_config(original, patches)
        dirty = config != original

        while True:
            show_menu(patches, config, dirty)
            try:
                choice = input("\nChoice: ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                return 0
            if choice.lower() == "s":
                save_config(args.config, config)
                dirty = False
                print("Configuration saved.")
            elif choice.lower() == "r":
                new_patches = scan_patches(args.patch_dir)
                new_config = sync_config(config, new_patches)
                dirty = dirty or new_config != config
                patches, config = new_patches, new_config
                print("Patch definitions rescanned.")
            elif choice.lower() == "q":
                if dirty:
                    answer = input("Quit without saving? [y/N]: ").strip().lower()
                    if answer not in ("y", "yes"):
                        continue
                return 0
            elif choice.isdigit() and 1 <= int(choice) <= len(patches):
                patch_id = list(patches)[int(choice) - 1]
                config[patch_id] = not config[patch_id]
                dirty = True
            else:
                print("Unknown option.")
    except (OSError, ValueError) as error:
        print(f"custom_binpatcher_configurator: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
