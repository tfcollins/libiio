# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline harness tests: never reserve, boot, or connect to the DUT."""

import subprocess
import sys
import xml.etree.ElementTree as ET

import pytest

import managed
from board_api import profile_text


def report(path, names=managed.CASES, bad=None):
    root = ET.Element("testsuites")
    suite = ET.SubElement(
        root, "testsuite", tests=str(len(names)), errors="0", failures="0", skipped="0"
    )
    for name in names:
        case = ET.SubElement(
            suite, "testcase", classname="tests.hardware.test_managed_board", name=name
        )
        if bad:
            ET.SubElement(case, bad)
    ET.ElementTree(root).write(path)


def test_exact_junit(tmp_path):
    path = tmp_path / "junit.xml"
    report(path)
    assert managed.validate_junit(path) == 2


@pytest.mark.parametrize("bad", ["skipped", "failure", "error"])
def test_junit_rejects_unsuccessful(tmp_path, bad):
    path = tmp_path / "junit.xml"
    report(path, bad=bad)
    with pytest.raises(ValueError):
        managed.validate_junit(path)


@pytest.mark.parametrize(
    "names",
    [
        (),
        managed.CASES[:1],
        managed.CASES * 2,
        ("wrong", "test_legacy_daqiri_rejected"),
    ],
)
def test_junit_rejects_wrong_cases(tmp_path, names):
    path = tmp_path / "junit.xml"
    report(path, names)
    with pytest.raises(ValueError):
        managed.validate_junit(path)


@pytest.mark.parametrize(
    "key", ["IIO_URI", "LG_ENV", "LG_TOKEN", "LG_STATE", "PYTEST_ADDOPTS", "LD_PRELOAD"]
)
def test_no_unmanaged_bypass(key):
    with pytest.raises(ValueError):
        managed.validate_environment({key: "untrusted"})


def test_place_validation_does_not_return_resources():
    raw = "Place 'tron':\n  tags: " + ", ".join(
        k + "=" + v for k, v in managed.TAGS.items()
    )
    assert managed.parse_place(raw + "\nResource: secret-password") == managed.TAGS
    with pytest.raises(ValueError):
        managed.parse_place(raw.replace("adrv9009zu11eg", "other"))


def test_lock_is_bounded(tmp_path, monkeypatch):
    monkeypatch.setattr(managed, "LOCK", tmp_path / "shared.lock")
    with managed.hardware_lock(0):
        with pytest.raises(ValueError, match="lock timeout"):
            with managed.hardware_lock(0):
                pytest.fail("lock was not exclusive")


def test_no_static_ip_fallback():
    class Shell:
        def run(self, command, timeout):
            return [], [], 0

    with pytest.raises(ValueError, match="default route"):
        managed.live_address(Shell())


def test_live_routed_address():
    class Shell:
        def run(self, command, timeout):
            if "route" in command:
                return ["default via 10.0.0.1 dev eth2"], [], 0
            assert "dev eth2 up scope global" in command
            return ["3: eth2 inet 10.0.0.99/24 scope global eth2"], [], 0

    assert managed.live_address(Shell()) == "10.0.0.99"


@pytest.mark.parametrize(
    "failed", [None, "power", "target", "release", "show", "reservations"]
)
def test_cleanup_attempts_independent_legs(monkeypatch, failed):
    from types import SimpleNamespace

    calls = []
    alarms = []
    monkeypatch.setattr(managed.signal, "alarm", alarms.append)

    def operation(name):
        calls.append(name)
        if failed == name:
            raise RuntimeError("simulated failure or timeout")

    power = SimpleNamespace(off=lambda: operation("power"))
    # A stale cached powered_off state must never bypass the actual power call.
    strategy = SimpleNamespace(power=power, status="powered_off")
    target = SimpleNamespace(
        activate=lambda p: operation("activate"), cleanup=lambda: operation("target")
    )
    reservation = SimpleNamespace(release=lambda *a, **k: operation("release"))

    def client(*args, **kwargs):
        name = "show" if "show" in args else "reservations"
        operation(name)
        return SimpleNamespace(stdout="  acquired: None\n" if name == "show" else "")

    monkeypatch.setattr(managed, "client", client)
    result = managed.cleanup_owned(
        target, strategy, "private-token", reservation, SimpleNamespace
    )
    assert calls == ["activate", "power", "target", "release", "show", "reservations"]
    assert result["power_off"] == (failed != "power")
    assert result["target_cleanup"] == (failed != "target")
    assert result["place_released"] == (failed != "show")
    assert result["reservation_absent"] == (failed != "reservations")
    assert alarms == [25, 0, 25, 0, 35, 0, 25, 0, 25, 0]


def test_lock_cannot_move_to_another_host(monkeypatch):
    import socket

    monkeypatch.setattr(socket, "gethostname", lambda: "picard")
    with pytest.raises(ValueError, match="host-local"):
        managed.validate_owner_host()
    monkeypatch.setattr(socket, "gethostname", lambda: "nemo")
    managed.validate_owner_host()


def test_existing_owner_rejected():
    managed.validate_place_available("  acquired: None\n", "")
    with pytest.raises(ValueError, match="already acquired"):
        managed.validate_place_available("  acquired: nemo/someone\n", "")
    with pytest.raises(ValueError, match="reservation exists"):
        managed.validate_place_available("  acquired: None\n", "    main: tron\n")


def test_exporter_paths_never_shadowed(tmp_path):
    from types import SimpleNamespace

    payload = tmp_path / "shadow.bin"
    payload.write_bytes(b"not the exporter payload")
    strategy = SimpleNamespace(
        bitstream_path=str(payload), psu_init_tcl="exporter:/canonical/init.tcl"
    )
    managed.exporter_payloads(strategy)
    managed.exporter_payloads(strategy)
    assert strategy.bitstream_path == "exporter:" + str(payload)
    assert strategy.psu_init_tcl == "exporter:/canonical/init.tcl"
    with pytest.raises(ValueError, match="absolute exporter"):
        managed.exporter_payloads(SimpleNamespace(bitstream_path="relative.bin"))


def test_profile_is_management_only():
    assert "management=10.0.0.99\n" in profile_text("10.0.0.99")
    assert "yaml=/nonexistent-not-opened-before-buffer" in profile_text("10.0.0.99")
    with pytest.raises(ValueError):
        profile_text("10.0.0.99\nmode=unsafe")


def test_selected_without_launcher_fails_not_skips(tmp_path):
    import os

    env = {k: v for k, v in os.environ.items() if not k.startswith("LIBIIO_MANAGED_")}
    env["PYTEST_DISABLE_PLUGIN_AUTOLOAD"] = "1"
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "pytest",
            "-q",
            managed.TEST,
            "--junitxml=" + str(tmp_path / "missing.xml"),
        ],
        cwd=managed.ROOT,
        env=env,
        capture_output=True,
        timeout=20,
    )
    assert result.returncode != 0
    root = ET.parse(tmp_path / "missing.xml").getroot()
    assert len(list(root.iter("testcase"))) == 2
    assert len(list(root.iter("error"))) == 2
    assert not list(root.iter("skipped"))
