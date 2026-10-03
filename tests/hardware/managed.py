#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Managed tron baseline/negative negotiation; never streaming qualification."""

import argparse
from collections import Counter
from contextlib import contextmanager
import fcntl
import importlib.metadata as metadata
import ipaddress
import json
import logging
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
COORDINATOR = "10.0.0.41:20408"
PLACE = "tron"
PLUGIN_SHA = "79072b018e696dc7637f2ee2cfeea6980d667e46"
LOCK = Path("/tmp/vrt49-corundum-hardware.lock")
TEST = "tests/hardware/test_managed_board.py"
CASES = ("test_network_enumeration_and_attributes", "test_legacy_daqiri_rejected")
TAGS = {
    "daughter-board": "adrv9009zu11eg",
    "carrier": "adrv2crr-fmc",
    "boot-strategy": "BootZynqMPJTAG",
    "runner": "hw-nemo",
}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def validate_environment(env):
    for key in (
        "LG_ENV",
        "LG_PLACE",
        "LG_TOKEN",
        "LG_STATE",
        "IIO_URI",
        "IIO_URI_OVERRIDE",
        "PYTEST_ADDOPTS",
        "PYTEST_PLUGINS",
        "LD_PRELOAD",
    ):
        require(not env.get(key), "unset inherited " + key)
    for key in ("LG_COORDINATOR", "ADI_LG_COORDINATOR"):
        require(env.get(key, COORDINATOR) == COORDINATOR, "wrong coordinator")


def parse_place(raw):
    lines = raw.splitlines()
    require(lines and lines[0] == "Place 'tron':", "wrong place")
    tags = [line[8:] for line in lines if line.startswith("  tags: ")]
    require(len(tags) == 1, "missing/ambiguous place tags")
    pairs = [item.partition("=") for item in tags[0].split(", ")]
    require(all(sep for _, sep, _ in pairs), "invalid tags")
    values = {key: value for key, _, value in pairs}
    require(len(values) == len(pairs), "duplicate tags")
    require(all(values.get(k) == v for k, v in TAGS.items()), "place tag mismatch")
    return values


def client(*args, timeout=30, check=True):
    env = {k: v for k, v in os.environ.items() if not k.startswith("LG_")}
    result = subprocess.run(
        [sys.executable, "-m", "labgrid.remote.client", "-x", COORDINATOR, *args],
        env=env,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    require(
        not check or result.returncode == 0,
        "labgrid operation failed (raw output withheld)",
    )
    return result


def validate_junit(path):
    root = ET.parse(path).getroot()
    require(root.tag in ("testsuite", "testsuites"), "invalid JUnit root")
    cases = list(root.iter("testcase"))
    expected = Counter(("tests.hardware.test_managed_board", name) for name in CASES)
    require(
        Counter((c.get("classname"), c.get("name")) for c in cases) == expected,
        "JUnit exact case identities/count mismatch",
    )
    require(
        not any(list(root.iter(t)) for t in ("failure", "error", "skipped")),
        "JUnit contains unsuccessful cases",
    )
    for suite in root.iter("testsuite"):
        require(
            all(suite.get(k, "0") == "0" for k in ("errors", "failures", "skipped")),
            "JUnit reports unsuccessful cases",
        )
        require(
            int(suite.get("tests", "-1")) == len(list(suite.iter("testcase"))),
            "JUnit declared count mismatch",
        )
    return len(cases)


@contextmanager
def hardware_lock(timeout):
    fd = os.open(LOCK, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        deadline = time.monotonic() + timeout
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                require(time.monotonic() < deadline, "shared hardware lock timeout")
                time.sleep(0.1)
        yield
    finally:
        os.close(fd)


def live_address(shell):
    # No NetworkService/static URI fallback. Require one live routed IPv4.
    out, _, rc = shell.run("ip -4 route show default", timeout=15)
    devices = set(re.findall(r"\bdev ([A-Za-z0-9_.-]+)", "\n".join(out)))
    require(rc == 0 and len(devices) == 1, "missing/ambiguous management default route")
    dev = devices.pop()
    out, _, rc = shell.run(
        "ip -4 -o addr show dev " + dev + " up scope global", timeout=15
    )
    addresses = re.findall(r"\binet ([0-9.]+)/", "\n".join(out))
    require(
        rc == 0 and len(addresses) == 1, "missing/ambiguous live management address"
    )
    address = ipaddress.IPv4Address(addresses[0])
    require(
        not address.is_loopback
        and not address.is_link_local
        and not address.is_unspecified,
        "invalid management address",
    )
    return str(address)


def validate_place_available(raw, reservations):
    require(
        re.search(r"^  acquired: None$", raw, re.MULTILINE),
        "place already acquired; do not compete with its owner",
    )
    require(
        not re.search(r"\btron\b", reservations),
        "tron reservation exists; do not queue behind another owner",
    )


def exporter_payloads(strategy):
    for key in (
        "bitstream_path",
        "psu_init_tcl",
        "pmufw_bin",
        "uboot_bin",
        "handoff_bin",
        "bl31_bin",
        "atf_handoff_bin",
        "pm_config_bin",
        "ddr_scrub_elf",
    ):
        value = getattr(strategy, key, None)
        if value:
            require(
                value.removeprefix("exporter:").startswith("/"),
                "boot payload must be an absolute exporter path",
            )
            setattr(strategy, key, "exporter:" + value.removeprefix("exporter:"))


def preflight():
    require(
        (Path(sys.executable).parent / "labgrid-client").is_file(),
        "labgrid-client must be installed beside the active interpreter",
    )
    require(metadata.version("labgrid") == "26.0", "labgrid==26.0 required")
    require(metadata.version("pytest") == "8.3.5", "pytest==8.3.5 required")
    distributions = metadata.packages_distributions().get("adi_lg_plugins", [])
    require(len(distributions) == 1, "missing/ambiguous plugin distribution")
    direct = json.loads(
        metadata.distribution(distributions[0]).read_text("direct_url.json") or "{}"
    )
    require(
        direct.get("vcs_info", {}).get("commit_id") == PLUGIN_SHA,
        "plugin commit mismatch",
    )
    from adi_lg_plugins.strategies.bootzynqmpjtag import Status
    from adi_lg_plugins.hw_ci.schema import validate_place
    from adi_lg_plugins.hw_ci.render_env import render_env_to
    from adi_lg_plugins.request.reservation import Reservation

    require(
        hasattr(Status, "kuiper_shell") and hasattr(Status, "powered_off"),
        "boot API mismatch",
    )
    raw = client("-p", PLACE, "show").stdout
    tags = parse_place(raw)
    validate_place_available(raw, client("reservations").stdout)
    place = validate_place({"name": PLACE, "tags": tags})
    return place, render_env_to, Reservation


def cleanup_owned(target, strategy, token, reservation, Reservation):
    """Attempt every teardown leg with a fresh, bounded budget.

    Never trust strategy.status after a partially failed transition: power.on()
    may have succeeded while the cached state still says powered_off.
    """
    cleanup = {"power_off": False, "place_released": False, "reservation_absent": False}

    def attempt(operation, seconds=25):
        signal.alarm(seconds)
        try:
            return operation()
        except Exception:
            return False
        finally:
            signal.alarm(0)

    def power_off():
        target.activate(strategy.power)
        strategy.power.off()
        return True

    if strategy is not None:
        cleanup["power_off"] = bool(attempt(power_off))
    if target is not None:

        def cleanup_target():
            target.cleanup()
            return True

        cleanup["target_cleanup"] = bool(attempt(cleanup_target))
    if token:
        attempt(
            lambda: reservation.release(
                COORDINATOR,
                Reservation(place=PLACE, token=token),
                client=str(Path(sys.executable).parent / "labgrid-client"),
            ),
            seconds=35,
        )
        cleanup["place_released"] = bool(
            attempt(
                lambda: re.search(
                    r"^  acquired: None$",
                    client("-p", PLACE, "show", timeout=15).stdout,
                    re.MULTILINE,
                )
            )
        )
        cleanup["reservation_absent"] = bool(
            attempt(lambda: token not in client("reservations", timeout=15).stdout)
        )
    return cleanup


def remote_request(args, mode, address=None):
    """Only explicit prepared paths; never serialize the owner's environment."""
    import hashlib

    require(
        args.test_host == "picard.local", "only explicit picard.local worker supported"
    )
    require(
        str(args.remote_source) == "/tmp/libiio-managed-review/source",
        "remote source must be the prepared Picard source",
    )
    require(
        str(args.build) == "/tmp/libiio-managed-review/build",
        "remote build must be the prepared Picard build",
    )
    require(
        re.fullmatch(r"[0-9a-f]{64}", args.remote_library_sha256 or ""),
        "explicit remote library SHA-256 required",
    )
    require(1 <= args.test_timeout <= 120, "test timeout must be 1..120 seconds")
    tracked = (
        subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT)
        .decode()
        .split("\0")
    )
    names = set(n for n in tracked if n and (ROOT / n).is_file())
    names.update(
        "tests/hardware/" + n
        for n in (
            "managed.py",
            "board_api.py",
            "test_managed_board.py",
            "remote_worker.py",
        )
    )
    hashes = {
        n: hashlib.sha256((ROOT / n).read_bytes()).hexdigest() for n in sorted(names)
    }
    return {
        "mode": mode,
        "address": address,
        "timeout": args.test_timeout,
        "source_hashes": hashes,
        "library_sha256": args.remote_library_sha256,
    }


def remote_pytest(args, mode, address=None):
    import shlex

    request = remote_request(args, mode, address)
    worker = (ROOT / "tests/hardware/remote_worker.py").read_text()
    command = shlex.join(
        [
            "/usr/bin/env",
            "-i",
            "PATH=/usr/bin:/bin",
            "LANG=C.UTF-8",
            "/tmp/libiio-managed-review/venv/bin/python",
            "-I",
            "-c",
            worker,
        ]
    )
    # Disable environment forwarding even if user ssh_config requests it.
    argv = [
        "ssh",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=10",
        "-o",
        "SendEnv=-*",
        "-o",
        "ServerAliveInterval=5",
        "-o",
        "ServerAliveCountMax=2",
        "picard.local",
        command,
    ]
    deadline = time.monotonic() + args.test_timeout + 40
    child = subprocess.Popen(
        argv,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        output, _ = child.communicate(
            json.dumps(request), timeout=args.test_timeout + 40
        )
        require(child.returncode == 0, "remote worker/SSH failed")
    except BaseException:
        # SSH death does not prove the worker stopped. Keep Nemo's lease/lock
        # until its independent watchdog must have killed the test group.
        handlers = {
            sig: signal.signal(sig, signal.SIG_IGN)
            for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGALRM)
        }
        try:
            child.kill()
            child.wait(timeout=5)
            while time.monotonic() < deadline:
                time.sleep(min(0.2, max(0, deadline - time.monotonic())))
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
        raise
    result = json.loads(output)
    (args.evidence / (mode + "-remote.json")).write_text(
        json.dumps(result, indent=2) + "\n"
    )
    (args.evidence / (mode + "-pytest.log")).write_text(result["log"])
    if result.get("junit") is not None:
        (args.evidence / "junit.xml").write_text(result["junit"])
    require(result["returncode"] == 0, "remote pytest/verification failed")
    if mode == "collect":
        nodes = [line for line in result["log"].splitlines() if "::" in line]
        require(
            Counter(nodes) == Counter(TEST + "::" + name for name in CASES),
            "remote collection identities/count mismatch",
        )
    else:
        validate_junit(args.evidence / "junit.xml")
        root = ET.parse(args.evidence / "junit.xml").getroot()
        hashes = [
            p.get("value")
            for p in root.iter("property")
            if p.get("name") == "library_sha256"
        ]
        require(hashes == [args.remote_library_sha256], "JUnit library hash mismatch")
    return result


def validate_owner_host():
    import socket

    require(
        socket.gethostname().split(".")[0] == "nemo",
        "run ownership on nemo: the shared lock is host-local",
    )


def execute(args):
    validate_owner_host()
    validate_environment(os.environ)
    args.evidence.mkdir(parents=True, exist_ok=False)
    # Build and validate bindings before any reservation; no installed libiio fallback.
    if args.test_host:
        remote_pytest(args, "collect")
    else:
        subprocess.run(
            ["cmake", "--build", str(args.build), "--parallel", "2"],
            check=True,
            timeout=300,
        )
        from board_api import prepare_library

        prepare_library(args.build)
    with hardware_lock(args.wait):
        place, render, Reservation = preflight()
        from labgrid import Environment
        from adi_lg_plugins.request import reservation

        target = strategy = token = None

        cleanup = {
            "power_off": False,
            "place_released": False,
            "reservation_absent": False,
        }
        try:
            # Do NOT use reserve --wait: the pinned helper can lose its token on
            # timeout. Capture our token first, then bound the separate wait.
            output = client("reserve", "--shell", "name=" + PLACE).stdout
            match = re.search(r"^export LG_TOKEN=([A-Za-z0-9]+)$", output, re.MULTILINE)
            if not match:
                match = re.search(r"^LG_TOKEN=([A-Za-z0-9]+)$", output, re.MULTILINE)
            require(
                match is not None,
                "cannot parse own reservation token; operator inspection required",
            )
            token = match.group(1)
            client("wait", token, timeout=args.wait)
            allocation = reservation._parse_allocated_place(
                client("reservations").stdout, token
            )
            require(allocation == PLACE, "reservation allocated wrong place")
            client("-p", "+" + token, "acquire")

            with tempfile.TemporaryDirectory(prefix="libiio-managed-") as tmp:
                env_path = Path(tmp) / "env.yaml"
                render(place, env_path)
                os.environ["LG_COORDINATOR"] = COORDINATOR
                target = Environment(str(env_path)).get_target("main")
                strategy = target.get_driver("BootZynqMPJTAG")
                # Avoid shadowing exporter boot inputs with runner-local files.
                exporter_payloads(strategy)
                strategy.transition("kuiper_shell")
                address = live_address(target.get_driver("ADIShellDriver"))
                env = dict(
                    os.environ,
                    LIBIIO_MANAGED_ADDRESS=address,
                    LIBIIO_MANAGED_BUILD=str(args.build),
                    LIBIIO_MANAGED_PLACE=PLACE,
                    PYTEST_DISABLE_PLUGIN_AUTOLOAD="1",
                )
                if args.test_host:
                    remote_pytest(args, "run", address)
                else:
                    result = subprocess.run(
                        [
                            sys.executable,
                            "-m",
                            "pytest",
                            "-q",
                            TEST,
                            "--junitxml=" + str(args.evidence / "junit.xml"),
                        ],
                        cwd=ROOT,
                        env=env,
                        timeout=120,
                    )
                    require(result.returncode == 0, "hardware pytest failed")
        finally:
            cleanup = cleanup_owned(target, strategy, token, reservation, Reservation)
            (args.evidence / "cleanup.json").write_text(
                json.dumps(cleanup, indent=2) + "\n"
            )
            require(
                all(cleanup.values()),
                "managed cleanup incomplete; inspect cleanup.json",
            )
        count = validate_junit(args.evidence / "junit.xml")
        print(
            f"Verified {count} baseline/negative negotiation cases; NOT accelerated streaming qualification"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--run", action="store_true", help="explicitly reserve and boot tron"
    )
    parser.add_argument("--build", type=Path)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--wait", type=int, default=120)
    parser.add_argument("--test-host", choices=["picard.local"])
    parser.add_argument("--remote-source", type=Path)
    parser.add_argument("--remote-library-sha256")
    parser.add_argument("--test-timeout", type=int, default=120)
    parser.add_argument(
        "--remote-check",
        action="store_true",
        help="verify remote software and exact collection; NEVER acquire hardware",
    )
    args = parser.parse_args()
    require(1 <= args.wait <= 1800, "wait must be 1..1800 seconds")
    if args.remote_check:
        require(not args.run, "--remote-check cannot accompany --run")
        require(args.evidence is not None, "--evidence required")
        args.evidence = args.evidence.resolve()
        args.evidence.mkdir(parents=True, exist_ok=False)
        remote_pytest(args, "collect")
        print("Remote software/hash/collection check passed; NO hardware qualification")
        return
    if not args.run:
        print(
            json.dumps(
                {
                    "scope": "baseline-and-negative-negotiation",
                    "place": PLACE,
                    "coordinator": COORDINATOR,
                    "lock": str(LOCK),
                    "cases": CASES,
                    "plugin_sha": PLUGIN_SHA,
                    "daqiri_streaming_qualified": False,
                },
                indent=2,
            )
        )
        return
    require(
        args.build is not None and args.evidence is not None,
        "--build and --evidence required",
    )
    args.build, args.evidence = args.build.resolve(), args.evidence.resolve()

    def interrupted(signum, frame):
        raise RuntimeError("bounded run interrupted")

    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGALRM):
        signal.signal(sig, interrupted)
    signal.alarm(900 + args.wait * 2)
    try:
        logging.disable(logging.CRITICAL)  # plugin exceptions/logs can carry tokens
        execute(args)
    finally:
        signal.alarm(0)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        # Coordinator/plugin exception strings can contain credentials/tokens.
        print(
            "Managed hardware run failed: "
            + type(exc).__name__
            + "; inspect redacted evidence and prerequisites",
            file=sys.stderr,
        )
        sys.exit(1)
