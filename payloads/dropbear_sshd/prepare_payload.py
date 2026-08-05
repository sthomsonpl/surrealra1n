#!/usr/bin/env python3
"""Build a minimal rootless Dropbear/neofetch tree from Procursus packages."""

from __future__ import annotations

import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path


REPOSITORY = "https://apt.procurs.us"
PACKAGES_URL = f"{REPOSITORY}/dists/1800/main/binary-iphoneos-arm64/Packages"
BASE_PACKAGES_URL = (
    f"{REPOSITORY}/dists/iphoneos-arm64/1700/main/binary-iphoneos-arm/Packages"
)
SEED_PACKAGES = (
    "dropbear",
    "bash",
    "zsh",
    "coreutils",
    "darwintools",
    "system-cmds",
    "shell-cmds",
    "gawk",
    "grep",
    "sed",
    "findutils",
    "file",
    "less",
    "nano",
    "vim",
    "tar",
    "gzip",
    "bzip2",
    "xz-utils",
    "zip",
    "unzip",
    "curl",
    "wget",
    "rsync",
    "tmux",
    "top",
    "htop",
    "toybox",
    "launchctl",
    "openssh-client",
    "openssh-sftp-server",
    "neofetch",
)
PROVIDERS = {"awk": "gawk", "sh": "dash"}
IGNORED_DEPENDENCIES = {"firmware", "profile.d"}

HERE = Path(__file__).resolve().parent
CACHE = HERE / ".cache"
OUTPUT = HERE / "rootfs"
MANIFEST = HERE / "payload-manifest.txt"
METADATA = HERE / "payload-metadata.json"
ALPINE_HASH = (
    "$6$surrealra1n$93k/ZXbA1HZ/ly4DRE1WyJeQf4YvYl1.vH7qwrd1tzejr9BX"
    "DE6PXsownwLth8E.i/4KkmxkIRnn4RHWqwTEZ."
)

NEOFETCH_DARWIN_PLIST_READER = """\
        IFS=$'\\n' read -d "" -ra sw_vers <<< "$(awk -F'<|>' '/key|string/ {print $3}' \\
                            "/System/Library/CoreServices/SystemVersion.plist")"
        for ((i=0;i<${#sw_vers[@]};i+=2)) {
            case ${sw_vers[i]} in
                ProductName)          darwin_name=${sw_vers[i+1]} ;;
                ProductFamily)        darwin_family=${sw_vers[i+1]} ;;
                ProductVersion)       osx_version=${sw_vers[i+1]} ;;
                ProductBuildVersion)  osx_build=${sw_vers[i+1]}   ;;
            esac
        }
"""

NEOFETCH_SW_VERS_READER = """\
        local sw_vers_command
        if [[ -x /var/jb/usr/bin/sw_vers ]]; then
            sw_vers_command=/var/jb/usr/bin/sw_vers
        elif command -v sw_vers >/dev/null 2>&1; then
            sw_vers_command=sw_vers
        fi

        if [[ $sw_vers_command ]]; then
            darwin_name=$("$sw_vers_command" -productName 2>/dev/null)
            osx_version=$("$sw_vers_command" -productVersion 2>/dev/null)
            osx_build=$("$sw_vers_command" -buildVersion 2>/dev/null)
        fi

        # Fall back to the system plist.
        if [[ -z $darwin_name || -z $osx_version ]] && command -v awk >/dev/null 2>&1; then
            IFS=$'\\n' read -d "" -ra sw_vers <<< "$(awk -F'<|>' '/key|string/ {print $3}' \\
                                "/System/Library/CoreServices/SystemVersion.plist")"
            for ((i=0;i<${#sw_vers[@]};i+=2)) {
                case ${sw_vers[i]} in
                    ProductName)          darwin_name=${sw_vers[i+1]} ;;
                    ProductFamily)        darwin_family=${sw_vers[i+1]} ;;
                    ProductVersion)       osx_version=${sw_vers[i+1]} ;;
                    ProductBuildVersion)  osx_build=${sw_vers[i+1]}   ;;
                esac
            }
        fi
"""


def download(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(url, headers={"User-Agent": "curl/8.7.1"})
    with urllib.request.urlopen(request) as response, destination.open("wb") as output:
        shutil.copyfileobj(response, output)


def parse_control(text: str) -> list[dict[str, str]]:
    paragraphs: list[dict[str, str]] = []
    for raw_paragraph in re.split(r"\r?\n\r?\n", text.strip()):
        fields: dict[str, str] = {}
        current = ""
        for line in raw_paragraph.splitlines():
            if line[:1].isspace() and current:
                fields[current] += " " + line.strip()
                continue
            if ":" not in line:
                continue
            current, value = line.split(":", 1)
            fields[current] = value.strip()
        if "Package" in fields:
            paragraphs.append(fields)
    return paragraphs


def version_is_newer(candidate: str, current: str) -> bool:
    return subprocess.run(
        ["dpkg", "--compare-versions", candidate, "gt", current], check=False
    ).returncode == 0


def select_packages(
    paragraphs: list[dict[str, str]], architectures: set[str]
) -> dict[str, dict[str, str]]:
    selected: dict[str, dict[str, str]] = {}
    for package in paragraphs:
        name = package["Package"]
        architecture = package.get("Architecture", "")
        if architecture not in architectures:
            continue
        if name not in selected or version_is_newer(
            package.get("Version", "0"), selected[name].get("Version", "0")
        ):
            selected[name] = package
    return selected


def dependency_names(value: str) -> list[list[str]]:
    groups: list[list[str]] = []
    for requirement in value.split(","):
        alternatives: list[str] = []
        for alternative in requirement.split("|"):
            name = re.split(r"\s|\(", alternative.strip(), maxsplit=1)[0]
            name = name.split(":", 1)[0]
            if name:
                alternatives.append(name)
        if alternatives:
            groups.append(alternatives)
    return groups


def resolve(selected: dict[str, dict[str, str]]) -> list[dict[str, str]]:
    resolved: dict[str, dict[str, str]] = {}
    pending = list(SEED_PACKAGES)
    while pending:
        requested = pending.pop(0)
        name = PROVIDERS.get(requested, requested)
        if name in resolved or name in IGNORED_DEPENDENCIES:
            continue
        if name not in selected:
            raise RuntimeError(f"Procursus index does not contain required package: {name}")
        package = selected[name]
        resolved[name] = package
        for field in ("Pre-Depends", "Depends"):
            for alternatives in dependency_names(package.get(field, "")):
                choice = next(
                    (
                        PROVIDERS.get(candidate, candidate)
                        for candidate in alternatives
                        if PROVIDERS.get(candidate, candidate) in selected
                        or candidate in IGNORED_DEPENDENCIES
                    ),
                    None,
                )
                if choice and choice not in IGNORED_DEPENDENCIES:
                    pending.append(choice)
                elif not any(candidate in IGNORED_DEPENDENCIES for candidate in alternatives):
                    raise RuntimeError(
                        "Procursus indexes do not satisfy dependency: "
                        + " | ".join(alternatives)
                    )
    return [resolved[name] for name in sorted(resolved)]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def materialize_symlinks(root: Path) -> None:
    links: list[Path] = []
    for directory, directory_names, file_names in os.walk(root, followlinks=False):
        base = Path(directory)
        links.extend(base / name for name in directory_names if (base / name).is_symlink())
        links.extend(base / name for name in file_names if (base / name).is_symlink())
    for link in sorted(links, key=lambda path: len(path.parts), reverse=True):
        target_text = os.readlink(link)
        if target_text.startswith("/"):
            target = root / target_text.lstrip("/")
        else:
            target = link.parent / target_text
        target = target.resolve(strict=False)
        if not target.exists():
            link.unlink()
            continue
        link.unlink()
        if target.is_dir():
            shutil.copytree(target, link, symlinks=False)
        else:
            shutil.copy2(target, link)


def patch_neofetch(neofetch: Path) -> None:
    script = neofetch.read_text(encoding="utf-8")
    if NEOFETCH_DARWIN_PLIST_READER not in script:
        raise RuntimeError("Could not locate neofetch's Darwin version reader")
    neofetch.write_text(
        script.replace(
            NEOFETCH_DARWIN_PLIST_READER,
            NEOFETCH_SW_VERS_READER,
            1,
        ),
        encoding="utf-8",
    )


def prune_terminfo(jb: Path) -> None:
    """Keep common SSH terminal definitions without the 5,000-entry database."""

    patterns = (
        "ansi",
        "dumb",
        "linux",
        "vt100",
        "vt220",
        "alacritty*",
        "contour*",
        "foot*",
        "ghostty*",
        "kitty*",
        "putty*",
        "rxvt*",
        "screen*",
        "tmux*",
        "wezterm*",
        "xterm*",
    )
    locations = (
        jb / "usr" / "lib" / "terminfo",
        jb / "usr" / "share" / "terminfo",
    )
    if not any(terminfo.exists() for terminfo in locations):
        raise RuntimeError("Prepared tree does not contain a terminfo database")

    for terminfo in locations:
        if not terminfo.exists():
            continue
        for entry in terminfo.rglob("*"):
            if entry.is_file() and not any(
                fnmatch.fnmatchcase(entry.name, pattern) for pattern in patterns
            ):
                entry.unlink()
        for directory in sorted(
            (path for path in terminfo.rglob("*") if path.is_dir()),
            key=lambda path: len(path.parts),
            reverse=True,
        ):
            try:
                directory.rmdir()
            except OSError:
                pass


def copy_command(source: Path, destination: Path) -> None:
    if not source.exists():
        raise RuntimeError(f"Prepared tree does not contain {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def write_payload_metadata(root: Path) -> None:
    entries: list[dict[str, object]] = []
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        mode = path.stat().st_mode
        entries.append(
            {
                "path": relative,
                "type": "directory" if path.is_dir() else "file",
                "uid": 0,
                "gid": 0,
                "mode": "0755" if path.is_dir() or mode & 0o111 else "0644",
            }
        )
    metadata = {
        "schema_version": 1,
        "rootfs": entries,
        "ramdisk": [
            {
                "source": "com.surrealra1n.install-dropbear.plist",
                "destination": (
                    "System/Library/LaunchDaemons/"
                    "com.surrealra1n.install-dropbear.plist"
                ),
                "uid": 0,
                "gid": 0,
                "mode": "0644",
            }
        ],
    }
    METADATA.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    if shutil.which("dpkg-deb") is None or shutil.which("dpkg") is None:
        print("[!] dpkg and dpkg-deb are required to prepare the SSH payload.", file=sys.stderr)
        return 1

    CACHE.mkdir(parents=True, exist_ok=True)
    index_path = CACHE / "Packages"
    base_index_path = CACHE / "Packages.base"
    print(f"[*] Fetching Procursus package index: {PACKAGES_URL}")
    download(PACKAGES_URL, index_path)
    print(f"[*] Fetching Procursus base index: {BASE_PACKAGES_URL}")
    download(BASE_PACKAGES_URL, base_index_path)
    selected = select_packages(
        parse_control(index_path.read_text(encoding="utf-8")),
        {"iphoneos-arm64", "all"},
    )
    base_packages = select_packages(
        parse_control(base_index_path.read_text(encoding="utf-8")),
        {"iphoneos-arm", "all"},
    )
    for name, package in base_packages.items():
        selected.setdefault(name, package)
    packages = resolve(selected)

    with tempfile.TemporaryDirectory(prefix="surrealra1n-sshd-") as temporary:
        extracted = Path(temporary) / "rootfs"
        extracted.mkdir()
        manifest_lines = [f"Repository: {REPOSITORY}", "Suite: 1800", ""]
        for package in packages:
            filename = package["Filename"]
            archive = CACHE / Path(filename).name
            expected = package["SHA256"]
            if not archive.exists() or sha256(archive) != expected:
                print(f"[*] Downloading {package['Package']} {package['Version']}")
                download(f"{REPOSITORY}/{filename}", archive)
            actual = sha256(archive)
            if actual != expected:
                raise RuntimeError(f"SHA256 mismatch for {archive.name}")
            subprocess.run(["dpkg-deb", "-x", archive, extracted], check=True)
            manifest_lines.append(
                f"{package['Package']} {package['Version']} {expected} {filename}"
            )

        # Move legacy libraries into /var/jb.
        rootful_usr = extracted / "usr"
        if rootful_usr.exists():
            shutil.copytree(
                rootful_usr,
                extracted / "var" / "jb" / "usr",
                dirs_exist_ok=True,
                symlinks=True,
            )
            shutil.rmtree(rootful_usr)

        jb = extracted / "var" / "jb"
        if not (jb / "usr" / "sbin" / "dropbear").exists():
            raise RuntimeError("Prepared tree does not contain /var/jb/usr/sbin/dropbear")
        if not (jb / "usr" / "bin" / "bash").exists():
            raise RuntimeError("Prepared tree does not contain /var/jb/usr/bin/bash")
        if not (jb / "usr" / "bin" / "neofetch").exists():
            raise RuntimeError("Prepared tree does not contain /var/jb/usr/bin/neofetch")

        # Create alternatives skipped by package scripts.
        copy_command(jb / "usr" / "bin" / "gawk", jb / "usr" / "bin" / "awk")

        if not (jb / "usr" / "bin" / "which").exists():
            raise RuntimeError("Prepared tree does not contain /var/jb/usr/bin/which")
        for vim_command in ("vim", "vi", "view"):
            copy_command(
                jb / "usr" / "bin" / "vim.basic",
                jb / "usr" / "bin" / vim_command,
            )
        for toybox_command in ("ps", "pgrep", "pkill"):
            copy_command(
                jb / "usr" / "bin" / "toybox",
                jb / "usr" / "bin" / toybox_command,
            )

        # Add a short SFTP path for Dropbear.
        copy_command(
            jb / "usr" / "libexec" / "sftp-server",
            jb / "sftp-server",
        )
        patch_neofetch(jb / "usr" / "bin" / "neofetch")

        etc = jb / "etc"
        etc.mkdir(parents=True, exist_ok=True)
        (etc / "master.passwd").write_text(
            f"root:{ALPINE_HASH}:0:0::0:0:System Administrator:/var/root:"
            "/var/jb/usr/bin/bash\n",
            encoding="utf-8",
        )
        (etc / "passwd").write_text(
            "root:*:0:0:System Administrator:/var/root:/var/jb/usr/bin/bash\n",
            encoding="utf-8",
        )
        (etc / "group").write_text("wheel:*:0:root\n", encoding="utf-8")
        (etc / "shells").write_text(
            "/bin/sh\n"
            "/var/jb/usr/bin/bash\n"
            "/var/jb/usr/bin/zsh\n"
            "/var/jb/usr/bin/sh\n",
            encoding="utf-8",
        )
        (etc / "profile").write_text(
            "export PATH=/var/jb/data/usr/local/bin:/var/jb/data/usr/local/sbin:"
            "/var/jb/usr/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin\n"
            "export HOME=/var/root\n"
            "export SHELL=/var/jb/usr/bin/bash\n"
            "export TMPDIR=/private/var/tmp\n"
            "export PS1='surrealra1n:\\w \\u\\$ '\n"
            "umask 022\n",
            encoding="utf-8",
        )

        wrapper = jb / "usr" / "libexec" / "dropbear-wrapper"
        wrapper.write_text(
            "#!/var/jb/usr/bin/bash\n"
            "export PATH=/var/jb/data/usr/local/bin:/var/jb/data/usr/local/sbin:"
            "/var/jb/usr/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin\n"
            "export DYLD_LIBRARY_PATH=/var/jb/usr/lib\n"
            "umask 077\n"
            "echo '[surrealra1n dropbear] initializing runtime' >&2\n"
            "if [ ! -s /var/jb/etc/pwd.db ] || [ ! -s /var/jb/etc/spwd.db ]; then\n"
            "    if [ ! -w /var/jb/etc ]; then\n"
            "        echo '[surrealra1n dropbear] /var/jb/etc is not writable' >&2\n"
            "        exit 1\n"
            "    fi\n"
            "    echo '[surrealra1n dropbear] generating password database' >&2\n"
            "    rm -f /var/jb/etc/pwd.db /var/jb/etc/spwd.db\n"
            "    /var/jb/usr/sbin/pwd_mkdb -p /var/jb/etc/master.passwd || {\n"
            "        echo '[surrealra1n dropbear] pwd_mkdb failed' >&2\n"
            "        exit 1\n"
            "    }\n"
            "fi\n"
            "keydir=/var/jb/etc/dropbear\n"
            "mkdir -p \"$keydir\" || exit 1\n"
            "for type in rsa ecdsa ed25519; do\n"
            "    key=\"$keydir/dropbear_${type}_host_key\"\n"
            "    if [ ! -s \"$key\" ]; then\n"
            "        echo \"[surrealra1n dropbear] generating $type host key\" >&2\n"
            "        dropbearkey -t \"$type\" -f \"$key\" || exit 1\n"
            "    fi\n"
            "done\n"
            "echo '[surrealra1n dropbear] listening on port 22' >&2\n"
            "exec /var/jb/usr/sbin/dropbear "
            "-r \"$keydir/dropbear_rsa_host_key\" "
            "-r \"$keydir/dropbear_ecdsa_host_key\" "
            "-r \"$keydir/dropbear_ed25519_host_key\" "
            "-F -E -p \"${1:-22}\"\n",
            encoding="utf-8",
        )
        wrapper.chmod(0o755)

        services = jb / "Library" / "SurrealLoader" / "Services"
        services.mkdir(parents=True, exist_ok=True)
        dropbear_service = services / "dropbear.service"
        dropbear_service.write_text(
            "#!/var/jb/usr/bin/bash\n"
            "exec /var/jb/usr/libexec/dropbear-wrapper 22\n",
            encoding="utf-8",
        )
        dropbear_service.chmod(0o755)

        # Prevent a second Dropbear service.
        (jb / "Library" / "LaunchDaemons" / "com.mkj.dropbear.plist").unlink(
            missing_ok=True
        )

        for relative in (
            "var/jb/usr/share/doc",
            "var/jb/usr/share/info",
            "var/jb/usr/share/man",
            "var/jb/usr/share/locale",
            "var/jb/usr/include",
            "var/jb/usr/lib/pkgconfig",
        ):
            shutil.rmtree(extracted / relative, ignore_errors=True)

        # Remove development archives.
        for static_archive in (jb / "usr" / "lib").rglob("*.a"):
            static_archive.unlink()

        # Replace symlinks before hfsplus embedding.
        materialize_symlinks(extracted)
        prune_terminfo(jb)

        shutil.rmtree(OUTPUT, ignore_errors=True)
        shutil.copytree(extracted, OUTPUT, symlinks=True)
        MANIFEST.write_text("\n".join(manifest_lines) + "\n", encoding="utf-8")
        write_payload_metadata(OUTPUT)

    print(f"[*] SSH payload is ready under {OUTPUT}")
    print(f"[*] SSH payload metadata is ready at {METADATA}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"[!] Failed to prepare SSH payload: {error}", file=sys.stderr)
        raise SystemExit(1)
