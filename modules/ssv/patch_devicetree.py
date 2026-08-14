#!/usr/bin/env python3
"""Apply validated iOS 17 profiles to an Apple flattened DeviceTree.

The legacy boot profile follows pwnerblu's a12-a13-ios17-early-POC.  The SEP
compatibility and restore profiles keep stock ephemeral storage so normal boot
and Rose avoid the POC-only storage path.  The parser and serializer live in
Forge so IPSW builds do not download executable code.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass, field
from pathlib import Path


NAME_SIZE = 32
LENGTH_MASK = 0x7FFFFFFF
PLACEHOLDER_FLAG = 0x80000000
U32_ONE = struct.pack("<I", 1)
U32_ZERO = struct.pack("<I", 0)


def align4(value: int) -> int:
    return (value + 3) & ~3


@dataclass
class Property:
    name_raw: bytes
    raw_length: int
    value: bytes

    @property
    def name(self) -> str:
        return self.name_raw.split(b"\0", 1)[0].decode("ascii", "strict")

    @classmethod
    def create(cls, name: str, value: bytes) -> "Property":
        encoded = name.encode("ascii")
        if len(encoded) >= NAME_SIZE:
            raise ValueError(f"property name is too long: {name}")
        return cls(encoded.ljust(NAME_SIZE, b"\0"), len(value), value)

    def set_value(self, value: bytes) -> None:
        flags = self.raw_length & PLACEHOLDER_FLAG
        self.raw_length = flags | len(value)
        self.value = value

    def serialize(self) -> bytes:
        padding = b"\0" * (align4(len(self.value)) - len(self.value))
        return self.name_raw + struct.pack("<I", self.raw_length) + self.value + padding


@dataclass
class Node:
    properties: list[Property] = field(default_factory=list)
    children: list["Node"] = field(default_factory=list)

    @property
    def name(self) -> str:
        prop = self.property("name")
        if prop is None:
            return ""
        return prop.value.split(b"\0", 1)[0].decode("ascii", "strict")

    def property(self, name: str) -> Property | None:
        return next((prop for prop in self.properties if prop.name == name), None)

    def child(self, name: str) -> "Node | None":
        return next((child for child in self.children if child.name == name), None)

    def serialize(self) -> bytes:
        output = bytearray(struct.pack("<II", len(self.properties), len(self.children)))
        for prop in self.properties:
            output.extend(prop.serialize())
        for child in self.children:
            output.extend(child.serialize())
        return bytes(output)


def parse_node(data: bytes, offset: int) -> tuple[int, Node]:
    if offset + 8 > len(data):
        raise ValueError(f"truncated node header at {offset:#x}")
    property_count, child_count = struct.unpack_from("<II", data, offset)
    offset += 8
    node = Node()
    for _ in range(property_count):
        if offset + NAME_SIZE + 4 > len(data):
            raise ValueError(f"truncated property header at {offset:#x}")
        name_raw = data[offset : offset + NAME_SIZE]
        raw_length = struct.unpack_from("<I", data, offset + NAME_SIZE)[0]
        length = raw_length & LENGTH_MASK
        value_offset = offset + NAME_SIZE + 4
        end = value_offset + align4(length)
        if end > len(data):
            raise ValueError(f"truncated property value at {offset:#x}")
        node.properties.append(
            Property(name_raw, raw_length, data[value_offset : value_offset + length])
        )
        offset = end
    for _ in range(child_count):
        offset, child = parse_node(data, offset)
        node.children.append(child)
    return offset, node


def resolve(root: Node, path: str) -> Node | None:
    node = root
    for component in path.strip("/").split("/"):
        if not component:
            continue
        node = node.child(component)
        if node is None:
            return None
    return node


def set_property(root: Node, path: str, name: str, value: bytes) -> str:
    node = resolve(root, path)
    if node is None:
        raise ValueError(f"DeviceTree node not found: {path}")
    prop = node.property(name)
    if prop is None:
        node.properties.append(Property.create(name, value))
        return "added"
    if prop.value == value:
        return "unchanged"
    prop.set_value(value)
    return "updated"


def delete_property(root: Node, path: str, name: str) -> str:
    node = resolve(root, path)
    if node is None:
        raise ValueError(f"DeviceTree node not found: {path}")
    prop = node.property(name)
    if prop is None:
        return "absent"
    node.properties.remove(prop)
    return "removed"


PATCH_PROFILES = {
    "boot": (
        ("delete", "/defaults", "content-protect", None),
        ("set", "/product", "boot-ios-diagnostics", U32_ONE),
        ("set", "/chosen", "ephemeral-storage", U32_ONE),
        ("set", "/chosen", "disable-transport-rm", U32_ONE),
    ),
    "restore": (
        ("delete", "/defaults", "content-protect", None),
    ),
    "sep-compat": (
        ("delete", "/defaults", "content-protect", None),
        ("set", "/product", "boot-ios-diagnostics", U32_ONE),
        ("set", "/chosen", "disable-transport-rm", U32_ONE),
    ),
}


def verify(root: Node, profile: str) -> None:
    for operation, path, name, value in PATCH_PROFILES[profile]:
        node = resolve(root, path)
        prop = node.property(name) if node else None
        valid = prop is None if operation == "delete" else prop is not None and prop.value == value
        if not valid:
            raise ValueError(f"verification failed for {path}/{name}")
    if profile in ("restore", "sep-compat"):
        chosen = resolve(root, "/chosen")
        ephemeral = chosen.property("ephemeral-storage") if chosen else None
        if ephemeral is None or ephemeral.value != U32_ZERO:
            raise ValueError(
                f"{profile} profile requires stock /chosen/ephemeral-storage=0"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--profile", choices=tuple(PATCH_PROFILES), default="boot"
    )
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    source = args.input.read_bytes()
    parsed_size, root = parse_node(source, 0)
    if parsed_size != len(source):
        raise SystemExit(
            f"DeviceTree parser stopped at {parsed_size:#x}, file size is {len(source):#x}"
        )
    if root.serialize() != source:
        raise SystemExit("DeviceTree failed the lossless pre-patch round trip")

    for operation, path, name, value in PATCH_PROFILES[args.profile]:
        if operation == "delete":
            status = delete_property(root, path, name)
        else:
            status = set_property(root, path, name, value)
        print(f"[+] {operation:6s} {path}/{name}: {status}")

    output = root.serialize()
    reparsed_size, reparsed_root = parse_node(output, 0)
    if reparsed_size != len(output):
        raise SystemExit("patched DeviceTree did not parse completely")
    verify(reparsed_root, args.profile)
    args.output.write_bytes(output)
    print(
        f"[+] wrote {args.profile} profile to {args.output} "
        f"({len(source)} -> {len(output)} bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
