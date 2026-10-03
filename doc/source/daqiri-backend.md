# Experimental DAQIRI backend

## Status and prerequisites

This checkout implements an **opt-in, built-in** `daqiri:` backend for Linux,
using CPU-accessible native IIO blocks. It is experimental, not a qualified
hardware streaming solution. It is not the loadable module or VRT49 integration
proposed in the [design and roadmap](daqiri-backend-design.md).

**A compatible peer must be implemented separately.** Its iiod management XML
must advertise the context attribute `daqiri.protocol` with the exact value
`dqi1-cpu-strict-credit-v1`, and its packet endpoint must implement the matching
experimental DQI1 handshake, layout agreement, credits and control commands.
This backend is **not compatible with current VRT49 firmware**, nor does an
unmodified iiod provide the required packet peer. The software fixtures in
`tests/` are tests, not a deployable server or firmware example. There is no
silent fallback to `ip:` or ordinary iiod sample streaming.

Context creation fetches real topology over an independently owned TCP iiod
connection. Missing or different capability values fail with
`-EPROTONOSUPPORT` before packet-runtime initialization. The DAQIRI runtime is
opened only when a buffer is opened. Advertising the capability alone does not
prove that a peer can stream.

Production packet I/O requires **raw ibverbs**. Real DAQIRI socket-engine tests
use a **separate test-only adapter**; the production adapter does not accept a
socket profile. Raw-ibverbs hardware has **not been run/qualified** for this
implementation. Full streaming through the public libiio buffer/block API has
**not been verified** by those tests.

## Build and dependency

The source baseline is NVIDIA DAQIRI commit
[`993363da36aa320b43f4160aa848e8ddd2373a36`](https://github.com/NVIDIA/daqiri/tree/993363da36aa320b43f4160aa848e8ddd2373a36).
Use that dependency revision and a compatible C++ toolchain. CMake discovers an
installed `daqiri` CONFIG package and links `daqiri::daqiri`; it does **not**
download DAQIRI or enforce that commit automatically. The pinned dependency's
build requires CUDA/CUDAToolkit even though this backend accepts only host
memory. CPU-only here does not mean a CUDA-free dependency installation.

`WITH_DAQIRI_BACKEND` defaults to `OFF`. With it off, this integration does not
enable C++ or discover DAQIRI/CUDA. With it on, the build requires Linux, real
thread support (`WITH_LIBTINYIIOD` is rejected), C++17, Threads, the iiod client
and libxml2. `WITH_NETWORK_BACKEND` is not required for this separately owned
management connection.

Given an already installed pinned DAQIRI dependency, configure from the libiio
source directory (replace the installation paths):

```sh
cmake -S . -B build-daqiri \
  -DWITH_DAQIRI_BACKEND=ON \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/daqiri-prefix \
  -DCUDAToolkit_ROOT=/absolute/path/to/cuda \
  -DDAQIRI_PROFILE_DIR=/etc/libiio/daqiri
cmake --build build-daqiri
```

`DAQIRI_PROFILE_DIR` is an absolute, compile-time CMake cache path, defaulting to
`/etc/libiio/daqiri`. It is not a URI parameter or runtime environment override.
Registration is built into libiio via `context.c`; no separate backend module
is produced. The implementation adds no public libiio API.

## URI and local profile schema

The exact URI grammar is `daqiri:<profile-name>`. The name is 1–64 characters
from ASCII `A-Z`, `a-z`, `0-9`, `_` and `-`. No paths, extensions, query strings,
credentials or remote configuration URLs are accepted in the name. The loader
reads `<DAQIRI_PROFILE_DIR>/<profile-name>.conf`.

The profile is a strict `key=value` text file, not YAML. Every key below is
required exactly once; unknown or duplicate keys are rejected. Empty lines and
lines whose first character is `#` are ignored. No whitespace trimming,
quoting, interpolation or inline comments are supported; use LF line endings.
Each line is limited to 1024 characters. Numeric values are positive decimal
digits only (leading zeroes are accepted).

| Key | Required value or limit |
| --- | --- |
| `version` | Exactly `1`. |
| `mode` | Exactly `raw-ibverbs-cpu`. |
| `trusted_network` | Exactly `yes`; an administrator acknowledgment, not authentication. |
| `management` | Numeric IPv4 address of the TCP iiod endpoint; no hostname, URI or IPv6. |
| `management_port` | 1–65535; no implicit default. |
| `yaml` | Absolute local path to DAQIRI network YAML, at most 511 bytes. |
| `interface` | DAQIRI interface name, at most 63 bytes; must match the YAML interface name. |
| `device` | Real IIO device ID to stream, at most 127 bytes; not a label or invented topology. |
| `local_ip`, `peer_ip` | Numeric IPv4 packet endpoints, each at most 15 bytes. |
| `peer_mac` | Destination MAC text, at most 17 bytes; MAC validity is delegated to DAQIRI endpoint creation. |
| `local_port`, `peer_port` | 1–65534; base data UDP ports. Control uses the corresponding base port plus one. |
| `payload` | 1–1400 sample bytes per packet; must also be a multiple of the selected scan stride. |
| `max_block` | 1–67108864 bytes; must accommodate the selected scan stride. |
| `max_blocks` | 1–64 blocks. |
| `timeout_ms` | 1–60000 milliseconds. |

The product `max_block * max_blocks` must not exceed 268435456 bytes. Profile
loading rejects missing files with `-ENOENT`, invalid syntax/limits with
`-EINVAL`, and unsupported version/mode/trust values with `-ENOTSUP`.
Nonzero context timeout parameters override the profile timeout; the backend's
registered default is 1000 ms. The effective timeout must not exceed 60000 ms.
Runtime timeout updates accept only 1–60000 ms and are blocked while a buffer
is open.

### DAQIRI YAML constraints

The YAML is parsed by DAQIRI, then checked by `daqiri-transport.cpp`; this page
does not substitute an invented hardware configuration for a qualified one.
The current validator requires:

* RAW stream type and IBVERBS in both engine fields; exactly one interface.
* Exactly two RX and two TX queues, in order with IDs 0 and 1, all INDIRECT.
  Lane 0 is data and lane 1 is control.
* Exactly four distinct HOST memory regions, one per queue, with no shared
  queue pool. Each region has 1–4096 buffers of 1514–65536 bytes; aggregate
  region storage is at most 256 MiB. RX metadata-buffer count must be 4099–16384;
  TX must be 8194–16384. Zero/default counts fail closed. These lower bounds
  cover both shared metadata pools' maximum ring, worker and adapter holdings,
  including a TX worker stalled on a full send queue. See
  `tests/daqiri-metadata.md` for the pinned-source proof and allocation-seam tests.
* Queue batch sizes of 1–64, one memory region per queue, no split boundary,
  no queue offloads and no RX reorder configurations.

Named raw UDP endpoints use an L2 MTU of 1514 bytes excluding FCS. The adapter's
RX parser accepts untagged Ethernet/IPv4/UDP only: no VLAN, IPv4 options or IP
fragments. It validates source/destination IPv4 addresses, UDP ports and lengths
rather than trusting steering alone. Configuring and qualifying the NIC,
receive steering, pool sizing and permissions remains an administrator task.
The test socket YAML is **not** a production raw-ibverbs configuration.

## Implemented callbacks and restrictions

The following describes source implementation, not a claim that every path
has passed public-API integration or hardware testing.

| Callback group | Current implementation |
| --- | --- |
| `create`, `shutdown` | Own TCP transport and `iiod_client`; preserve DAQIRI context identity and remote topology; enforce the capability gate. Close buffers before destroying the context. |
| `read_attr`, `write_attr` | Delegate device-associated attributes to `iiod_client`. Attributes without a device are not handled by these callbacks (`-ENOTSUP`); context metadata comes from XML. Writes return `-EBUSY` while a buffer is open. |
| `get_trigger`, `set_trigger`, `ping` | Delegate to iiod; trigger changes return `-EBUSY` while a buffer is open. |
| `set_timeout` | Update management and subsequent stream timeout configuration, subject to the bounds and busy gate above. |
| `open_buffer`, `close_buffer` | Only configured device ID and buffer index 0. One open buffer per context; acquire/release exclusive packet runtime ownership. |
| `enable_buffer`, `cancel_buffer` | Delegate start/stop and local cancellation to the stream core. `samples` is not used to size native blocks. Cyclic operation is rejected with `-ENOTSUP` by the backend. |
| `create_block`, `free_block` | Allocate contiguous CPU storage within profile limits; positive scan-aligned sizes only. Freeing a block stops the stream. |
| `enqueue_block`, `dequeue_block` | Native block ownership and bounded waits. Pending nonblocking dequeue returns `-EBUSY`; invalid ownership transitions return `-EPERM`. Cyclic enqueue is rejected with `-ENOTSUP` by the backend. |
| `get_dmabuf_fd`, `disable_cpu_access` | Explicitly return `-ENOTSUP`. No DMA-BUF export or device-only memory path. |

No callbacks are registered for scanning, register read/write, format refresh,
remote version queries, event streams, or legacy `readbuf`/`writebuf` streaming.
Do not infer support for these operations from the broader design proposal.

Enabled channels must be scan elements of a single direction, with valid
byte-addressable integer storage formats, nonzero repeat, and a valid sample
stride. Mixed RX/TX masks are rejected. The serialized layout description is
bounded to 1024 bytes. No conversion, GPU path, RoCE, DPDK, multi-device stream,
or simultaneous bidirectional stream is implemented.

Management-only contexts may coexist, but the backend runtime broker permits
only one active DAQIRI runtime in the process (`-EBUSY` for a second backend
lease). Concurrent direct application use of DAQIRI is forbidden: the broker
cannot coordinate unrelated callers. It does not shut down a foreign engine
when initialization fails; DAQIRI's foreign-engine error currently maps to
`-EIO` rather than necessarily `-EBUSY`.

## Stream contract and security

DQI1 is an experimental private protocol, not VITA 49 framing or a stable
public wire specification. Exact packet encoding, layout negotiation, RX
extent agreement and START/STOP failure handling are defined by
`daqiri-wire.hpp` and `daqiri-stream.cpp`; consult the matching sources and test
peer together rather than implementing a peer from the older proposal.

For RX, the first block allocation fixes the session's block extent. All RX
blocks and enqueue extents must match it. Before START, mandatory CONFIG
(opcode 8) carries the actual extent as a big-endian 32-bit value and requires
an exact acknowledgement. HELLO/START-only peers cannot proceed. TX block
extents remain variable. If START fails after possible peer acceptance, the
backend attempts bounded STOP cleanup while preserving the original error;
a missing STOP acknowledgement does not establish remote quiescence.

The core uses separately serviced control/data lanes, cumulative credits and
bounded host blocks. RX grants correspond to enqueued storage; application-held
blocks do not become reusable until re-enqueued. TX waits for remote credits.
Successful RX completion requires the agreed extent, not a zero-filled gap.
The current strict receiver does not implement the proposal's reorder window:
an ahead-of-expected packet fails the stream rather than being buffered for
reordering. There is no sample retransmission. TX dequeue represents local
transport acceptance, **not DAC playback or remote application consumption**.
Cancel/disable terminates the stream; it is not a reusable pause/resume path.

Management is plain TCP in this backend, **without TLS**. Data and control are
neither authenticated nor encrypted. Session identities and address checks do
not prevent hostile injection. Use only an isolated trusted network, protect
administrator-owned profiles/YAML, and provision raw-NIC privileges and
locked-memory limits externally. The backend does not configure system
privileges or supply a security tunnel.

## Verification boundaries

`tests/daqiri-management.py` exercises the real shared library through public
context/attribute APIs using an independent loopback ASCII iiod fixture. Its
scope includes registration, real topology identity, capability rejection,
attribute dispatch and connection cleanup. It deliberately rejects binary iiod
negotiation and does not open a streaming buffer. See
`tests/daqiri-management.md` for commands and recorded results.

`tests/run-daqiri-socket.sh` builds the production C++ sources against installed
DAQIRI headers/libraries and exercises the stream core via a separate adapter
around the **real DAQIRI socket engine**, with an independently encoded POSIX
UDP peer. `tests/daqiri-socket.md` records sanitizer results and coverage. These
are not mocked DAQIRI APIs, but they bypass the production raw-ibverbs adapter
for packet I/O and do not prove public libiio streaming integration.

Outstanding gates include full public buffer/block integration, raw-NIC
parsing/steering, queue-full ownership and DMA teardown under hardware faults,
peer interoperability, throughput/latency/soak measurements, and a broader
concurrency/fault matrix. No hardware or performance result is claimed here.
The [design document](daqiri-backend-design.md) retains the broader roadmap;
its proposed callbacks, security, module packaging and VRT49 framing are not
implemented merely because they appear there.
