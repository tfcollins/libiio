# Metadata starvation bound and NIC-free adapter evidence

## Enforced contract

The raw adapter rejects YAML unless `rx_meta_buffers >= 4099` and
`tx_meta_buffers >= 8194` (both at most 16384). Zero/default counts are rejected.
These are derived worst-case occupancy bounds, not empirical tuning. Four
separate packet MRs alone do **not** reserve metadata. No upstream patch or new
DAQIRI API is required. This proof is tied to NVIDIA DAQIRI commit
`993363da36aa320b43f4160aa848e8ddd2373a36`; re-audit before changing that dependency.
CMake does not automatically enforce the dependency commit.

Prerequisites enforced by `valid_config`: one interface, exactly two INDIRECT
RX and TX queues, no reorder, no shared packet MRs. The runtime owns DAQIRI
exclusively; unrelated direct DAQIRI users and runtime queue creation are
unsupported. Adapter mutexes serialize TX allocations through submission and
RX dequeues through release; shutdown requires joined stream users, as before.

### Source-grounded capacity calculation

Paths and line numbers below refer to the pinned upstream checkout.
`src/daqiri_ring.h:127-144,221-223`: requested power-of-two count N yields
N-1 usable entries. `src/daqiri_pool.h:44-68` allocates exactly n objects.
`src/engines/ibverbs/daqiri_ibverbs_engine.cpp:4012-4022` passes configured
metadata count **minus one** to ObjectPool for both globally shared pools.

* RX ring creation at 3939-3945 requests 2048, hence **2047** descriptors per
  queue. Each worker owns at most one `cur_burst` (4180-4217, 4237-4249,
  4277-4283, 4392). A flush transfers ownership to the ring or returns the
  descriptor on ring-full; it does not retain a second descriptor. The adapter
  retains at most one dequeued burst per lane, including partial batches.
  Thus total occupancy is bounded by `2 * (2047 ring + 1 worker + 1 adapter)
  = 4098`. Configured count must be at least **4099**. A worker needing a new
  descriptor necessarily owns no current descriptor, so total occupancy is at
  most 4097 and a descriptor is available, even with every data holding full.
  If all 4098 objects are occupied, both workers already own their current
  descriptors; queued and adapter-held control bursts remain consumable.
* TX handoff rings at 8075-8082 request 4096, hence **4095** descriptors each.
  Each worker retains one dequeued pending descriptor while its SQ is full
  (8296-8337). Posting returns metadata immediately (8325-8326): hardware
  in-flight packet slots do not retain metadata. Each adapter send holds at
  most one additional descriptor globally under `tx_lock`. Thus the bound is
  `2 * (4095 ring + 1 worker) + 1 adapter = 8193`; configured count must be
  at least **8194**. Before any serialized allocation, engine occupancy is
  at most 8192, so one descriptor is available regardless of stalled data SQ.
  Queue-full send returns metadata and rolls back slots (8932-8941); allocation
  failures use the adapter guard. Packet slots are separately bounded by the
  single MR's `num_bufs` (8039-8053, 8405-8422), at most 4096 here. We deliberately
  use the ring/worker bound rather than depend on completion timing to reduce
  the metadata bound.

This proves **no data-induced metadata starvation**, not guaranteed delivery
when the control lane itself is full, a NIC is broken, or a worker is never
scheduled. It does not require priority polling or data queue progress.

## Executable evidence

```sh
DAQIRI_PREFIX=/path/to/installed/daqiri \
DAQIRI_SOURCE=/path/to/pinned/nvidia-daqiri \
sh tests/run-daqiri-metadata.sh
```

The test includes the actual production `daqiri-transport.cpp` and invokes its
`valid_config`, `send_packet` and `recv_packet`. A private compile-time
`DQ_PACKET_API` seam replaces only packet operations, using the pinned actual
ObjectPool and Ring implementations. This is **allocation/ownership seam
evidence**, not execution of the ibverbs worker or real NIC hardware. Normal
builds bind the seam to the actual DAQIRI namespace; there is no runtime socket
fallback. The separate socket suite compiles the unmodified production binding
against real installed headers/libraries.

Tests reproduce control TX starvation with the old 4095-object shared pool;
reject zero, undersized and one-below-bound metadata configs; reject socket
engine; saturate data TX with a retained worker descriptor; keep control TX
allocatable with both rings/workers full; exercise allocation-fault EAGAIN and
recovery; fill every RX holding (both rings, both workers, both adapter bursts)
to true total metadata exhaustion and continue control consumption and worker
reallocation without releasing the data lane. Pool in-use counts finish zero.

Verified with real installed headers/libraries on picard.local, prefix
`/tmp/libiio-daqiri-verification/prefix`, ASan/UBSan, leak detection and
`-Wall -Wextra -Werror`. Snapshot and socket log:
`/tmp/libiio-daqiri-verification/metadata.3paJ4N/`.
The full 17-scenario real socket-engine suite also passed there. Hardware
steering, CQ/SQ execution, faulted DMA teardown and throughput remain unqualified.
