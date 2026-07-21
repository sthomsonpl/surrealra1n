#!/usr/bin/env python3
"""Replace an equally sized, stored ZIP member without rewriting the archive."""

import argparse
import os
import struct
import sys
import zipfile
import zlib


LOCAL_SIGNATURE = 0x04034B50
CENTRAL_SIGNATURE = 0x02014B50
ZIP64_EXTRA_ID = 0x0001


def zip64_local_offset(extra, compressed_size, uncompressed_size, local_offset):
    cursor = 0
    while cursor + 4 <= len(extra):
        field_id, field_size = struct.unpack_from("<HH", extra, cursor)
        cursor += 4
        field = extra[cursor : cursor + field_size]
        cursor += field_size
        if field_id != ZIP64_EXTRA_ID:
            continue

        field_cursor = 0
        if uncompressed_size == 0xFFFFFFFF:
            field_cursor += 8
        if compressed_size == 0xFFFFFFFF:
            field_cursor += 8
        if local_offset == 0xFFFFFFFF:
            if field_cursor + 8 > len(field):
                raise ValueError("truncated ZIP64 local-header offset")
            return struct.unpack_from("<Q", field, field_cursor)[0]
        return local_offset

    if local_offset == 0xFFFFFFFF:
        raise ValueError("missing ZIP64 local-header offset")
    return local_offset


def find_central_crc_offset(handle, central_offset, member, wanted_local_offset):
    handle.seek(central_offset)
    while True:
        entry_offset = handle.tell()
        fixed = handle.read(46)
        if len(fixed) < 4 or struct.unpack_from("<I", fixed)[0] != CENTRAL_SIGNATURE:
            break
        if len(fixed) != 46:
            raise ValueError("truncated central-directory entry")

        flags = struct.unpack_from("<H", fixed, 8)[0]
        compressed_size = struct.unpack_from("<I", fixed, 20)[0]
        uncompressed_size = struct.unpack_from("<I", fixed, 24)[0]
        name_length, extra_length, comment_length = struct.unpack_from(
            "<HHH", fixed, 28
        )
        local_offset = struct.unpack_from("<I", fixed, 42)[0]
        name_bytes = handle.read(name_length)
        extra = handle.read(extra_length)
        handle.seek(comment_length, os.SEEK_CUR)

        encoding = "utf-8" if flags & 0x800 else "cp437"
        name = name_bytes.decode(encoding)
        resolved_offset = zip64_local_offset(
            extra, compressed_size, uncompressed_size, local_offset
        )
        if name == member and resolved_offset == wanted_local_offset:
            return entry_offset + 16

    raise ValueError(f"central-directory entry not found: {member}")


def patch_member(archive_path, member, replacement_path):
    with open(replacement_path, "rb") as replacement_file:
        replacement = replacement_file.read()
    with zipfile.ZipFile(archive_path, "r") as archive:
        info = archive.getinfo(member)
        central_offset = archive.start_dir

    if info.compress_type != zipfile.ZIP_STORED:
        raise ValueError("member is compressed; in-place replacement is unsafe")
    if info.flag_bits & 0x1:
        raise ValueError("encrypted ZIP members are unsupported")
    if info.flag_bits & 0x8:
        raise ValueError("members using a data descriptor are unsupported")
    if len(replacement) != info.file_size or info.compress_size != info.file_size:
        raise ValueError(
            f"replacement must be exactly {info.file_size} bytes "
            f"(received {len(replacement)})"
        )

    with open(archive_path, "r+b", buffering=0) as handle:
        handle.seek(info.header_offset)
        local = handle.read(30)
        if len(local) != 30 or struct.unpack_from("<I", local)[0] != LOCAL_SIGNATURE:
            raise ValueError("invalid local ZIP header")
        name_length, extra_length = struct.unpack_from("<HH", local, 26)
        data_offset = info.header_offset + 30 + name_length + extra_length
        central_crc_offset = find_central_crc_offset(
            handle, central_offset, member, info.header_offset
        )

        handle.seek(data_offset)
        original = handle.read(info.file_size)
        old_local_crc = local[14:18]
        handle.seek(central_crc_offset)
        old_central_crc = handle.read(4)
        new_crc = struct.pack("<I", zlib.crc32(replacement) & 0xFFFFFFFF)

        try:
            handle.seek(data_offset)
            handle.write(replacement)
            handle.seek(info.header_offset + 14)
            handle.write(new_crc)
            handle.seek(central_crc_offset)
            handle.write(new_crc)
            os.fsync(handle.fileno())

            with zipfile.ZipFile(archive_path, "r") as verify_archive:
                if verify_archive.read(member) != replacement:
                    raise ValueError("ZIP verification returned different data")
        except Exception:
            handle.seek(data_offset)
            handle.write(original)
            handle.seek(info.header_offset + 14)
            handle.write(old_local_crc)
            handle.seek(central_crc_offset)
            handle.write(old_central_crc)
            os.fsync(handle.fileno())
            raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("archive")
    parser.add_argument("member")
    parser.add_argument("replacement")
    args = parser.parse_args()

    try:
        patch_member(args.archive, args.member, args.replacement)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"patch_stored_zip_member: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
