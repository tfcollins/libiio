# SPDX-License-Identifier: LGPL-2.1-or-later
"""Public libiio bindings; no system-library or static-address fallback."""

import ctypes as C
import hashlib
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def prepare_library(build):
    build = Path(build).resolve()
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if "=" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    if (
        cache.get("WITH_DAQIRI_BACKEND") != "ON"
        or cache.get("WITH_NETWORK_BACKEND") != "ON"
    ):
        raise ValueError("build requires DAQIRI and network backends")
    if Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() != ROOT:
        raise ValueError("build must belong to this source checkout")
    profiles = Path(cache["DAQIRI_PROFILE_DIR"])
    if not profiles.is_absolute() or not profiles.is_dir():
        raise ValueError("compiled profile directory must exist and be writable")
    # Probe actual creation, not os.access (ACLs/read-only mounts can disagree).
    import tempfile

    with tempfile.NamedTemporaryFile(prefix=".managed-preflight-", dir=profiles):
        pass
    library = (build / "libiio.so").resolve(strict=True)
    if build not in library.parents:
        raise ValueError("library must reside in selected build")
    spec = importlib.util.spec_from_file_location(
        "management_api", ROOT / "tests/daqiri-management.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    lib = module.load_library(library)
    for name, result, args in (
        ("iio_device_get_name", C.c_char_p, [C.c_void_p]),
        ("iio_context_find_device", C.c_void_p, [C.c_void_p, C.c_char_p]),
        ("iio_context_find_attr", C.c_void_p, [C.c_void_p, C.c_char_p]),
        ("iio_attr_get_static_value", C.c_char_p, [C.c_void_p]),
    ):
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = result, args
    return lib, profiles, hashlib.sha256(library.read_bytes()).hexdigest()


def profile_text(address):
    # Packet fields are syntactically valid but deliberately unusable. No buffer
    # is opened: the real legacy management capability MUST reject this context.
    import ipaddress

    address = str(ipaddress.IPv4Address(address))
    return f"""version=1
mode=raw-ibverbs-cpu
trusted_network=yes
management={address}
management_port=30431
yaml=/nonexistent-not-opened-before-buffer
interface=unused
device=axi-adrv9009-rx-hpc
local_ip=127.0.0.1
peer_ip=127.0.0.2
peer_mac=02:00:00:00:00:02
local_port=12000
peer_port=13000
payload=1024
max_block=4096
max_blocks=4
timeout_ms=5000
"""
