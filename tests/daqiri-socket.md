# DAQIRI core socket verification

Run with a real DAQIRI installation (including its socket plugin):

```sh
DAQIRI_PREFIX=/path/to/prefix CUDA_HOME=/usr/local/cuda tests/run-daqiri-socket.sh
```

The runner compiles both production C++ files against real installed headers and
libraries, with ASan/UBSan by default. The dependency itself is not rebuilt with
sanitizers. Four loopback UDP ports (25101, 25102, 25111, 25112) must be free.
CPU 0 must be allowed by the host affinity policy, or adjust the test YAML.
Do not run multiple instances concurrently.

The stream uses a test-only adapter around the real DAQIRI socket API. Its peer
uses independent POSIX UDP sockets and independent DQI1 encoding, not the
production wire codec. No DAQIRI API is mocked. Seventeen scenarios cover:

* Full RX blocks including a short final packet; byte-exact first block.
* Cumulative RX credit remains unchanged while application owns storage, then
  increases on re-enqueue; a second complete block is received.
* Full byte-exact TX; no transmission before peer credits arrive.
* Invalid ownership transitions, max-block capacity, scan alignment and cyclic rejection.
* Wrong-size data frame fails closed.
* Data deadline, incorrect HELLO echo, and missing HELLO response.
* Cancellation wakes a blocking dequeue within 100 ms, including missing STOP ACK.
* Stream handle closes before the surviving block is freed; worker is joined.
* RX extents 8, 24, 32 and 40 bytes with the same 40-byte ceiling; the peer derives
  every RX packet length from HELLO payload capacity and CONFIG actual extent,
  not hard-coded packet positions. A two-block queue also exercises short tails.
* Incompatible RX block allocations and partial enqueue extents fail with EINVAL
  before and after START. Wrong or missing CONFIG echoes prevent START.
* Peer accepts START but suppresses its ACK: timeout and cancellation both send
  STOP. Missing STOP ACK and malformed START ACK preserve the original error.
  Each failed enable, including STOP cleanup, is asserted to finish within 1 s
  using a 300 ms command timeout; surviving blocks are freed under leak detection.

Verified on picard.local with the existing installation at
`/tmp/libiio-daqiri-verification/prefix`: the expanded suite is run with
ASan/UBSan, leak detection and `-Wall -Wextra -Werror`. Installed DAQIRI headers
must be marked system headers for Werror: they contain upstream switch and
unused-parameter warnings. Add
`-isystem /tmp/libiio-daqiri-verification/prefix/include` to
`DAQIRI_TEST_CXXFLAGS` alongside the sanitizer and warning flags.
All 17 scenarios passed with exit 0 and no sanitizer/leak reports. The copied
source snapshot and output are retained at
`/tmp/libiio-daqiri-verification/spec-blockers.9tKN9C/result.log` on picard.local.

## Experimental fixed RX extent contract

The first successful RX block allocation fixes the actual extent for the epoch;
all subsequent RX allocations and enqueue sizes must equal it. Partial RX
enqueues are deliberately unsupported. TX sizes remain variable. HELLO's
`max_block` remains only an allocation ceiling, not the actual extent.

After HELLO and before START, CONFIG (opcode 8, request ID 4, header value zero)
carries a four-byte big-endian actual RX extent, or zero for TX. Its ACK must
echo the exact body and opcode. START is never sent if CONFIG fails. This is a
mandatory refinement of the experimental DQI1 contract, not compatibility with
an older peer that only understands HELLO/START. CONFIG must not arm acquisition.
The peer computes `slots = ceil(extent / payload)` and the length for sequence
`s` as `min(payload, extent - (s % slots) * payload)`. Slot credit remains
cumulative and is granted only against enqueued storage.

Every failed START attempt is treated as potentially accepted by the peer:
send bounded, cancellation-independent STOP for the same epoch, then return the
original START error. STOP failure cannot establish remote quiescence; the local
epoch remains failed and cannot restart. A peer must deduplicate commands and
retain a terminal STOP epoch tombstone so delayed START retransmissions cannot
re-arm it.

## Boundaries and remaining work

This is **not raw-ibverbs hardware qualification**, throughput testing, or a full
backend/iiod integration test. Production raw packet parsing, flow steering,
queue-full consumption, foreign process-global ownership, initialization failure,
DMA drain/quarantine, reordering/duplicate/stale-epoch fault matrices and concurrent
free versus application calls remain separate gates. DAQIRI reports an existing
foreign engine as INTERNAL_ERROR, not RESOURCE_IN_USE; the shim fails without
shutting that engine down but currently maps that result to EIO. Direct DAQIRI
use concurrent with this backend remains forbidden. The runtime must outlive
streams; the backend must close/join streams before runtime release.

The repository design and `DAQIRI-IMPLEMENTATION-PLAN.md` were read. The plan's
software-peer validation gate is exercised here; hardware gates remain separate.
