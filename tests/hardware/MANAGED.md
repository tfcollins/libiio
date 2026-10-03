# Managed ADRV9009-ZU11EG baseline

This opt-in gate reserves **tron** at **10.0.0.41:20408**, renders the live
place with the pinned ADI plugin, boots `BootZynqMPJTAG.kuiper_shell`, and
uses the board's live default-route IPv4 address. No static URI option exists.
It uses the production boot inputs from place tags, not custom DAQIRI firmware.

**Scope:** freshly built libiio public network topology and read-only attributes,
plus explicit `-EPROTONOSUPPORT` from DAQIRI against legacy management firmware.
This is NOT DQI1, accelerated streaming, RDMA, sample integrity, or throughput
qualification. No buffer, TX, RF setting write, or packet runtime is opened.

## Dependencies and preparation

Use a dedicated Python 3.11 environment; do not alter Nemo's working environment:

```sh
python3.11 -m venv .venv-managed
.venv-managed/bin/python -m pip install -r tests/hardware/requirements-managed.txt
```

The plugin pin is the existing vrt49-daqiri Corundum workflow pin. Force-reinstall
that VCS distribution when updating a persistent environment: its version can
remain unchanged between commits. Launcher validates PEP 610 commit metadata and
Labgrid version before acquisition. Vendor tools, exporter SSH/rsync access and
production recovery assets must already work in the chosen runner environment.
Do not invent a GitHub runner label: workflow integration is intentionally absent
until live repository runner inventory and tooling are verified.

Configure a fresh **shared**, network+DAQIRI-enabled build using the real pinned
DAQIRI dependency and a private writable absolute `DAQIRI_PROFILE_DIR` (see
`doc/source/daqiri-backend.md`). Do not point at a system libiio or another source
checkout. The launcher runs `cmake --build` and validates the CMake cache/library
before acquiring. The profile directory must already exist.

```sh
.venv-managed/bin/python tests/hardware/managed.py  # offline inspection
.venv-managed/bin/python tests/hardware/managed.py --run \
  --build /absolute/path/to/enabled-build \
  --evidence /absolute/path/to/new-evidence-directory --wait 120
```

Run ownership on **Nemo** (`nemo.local`), with no existing reservation or IIO URI
in its environment. The launcher rejects other hostnames: the same lock pathname
on Picard would be a different lock and would not serialize Nemo's CI jobs.
All launch paths must share
`/tmp/vrt49-corundum-hardware.lock`. This launcher waits boundedly on that lock
and holds it until power-off, release, cancellation, readback and JUnit checking.
Do not nest it beneath an independently held copy of that lock.

The hw-request reservation helper's combined `reserve --wait` can lose its token
on timeout at this pin. Therefore this wrapper creates the reservation without
waiting, captures its token privately, then issues a bounded separate `wait`.
It uses the pinned hw-request `Reservation`/release API and allocated-place
parser, and `hw_ci.schema.validate_place`/`render_env_to`; it never constructs a
static Labgrid environment. Release is followed by independent exact-place and
own-token readbacks. Tokens and raw coordinator resources are not evidence.

`junit.xml` must have exactly these two passing identities, no skips/errors:

- `tests.hardware.test_managed_board.test_network_enumeration_and_attributes`
- `tests.hardware.test_managed_board.test_legacy_daqiri_rejected`

`cleanup.json` must report power-off, place released, and own reservation absent.
JUnit includes the tested library SHA-256 and explicitly disclaims streaming
qualification. Missing setup, context, family, attribute, collection, or cleanup
is a failure, not a skip. Selecting the hardware file without the launcher fails.
SIGINT/SIGTERM and the bounded run alarm unwind through cleanup; SIGKILL or host
loss cannot be recovered by Python and require independent operator readback.

## Offline checks (no hardware contact)

```sh
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python3 -m pytest -q tests/hardware/test_managed_offline.py
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python3 -m pytest --collect-only -q tests/hardware/test_managed_board.py
python3 tests/hardware/managed.py
```

## Independent review and preparation

The launcher rejects an acquired place or an existing tron reservation before
creating its own reservation. Before launching, also query hosted jobs: a queued
hardware workflow can be preparing artifacts while the place and lock look free.
Do not compete with that workflow.

Cleanup calls the power driver directly, rather than trusting a cached strategy
state after a partially failed power-on. Every cleanup leg has a fresh timeout;
power, target cleanup, release/cancellation, and both coordinator readbacks are
independently attempted. A failed target cleanup is retained in `cleanup.json`.
Exporter payload prefixes are mandatory even when runner-local shadows exist.

Read-only preparation created an isolated pinned environment on Nemo at
`/tmp/libiio-managed-review/venv`; its pinned API and coordinator preflight passed.
Nemo's ambient `~/venv_WORKING` remains untouched. A fresh real network+DAQIRI
shared build on Picard lives at `/tmp/libiio-managed-review/build` (source under
`source`, writable profiles under `profiles`). It loaded successfully through
`board_api.prepare_library`, and all four independent software management-fixture
cases passed. These are **not physical board results**.

Physical execution was deliberately withheld: original VRT49 run `37084757729`
was canceled, but replacement Corundum run `37085080336` at
`b697141516e63c52295015d43a9f58d190fbdb1d` had completed board preflight and was
preparing its kernel/bitstream. Read-only coordinator inspection found tron free
and no reservations; this does not override the impending hosted ownership.

No board was acquired, booted, powered off, or released by this review.

## Explicit Picard pytest worker

The default remains local build/pytest. `--test-host picard.local` moves only
library loading, collection and pytest to the prepared Picard workspace. Nemo
still owns the shared flock, reservation, boot, live address discovery and every
cleanup/readback operation. **Do not run while Corundum run `37085080336` has
priority**, even if the place currently looks free.

Only these prepared paths are accepted (no remote configure/build, arbitrary
host, interpreter, command or pytest arguments):

- source: `/tmp/libiio-managed-review/source`
- build: `/tmp/libiio-managed-review/build`
- Python: `/tmp/libiio-managed-review/venv/bin/python`, with pytest 8.3.5
- real DAQIRI prefix: `/tmp/libiio-daqiri-verification/prefix`

Prepare the source independently: all tracked files plus `managed.py`,
`remote_worker.py`, `board_api.py` and `test_managed_board.py` must match the owner
checkout byte for byte. The launcher sends hashes, **not a source deployment**.
Provide the independently recorded library SHA-256 explicitly. The worker checks
all source hashes and the library before and after execution, validates the CMake
source/backend/profile contract and real dependency prefix, and loads the actual
library. Do not rebuild or edit the prepared tree during a run.

Hardware-free check (allowed from any host; no coordinator calls or lock):

```sh
python3 tests/hardware/managed.py --remote-check \
  --test-host picard.local --remote-source /tmp/libiio-managed-review/source \
  --build /tmp/libiio-managed-review/build \
  --remote-library-sha256 a9b89b7d9c9848b4152c085ce413e020547e85129f2264053779fbbb96232d4b \
  --evidence /tmp/libiio-remote-check-NEW
```

After hosted ownership ends, from the matching checkout **on Nemo**:

```sh
/tmp/libiio-managed-review/venv/bin/python tests/hardware/managed.py --run \
  --test-host picard.local --remote-source /tmp/libiio-managed-review/source \
  --build /tmp/libiio-managed-review/build \
  --remote-library-sha256 a9b89b7d9c9848b4152c085ce413e020547e85129f2264053779fbbb96232d4b \
  --evidence /tmp/libiio-managed-physical-NEW --wait 120 --test-timeout 120
```

Choose a new owner evidence path each time. Picard independently creates a
private unique `/tmp/libiio-managed-evidence-*` directory. SSH uses argument
quoting, batch authentication and no environment forwarding. The worker starts
under `env -i`; pytest receives only fixed PATH/LANG, plugin-autoload/bytecode
controls, prepared build, and (for a physical run only) the live address/place.
No coordinator credentials, reservation token, generated YAML, arbitrary owner
environment or static URI is copied.

`--test-timeout` is bounded to 1..120 seconds. The worker has its own watchdog
and kills the entire pytest process group, including on normal exit. If SSH
fails or the owner is interrupted, Nemo waits out the independent remote budget
before teardown, keeping the lease/flock throughout. Host loss/SIGKILL still
requires independent operator inspection. A failed remote run never bypasses
Nemo's `finally` cleanup. JUnit travels back in the worker's JSON response and is
saved locally before checking return status; success requires the exact two
passing cases, no skips/errors, and the expected library hash property. Missing
or malformed evidence is failure, not qualification. Collection has separate
`collect-*` evidence and is never accepted as a physical pass.

Offline regression tests:

```sh
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python3 -m pytest -q \
  tests/hardware/test_managed_offline.py tests/hardware/test_remote_offline.py
```

