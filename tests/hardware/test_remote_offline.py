# SPDX-License-Identifier: LGPL-2.1-or-later
"""Remote execution contract checks; no hardware or SSH required."""

import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest
import managed
import remote_worker
from test_managed_offline import report


def args(tmp_path):
    return SimpleNamespace(
        test_host="picard.local",
        remote_source=remote_worker.SOURCE,
        build=remote_worker.BUILD,
        remote_library_sha256="a" * 64,
        test_timeout=2,
        evidence=tmp_path,
    )


@pytest.mark.parametrize(
    "key,value",
    [
        ("test_host", "evil; touch /tmp/no"),
        ("remote_source", Path("/tmp/source;evil")),
        ("build", Path("/tmp/build")),
        ("remote_library_sha256", "a;echo secret"),
        ("test_timeout", 0),
        ("test_timeout", 121),
    ],
)
def test_reject_remote_arguments(tmp_path, key, value):
    options = args(tmp_path)
    setattr(options, key, value)
    with pytest.raises(ValueError):
        managed.remote_request(options, "collect")


def test_request_has_no_owner_secrets(tmp_path, monkeypatch):
    monkeypatch.setenv("LG_TOKEN", "secret-do-not-copy")
    monkeypatch.setenv("AWS_SECRET_ACCESS_KEY", "secret-do-not-copy")
    # Worker source need not contain Git metadata; mock owner inventory here.
    monkeypatch.setattr(
        managed.subprocess,
        "check_output",
        lambda *a, **k: managed.TEST.encode() + b"\0",
    )
    request = managed.remote_request(args(tmp_path), "collect")
    assert "secret-do-not-copy" not in json.dumps(request)
    assert set(request) == {
        "mode",
        "address",
        "timeout",
        "source_hashes",
        "library_sha256",
    }
    assert managed.TEST in request["source_hashes"]


@pytest.mark.parametrize("bad", ["missing", "skipped", "wrong-hash", "exit", None])
def test_remote_result_fail_closed(tmp_path, monkeypatch, bad):
    xml = tmp_path / "fixture.xml"
    report(xml, bad="skipped" if bad == "skipped" else None)
    import xml.etree.ElementTree as ET

    root = ET.parse(xml).getroot()
    suite = root.find("testsuite")
    assert suite is not None
    props = ET.SubElement(suite, "properties")
    ET.SubElement(
        props,
        "property",
        name="library_sha256",
        value=("b" if bad == "wrong-hash" else "a") * 64,
    )
    result = {
        "returncode": 1 if bad == "exit" else 0,
        "log": "",
        "junit": None if bad == "missing" else ET.tostring(root, encoding="unicode"),
    }

    class Child:
        returncode = 0

        def __init__(self, argv, **kwargs):
            assert argv[-2] == "picard.local"
            assert "SendEnv=-*" in argv
            assert "/usr/bin/env -i" in argv[-1]

        def communicate(self, payload, timeout):
            assert "LG_TOKEN" not in payload
            return json.dumps(result), ""

    monkeypatch.setattr(managed.subprocess, "Popen", Child)
    monkeypatch.setattr(managed, "remote_request", lambda *a: {})
    if bad is None:
        managed.remote_pytest(args(tmp_path), "run", "10.0.0.99")
    else:
        with pytest.raises((ValueError, FileNotFoundError)):
            managed.remote_pytest(args(tmp_path), "run", "10.0.0.99")


def test_worker_timeout_kills_process_group(tmp_path, monkeypatch):
    monkeypatch.setattr(remote_worker, "SOURCE", tmp_path)
    pidfile = tmp_path / "pid"
    program = (
        "import os,time; open("
        + repr(str(pidfile))
        + ',"w").write(str(os.getpid())); time.sleep(30)'
    )
    with (tmp_path / "log").open("w") as log:
        with pytest.raises(subprocess.TimeoutExpired):
            remote_worker.bounded(
                [sys.executable, "-c", program], {"PATH": "/usr/bin:/bin"}, log, 1
            )
    with pytest.raises(ProcessLookupError):
        os.kill(int(pidfile.read_text()), 0)


def test_worker_hash_mismatch_before_import(tmp_path, monkeypatch):
    monkeypatch.setattr(remote_worker, "SOURCE", tmp_path)
    (tmp_path / "source").write_text("changed")
    with pytest.raises(ValueError, match="source hash"):
        remote_worker.verify({"source_hashes": {"source": "0" * 64}})


def test_worker_library_mismatch_before_import(tmp_path, monkeypatch):
    monkeypatch.setattr(remote_worker, "BUILD", tmp_path)
    (tmp_path / "libiio.so").write_bytes(b"wrong")
    with pytest.raises(ValueError, match="library hash"):
        remote_worker.verify({"source_hashes": {}, "library_sha256": "0" * 64})


def test_remote_collection_not_hardware_pass(tmp_path, monkeypatch):
    monkeypatch.setattr(managed, "remote_request", lambda *a: {})

    class Child:
        returncode = 0

        def __init__(self, *a, **k):
            pass

        def communicate(self, *a, **k):
            return json.dumps(
                {"returncode": 0, "log": "0 tests collected", "junit": None}
            ), ""

    monkeypatch.setattr(managed.subprocess, "Popen", Child)
    with pytest.raises(ValueError, match="collection"):
        managed.remote_pytest(args(tmp_path), "collect")


@pytest.mark.parametrize(
    "failure", [ValueError("pytest failed"), subprocess.TimeoutExpired("ssh", 1)]
)
def test_remote_failure_still_cleans_up_on_owner(tmp_path, monkeypatch, failure):
    from contextlib import nullcontext

    options = args(tmp_path / "evidence")
    options.wait = 1
    calls = []
    monkeypatch.setattr(managed, "validate_owner_host", lambda: calls.append("nemo"))
    monkeypatch.setattr(managed, "validate_environment", lambda env: None)
    monkeypatch.setattr(managed, "hardware_lock", lambda wait: nullcontext())
    monkeypatch.setattr(
        managed, "preflight", lambda: ({}, lambda *a: None, SimpleNamespace)
    )
    monkeypatch.setattr(
        managed,
        "client",
        lambda *a, **k: SimpleNamespace(stdout="LG_TOKEN=privateToken"),
    )
    monkeypatch.setattr(managed, "live_address", lambda shell: "10.0.0.99")
    monkeypatch.setattr(managed, "exporter_payloads", lambda strategy: None)
    strategy = SimpleNamespace(transition=lambda state: None)
    target = SimpleNamespace(get_driver=lambda name: strategy)
    reservation = SimpleNamespace(_parse_allocated_place=lambda *a: "tron")
    monkeypatch.setitem(
        sys.modules,
        "labgrid",
        SimpleNamespace(
            Environment=lambda path: SimpleNamespace(get_target=lambda name: target)
        ),
    )
    monkeypatch.setitem(sys.modules, "adi_lg_plugins", SimpleNamespace())
    monkeypatch.setitem(
        sys.modules, "adi_lg_plugins.request", SimpleNamespace(reservation=reservation)
    )

    def remote(options, mode, address=None):
        calls.append(mode)
        if mode == "run":
            raise failure

    monkeypatch.setattr(managed, "remote_pytest", remote)

    def cleanup(*a):
        calls.append("cleanup")
        return {"power_off": True, "place_released": True, "reservation_absent": True}

    monkeypatch.setattr(managed, "cleanup_owned", cleanup)
    with pytest.raises(type(failure)):
        managed.execute(options)
    assert calls == ["nemo", "collect", "run", "cleanup"]
    assert json.loads((options.evidence / "cleanup.json").read_text())["place_released"]
