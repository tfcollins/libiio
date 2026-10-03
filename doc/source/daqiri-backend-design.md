# DAQIRI backend design and roadmap

## Status and source baseline

This document retains the **original proposal and future acceptance gates**.
An experimental implementation now exists in this checkout; see the
[implemented backend guide](daqiri-backend.md) for its actual build flags,
profile schema, callback restrictions and verification boundaries. The remaining
sections describe design intent, not a statement that every feature is present.

The current implementation differs deliberately from this proposal: it is an
OFF-by-default **built-in backend**, not a loadable module; it owns an independent
plain-TCP iiod connection, not a TLS transport factored from `network.c`; and it
uses private DQI1 framing, **not VRT49 framing**. It implements a narrower CPU
native-block callback set and strict in-order RX rather than the proposed
reorder window. Its production adapter accepts only raw ibverbs. Real DAQIRI
socket-engine tests use a separate test adapter and are not a production socket
fallback or proof of full public libiio streaming integration.

A matching management capability and DQI1 packet peer are prerequisites. No
compatible production server is supplied, and **current VRT49 firmware is not
compatible**. Raw-ibverbs hardware has **not been run/qualified**; hardware
performance and interoperability remain open gates. Existing network, local,
USB and serial backends remain unchanged.

Exact experimental wire details, including RX extent agreement and START/STOP
failure cleanup, belong to the matching `daqiri-wire.hpp`, `daqiri-stream.cpp`
and test peer, not the proposed wire contract below.

The design was inspected against these immutable revisions:

| Repository | Commit | Relevant source |
| --- | --- | --- |
| [tfcollins/libiio](https://github.com/tfcollins/libiio/tree/1f2ade12b3a97527334a8ed9319d4c2b63b9d4b7) | `1f2ade12b3a97527334a8ed9319d4c2b63b9d4b7` | `include/iio/iio-backend.h`, `network.c`, `iiod-client.c`, `block.c`, `buffer.c`, `dynamic.c` |
| [NVIDIA/daqiri](https://github.com/NVIDIA/daqiri/tree/993363da36aa320b43f4160aa848e8ddd2373a36) | `993363da36aa320b43f4160aa848e8ddd2373a36` | `include/daqiri/common.h`, `include/daqiri/types.h`, `src/common.cpp`, `src/engines/ibverbs/daqiri_ibverbs_engine.cpp`, `src/engines/rdma/daqiri_rdma_engine.cpp` |
| [tfcollins/vrt49](https://github.com/tfcollins/vrt49) | `a958d2e116c5d9602a11f5557ce33e623a5daa0f` | `README.md`: VITA 49.2 over UDP and `iiod-vrt49` command/context translation |

The vrt49 SHA is verified in the local checkout; its public GitHub commit URL
returned HTTP 404 during validation, so public availability is not established.
Paths and symbols below refer to these revisions, not an assumed released API.
In particular, DAQIRI provides packet I/O and resource management; the IIO
session handshake, cumulative credits, command reliability, and VRT49 mapping
below are **new integration protocol work**, not DAQIRI-native features.

The companion [VRT49 Ethernet design](https://github.com/tfcollins/vrt49/blob/design/daqiri-flow-control/doc/source/explanation/daqiri-ethernet-design.md)
is published for review in [vrt49 PR #40](https://github.com/tfcollins/vrt49/pull/40).
It is a design deliverable, not part of the pinned implementation baseline.

## Architecture and bootstrap

```text
unchanged libiio C API / language bindings
              |
      proposed daqiri.c backend
         |                  |
 existing iiod management   private C ABI shim -> DAQIRI C++
 (TCP, TLS where configured)      |                    |
         |                 bounded data queues   bounded control queues
 device metadata, attrs,          |                    |
 triggers, session capability     +-- UDP/VRT49 peer --+
```

Use iiod for enumeration, XML topology, attributes, triggers, register access,
and initial capability negotiation. Do not invent device/channel descriptions
from a DAQIRI configuration. Factor transport helpers out of `network.c` rather
than calling its static functions or casting its private context data. The
existing `iiod_client_create_context()` accepts a backend descriptor; use that
route with the new descriptor and explicitly owned client state. Do not attach
network backend private pointers to DAQIRI objects.

The server/companion gateway must implement a versioned capability/session
extension. This is an explicit prerequisite: an unmodified iiod or legacy
VRT49 target cannot be assumed to support credit-controlled streaming. Negotiate
protocol version, session nonce, device ID, buffer index/direction, channel
mask, scan layout digest, sample stride, MTU, max datagram payload, queue limits,
credit unit, timeout, and supported commands. Both sides must agree before
allocating/arming the stream. Reject unknown required capabilities. Explicit
`daqiri:` selection must fail, not silently downgrade to ordinary `ip:`.

Bootstrap reserves data/control ports and returns a peer-bound session identity.
Install receive rules and queue storage, post RX blocks, establish control
liveness, then grant initial credits and issue START. STOP/RESET has an
independent control path even when sample credits are zero. Dynamic changes to
scan format require quiescence, a new layout digest and new session epoch; no
mid-block reinterpretation of samples.

## Backend registration and callback mapping

`include/iio/iio-backend.h:95-147` is the actual callback contract. This checkout
uses `iio_buffer_open()` and `iio_buffer_stream_create_block()`, not a design
based solely on the historical refill/push interface.

Propose `WITH_DAQIRI_BACKEND=OFF` by default and a loadable module exporting the
C symbol `iio_daqiri_backend`, with `IIO_BACKEND_API_V1`, name `daqiri`, prefix
`daqiri:`. `dynamic.c:iio_module_get_backend()` resolves `iio_%s_backend` and
strips `uri_prefix` before calling `create`; do not parse the prefix twice.
Follow the existing dynamic-module CMake mechanism, with no C++/CUDA dependency
in the default libiio build. No new public libiio symbol is required for the
CPU milestone.

| Callback | Proposed implementation and contract |
| --- | --- |
| `scan(params, scan, args)` | Explicitly configured endpoints only initially; do not advertise ordinary iiod as DAQIRI-capable without capability probing. |
| `create(params, uri)` | Parse local profile, open management client, obtain real topology, negotiate session capability, acquire exclusive DAQIRI runtime ownership; transactional unwind on failure. |
| `read_attr`, `write_attr` | Use `iiod_client_attr_read/write` as in `network.c`; serialize with configuration changes. Backend diagnostics should be distinguishable from remote device attributes. |
| `get_trigger`, `set_trigger`, `reg_read`, `reg_write`, `refresh_format`, `ping` | Follow the corresponding management operations in `network.c`; retain binary-protocol checks for operations that require them. Freeze layout-changing writes while streaming or stop and renegotiate. |
| `get_version` | Preserve remote version semantics; expose DAQIRI pin and negotiated protocol separately as diagnostics. |
| `set_timeout(ctx, timeout)` | Propagate management timeout and update bounded backend wait deadlines under lock; obey libiio's already-resolved context timeout semantics. |
| `open_buffer(dev, idx, mask)` | Validate direction, index, enabled channels and stride; allocate per-stream state, reserve queues and negotiate peer mapping. Never open an ordinary iiod sample stream alongside the accelerated stream unintentionally. |
| `create_block(pdata, size, void **data)` | Allocate stable, contiguous host storage; reject nonintegral scan sizes and arithmetic overflow. Record block size internally. Return encoded libiio error pointers on failure. |
| `enqueue_block(block, bytes_used, cyclic)` | Validate size/scan alignment and ownership; transfer block to backend pending queue. RX makes storage eligible for credits; TX packetizes into DAQIRI-owned slots only when remote credit exists. Reject cyclic initially with `-ENOSYS`, not an endlessly repeated UDP approximation. |
| `dequeue_block(block, nonblock)` | Publish only a complete block after copy/synchronization and validation; return `-EBUSY` if pending in nonblocking mode, matching `block.c`, and `-EPERM` for invalid ownership transitions. No success for a block containing an unreported gap. |
| `enable_buffer(pdata, nb_samples, enable, cyclic)` | Arm/stop peer after queues and layout are valid. `buffer.c` can supply zero `nb_samples` for backend-created blocks; derive capacity from recorded block sizes, not from this argument alone. |
| `cancel_buffer(pdata)` | Idempotent local cancellation; wake blocked calls immediately, revoke future grants by ending session, request peer STOP, then drain safely. |
| `free_block`, `close_buffer`, `shutdown` | Quiesce users/NIC/GPU before freeing; close stream resources before management context; release DAQIRI only if this backend owns it. |
| `open_ev`, `close_ev`, `read_ev` | Reuse supported management event operations; do not reinterpret sample/control packets as IIO events. |
| `get_dmabuf_fd`, `disable_cpu_access` | Leave unsupported initially. A DAQIRI pointer is not a DMA-BUF descriptor. |
| `readbuf`, `writebuf` | Not needed for the selected native-block path; do not leave partial native block support that accidentally falls back to these callbacks. |

`block.c` dispatches directly to native enqueue/dequeue callbacks when block
private data exists, bypassing its task-token ownership guard. Therefore the
backend itself must reject double enqueue/dequeue and synchronize destruction.
Its successful dequeue has no valid-byte-count return channel: the first
implementation must fill the requested RX extent or return an error, not expose
short or uninitialized samples. `buffer.c` cancellation flushes the generic
worker but cannot automatically join a backend-owned worker.

## C/C++ boundary and process ownership

Keep registration, libiio callback tables and error-pointer construction in C
(`daqiri.c`). Put all DAQIRI includes and types in `daqiri-transport.cpp`, behind
a private C-compatible header with opaque handles, fixed-width integers,
explicit lengths, and integer status returns. Proposed shim operations cover
runtime acquire/release, stream open/close, packet polling/submission, and
cancel/drain. These are new private functions, not existing DAQIRI APIs.

Use `extern "C"` on shim declarations/definitions; never export STL, exceptions,
`BurstParams`, CUDA handles or DAQIRI enum representation through libiio ABI.
Catch `std::bad_alloc`, other standard exceptions and unknown exceptions at each
boundary; map to `-ENOMEM` or a logged `-EIO`. Keep allocation and release in the
same language/runtime owner. Pin the C++ compiler/standard-library ABI alongside
the DAQIRI package; do not assume its C++ interface is a stable binary ABI.

`src/common.cpp:740-743` rejects initialization when its process-global
`g_daqiri_engine` already exists; `shutdown()` destroys the active engine. Start
with **one DAQIRI-owning context per process**, fail additional acquisitions
with `-EBUSY`, and forbid concurrent use of DAQIRI directly by the application.
Implement a process-global runtime broker with a mutex, normalized configuration
fingerprint, ownership flag and reference count for context/stream/worker leases.
The initial broker admits one context and its child leases; reject incompatible
engine, memory, NIC or queue configuration with `-EBUSY`, never reinitialize an
active engine. Only final release after all drains may invoke `shutdown()`.
A shim mutex coordinates this backend, not unrelated callers. Multi-context
sharing is deferred until queue partitioning and an upstream ownership contract
(or isolated helper process) are proven; never steal an existing engine.
One owner worker per direct TX queue avoids DAQIRI's direct-queue thread checks.
`src/engine.cpp:get_default_engine_type()` prefers ibverbs when built, then DPDK.
Select ibverbs explicitly in the baseline profile; DPDK is a separate explicit
qualification target, not an assumed default or interchangeable engine.

## Concrete DAQIRI API use

All names in this section exist in `include/daqiri/common.h` at the pinned SHA.
`daqiri.h` includes C++ headers; it is not a C binding despite its suffix.

1. Parse only an administrator-selected local file using
   `daqiri::parse_network_config_from_yaml_file(path, NetworkConfig&)`; validate
   local and negotiated limits, then `daqiri::daqiri_init(config)`. Explicit file
   APIs avoid the ambiguous YAML-string-or-path overload. DAQIRI also exposes
   `get_memory_region_requirements()` and `MemoryRegionBindings` for externally
   allocated memory, but do not require external registration for the first path.
2. Resolve `get_port_id(interface)` and configured queue IDs. For raw ibverbs TX,
   construct `RawUdpEndpointConfig` and call `add_endpoint(config, &id)`;
   `get_endpoint_id(name, &id)` and `delete_endpoint(id)` manage lookup/lifetime.
   `mtu_` is **L2 bytes excluding FCS**, not the Linux IP MTU. Segment zero is
   payload only for `send_tx_burst(endpoint_id, queue_id, burst)`; named endpoints
   reject multi-segment/HDS TX. See `examples/named_endpoints_example.cpp:53-65`.
3. RX: `get_rx_burst(&burst, port, queue)`; on success inspect `get_num_packets`,
   `get_segment_packet_ptr`, `get_segment_packet_length` and, where configured,
   `get_packet_flow_id`. Bounds-check the full packet and protocol headers before
   copying scan bytes. Release with `free_all_packets_and_burst_rx` only after
   all synchronous/asynchronous readers complete.
4. TX: `create_tx_burst_params`, `set_header`, optionally
   `is_tx_burst_available`, then `get_tx_packet_burst`. Fill packet storage and
   `set_packet_lengths`; submit via the endpoint overload. Header-building APIs
   `set_eth_header`, `set_ipv4_header`, `set_udp_header` exist for the non-endpoint
   path, but must not duplicate the named endpoint's inline headers.
5. **Ownership exception:** `send_tx_burst` consumes storage on both `SUCCESS`
   **and `NO_SPACE_AVAILABLE`**. Never free or retry that pointer in either case.
   Validation failure and direct-queue `NOT_READY` do not consume it. Free an
   allocated but unsent burst with `free_all_packets_and_burst_tx`; free metadata
   alone with `free_tx_metadata` when no packet storage was acquired. Retrying
   requires fresh storage and a retained source payload.
6. `wait_for_tx_idle(timeout_ms)` is an engine-dependent local drain boundary
   (`NOT_READY` on timeout, `NOT_SUPPORTED` where unavailable), not peer-consumer
   completion. Runtime raw-ibverbs queues/memory can be added/deleted using
   `add_rx_queue_async`, `add_tx_queue_async`, `add_memory_region_async`, their
   delete counterparts and `poll_resource_op`. Flow operations use
   `add_rx_flow_async`, `delete_flow_async`, `poll_flow_op`. Acceptance is not
   completion: retain handles/storage until completion, including partial batch
   flow-install failures. Static configured queues are sufficient initially.

Translate actual `Status` values from `types.h:81-95`, not stale enum names in
API comments: resource scarcity/`NOT_READY` means pending or `-EBUSY` under
nonblocking policy; `INVALID_PARAMETER` -> `-EINVAL`; `NOT_SUPPORTED` ->
`-ENOSYS`; internal failure -> `-EIO`; deadline -> `-ETIMEDOUT`; cancellation ->
`-ECANCELED`. Distinguish RX polling with no burst from invalid arguments. Log
original status and operation without exposing keys or credentials.

## Memory and completion contract

| Memory path | Compatibility and release boundary |
| --- | --- |
| Host / host-pinned DAQIRI packets -> host IIO blocks | Baseline. Copy and reassemble payload only into contiguous scan layout; return packet storage after the copy, but only grant further end-to-end credit against genuinely available destination capacity. |
| Device DAQIRI packets -> host IIO blocks | Optional staging: perform device-to-host transfer on an owned CUDA stream, wait for completion before dequeue, and retain burst until copy event completes. More copies, not claimed zero-copy. |
| Device packets -> application GPU processing | Deferred separate opt-in API with memory-domain query and explicit stream/event/release contract; never return a device-only pointer as an ordinary CPU-dereferenceable `iio_block_start()`. |
| DMA-BUF / registered application memory | Deferred; prove export/import, coherency and unregister completion. `add_memory_region_async(config, ExternalMemoryRegion, ...)` never frees external storage, which must survive deletion completion. |

DAQIRI's `MemoryKind` includes `HOST`, `HOST_PINNED`, `HUGE`, `DEVICE`.
`BurstParams` contains `cudaEvent_t event` and C++ ownership members.
GPU reorder metadata from `get_reorder_burst_info()` is valid after that event
completes. Do not assume every raw RX burst has a valid reorder event; use the
selected engine/profile's documented completion rules. Disable reorder,
quantization and sample conversion initially, preserving IIO endian, repeat,
shift, sign and channel padding semantics. Strip transport/VRT headers, not IIO
scan padding. Hardware RX timestamp is arrival time, not ADC acquisition time;
`get_packet_rx_timestamp()` requires working PTP and does not validate it.

Block state is `APP_OWNED -> QUEUED -> IN_FLIGHT -> COMPLETE -> APP_OWNED`;
error/cancel transitions wake waiters but do not free still-referenced memory.
An RX dequeue transfers completed bytes to the application; **re-enqueue**, not
dequeue, makes that block available for another receive. Bound intermediate
packet/reassembly storage separately. TX copies from the application block into
DAQIRI-owned storage; retain source while retry is possible. Define successful
TX dequeue as all bytes accepted by the local transport (not DAC playback).
Subsequent remote failure is a stream error/diagnostic, not proof that samples
played. Any stronger remote-completion guarantee needs a negotiated response.

## Proposed flow-control and wire contract

Use independent data and control UDP flows, queues and memory pools. VRT49 is
the sample envelope; a versioned session extension carries the fields absent
from legacy streams. Agree exact encoding and golden packets in the vrt49
implementation before accepting wire compatibility. Do not rely on a short
VRT packet count alone for long-running flow control.

For each direction and session epoch, negotiate fixed payload slot capacity
`P`, with only whole scans per slot. A final short packet still consumes one
slot. Carry a 64-bit packet/slot sequence, stream identity, epoch, valid payload
length, layout version, and acquisition metadata as applicable. Receiver grant
`G` is an exclusive cumulative slot limit; sender may transmit sequence `s`
only if `s < G`. Initial `G` covers reserved available slots, not theoretical
RAM. Advance `G` only across the contiguous released prefix, after storage is
reusable; hold out-of-order releases in a bounded bitmap so a ring hole cannot
be overwritten. Receiver
capacity must cover granted-but-not-yet-arrived, queued/reordered, copying and
application-held storage; no credit for storage merely completed by the NIC.
Reserve destination capacity before admitting a packet. Bound all sequence arithmetic and reset the session before counter wrap.

Use adapter-owned bounded **software reassembly/reorder** for this baseline.
Do not feed its 64-bit monotonic slot sequence directly into DAQIRI hardware
reordering. `daqiri_ibverbs_engine.cpp:2540` describes exact 32-bit programmable
samples; lines 7119-7124 require `cyclic_sequence: true` and reject wide
monotonic keys. A future hardware-placement profile needs a separate finite
cyclic placement key and a proven epoch/reuse guard, never truncation of the
application credit counter.

Repeated cumulative grant messages are idempotent; use monotonic maximum only
within the authenticated/current epoch. Never subtract a credit twice for a
retry or duplicate. Stale epochs are discarded; malformed, over-grant and
out-of-window packets are rejected and counted. A stalled application reaches
zero credit without overwrite. Sender also observes its local TX capacity;
`is_tx_burst_available()` alone is not receiver flow control. A periodic grant
refresh and a bounded status query avoid deadlock on lost credit messages.

Credits prevent overrun; they do **not** make UDP reliable. Baseline is strict
loss detection: maintain bounded reorder state, discard duplicates and fail the
stream on a missing packet after its deadline (`-EIO` with loss diagnostics).
Never return zero-filled holes as valid IIO samples. Loss recovery by sample
retransmission is a later profile with a bounded retention window. On baseline
loss end the epoch rather than granting unseen slots speculatively. Real ADCs
may not be pausable: the peer needs a sized acquisition FIFO, explicit overflow
reporting and a stop/error policy when it cannot honor backpressure. No finite
FIFO makes an indefinitely stalled consumer lossless.

Control has a separate bounded reliable command protocol: session epoch,
request ID, opcode, payload length, response status and integrity binding.
Retransmit identical requests with deadline/backoff; cache results in a bounded
deduplication window so START/STOP/configuration retries do not repeat effects.
ACK means command acceptance/result as specified, not a returned sample slot.
Bound outstanding commands, rate-limit status traffic and reserve capacity for
STOP/RESET/grant refresh. A full command queue returns busy; never accumulate
unbounded retries. If a result is ambiguous after timeout, query session state
or reset; do not blindly repeat a non-idempotent action under a new request ID.
Reserve worker service budget and NIC resources for control so saturated data
cannot starve cancellation. iiod management remains available independently;
control over UDP is an integration extension, not an existing DAQIRI RPC API.

### RoCE is a separate advanced profile

DAQIRI has an RDMA engine and `BurstTransportHeader` fields for connection ID,
RDMA opcodes, remote address/key and completion metadata. That does not make a
raw-Ethernet FPGA a RoCE endpoint. Require an independently verified RDMA peer,
QP/CM setup, receive posting, MR permissions and bounds, retry/RNR behavior,
ordering and GPU visibility tests. Local completion, transport ACK, remote
placement and remote application consumption are different events. Neither ACK
nor successful RDMA write grants permission to overwrite an application-held
slot. Retain the same application-level release/credit protocol, and revoke
remote access/drain QPs before deregistering memory. Do not expose remote keys
through normal context attributes or logs. RoCE is not a fallback for UDP.

## Configuration, security and teardown

Proposed URI grammar is `daqiri:<profile-name>`; profile names are local allowlisted
identifiers, not filesystem paths, credentials, remote YAML or arbitrary command
strings. A local versioned profile supplies management URI/TLS policy, DAQIRI
YAML path, selected interface/queues, memory domain, peer allowlist, MTU, block
and pool ceilings, command/reorder windows, polling budget and deadlines. Reject
unknown keys, incompatible GPU/engine settings, conflicting queue ownership,
insufficient locked-memory limits, and limits exceeding peer negotiation.
The direct socket TCP/UDP path in `src/common.cpp:777-815` rejects GPU memory
and external bindings. A CPU data path does not imply a CUDA-free DAQIRI build:
its top-level CMake declares CUDA and requires CUDAToolkit.

Authenticate management using the existing deployment TLS facilities and bind
the negotiated session to the authenticated peer. Raw UDP itself is neither
authenticated nor encrypted. A nonce/flow filter prevents accidental cross-talk,
not hostile injection. Initial raw deployment must be an isolated trusted
network with ingress controls; untrusted deployment requires authenticated data
and control framing or a protected tunnel, with overhead and performance
measured separately. Restrict peer addresses/ports, validate every length before
access, cap memory/CPU per session, reject replays, and never grant credits from
unauthenticated network input. Least-privilege raw-NIC permissions, locked-memory
limits and optional hugepages are installation concerns; never change sysctls,
NIC bindings or privileges from context creation. Review LGPL/Apache dependency
licensing and distribution obligations before shipping the module.

Cancellation has two phases: immediately mark cancelled, stop new admissions
and wake all libiio waiters; then quiesce asynchronously owned resources. Send
STOP over reserved control capacity, detach RX steering, drain TX with a bounded
deadline, join workers and complete GPU work, delete queues/MRs and wait for
resource completions, free blocks, then destroy owned engine/context. No
port-wide `drop_all_traffic()` on a NIC shared with unrelated traffic. A peer
that ignores STOP cannot hold local waiters indefinitely. A local device that
cannot prove DMA quiescence must remain quarantined with its allocations intact
until safe engine/device teardown; timeout is never permission to free live
DMA memory. Cancel is per buffer, not a process-global DAQIRI shutdown.

## Implementation milestones and acceptance gates

1. **Protocol and dependency contract:** pin DAQIRI build/toolchain, publish
   vrt49 capability/session/credit/control encodings and golden packets, document
   target FPGA/gateway support and threat model. Prove unsupported peers fail
   before START. No runtime backend claim from documentation alone.
2. **Opt-in CPU module:** add `daqiri.c`, private shim and CMake wiring; reuse
   management client helpers with regression tests. Check default C-only builds
   have no new dependency, load/unload exported symbol, C/C++ error boundaries,
   malformed profiles and exclusive-runtime conflicts. Management-only tests
   must not be mistaken for streaming support.
3. **Reference peer and CPU streaming:** implement RX/TX native blocks and strict
   parser against a deterministic software peer. Compare channel layout and
   payload byte-for-byte with ordinary libiio; cover multi-device/buffer indices,
   simultaneous directions, odd sizes, overflow, masks and full pool pressure.
   Run ASan/UBSan and race tests over cancel, timeout, double enqueue and teardown.
4. **Credit and command fault injection:** prove no overwrite when application
   holds every block; duplicate/reorder/drop grants and commands, restart peer,
   inject stale epochs and malformed lengths, drop a data packet, lose STOP ACK,
   exhaust control queue, and saturate data while measuring bounded control
   latency. Verify memory usage stays within configured ceilings throughout.
5. **Raw-ibverbs hardware qualification:** measure throughput, loss, CPU load,
   copy cost and latency distribution with pinned NIC/firmware/driver, NUMA,
   MTU, link rate, channel stride and queue sizes. Record single/bidirectional
   runs, slow consumer and sustained soak results. Test consumed-on-error TX
   ownership and drain failures. Set product performance thresholds before
   claiming high-speed operation; this document reports no benchmark results.
6. **Optional GPU then RoCE:** qualify GPU-to-host staging first, then propose
   explicit GPU application ownership API separately. Test delayed CUDA kernels,
   event lifetime and cancel under GPU load. Qualify RoCE only with an actual
   compatible peer and application-release tests; do not gate baseline on it.

Documentation acceptance: render this MyST page, verify its index entry and
pinned source links, and run the repository's documented HTML/link checks.
Hardware, runtime and interoperability gates remain future implementation work.
