#!/usr/bin/env python3
"""Fail-closed extended-attribute access for the SSV patch pipeline."""

import ctypes
import errno
import os
import sys


if all(
    hasattr(os, name)
    for name in ("listxattr", "getxattr", "setxattr", "removexattr")
):

    def list_xattrs(path):
        return sorted(os.listxattr(path))


    def get_xattr(path, name):
        return os.getxattr(path, name)


    def set_xattr(path, name, value):
        os.setxattr(path, name, value)


    def remove_xattr(path, name):
        os.removexattr(path, name)

elif sys.platform == "darwin":
    _libc = ctypes.CDLL(None, use_errno=True)
    _libc.listxattr.argtypes = [
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_int,
    ]
    _libc.listxattr.restype = ctypes.c_ssize_t
    _libc.getxattr.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_uint32,
        ctypes.c_int,
    ]
    _libc.getxattr.restype = ctypes.c_ssize_t
    _libc.setxattr.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_uint32,
        ctypes.c_int,
    ]
    _libc.setxattr.restype = ctypes.c_int
    _libc.removexattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    _libc.removexattr.restype = ctypes.c_int

    def _path_bytes(path):
        return os.fsencode(path)

    def _name_bytes(name):
        return name.encode("utf-8", "surrogateescape")

    def _raise_oserror(path):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), path)

    def list_xattrs(path):
        raw_path = _path_bytes(path)
        for _ in range(3):
            size = _libc.listxattr(raw_path, None, 0, 0)
            if size < 0:
                _raise_oserror(path)
            if size == 0:
                return []
            buffer = ctypes.create_string_buffer(size)
            result = _libc.listxattr(raw_path, buffer, size, 0)
            if result >= 0:
                names = buffer.raw[:result].rstrip(b"\0").split(b"\0")
                return sorted(
                    name.decode("utf-8", "surrogateescape")
                    for name in names
                    if name
                )
            if ctypes.get_errno() != errno.ERANGE:
                _raise_oserror(path)
        raise OSError(errno.ERANGE, os.strerror(errno.ERANGE), path)

    def get_xattr(path, name):
        raw_path = _path_bytes(path)
        raw_name = _name_bytes(name)
        for _ in range(3):
            size = _libc.getxattr(raw_path, raw_name, None, 0, 0, 0)
            if size < 0:
                _raise_oserror(path)
            buffer = ctypes.create_string_buffer(size if size else 1)
            result = _libc.getxattr(raw_path, raw_name, buffer, size, 0, 0)
            if result >= 0:
                return buffer.raw[:result]
            if ctypes.get_errno() != errno.ERANGE:
                _raise_oserror(path)
        raise OSError(errno.ERANGE, os.strerror(errno.ERANGE), path)

    def set_xattr(path, name, value):
        raw_value = bytes(value)
        buffer = ctypes.create_string_buffer(raw_value, max(1, len(raw_value)))
        if _libc.setxattr(
            _path_bytes(path),
            _name_bytes(name),
            buffer,
            len(raw_value),
            0,
            0,
        ) != 0:
            _raise_oserror(path)

    def remove_xattr(path, name):
        if _libc.removexattr(_path_bytes(path), _name_bytes(name), 0) != 0:
            _raise_oserror(path)

else:

    def _unsupported(*_arguments):
        raise OSError(
            errno.ENOTSUP,
            "extended-attribute APIs are unavailable on this host",
        )

    list_xattrs = _unsupported
    get_xattr = _unsupported
    set_xattr = _unsupported
    remove_xattr = _unsupported
