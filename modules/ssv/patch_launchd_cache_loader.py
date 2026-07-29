#!/usr/bin/env python3
"""Force launchd_cache_loader to use its built-in unsecure path."""

import argparse
import os
import stat
import struct
import sys


KERN_BOOTARGS = b"kern.bootargs\0"
UNSECURE_CACHE = b"launchd_unsecure_cache=\0"
NOP = 0xD503201F


def instruction(contents: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(contents):
        raise IndexError
    return struct.unpack_from("<I", contents, offset)[0]


def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def is_adr(value: int, register: int) -> bool:
    return value & 0x9F00001F == 0x10000000 | register


def adr_target(value: int, offset: int) -> int:
    immediate = ((value >> 5) & 0x7FFFF) << 2
    immediate |= (value >> 29) & 0x3
    return offset + sign_extend(immediate, 21)


def references_literal(
    contents: bytes, offset: int, register: int, literal: bytes
) -> bool:
    try:
        value = instruction(contents, offset)
    except IndexError:
        return False
    if not is_adr(value, register):
        return False
    target = adr_target(value, offset)
    return 0 <= target <= len(contents) - len(literal) and contents.startswith(
        literal, target
    )


def is_bl(value: int) -> bool:
    return value & 0xFC000000 == 0x94000000


def is_cbz_x0(value: int) -> bool:
    return value & 0xFF00001F == 0xB4000000


def is_stack_zero_store(value: int) -> bool:
    # str xzr, [sp, #imm]
    return value & 0xFFC003FF == 0xF90003FF


def stack_store_offset(value: int) -> int:
    return ((value >> 10) & 0xFFF) * 8


def is_stack_address(value: int, stack_offset: int) -> bool:
    # add x1, sp, #imm
    return (
        value & 0xFFC003FF == 0x910003E1
        and ((value >> 10) & 0xFFF) == stack_offset
    )


def branch_target(value: int, offset: int) -> int | None:
    if value & 0xFC000000 != 0x14000000:
        return None
    return offset + sign_extend(value & 0x03FFFFFF, 26) * 4


def encode_branch(source: int, target: int) -> int:
    displacement = target - source
    if displacement % 4 != 0 or not -(1 << 27) <= displacement < (1 << 27):
        raise ValueError("unsecure cache path is outside the ARM64 branch range")
    return 0x14000000 | ((displacement // 4) & 0x03FFFFFF)


def following_non_nop(contents: bytes, offset: int, limit: int = 2) -> int | None:
    for _ in range(limit + 1):
        try:
            if instruction(contents, offset) != NOP:
                return offset
        except IndexError:
            return None
        offset += 4
    return None


def unsecure_path_entry(contents: bytes, reference: int) -> int | None:
    if not references_literal(contents, reference, 1, UNSECURE_CACHE):
        return None
    move = following_non_nop(contents, reference + 4)
    if move is None:
        return None
    try:
        # mov x2, #0; bl strstr; cbz x0, secure_path
        if instruction(contents, move) != 0xD2800002:
            return None
        if not is_bl(instruction(contents, move + 4)):
            return None
        if not is_cbz_x0(instruction(contents, move + 8)):
            return None
    except IndexError:
        return None
    return move + 12


def find_control_candidates(contents: bytes) -> list[tuple[int, int, str]]:
    candidates: list[tuple[int, int, str]] = []
    for reference in range(0, len(contents) - 3, 4):
        destination = unsecure_path_entry(contents, reference)
        if destination is None:
            continue

        # Linker relaxation may change ADR and BL immediates or insert NOPs.
        search_start = max(4, reference - 0x80)
        for control in range(search_start, reference, 4):
            try:
                store = instruction(contents, control - 4)
                value = instruction(contents, control)
            except IndexError:
                continue
            if not is_stack_zero_store(store):
                continue

            if references_literal(contents, control, 0, KERN_BOOTARGS):
                kind = "stock"
            elif branch_target(value, control) == destination:
                kind = "patched"
            else:
                continue

            stack_offset = stack_store_offset(store)
            address = following_non_nop(contents, control + 4)
            if address is None:
                continue
            try:
                if not is_stack_address(
                    instruction(contents, address), stack_offset
                ):
                    continue
                if not is_bl(instruction(contents, address + 4)):
                    continue
                if not is_cbz_x0(instruction(contents, address + 8)):
                    continue
            except IndexError:
                continue

            if not any(
                is_cbz_x0(instruction(contents, offset))
                for offset in range(address + 12, reference, 4)
            ):
                continue
            candidates.append((control, destination, kind))
    return candidates


def patch(source: str, destination: str) -> None:
    metadata = os.stat(source)
    with open(source, "rb") as executable:
        contents = executable.read()

    candidates = find_control_candidates(contents)
    if len(candidates) != 1:
        stock = sum(kind == "stock" for _, _, kind in candidates)
        patched = sum(kind == "patched" for _, _, kind in candidates)
        raise ValueError(
            "expected exactly one semantic launchd cache-loader sequence "
            f"(found {stock} stock and {patched} patched)"
        )

    offset, unsecure_entry, kind = candidates[0]
    patched_branch = encode_branch(offset, unsecure_entry)
    if kind == "stock":
        contents = (
            contents[:offset]
            + struct.pack("<I", patched_branch)
            + contents[offset + 4 :]
        )
    if branch_target(instruction(contents, offset), offset) != unsecure_entry:
        raise ValueError("unsecure cache branch verification failed")

    temporary = f"{destination}.surrealra1n"
    with open(temporary, "wb") as executable:
        executable.write(contents)
        executable.flush()
        os.fsync(executable.fileno())
    os.chmod(temporary, stat.S_IMODE(metadata.st_mode))
    os.replace(temporary, destination)
    print(f"[*] launchd_cache_loader unsecure branch patched at 0x{offset:x}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("destination")
    arguments = parser.parse_args()
    try:
        patch(arguments.source, arguments.destination)
    except (OSError, ValueError) as error:
        print(f"patch_launchd_cache_loader: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
