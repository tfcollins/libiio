# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline model of Labgrid 26's _run_check; no hardware imports or access."""

import json
from types import SimpleNamespace

import pytest

import managed


class ExecutionError(Exception):
    pass


@pytest.mark.parametrize("failed", list(managed.POSTBOOT_CHECKS))
def test_exact_failure_original_exception_and_restoration(tmp_path, failed):
    calls = []
    error = ExecutionError(failed, ["private-output"], ["private-secret"])

    class Shell:
        def _run(self, cmd, **kwargs):
            calls.append((cmd, kwargs))
            if cmd in managed.POSTBOOT_COUNTS.values():
                return ["2"], [], 0
            return ["private-output"], ["private-secret"], 7 if cmd == failed else 0

        def run_check(self, cmd, **kwargs):
            out, err, rc = self._run(cmd, **kwargs)
            if rc:
                raise error
            return out

    shell = Shell()
    args = SimpleNamespace(evidence=tmp_path, _evidence_ready=True)
    original = shell._run
    cleanup = []
    try:
        with pytest.raises(ExecutionError) as caught:
            with managed.postboot_diagnostics(SimpleNamespace(shell=shell), args):
                for cmd in managed.POSTBOOT_CHECKS:
                    shell.run_check(cmd, timeout=120)
        assert caught.value is error
        managed.record_failure(args, caught.value, "boot")
    finally:
        assert shell._run == original
        assert "_run" not in vars(shell)
        cleanup.append("unchanged-cleanup")
    assert cleanup == ["unchanged-cleanup"]
    record = json.loads((tmp_path / "failure.json").read_text())
    last = record["postboot_checks"][-1]
    assert last["check_id"] == managed.POSTBOOT_CHECKS[failed]
    assert last["exit_status"] == 7
    assert last["snapshot_counts"] == dict.fromkeys(managed.POSTBOOT_COUNTS, 2)
    assert "private-" not in json.dumps(record)
    assert failed not in json.dumps(record)
    assert record["exceptions"][0]["condition"] == "untrusted exception text withheld"
    executed = [cmd for cmd, _ in calls if cmd in managed.POSTBOOT_CHECKS]
    expected = list(managed.POSTBOOT_CHECKS)
    assert executed == expected[: expected.index(failed) + 1]


@pytest.mark.parametrize(
    "output", [["private-secret"], ["1234567"], ["2", "secret"], ["-1"]]
)
def test_unknown_commands_and_untrusted_count_text_withheld(tmp_path, output):
    def run(cmd, **kwargs):
        return output, ["private-stderr"], 0

    shell = SimpleNamespace(_run=run)
    args = SimpleNamespace(evidence=tmp_path)
    with managed.postboot_diagnostics(SimpleNamespace(shell=shell), args):
        assert shell._run("secret unknown command") == (output, ["private-stderr"], 0)
        shell._run(next(iter(managed.POSTBOOT_CHECKS)))
    assert shell._run is run
    record = json.loads((tmp_path / "postboot-checks.json").read_text())
    assert len(record["checks"]) == 1
    assert record["checks"][0]["snapshot_counts"] == dict.fromkeys(
        managed.POSTBOOT_COUNTS
    )
    assert "secret" not in json.dumps(record)


def test_transport_error_and_evidence_io_do_not_change_error(tmp_path, monkeypatch):
    error = RuntimeError("private-secret")

    def run(*args, **kwargs):
        raise error

    shell = SimpleNamespace(_run=run)
    args = SimpleNamespace(evidence=tmp_path)
    monkeypatch.setattr(managed.Path, "write_text", run)
    with pytest.raises(RuntimeError) as caught:
        with managed.postboot_diagnostics(SimpleNamespace(shell=shell), args):
            shell._run(next(iter(managed.POSTBOOT_CHECKS)))
    assert caught.value is error
    assert shell._run is run
    assert args._postboot_checks[0]["exit_status"] is None
