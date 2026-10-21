#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Managed tron baseline/negative negotiation; never streaming qualification."""

import argparse
import ast
from types import SimpleNamespace
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


# Exact defaults audited at PLUGIN_SHA; never classify by substring.
POSTBOOT_CHECKS = {
    "i=0; until ip -4 addr show dev eth0 | grep -q 'inet '; do i=$((i+1)); test $i -lt 90 || exit 1; sleep 1; done": "eth0_ipv4",
    "test $(for n in /sys/bus/iio/devices/iio:device*/name; do cat \"$n\"; done | grep -c '^adrv9009-phy') -eq 2": "adrv9009_phy_count",
    "test $(dmesg | grep -c 'successfully initialized via jesd204-fsm') -ge 2": "jesd204_fsm_initialized_count",
}
POSTBOOT_COUNTS = {
    "eth0_ipv4_count": "ip -4 addr show dev eth0 | grep -c 'inet '",
    "adrv9009_phy_count": "for n in /sys/bus/iio/devices/iio:device*/name; do cat \"$n\"; done | grep -c '^adrv9009-phy'",
    "jesd204_fsm_initialized_count": "dmesg | grep -c 'successfully initialized via jesd204-fsm'",
}


@contextmanager
def postboot_diagnostics(strategy, args):
    """Observe original _run results without changing run_check's failure path.

    Labgrid 26 drops exitcode when constructing ExecutionError. Observe the
    instance's _run boundary instead; no class/global patch and no rerun of a
    qualification command. Additional read-only counts are best-effort snapshots,
    not qualification, and never expose the command or its output.
    """
    shell = getattr(strategy, "shell", None)
    if shell is None:
        yield
        return
    original = shell._run
    had_override = "_run" in vars(shell)
    previous = vars(shell).get("_run")
    records = args._postboot_checks = []

    def observed(cmd, *a, **kw):
        check_id = POSTBOOT_CHECKS.get(cmd) if type(cmd) is str else None
        if check_id is None or len(records) >= 3:
            return original(cmd, *a, **kw)
        record = {"check_id": check_id, "exit_status": None}
        records.append(record)
        # Transport errors propagate unchanged with unknown exit status.
        result = original(cmd, *a, **kw)
        rc = result[2]
        if type(rc) is int and 0 <= rc <= 255:
            record["exit_status"] = rc
        counts = record["snapshot_counts"] = {}
        for key, command in POSTBOOT_COUNTS.items():
            counts[key] = None
            try:
                out, _, status = original(command, timeout=5)
                # grep -c returns 1 for zero matches. Accept only a sole bounded
                # decimal, never names, addresses, kernel logs or free text.
                if (
                    type(status) is int
                    and status in (0, 1)
                    and type(out) is list
                    and len(out) == 1
                    and type(out[0]) is str
                    and re.fullmatch(r"[0-9]{1,6}", out[0])
                ):
                    counts[key] = int(out[0])
            except Exception:
                pass
        return result

    shell._run = observed
    try:
        yield
    finally:
        if had_override:
            shell._run = previous
        else:
            del shell._run
        try:
            (args.evidence / "postboot-checks.json").write_text(
                json.dumps({"checks": records}, indent=2) + "\n"
            )
        except Exception:
            pass  # Evidence I/O must not replace the original error or teardown.


def failure_evidence(exc, stage):
    """Bounded structural traceback, never exception repr/locals/resource output.

    Plugin errors may embed credentials, subprocess argv/output, or entire
    serial buffers. Deny arbitrary message text rather than trying to guess
    every secret spelling. Only fixed, audited conditions are disclosed.
    File basenames + function/line numbers locate the cause in the pinned code.
    """
    conditions = (
        (re.escape(EXPORTER_IDENTITY_ERROR), EXPORTER_IDENTITY_ERROR),
        (
            r"production U-Boot prompt .+ not observed",
            "production U-Boot prompt not observed",
        ),
        (r"PMU ROM did not enter sleep", "PMU ROM did not enter sleep"),
        (
            r"PMU firmware did not claim FW_IS_PRESENT",
            "PMU firmware did not claim FW_IS_PRESENT",
        ),
        (
            r"ZynqMP production U-Boot handoff failed:.*",
            "production JTAG handoff failed",
        ),
    )
    chain, seen = [], set()
    relation = "raised"
    while exc is not None and id(exc) not in seen and len(chain) < 16:
        seen.add(id(exc))
        frames = []
        tb = exc.__traceback__
        while tb is not None:
            code = tb.tb_frame.f_code
            frames.append(
                {
                    "file": Path(code.co_filename).name,
                    "function": code.co_name,
                    "line": tb.tb_lineno,
                }
            )
            tb = tb.tb_next
        # Only string args are inspected; never invoke a plugin's __str__.
        message = exc.args[0] if exc.args and type(exc.args[0]) is str else ""
        condition = next(
            (
                safe
                for pattern, safe in conditions
                if re.fullmatch(pattern, message, re.DOTALL)
            ),
            "untrusted exception text withheld",
        )
        item = {
            "type": type(exc).__name__,
            "relation": relation,
            "condition": condition,
            "frames": frames[-12:],
            "frames_truncated": len(frames) > 12,
        }
        # Return codes and errno are useful; argv/stdout/stderr are not safe.
        for field in ("returncode", "errno"):
            value = getattr(exc, field, None)
            if type(value) is int:
                item[field] = value
        if isinstance(exc, ExporterIdentityError):
            item["identity_stage"] = exc.identity_stage
            if exc.hostname is not None:
                item["hostname"] = exc.hostname
            if hasattr(exc, "safe_cause"):
                item["cause_evidence"] = exc.safe_cause
        chain.append(item)
        if exc.__cause__ is not None:
            exc, relation = exc.__cause__, "cause"
        elif not exc.__suppress_context__:
            exc, relation = exc.__context__, "context"
        else:
            exc = None
    return {
        "stage": stage,
        "plugin_sha": PLUGIN_SHA,
        "exceptions": chain,
        "chain_truncated": exc is not None,
        "redaction": "arbitrary messages, paths, locals, argv and output withheld",
    }


def record_failure(args, exc, stage):
    """Evidence failure must never prevent the independent teardown legs."""
    if getattr(args, "_failure_recorded", False):
        return
    args._failure_recorded = True
    try:
        evidence = failure_evidence(exc, stage)
        evidence["postboot_checks"] = getattr(args, "_postboot_checks", [])
        record = json.dumps(evidence, indent=2) + "\n"
        print("Managed hardware failure evidence: " + record, file=sys.stderr)
        if getattr(args, "_evidence_ready", False):
            (args.evidence / "failure.json").write_text(record)
    except Exception:
        print(
            "Managed failure evidence could not be saved (details withheld)",
            file=sys.stderr,
        )


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


EXPORTER_IDENTITY_ERROR = (
    "exporter identity preflight failed before reservation; repair the canonical "
    "exporter advertisement or runner DNS/SSH mapping for tron, then verify the "
    "resource-resolved SSH endpoint reports hostname tron; remote output withheld"
)


class ExporterIdentityError(RuntimeError):
    """Safe stage-specific identity failure."""

    def __init__(self, stage, returncode=None, hostname=None):
        super().__init__(EXPORTER_IDENTITY_ERROR)
        self.__suppress_context__ = True
        self.identity_stage = stage
        self.returncode = returncode
        self.hostname = hostname


@contextmanager
def identity_stage(stage):
    try:
        yield
    except ExporterIdentityError:
        raise
    except Exception as exc:
        error = ExporterIdentityError(stage)
        error.safe_cause = failure_evidence(exc, stage)
        raise error from None


def query_identity_resources():
    with identity_stage("resource-query"):
        result = client("-p", PLACE, "show", check=False)
        if result.returncode != 0:
            raise ExporterIdentityError("resource-query", result.returncode)
        return result.stdout


def validate_exporter_identity(raw):
    """Read-only exact-resource probe; no target, reservation, or fallback."""
    with identity_stage("resource-parse"):
        blocks = re.split(r"^Matching resource ", raw, flags=re.MULTILINE)[1:]
        resources = []
        for block in blocks:
            header, body = block.split("\n", 1)
            if header.startswith("'XilinxDeviceJTAG' "):
                require(
                    header
                    == "'XilinxDeviceJTAG' (tron/tlab/XilinxDeviceJTAG/XilinxDeviceJTAG):",
                    EXPORTER_IDENTITY_ERROR,
                )
                resources.append(ast.literal_eval(body.strip()))
        require(len(resources) == 1, EXPORTER_IDENTITY_ERROR)
        entry = resources[0]
        require(
            isinstance(entry, dict)
            and entry.get("cls") == "XilinxDeviceJTAG"
            and entry.get("avail") is True
            and isinstance(entry.get("params"), dict),
            EXPORTER_IDENTITY_ERROR,
        )
        resource = SimpleNamespace(**entry["params"])
    with identity_stage("prefix-resolution"):
        from adi_lg_plugins.drivers.xilinxjtagdriver import XilinxJTAGDriver

        driver = object.__new__(XilinxJTAGDriver)
        driver.xilinxdevicejtag = resource
        require(driver._exporter_host(resource) is not None, EXPORTER_IDENTITY_ERROR)
        prefix = driver._remote_prefix()
        require(bool(prefix), EXPORTER_IDENTITY_ERROR)
    with identity_stage("ssh-exec"):
        result = subprocess.run(
            prefix + ["hostname"],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
        if result.returncode != 0:
            raise ExporterIdentityError("ssh-exec", result.returncode)
    with identity_stage("hostname-compare"):
        hostname = result.stdout.strip()
        if hostname != PLACE:
            # Only audited identities, never arbitrary valid-looking secrets.
            raise ExporterIdentityError(
                "hostname-compare",
                result.returncode,
                hostname if hostname in ("tron", "lbvm") else None,
            )
    return {"stage": "hostname-compare", "returncode": 0, "hostname": PLACE}


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
    raw = query_identity_resources()
    tags = parse_place(raw)
    validate_place_available(raw, client("reservations").stdout)
    place = validate_place({"name": PLACE, "tags": tags})
    validate_exporter_identity(raw)
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
    try:
        _execute(args)
    except Exception as exc:
        record_failure(args, exc, getattr(args, "_stage", "owner-preflight"))
        raise


def _execute(args):
    args._stage = "owner-preflight"
    validate_owner_host()
    validate_environment(os.environ)
    args.evidence.mkdir(parents=True, exist_ok=False)
    # Build and validate bindings before any reservation; no installed libiio fallback.
    args._evidence_ready = True
    args._stage = "software-preflight"
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
    args._stage = "hardware-lock"
    with hardware_lock(args.wait):
        args._stage = "coordinator-preflight"
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
            args._stage = "reserve"
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
            args._stage = "reservation-wait"
            client("wait", token, timeout=args.wait)
            allocation = reservation._parse_allocated_place(
                client("reservations").stdout, token
            )
            require(allocation == PLACE, "reservation allocated wrong place")
            args._stage = "acquire"
            client("-p", "+" + token, "acquire")

            with tempfile.TemporaryDirectory(prefix="libiio-managed-") as tmp:
                args._stage = "environment-render-and-bind"
                env_path = Path(tmp) / "env.yaml"
                render(place, env_path)
                os.environ["LG_COORDINATOR"] = COORDINATOR
                target = Environment(str(env_path)).get_target("main")
                strategy = target.get_driver("BootZynqMPJTAG")
                # Avoid shadowing exporter boot inputs with runner-local files.
                exporter_payloads(strategy)
                args._stage = "BootZynqMPJTAG.transition(kuiper_shell)"
                with postboot_diagnostics(strategy, args):
                    strategy.transition("kuiper_shell")
                args._stage = "live-management-address"
                address = live_address(target.get_driver("ADIShellDriver"))
                env = dict(
                    os.environ,
                    LIBIIO_MANAGED_ADDRESS=address,
                    LIBIIO_MANAGED_BUILD=str(args.build),
                    LIBIIO_MANAGED_PLACE=PLACE,
                    PYTEST_DISABLE_PLUGIN_AUTOLOAD="1",
                )
                args._stage = "hardware-pytest"
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
        except Exception as exc:
            # Preserve the original cause before cleanup can raise another error.
            record_failure(args, exc, args._stage)
            raise
        finally:
            args._stage = "cleanup"
            cleanup = cleanup_owned(target, strategy, token, reservation, Reservation)
            (args.evidence / "cleanup.json").write_text(
                json.dumps(cleanup, indent=2) + "\n"
            )
            require(
                all(cleanup.values()),
                "managed cleanup incomplete; inspect cleanup.json",
            )
        args._stage = "junit-verification"
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
        # No raw traceback: exception strings can contain credentials/tokens.
        print(
            "Managed hardware run failed: "
            + type(exc).__name__
            + "; inspect failure.json and independent cleanup.json",
            file=sys.stderr,
        )
        sys.exit(1)
