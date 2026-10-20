# DAQIRI backend

The `daqiri:` backend receives sample streams over Ethernet at rates the TCP
path of `iiod` cannot absorb, using NVIDIA
[DAQIRI](https://github.com/NVIDIA/daqiri) for the packet I/O. Applications do
not change: any program that opens a libiio context (C, the Python bindings,
`pyadi-iio`, the command line utilities) can use a `daqiri:` URI instead of
`ip:`.

## Status

This is the first phase of the backend.

- The management plane (context description, attributes, triggers) is a plain
  TCP connection to `iiod`, like `ip:`.
- The data plane has two framings: VITA 49.2 IF-Data packets over UDP
  (`framing=vrt`), and the stock `iiod` binary protocol (`framing=iiod`).
- Only the DAQIRI **socket engine** (Linux UDP sockets, any NIC) is supported.
  The DPDK engine, the raw ibverbs engine and GPU memory (`mem=device`) are
  later phases.
- Receive only.

The backend is a separate module, `libiio-daqiri.so`. `libiio.so` itself gains
no C++, CUDA or DAQIRI dependency.

## Building

DAQIRI has a hard CUDA build dependency. `CI/daqiri/README.md` explains how to
build and install DAQIRI itself (`CI/daqiri/build-daqiri.sh`). Then configure
libiio:

```
cmake -S . -B build -DWITH_MODULES=ON -DWITH_DAQIRI_BACKEND=ON \
    -DCMAKE_PREFIX_PATH=$HOME/opt/daqiri \
    -DCUDAToolkit_ROOT=/usr/local/cuda-13.4
cmake --build build -j
```

| Option | Meaning |
|---|---|
| `WITH_DAQIRI_BACKEND` | Build `libiio-daqiri.so`. Off by default. Linux only; configuration stops unless `BUILD_SHARED_LIBS` and `WITH_MODULES` are on. |
| `WITH_MODULES` | Load backends as modules (`libiio-*.so`). Required by the above. |
| `CMAKE_PREFIX_PATH` | Where `find_package(daqiri)` finds the DAQIRI install prefix. |
| `CUDAToolkit_ROOT` | Where `find_package(CUDAToolkit)` finds the CUDA toolkit. The CUDA language itself is not enabled, so no `CMAKE_CUDA_COMPILER` is needed. |
| `IIO_MODULES_DIR_OVERRIDE` | Advanced. Replaces the directory the library looks in for modules (default `<libdir>/libiio/`). Test builds in the build tree set it to the build directory, where all modules are written; leave it unset for builds that are installed. |

At run time `libdaqiri.so` and the CUDA runtime must be resolvable. The build
tree RUNPATH covers them; an installed module needs `LD_LIBRARY_PATH` (or an
`ldconfig` entry) that includes the DAQIRI `lib` directory, unless DAQIRI is in
a system prefix.

## URI

```
daqiri:<host>[:port][,key=value]*
```

`<host>` is the address of the board running `iiod`; the optional `port` is
its management port (default 30431). The options follow, separated by commas.
Unknown keys, and values that do not parse, make context creation fail with
`-EINVAL`.

| Key | Values | Default | Meaning |
|---|---|---|---|
| `yaml` | path | `$IIO_DAQIRI_YAML` | DAQIRI configuration. Required by `framing=vrt`. |
| `framing` | `vrt`, `iiod` | `vrt` | Data plane, see below. |
| `mem` | `host`, `pinned`, `device` | `host` | Memory of the blocks. `pinned` is `cudaHostAlloc` memory. `device` is not supported in this phase. |
| `gpu` | CUDA ordinal | `0` | GPU used for `mem=pinned` and `mem=device`. |
| `iface` | name or address | the first interface of the YAML | Which `interfaces:` entry of the YAML to receive on. |
| `rxq` | queue id | `0` | Receive queue of that interface. |
| `lossy` | `0`, `1` | `0` | Lost packets: `0` fails the block (`-EIO`), `1` zero-fills the hole and goes on (up to 1024 lost packets; gaps of unknown size and host overruns are flagged, not filled; see `framing=vrt` below). Readable and writable at run time as the `daqiri_lossy` buffer attribute. |
| `sid` | 32-bit | `0` | Stream ID to accept, compared under `sid_mask`. Ignored when `sid_mask` is `0`. |
| `sid_mask` | 32-bit | `0` | With `0`, the Stream ID of the first IF-Data packet is locked and packets of other Streams are counted as foreign. Otherwise a packet needs a Stream ID with `(stream_id & sid_mask) == (sid & sid_mask)`. |
| `swap` | `0`, `1` | `1` | Convert the big-endian wire samples to the little-endian format the XML describes. `0` leaves them as they are. |
| `gap_ts` | `0`, `1` | `1` | Use the VITA 49 fractional timestamp (a sample count that advances by the samples of each packet). `1`: the timestamp classifies packets (one that steps back a little is late, one that repeats the last timestamp and count is a duplicate; both are dropped and counted, not taken as losses) and sizes gaps, also past the 16-packet wrap of the 4-bit packet count. `0`: timestamps are not used at all, for a framer whose timestamp does not advance by exactly the samples per packet, or that sends none (with `gap_ts=1` such a framer makes every gap one of unknown size, which `lossy=1` flags but does not fill). Only the 4-bit packet count is used: a loss of 16 packets (or a multiple) is not seen, a repeated packet counts as a loss of 15 and a reordered one as a loss, nothing is counted late or duplicate, and with `lossy=1` those false losses are zero-filled. |
| `data_port` | port | the management port | TCP port of the data connection for `framing=iiod`. |
| `poll` | microseconds | `200` | Idle wait of the receive thread when the queue is empty, see below. |

`IIO_DAQIRI_YAML` is the fallback for `yaml`; a `yaml=` in the URI wins.

`poll` trades CPU for idle latency. The receive thread spins briefly, then
waits up to `poll` microseconds before looking at the queue again. With
`ppoll` the idle wait at 50 us costs about 13% of a core; 200 us keeps the
worst-case idle latency well below the engine's 64-packet queue at 1G rates.
Use a smaller value at higher packet rates. `poll=0` never sleeps.

Example:

```
iio_attr -u "daqiri:192.168.2.1,framing=vrt,yaml=/etc/daqiri/1g.yaml" -b cf-ad9361-lpc
```

## Framings

### `framing=vrt`

The board (or a framer in its FPGA) sends VITA 49.2 IF-Data packets over UDP to
the host NIC, and DAQIRI delivers them to a receive thread of the backend, one
thread per open buffer. For each packet the backend checks the headers (when
the engine hands over raw frames), parses the VITA prologue, keeps IF-Data and
drops Context packets (counted), filters on the Stream ID, converts the payload
from big-endian to the format of the channels, and copies it into the blocks
you queued. Losses are found from the 4-bit packet count of the prologue; with
`gap_ts=1` the fractional timestamp (a sample count) tells how many packets
were lost even when the count wrapped, and tells late and duplicate packets
from losses.

One Stream ID carries one channel group, so one buffer maps to one
`(interface, queue)` and the enabled channel mask **must match the stream
layout** (for example the I/Q pair of one converter). All channels must have
the same width, 16 or 32 bits; anything else is refused with `-ENOTSUP`.
Blocks must be filled completely: a dequeued RX block always holds the full
block size.

Block errors are returned by `iio_block_dequeue()`:

- `-EIO`: data was lost before the data that follows (a gap with `lossy=0`, a
  gap too large to zero-fill, or a host overrun, see below). The stream
  resynchronises: the packet after the loss starts the next block.
- `-EPROTO`: a packet whose packet class or payload size does not fit the
  sample size of the enabled channels, i.e. the channel mask does not match
  the stream layout. The stream stops: the current block, the queued ones and
  every block queued afterwards fail with `-EPROTO` until the buffer is
  disabled. Malformed datagrams (not a valid VITA 49 packet, for example
  truncated) are not block errors: `daqiri_rx_malformed` counts them and they
  are skipped.
- `-ECANCELED` after a cancel, and `-ETIMEDOUT` when nothing arrived in the
  context timeout (the block stays queued).
- Other negative error codes: a packet larger than a block can take
  (`-ENOBUFS`), or a failure of the engine or of memory.

Losses: with `lossy=0` a gap fails one block with `-EIO`, however many
packets it spans. With `lossy=1` a gap of known size is zero-filled, one
packet's worth of zeros per lost packet, so the samples that follow stay at
their place in the stream (the zeros may span several blocks). The zero-fill
is capped at 1024 lost packets (`DQ_VRT_MAX_FILL_PACKETS`): a larger sized
gap is a resynchronisation instead and fails a block with `-EIO` even with
`lossy=1`. A gap of unknown size cannot be filled: with `lossy=1` its block is
flagged in `daqiri_blocks_with_gaps`, not zero-filled, and the samples after
it are not kept at their place in the stream. With `gap_ts=1` every gap is of
unknown size when the packets carry no sample-count timestamp (the 4-bit count
may hide a multiple of 16), and so are timestamps that disagree with the
packet count and discontinuities such as a sender restart. For a framer
without a sample-count timestamp, use `gap_ts=0` to have `lossy=1` zero-fill
the gaps the 4-bit packet count sizes.

If no block is queued when a packet arrives the packet is dropped and
`daqiri_rx_dropped_no_block` counts it: keep enough blocks queued for your
packet rate.

Such a host overrun is a gap of unknown size in the data you receive, and is
reported like one: the first block that a packet finds after the drops fails
with `-EIO` (`lossy=0`) and the packet starts the next block, or, with
`lossy=1`, the block is counted in `daqiri_blocks_with_gaps` but not
zero-filled (the backend does not know how many samples a block would have
held), so overrun gaps are flagged, not filled. With `lossy=0` a block that
dequeues successfully therefore never hides an overrun before its data; with
`lossy=1` only `daqiri_blocks_with_gaps` tells of it. Each overrun
counts once in `daqiri_rx_overrun_events`, however many packets it dropped.
Drops after the `-EIO` block, before any packet reached a block again, are
part of the same overrun and fail no further block, so an application that
queues one block at a time during an overrun still receives data.

### `framing=iiod`

The data plane is the stock `iiod` binary protocol on a second TCP connection,
so unchanged boards running `iiod` work, and DAQIRI is not used. It gives one
URI family and the same benchmark tool as `framing=vrt`; the `daqiri_*` buffer
attributes below exist for `framing=vrt` only. Boards whose `iiod` only speaks the legacy ASCII protocol
are served by the library's fallback, without native blocks.

## Counters

Every input buffer of a `daqiri:` context with `framing=vrt` has these
buffer attributes (read them with `iio_buffer_find_attr()`, or the
`iio_attr` utility). Counters count from the creation of the buffer, and
packets that arrived before the stream was started are dropped, not counted.

| Attribute | Meaning |
|---|---|
| `daqiri_rx_packets` | IF-Data packets delivered to the assembler. |
| `daqiri_rx_bytes` | Payload bytes of those packets. |
| `daqiri_rx_lost_packets` | Packets found missing from the packet count / timestamp (a late packet that shows up afterwards is taken off again). |
| `daqiri_rx_gap_events` | Number of gaps (one gap may span several packets). |
| `daqiri_rx_dropped_no_block` | Packets dropped because no block was queued. |
| `daqiri_rx_overrun_events` | Host overruns: times packets started being dropped for lack of a queued block after a packet had been delivered (one per episode, not per packet). Each one fails (`lossy=0`) or flags (`lossy=1`) the next block. |
| `daqiri_rx_foreign_packets` | Packets of another Stream ID. |
| `daqiri_rx_context_packets` | Context packets, dropped. |
| `daqiri_rx_malformed` | Datagrams that are not valid VITA 49 packets (for example truncated), skipped. |
| `daqiri_rx_late_packets` | Packets that arrived after a later one (reordered), dropped. |
| `daqiri_rx_duplicate_packets` | Repeated packets, dropped. |
| `daqiri_rx_zero_filled_bytes` | Bytes zero-filled for lost packets (`lossy=1`). |
| `daqiri_blocks_completed` | Blocks filled. |
| `daqiri_blocks_failed` | Blocks completed with an error (`-EIO`, `-EPROTO`, ...); cancelled blocks are not counted. |
| `daqiri_blocks_with_gaps` | Blocks that report a loss: every block failed with `-EIO` for a gap, a resynchronisation or a host overrun, and, with `lossy=1`, every block flagged for a gap of unknown size or an overrun, or holding zero-fill. |
| `daqiri_stream_id` | Stream ID in use (hex). |
| `daqiri_last_error` | Last error, as a negative errno (0 if none). |
| `daqiri_lossy` | The `lossy` setting. Read/write. |
| `daqiri_mem_domain` | `host`, `pinned` or `device`. |
| `daqiri_engine_kind` | DAQIRI engine in use, for example `socket`. |

The context also has the static attributes `uri`, `ip,ip-addr`, `daqiri,yaml`,
`daqiri,framing`, `daqiri,mem`, `daqiri,engine` and `daqiri,version`.

## Measuring

`iio_daqiri_bench` queues blocks, receives for a given time and prints the
rate, dequeue latency percentiles, CPU use and the counters above (as the
difference over the run). It only uses the public API, so it runs against `ip:`
as well:

```
iio_daqiri_bench -u "daqiri:192.168.2.1,framing=vrt,yaml=1g.yaml" \
    -d cf-ad9361-lpc -c voltage0,voltage1 -s 1440 -n 64 -T 10 \
    --check ramp -j perf.json
iio_daqiri_bench -u ip:192.168.2.1 -d cf-ad9361-lpc -s 65536 -T 10
```

See `man iio_daqiri_bench`. At tens of thousands of blocks per second the
default of 8 blocks is only a fraction of a millisecond of buffering, and a
scheduling hiccup of the application shows up as `daqiri_rx_dropped_no_block`:
raise `-n`.

## Limitations

- **One YAML per process.** DAQIRI initialises once; a second `daqiri:`
  context in the same process with a different YAML fails with `-EBUSY` when a
  buffer is opened. Contexts that use the same YAML share the engine.
- **The engine lives as long as the process.** It is never shut down before
  exit, and the module pins itself in memory, because the DAQIRI engines do
  not support a shutdown/init cycle in one process. Buffers and their threads
  are still released when you close them.
- **Socket engine only.** DPDK, raw ibverbs (ConnectX) and GPU memory
  (`mem=device`) come in later phases; the socket engine is a functional and
  loss-accounting tier, not a 100G path.
- **VRT is receive only.** There is no VRT transmit and no cyclic buffer
  (`-ENOTSUP`). Only IF-Data packets carry samples; Context packets are
  counted, not interpreted.
- **One buffer per `(interface, queue)`**; opening a second one on the same
  queue fails with `-EBUSY`. A queue carries one Stream ID.
- **The channel mask must match the stream layout**, all channels 16 bit or
  all 32 bit, otherwise `-ENOTSUP`. A mask that does not match the sender gives
  wrong data or `-EPROTO`.
- **Gap detection is limited by the 4-bit packet count.** It wraps every 16
  packets, so a gap of a multiple of 16 packets cannot be seen from the count
  alone. With `gap_ts=1` and a sender that puts a sample-count timestamp in
  every packet, the timestamp sizes the gap and removes that ambiguity.
  Without comparable timestamps (or with `gap_ts=0`) a loss of 16 packets or
  a multiple of it is not detected, and a reordered packet looks like a loss.
  Late packets are dropped (and counted), not put back in order.
- Linux only.

## Security

`framing=vrt` receives raw UDP: there is no authentication, no encryption and
no session. Anyone who can send datagrams to the receive port (and, with the
socket engine, from the configured source address) can inject samples. Use it
on an isolated, trusted network or a direct link between the board and the
host. The management connection, and `framing=iiod`, are as unauthenticated as
`ip:`.

## License

The `daqiri:` module is LGPL-2.1-or-later, like the rest of libiio. It links
the Apache-2.0 licensed DAQIRI library and the NVIDIA CUDA runtime, but only as
a separate shared object (`libiio-daqiri.so`) that is loaded on demand:
`libiio.so` does not link either of them. If you redistribute a build with the
module, the DAQIRI and CUDA licenses apply to those libraries as well.
