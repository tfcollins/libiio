# SPDX-License-Identifier: LGPL-2.1-or-later
"""Private bounded Picard software worker. No coordinator or hardware ownership."""

import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

SOURCE = Path("/tmp/libiio-managed-review/source")
BUILD = Path("/tmp/libiio-managed-review/build")
PREFIX = Path("/tmp/libiio-daqiri-verification/prefix")
PYTHON = "/tmp/libiio-managed-review/venv/bin/python"
TEST = "tests/hardware/test_managed_board.py"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(request):
    for name, expected in request["source_hashes"].items():
        path = SOURCE / name
        if not path.resolve().is_relative_to(SOURCE) or digest(path) != expected:
            raise ValueError("remote source hash mismatch: " + name)
    if digest(BUILD / "libiio.so") != request["library_sha256"]:
        raise ValueError("remote library hash mismatch")
    cache = (BUILD / "CMakeCache.txt").read_text()
    if "daqiri_DIR:PATH=" + str(PREFIX / "lib/cmake/daqiri") not in cache:
        raise ValueError("wrong real DAQIRI prefix")
    sys.path.insert(0, str(SOURCE / "tests/hardware"))
    from board_api import prepare_library

    prepare_library(BUILD)
    import importlib.metadata

    if importlib.metadata.version("pytest") != "8.3.5":
        raise ValueError("remote pytest==8.3.5 required")


def bounded(argv, env, log, seconds):
    """Kill the entire test process group on timeout, signal, or normal exit."""
    child = subprocess.Popen(
        argv,
        cwd=SOURCE,
        env=env,
        stdout=log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    try:
        return child.wait(timeout=seconds)
    finally:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait(timeout=5)


def main(request):
    seconds = request["timeout"]
    if type(seconds) is not int or not 1 <= seconds <= 120:
        raise ValueError("invalid worker timeout")

    # The watchdog survives loss of the SSH client and bounds validation too.
    def interrupted(signum, frame):
        raise TimeoutError("remote worker interrupted")

    for sig in (signal.SIGALRM, signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, interrupted)
    signal.alarm(seconds + 20)
    evidence = Path(tempfile.mkdtemp(prefix="libiio-managed-evidence-"))
    os.chmod(evidence, 0o700)
    result = {
        "evidence": str(evidence),
        "returncode": 1,
        "junit": None,
        "library_sha256": request["library_sha256"],
        "source_hashes": request["source_hashes"],
        "mode": request["mode"],
    }
    try:
        verify(request)
        env = {
            "PATH": "/usr/bin:/bin",
            "LANG": "C.UTF-8",
            "PYTEST_DISABLE_PLUGIN_AUTOLOAD": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
            "LIBIIO_MANAGED_BUILD": str(BUILD),
        }
        mode = request["mode"]
        if mode == "run":
            import ipaddress

            env.update(
                LIBIIO_MANAGED_PLACE="tron",
                LIBIIO_MANAGED_ADDRESS=str(ipaddress.IPv4Address(request["address"])),
            )
        elif mode != "collect":
            raise ValueError("invalid worker mode")
        argv = [
            PYTHON,
            "-m",
            "pytest",
            "-q",
            "-o",
            "addopts=",
            "-p",
            "no:cacheprovider",
            "--confcutdir=" + str(SOURCE),
            TEST,
        ]
        if mode == "collect":
            argv.append("--collect-only")
        else:
            argv.append("--junitxml=" + str(evidence / "junit.xml"))
        with (evidence / "pytest.log").open("w") as log:
            result["returncode"] = bounded(argv, env, log, seconds)
        verify(request)  # reject any drift during the execution
    except Exception as exc:
        result["returncode"] = 1
        result["error"] = type(exc).__name__
    finally:
        signal.alarm(0)
    junit = evidence / "junit.xml"
    if junit.is_file():
        result["junit"] = junit.read_text()
    result["log"] = (
        (evidence / "pytest.log").read_text()
        if (evidence / "pytest.log").exists()
        else ""
    )
    (evidence / "result.json").write_text(json.dumps(result))
    print(json.dumps(result))


if __name__ == "__main__":
    main(json.load(sys.stdin))
