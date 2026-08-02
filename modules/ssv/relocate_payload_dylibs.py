#!/usr/bin/env python3

import argparse
import subprocess
from pathlib import Path


def output(*arguments: str) -> str:
    return subprocess.run(
        arguments,
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def is_macho(path: Path) -> bool:
    with path.open("rb") as binary:
        magic = binary.read(4)
    return magic in {
        b"\xfe\xed\xfa\xce",
        b"\xce\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf",
        b"\xcf\xfa\xed\xfe",
        b"\xca\xfe\xba\xbe",
        b"\xbe\xba\xfe\xca",
        b"\xca\xfe\xba\xbf",
        b"\xbf\xba\xfe\xca",
    }


def linked_images(path: Path) -> list[str]:
    lines = output("otool", "-L", str(path)).splitlines()[1:]
    return [line.strip().split(" ", 1)[0] for line in lines if line.strip()]


def install_name(path: Path) -> str | None:
    lines = output("otool", "-D", str(path)).splitlines()[1:]
    return lines[0].strip() if lines else None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Relocate payload-local /usr/lib dependencies into /var/jb"
    )
    parser.add_argument("root", type=Path)
    arguments = parser.parse_args()

    root = arguments.root.resolve()
    library_directory = root / "usr" / "lib"
    local_libraries = {
        path.name: path
        for path in library_directory.iterdir()
        if path.is_file() or path.is_symlink()
    }

    macho_files = [path for path in root.rglob("*") if path.is_file() and is_macho(path)]
    changes = 0
    for path in macho_files:
        identifier = install_name(path)
        if identifier and identifier.startswith("/usr/lib/"):
            basename = Path(identifier).name
            if basename in local_libraries:
                relocated = f"@rpath/{basename}"
                subprocess.run(
                    ["install_name_tool", "-id", relocated, str(path)],
                    check=True,
                )
                print(f"[*] {path.relative_to(root)}: id {identifier} -> {relocated}")
                changes += 1

        for dependency in linked_images(path):
            if dependency == identifier or not dependency.startswith("/usr/lib/"):
                continue
            basename = Path(dependency).name
            if basename not in local_libraries:
                continue
            relocated = f"@rpath/{basename}"
            subprocess.run(
                [
                    "install_name_tool",
                    "-change",
                    dependency,
                    relocated,
                    str(path),
                ],
                check=True,
            )
            print(
                f"[*] {path.relative_to(root)}: {dependency} -> {relocated}"
            )
            changes += 1

    unresolved: list[str] = []
    for path in macho_files:
        for dependency in linked_images(path):
            basename = Path(dependency).name
            if dependency.startswith("/usr/lib/") and basename in local_libraries:
                unresolved.append(f"{path.relative_to(root)}: {dependency}")
    if unresolved:
        raise RuntimeError(
            "payload-local absolute dependencies remain:\n" + "\n".join(unresolved)
        )

    print(f"[*] Relocated {changes} payload-local install names")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
