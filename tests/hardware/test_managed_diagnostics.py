# SPDX-License-Identifier: LGPL-2.1-or-later
"""Failure evidence tests; every hardware/remote operation is a fake."""

import json
import subprocess
import sys
from contextlib import nullcontext
from types import SimpleNamespace

import pytest

import managed


def nested_failure():
    try:
        raise subprocess.CalledProcessError(
            7,
            ["ssh", "password=private-password", "private-token"],
            output="resource dump private-output",
            stderr="private-stderr",
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError("Strategy wrapper private-token") from exc


def test_chain_is_actionable_without_secret_text():
    with pytest.raises(RuntimeError) as caught:
        nested_failure()
    record = managed.failure_evidence(caught.value, "boot")
    text = json.dumps(record)
    assert "private-" not in text
    assert "resource dump" not in text
    assert "RuntimeError" in text and "CalledProcessError" in text
    assert record["exceptions"][1]["returncode"] == 7
    assert record["exceptions"][1]["relation"] == "cause"
    assert record["exceptions"][1]["frames"][-1]["function"] == "nested_failure"
    assert record["exceptions"][1]["frames"][-1]["line"] > 0
    assert all("/" not in f["file"] for e in record["exceptions"] for f in e["frames"])


def test_context_suppression_and_cycle():
    inner = ValueError("private-token")
    outer = RuntimeError("private-password")
    outer.__context__ = inner
    assert len(managed.failure_evidence(outer, "boot")["exceptions"]) == 2
    outer.__suppress_context__ = True
    assert len(managed.failure_evidence(outer, "boot")["exceptions"]) == 1
    outer.__cause__ = inner
    inner.__cause__ = outer
    record = managed.failure_evidence(outer, "boot")
    assert len(record["exceptions"]) == 2
    assert record["chain_truncated"]


def test_long_chain_is_bounded():
    exc = ValueError("secret")
    for _ in range(30):
        outer = RuntimeError("secret")
        outer.__cause__ = exc
        exc = outer
    record = managed.failure_evidence(exc, "boot")
    assert len(record["exceptions"]) == 16
    assert record["chain_truncated"]


@pytest.mark.parametrize(
    "message,condition",
    [
        (
            "production U-Boot prompt 'private-secret' not observed",
            "production U-Boot prompt not observed",
        ),
        (
            "ZynqMP production U-Boot handoff failed: private-secret\nraw resource dump",
            "production JTAG handoff failed",
        ),
        (
            "PMU firmware did not claim FW_IS_PRESENT",
            "PMU firmware did not claim FW_IS_PRESENT",
        ),
        ("token=private-secret timed out", "untrusted exception text withheld"),
    ],
)
def test_only_audited_message_conditions(message, condition):
    record = managed.failure_evidence(RuntimeError(message), "boot")
    assert record["exceptions"][0]["condition"] == condition
    assert "private-secret" not in json.dumps(record)


def test_never_calls_exception_stringifier():
    class UnsafeError(Exception):
        def __str__(self):
            pytest.fail("must not stringify arbitrary plugin exceptions")

    managed.failure_evidence(UnsafeError({"token": "private-secret"}), "boot")


@pytest.mark.parametrize(
    "cleanup_ok,write_ok", [(True, True), (False, True), (True, False)]
)
def test_execute_preserves_boot_failure_and_runs_cleanup(
    tmp_path, monkeypatch, capsys, cleanup_ok, write_ok
):
    args = SimpleNamespace(evidence=tmp_path / "evidence", test_host="fake", wait=1)
    calls = []
    monkeypatch.setattr(managed, "validate_owner_host", lambda: None)
    monkeypatch.setattr(managed, "validate_environment", lambda env: None)
    monkeypatch.setattr(managed, "hardware_lock", lambda wait: nullcontext())
    monkeypatch.setattr(managed, "remote_pytest", lambda *a: calls.append("collect"))
    monkeypatch.setattr(managed, "exporter_payloads", lambda strategy: None)
    monkeypatch.setattr(managed, "preflight", lambda: (None, lambda *a: None, None))
    monkeypatch.setattr(
        managed,
        "client",
        lambda *a, **k: SimpleNamespace(stdout="LG_TOKEN=privatetoken"),
    )
    monkeypatch.setenv("LG_COORDINATOR", managed.COORDINATOR)
    strategy = SimpleNamespace(transition=lambda state: nested_failure())
    target = SimpleNamespace(get_driver=lambda name: strategy)
    monkeypatch.setitem(
        sys.modules,
        "labgrid",
        SimpleNamespace(
            Environment=lambda path: SimpleNamespace(get_target=lambda name: target)
        ),
    )
    reservation = SimpleNamespace(_parse_allocated_place=lambda *a: managed.PLACE)
    monkeypatch.setitem(sys.modules, "adi_lg_plugins", SimpleNamespace())
    monkeypatch.setitem(
        sys.modules, "adi_lg_plugins.request", SimpleNamespace(reservation=reservation)
    )

    def cleanup(*a):
        calls.append("cleanup")
        # Primary diagnostics must already exist before teardown starts.
        if write_ok:
            assert (args.evidence / "failure.json").is_file()
        return {
            "power_off": cleanup_ok,
            "place_released": True,
            "reservation_absent": True,
        }

    monkeypatch.setattr(managed, "cleanup_owned", cleanup)
    if not write_ok:
        original = managed.Path.write_text

        def fail_evidence(path, *a, **k):
            if path.name == "failure.json":
                raise OSError("private-path")
            return original(path, *a, **k)

        monkeypatch.setattr(managed.Path, "write_text", fail_evidence)
    with pytest.raises((RuntimeError, ValueError)):
        managed.execute(args)
    assert calls == ["collect", "cleanup"]
    assert (
        json.loads((args.evidence / "cleanup.json").read_text())["power_off"]
        == cleanup_ok
    )
    stderr = capsys.readouterr().err
    assert "private-" not in stderr
    assert "privatetoken" not in stderr
    assert "CalledProcessError" in stderr
    if write_ok:
        record = json.loads((args.evidence / "failure.json").read_text())
        assert record["stage"] == "BootZynqMPJTAG.transition(kuiper_shell)"
        assert record["exceptions"][-1]["returncode"] == 7


def test_existing_evidence_directory_is_not_overwritten(tmp_path, monkeypatch):
    path = tmp_path / "failure.json"
    path.write_text("original")
    args = SimpleNamespace(evidence=tmp_path)
    monkeypatch.setattr(managed, "validate_owner_host", lambda: None)
    monkeypatch.setattr(managed, "validate_environment", lambda env: None)
    with pytest.raises(FileExistsError):
        managed.execute(args)
    assert path.read_text() == "original"
