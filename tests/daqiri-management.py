#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Public libiio API regression against an independent ASCII iiod fixture.

Requires an enabled shared library and its compiled-in, writable test profile
 directory. No DAQIRI API is mocked; packet runtime must never be initialized.
"""
import argparse
import ctypes as C
import errno
from pathlib import Path
import socket
import threading
import uuid

CAPABILITY = "dqi1-cpu-strict-credit-v1"


def require(condition, detail):
    if not condition:
        raise AssertionError(detail)


def load_library(path):
    lib = C.CDLL(str(path.resolve()))
    signatures = {
        "iio_create_context": (C.c_void_p, [C.c_void_p, C.c_char_p]),
        "iio_context_destroy": (None, [C.c_void_p]),
        "iio_context_get_name": (C.c_char_p, [C.c_void_p]),
        "iio_context_get_devices_count": (C.c_uint, [C.c_void_p]),
        "iio_context_get_device": (C.c_void_p, [C.c_void_p, C.c_uint]),
        "iio_device_get_id": (C.c_char_p, [C.c_void_p]),
        "iio_device_get_channels_count": (C.c_uint, [C.c_void_p]),
        "iio_device_find_channel": (C.c_void_p, [C.c_void_p, C.c_char_p, C.c_bool]),
        "iio_channel_find_attr": (C.c_void_p, [C.c_void_p, C.c_char_p]),
        "iio_device_find_attr": (C.c_void_p, [C.c_void_p, C.c_char_p]),
        "iio_attr_read_raw": (C.c_ssize_t, [C.c_void_p, C.c_void_p, C.c_size_t]),
        "iio_attr_write_raw": (C.c_ssize_t, [C.c_void_p, C.c_void_p, C.c_size_t]),
        "iio_get_builtin_backends_count": (C.c_uint, []),
        "iio_get_builtin_backend": (C.c_char_p, [C.c_uint]),
    }
    for name, (result, args) in signatures.items():
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = result, args
    require(b"daqiri" in [lib.iio_get_builtin_backend(i) for i in
                          range(lib.iio_get_builtin_backends_count())],
            "library must contain the DAQIRI backend")
    return lib


def check(lib, profiles, capability):
    commands, errors = [], []
    listener = socket.socket()
    listener.settimeout(5)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    name = "management-" + uuid.uuid4().hex
    profile = profiles / (name + ".conf")
    attr = "" if capability is None else (
        f'<context-attribute name="daqiri.protocol" value="{capability}"/>')
    xml = (f'<?xml version="1.0" encoding="utf-8"?><context name="fixture" '
           f'version-major="1" version-minor="0" version-patch="0" version-git="fixture">{attr}'
           '<device id="iio:device0" name="fixture"><attribute name="test"/>'
           '<channel id="voltage0" type="input"><attribute name="scale"/>'
           '<scan-element index="0" format="le:S16/16&gt;&gt;0"/></channel>'
           '</device></context>').encode()

    def serve():
        try:
            conn, _ = listener.accept()
            conn.settimeout(5)
            with conn, conn.makefile("rb") as stream:
                while True:
                    line = stream.readline()
                    if not line:
                        break
                    cmd = line.strip()
                    commands.append(cmd)
                    if cmd in (b"BINARY", b"ZPRINT"):
                        # Explicitly negotiate the independent ASCII fixture.
                        conn.sendall(b"-22\n")
                    elif cmd == b"PRINT":
                        conn.sendall(str(len(xml)).encode() + b"\n" + xml + b"\n")
                    elif cmd.startswith(b"TIMEOUT "):
                        conn.sendall(b"0\n")
                    elif cmd == b"READ iio:device0 test":
                        conn.sendall(b"3\n123\n")
                    elif cmd == b"READ iio:device0 INPUT voltage0 scale":
                        conn.sendall(b"4\n0.25\n")
                    elif cmd == b"WRITE iio:device0 test 3":
                        require(stream.read(3) == b"456", "incorrect attribute write")
                        conn.sendall(b"3\n")
                    else:
                        raise AssertionError(f"unexpected iiod command: {cmd!r}")
        except Exception as exc:
            errors.append(exc)
            print(f"Fixture error: {exc!r}; commands: {commands}", flush=True)
        finally:
            listener.close()

    thread = threading.Thread(target=serve, daemon=True)
    ctx = None
    try:
        with profile.open("x") as stream:
            stream.write(f"""version=1
mode=raw-ibverbs-cpu
trusted_network=yes
management=127.0.0.1
management_port={listener.getsockname()[1]}
yaml=/nonexistent-not-opened-before-buffer
interface=test
device=iio:device0
local_ip=127.0.0.1
peer_ip=127.0.0.2
peer_mac=02:00:00:00:00:02
local_port=12000
peer_port=13000
payload=1024
max_block=4096
max_blocks=4
timeout_ms=1000
""")
        thread.start()
        pointer = lib.iio_create_context(None, ("daqiri:" + name).encode())
        result = C.c_ssize_t(pointer).value
        if capability != CAPABILITY:
            if result > 0:
                ctx = pointer
            require(result == -errno.EPROTONOSUPPORT, result)
        else:
            require(result > 0, result)
            ctx = pointer
            require(lib.iio_context_get_name(ctx) == b"daqiri", "backend identity")
            require(lib.iio_context_get_devices_count(ctx) == 1, "device count")
            dev = lib.iio_context_get_device(ctx, 0)
            require(lib.iio_device_get_id(dev) == b"iio:device0", "device ID")
            require(lib.iio_device_get_channels_count(dev) == 1, "channel count")
            channel = lib.iio_device_find_channel(dev, b"voltage0", False)
            require(channel, "missing input channel")
            for attribute, expected in [
                (lib.iio_device_find_attr(dev, b"test"), b"123"),
                (lib.iio_channel_find_attr(channel, b"scale"), b"0.25"),
            ]:
                require(attribute, "missing attribute")
                buf = C.create_string_buffer(64)
                n = lib.iio_attr_read_raw(attribute, buf, len(buf))
                require(n == len(expected) and buf.raw[:n] == expected, (n, buf.raw))
            attribute = lib.iio_device_find_attr(dev, b"test")
            require(lib.iio_attr_write_raw(attribute, b"456", 3) == 3, "write length")
    finally:
        if ctx:
            lib.iio_context_destroy(ctx)
        if thread.ident is not None:
            thread.join(6)
        listener.close()
        profile.unlink(missing_ok=True)
    require(not thread.is_alive(), "fixture did not terminate")
    require(not errors, errors)
    require(b"PRINT" in commands, "topology was not requested")
    require(not any(cmd.startswith(b"OPEN") for cmd in commands), "legacy stream opened")
    print(f"PASS capability {capability!r}: {commands}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--profile-dir", type=Path, required=True,
                        help="must match DAQIRI_PROFILE_DIR compiled into the library")
    args = parser.parse_args()
    args.profile_dir.mkdir(parents=True, exist_ok=True)
    lib = load_library(args.library)
    for capability in (None, "vrt49", CAPABILITY + "-unknown", CAPABILITY):
        check(lib, args.profile_dir, capability)
    print("PASS registration, topology, strict capability gate, device/channel attrs and cleanup")


if __name__ == "__main__":
    main()
