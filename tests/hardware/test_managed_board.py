# SPDX-License-Identifier: LGPL-2.1-or-later
"""Opt-in managed physical baseline. Selecting without launcher is an ERROR."""

import ctypes as C
import errno
import os
from pathlib import Path
import uuid

import pytest

from board_api import prepare_library, profile_text


@pytest.fixture(scope="module")
def board(record_testsuite_property):
    assert os.environ.get("LIBIIO_MANAGED_PLACE") == "tron", (
        "use tests/hardware/managed.py --run"
    )
    address = os.environ["LIBIIO_MANAGED_ADDRESS"]
    profile_text(address)  # Validate live IPv4 before constructing any URI.
    lib, profiles, digest = prepare_library(Path(os.environ["LIBIIO_MANAGED_BUILD"]))
    record_testsuite_property(
        "scope", "physical-network-baseline-and-legacy-negative-negotiation"
    )
    record_testsuite_property("daqiri_streaming_qualified", "false")
    record_testsuite_property("library_sha256", digest)
    record_testsuite_property("place", "tron")
    return lib, profiles, address


def test_network_enumeration_and_attributes(board):
    lib, _, address = board
    ctx = lib.iio_create_context(None, ("ip:" + address).encode())
    assert C.c_ssize_t(ctx).value > 0, "network context creation failed"
    try:
        assert lib.iio_context_get_name(ctx) == b"network"
        assert lib.iio_context_get_devices_count(ctx) > 0
        capability = lib.iio_context_find_attr(ctx, b"daqiri.protocol")
        assert (
            not capability
            or lib.iio_attr_get_static_value(capability) != b"dqi1-cpu-strict-credit-v1"
        ), "unexpected DQI1 firmware: reclassify this gate"
        for name in (b"adrv9009-phy", b"adrv9009-phy-b"):
            dev = lib.iio_context_find_device(ctx, name)
            assert dev, "required dual ADRV9009 PHY missing"
            channel = lib.iio_device_find_channel(dev, b"altvoltage0", True)
            assert channel, "LO channel missing"
            attr = lib.iio_channel_find_attr(channel, b"frequency")
            assert attr, "LO frequency attribute missing"
            value = C.create_string_buffer(128)
            size = lib.iio_attr_read_raw(attr, value, len(value))
            assert 0 < size < len(value), "public network attribute read failed"
            assert int(value.raw[:size].strip(b"\x00\n ")) > 0
        rx = lib.iio_context_find_device(ctx, b"axi-adrv9009-rx-hpc")
        assert rx and lib.iio_device_get_channels_count(rx) >= 8
    finally:
        lib.iio_context_destroy(ctx)


def test_legacy_daqiri_rejected(board):
    lib, profiles, address = board
    name = "managed-legacy-" + uuid.uuid4().hex
    path = profiles / (name + ".conf")
    try:
        with path.open("x") as stream:
            stream.write(profile_text(address))
        ctx = lib.iio_create_context(None, ("daqiri:" + name).encode())
        code = C.c_ssize_t(ctx).value
        if code > 0:
            lib.iio_context_destroy(ctx)
        assert code == -errno.EPROTONOSUPPORT, (
            "legacy board must explicitly reject DQI1 capability"
        )
    finally:
        path.unlink(missing_ok=True)
