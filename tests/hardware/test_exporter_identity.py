"""Offline exporter identity checks: no coordinator, SSH, or hardware."""

import sys
from types import SimpleNamespace

import pytest
import managed


def snapshot(params=None):
    return (
        "Matching resource 'XilinxDeviceJTAG' "
        "(tron/tlab/XilinxDeviceJTAG/XilinxDeviceJTAG):\n"
        + repr(
            {
                "cls": "XilinxDeviceJTAG",
                "avail": True,
                "params": params or {"extra": {"proxy": "tron"}},
            }
        )
    )


@pytest.fixture
def route(monkeypatch):
    seen = []

    class Driver:
        def _exporter_host(self, resource):
            seen.append(resource.extra["proxy"])
            return "tron.local"

        def _remote_prefix(self):
            return ["ssh", "exact-driver-resolved-endpoint", "--"]

    monkeypatch.setitem(
        sys.modules,
        "adi_lg_plugins.drivers.xilinxjtagdriver",
        SimpleNamespace(XilinxJTAGDriver=Driver),
    )
    return seen


@pytest.mark.parametrize(
    "output,code,valid",
    [
        ("tron\n", 0, True),
        ("lbvm\n", 0, False),
        ("tron\n", 1, False),
        ("secret\ntron\n", 0, False),
        ("", 0, False),
    ],
)
def test_exact_driver_route_and_identity(monkeypatch, route, output, code, valid):
    def run(argv, **kwargs):
        assert argv == ["ssh", "exact-driver-resolved-endpoint", "--", "hostname"]
        assert kwargs["timeout"] == 15
        return SimpleNamespace(stdout=output, stderr="secret", returncode=code)

    monkeypatch.setattr(managed.subprocess, "run", run)
    if valid:
        managed.validate_exporter_identity(snapshot())
    else:
        with pytest.raises(RuntimeError) as caught:
            managed.validate_exporter_identity(snapshot())
        assert str(caught.value) == managed.EXPORTER_IDENTITY_ERROR
        assert "secret" not in str(caught.value)
        assert caught.value.__suppress_context__
        evidence = managed.failure_evidence(caught.value, "coordinator-preflight")
        assert evidence["exceptions"][0]["condition"] == managed.EXPORTER_IDENTITY_ERROR
    assert route == ["tron"]


@pytest.mark.parametrize(
    "raw",
    [
        "",
        "malformed",
        snapshot() + "\n" + snapshot(),
        snapshot().replace("tron/tlab/", "other/tlab/"),
    ],
)
def test_missing_ambiguous_wrong_exporter_fails_closed(monkeypatch, route, raw):
    monkeypatch.setattr(
        managed.subprocess, "run", lambda *a, **k: pytest.fail("SSH forbidden")
    )
    with pytest.raises(RuntimeError, match="before reservation"):
        managed.validate_exporter_identity(raw)


def test_transport_exception_redacted(monkeypatch, route):
    def fail(*a, **k):
        raise OSError("secret-resource-password")

    monkeypatch.setattr(managed.subprocess, "run", fail)
    with pytest.raises(RuntimeError) as caught:
        managed.validate_exporter_identity(snapshot())
    assert str(caught.value) == managed.EXPORTER_IDENTITY_ERROR
    assert caught.value.__suppress_context__


@pytest.mark.parametrize(
    "raw",
    [
        snapshot().replace("True", "False"),
        snapshot().replace("'params':", "'missing':"),
        snapshot() + "invalid",
    ],
)
def test_parse_stage_is_distinct(monkeypatch, route, raw):
    monkeypatch.setattr(
        managed.subprocess, "run", lambda *a, **k: pytest.fail("SSH forbidden")
    )
    with pytest.raises(managed.ExporterIdentityError) as caught:
        managed.validate_exporter_identity(raw)
    assert caught.value.identity_stage == "resource-parse"


def test_live_regression_successful_ssh_to_coordinator(monkeypatch, route):
    monkeypatch.setattr(
        managed.subprocess,
        "run",
        lambda *a, **k: SimpleNamespace(returncode=0, stdout="lbvm\n"),
    )
    with pytest.raises(managed.ExporterIdentityError) as caught:
        managed.validate_exporter_identity(snapshot())
    evidence = managed.failure_evidence(caught.value, "coordinator-preflight")[
        "exceptions"
    ][0]
    assert evidence["identity_stage"] == "hostname-compare"
    assert evidence["returncode"] == 0
    assert evidence["hostname"] == "lbvm"
    assert route == ["tron"]  # No alternative-host retry.


def test_query_failure_stage(monkeypatch):
    monkeypatch.setattr(
        managed,
        "client",
        lambda *a, **k: SimpleNamespace(returncode=7, stdout="secret"),
    )
    with pytest.raises(managed.ExporterIdentityError) as caught:
        managed.query_identity_resources()
    assert caught.value.identity_stage == "resource-query"
    assert caught.value.returncode == 7


def test_prefix_failure_stage(monkeypatch, route):
    driver = sys.modules["adi_lg_plugins.drivers.xilinxjtagdriver"].XilinxJTAGDriver

    def fail(self):
        raise ValueError("secret")

    monkeypatch.setattr(driver, "_remote_prefix", fail)
    with pytest.raises(managed.ExporterIdentityError) as caught:
        managed.validate_exporter_identity(snapshot())
    assert caught.value.identity_stage == "prefix-resolution"
    assert "secret" not in str(
        managed.failure_evidence(caught.value, "coordinator-preflight")
    )
