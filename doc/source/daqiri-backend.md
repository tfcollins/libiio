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
- The data plane has two paths: UDP sample mode, in which the server sends the
  samples of an enabled buffer as UDP datagrams that DAQIRI receives, and the
  stock `iiod` binary protocol over TCP. The URI key `data=` and what the
  server advertises pick one when a buffer is opened.
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
| `data` | `auto`, `udp`, `tcp` | `auto` | Data path of the buffers, see [Data paths](#daqiri-data-paths). `auto`: UDP when the server advertises UDP sample mode and a DAQIRI configuration is set, otherwise TCP. `udp`: UDP or the buffer does not open. `tcp`: never UDP. |
| `yaml` | path | `$IIO_DAQIRI_YAML` | DAQIRI configuration. Needed for UDP sample mode only. |
| `udp_dst` | IPv4 address | the YAML's, else the control connection's local address | Where the server sends the datagrams. |
| `mem` | `host`, `pinned`, `device` | `host` | Memory of the blocks. `pinned` is `cudaHostAlloc` memory. `device` is not supported in this phase. |
| `gpu` | CUDA ordinal | `0` | GPU used for `mem=pinned` and `mem=device`. |
| `iface` | name or address | the first interface of the YAML | Which `interfaces:` entry of the YAML to receive on. |
| `rxq` | queue id | `0` | Receive queue of that interface. |
| `lossy` | `0`, `1` | `0` | Lost datagrams in UDP sample mode: `0` fails the block (`-EIO`), `1` zero-fills exactly the lost payloads and goes on (up to 1024 lost datagrams; host overruns are flagged, not filled; see [UDP sample mode](#daqiri-udp-sample-mode)). Readable and writable at run time as the `daqiri_lossy` buffer attribute. |
| `data_port` | port | the management port | TCP port of each buffer's own `iiod` connection, on both data paths. |
| `poll` | microseconds | `200` | Idle wait of the receive thread when the queue is empty, see below. |

`IIO_DAQIRI_YAML` is the fallback for `yaml`; a `yaml=` in the URI wins.

`poll` trades CPU for idle latency. The receive thread spins briefly, then
waits up to `poll` microseconds before looking at the queue again. With
`ppoll` the idle wait at 50 us costs about 13% of a core; 200 us keeps the
worst-case idle latency well below the engine's 64-packet queue at 1G rates.
Use a smaller value at higher packet rates. `poll=0` never sleeps.

Example:

```
iio_attr -u "daqiri:192.168.2.1,data=udp,yaml=/etc/daqiri/1g.yaml" -b cf-ad9361-lpc
```

(daqiri-data-paths)=

## Data paths

When a buffer is opened, `data=` and what the server advertises decide its
data path. A server offers UDP sample mode with the context attribute
`iiod,udp-stream` (value `1`) on a binary-protocol connection; the backend
only sends the setup command to such a server, because a stock `iiod` drops
the connection on a command it does not know.

| `data=` | Server advertises UDP sample mode | Server does not |
|---|---|---|
| `auto` | UDP for input buffers, if a DAQIRI configuration is set; otherwise TCP | TCP |
| `udp` | UDP | The buffer open fails with `-ENOTSUP`; the context stays usable |
| `tcp` | TCP | TCP |

Every buffer has an `iiod` connection of its own, as with `ip:`, and opens
the buffer on it. A UDP buffer then sends a probing setup right away:

- the server refuses it (`-ENOTSUP`, `-EINVAL` or `-EBUSY`): with
  `data=auto` the buffer streams over TCP on that connection, and its
  `daqiri_data_path` reads `tcp`; with `data=udp` the open fails with
  `-ENOTSUP`, and the context stays usable;
- any other error fails the open: a transport failure, a payload size that is
  not a whole number of samples of the enabled channels (`-EPROTO`), or, with
  the socket engine, a source other than the YAML's `remote_addr`
  (`-EADDRNOTAVAIL`).

The probe's stream is never used. Each enable sends a fresh setup (a new
stream ID, the sequence from 0), applies the same checks, then
`ENABLE_BUFFER`; a refusal at that point fails the enable with the server's
error and never falls back to TCP, since the data path was settled at open.
After any error between a successful setup and a successful enable, the
backend sends `DISABLE_BUFFER`, which drops the pending setup, before it
returns the error. Disabling sends `DISABLE_BUFFER`, then keeps receiving for
20 ms, since the board completes the datagram in progress before it answers.
If `DISABLE_BUFFER` fails (a board answers `-ETIMEDOUT` when its sender did not
drain), it is sent once more 100 ms later; if that fails too the buffer is
closed (`CLOSE_BUFFER`), the error is returned and the queued blocks fail
with it: the board may still be streaming.

(daqiri-udp-sample-mode)=

### UDP sample mode

The server sends the samples of an enabled receive buffer as UDP datagrams to
the host, and DAQIRI delivers them to a receive thread of the backend, one
thread per open buffer. Each datagram is a 16-byte header (version, flags,
payload length, stream ID, 64-bit sequence number) followed by the samples,
in the device's own format and layout for the enabled channels, exactly as
`iiod` sends them over TCP: the payload is copied into the blocks you queued
unchanged, never byte-swapped, so any channel mask the server accepts works.
The setup asks for the destination (`udp_dst=`, else the address of the
YAML's interface, else the local address of the control connection), the UDP
port of the queue (the socket engine's `local_addr`; on raw engines, the
`udp_dst` of the flow that steers to the queue), and the largest payload the
queue's buffers and the interface MTU take. The server answers the stream ID,
the payload size of every datagram and the source.

For each datagram the backend checks the headers when the engine hands over
raw frames, then the datagram header: another version or length is
malformed, another stream ID (or, on raw engines, another source) is foreign;
both are dropped and counted. The sequence number then tells exactly how many
datagrams were lost, also late (reordered) and duplicate ones, which are
dropped and counted, a late one taking back the loss it was counted as.

One buffer has one stream and one `(interface, queue)`. Blocks must be filled
completely: a dequeued RX block always holds the full block size, which must
be a whole number of samples.

Block errors are returned by `iio_block_dequeue()`:

- `-EIO`: data was lost before the data that follows (a gap with `lossy=0`, a
  gap too large to zero-fill, or a host overrun, see below). The stream
  resynchronises: the datagram after the loss starts the next block.
- `-ECANCELED` after a cancel, and `-ETIMEDOUT` when nothing arrived in the
  context timeout (the block stays queued).
- Other negative error codes: a payload larger than a block can take
  (`-ENOBUFS`), a failure of the engine or of memory, or the error of a
  `DISABLE_BUFFER` that failed twice.

A payload size that is not a whole number of samples is not a block error:
the open or the enable fails with `-EPROTO`.

Losses: with `lossy=0` a gap fails one block with `-EIO`, however many
datagrams it spans. With `lossy=1` a gap is zero-filled, one payload's worth
of zeros per lost datagram, so the samples that follow stay at their place in
the stream (the zeros may span several blocks). The zero-fill is capped at
1024 lost datagrams: a larger gap is a resynchronisation instead and fails a
block with `-EIO` even with `lossy=1`; when no block is queued as it happens,
the first block that a later datagram finds fails.

If no block is queued when a datagram arrives the datagram is dropped and
`daqiri_rx_dropped_no_block` counts it: keep enough blocks queued for your
datagram rate.

Such a host overrun is a gap of unknown size in the data you receive, and is
reported like one: the first block that a datagram finds after the drops
fails with `-EIO` (`lossy=0`) and the datagram starts the next block, or, with
`lossy=1`, the block is counted in `daqiri_blocks_with_gaps` but not
zero-filled (the backend does not know how many samples a block would have
held), so overrun gaps are flagged, not filled. With `lossy=0` a block that
dequeues successfully therefore never hides an overrun before its data; with
`lossy=1` only `daqiri_blocks_with_gaps` tells of it. Each overrun counts once
in `daqiri_rx_overrun_events`, however many datagrams it dropped. Drops after
the `-EIO` block, before any datagram reached a block again, are part of the
same overrun and fail no further block, so an application that queues one
block at a time during an overrun still receives data.

### TCP

The data plane is the stock `iiod` binary protocol on the buffer's own TCP
connection, so unchanged boards running `iiod` work, and DAQIRI is not used.
It gives one URI family and the same benchmark tool as UDP sample mode.
Boards whose `iiod` only speaks the legacy ASCII protocol are served by the
library's fallback, without native blocks.

## Counters

Every input buffer of a `daqiri:` context has these buffer attributes (read
them with `iio_buffer_find_attr()`, or the `iio_attr` utility). A buffer that
streams over TCP, or one not open, answers `daqiri_data_path` and the
defaults of the others (zeros, and the options it would start with).
Counters count from the opening of the buffer, and datagrams that arrived
before the stream was started are dropped, not counted.

| Attribute | Meaning |
|---|---|
| `daqiri_rx_packets` | Datagrams of the stream delivered to the assembler. |
| `daqiri_rx_bytes` | Payload bytes of those datagrams. |
| `daqiri_rx_lost_packets` | Datagrams missing from the sequence (a late one that shows up afterwards is taken off again). |
| `daqiri_rx_gap_events` | Number of gaps (one gap may span several datagrams). |
| `daqiri_rx_dropped_no_block` | Datagrams dropped because no block was queued. |
| `daqiri_rx_overrun_events` | Host overruns: times datagrams started being dropped for lack of a queued block after one had been delivered (one per episode, not per datagram). Each one fails (`lossy=0`) or flags (`lossy=1`) the next block. |
| `daqiri_rx_foreign_packets` | Datagrams of another stream ID, or on raw engines from another source than the setup answered, dropped. |
| `daqiri_rx_malformed` | Datagrams of another version, shorter than their header, or whose payload length or size is not the stream's payload size, dropped. |
| `daqiri_rx_late_packets` | Datagrams that arrived after a later one (reordered), dropped. |
| `daqiri_rx_duplicate_packets` | Datagrams that arrived before, dropped. |
| `daqiri_rx_l2_rejected` | Raw engines: frames that are not Ethernet (at most one VLAN tag) / IPv4 without options or fragments / UDP with consistent lengths, dropped before the datagram header is looked at. |
| `daqiri_rx_l2_last_reason` | Why the last of those was rejected (`truncated`, `not-ipv4`, `qinq`, `bad-ihl`, `not-udp`, `fragment`, `bad-ip-len`, `bad-udp-len`); `none` while nothing was. |
| `daqiri_rx_vlan_packets` | Raw engines: accepted frames that carried a VLAN tag. |
| `daqiri_rx_hds_packets` | Raw engines: frames that passed the header checks and came in two segments (header-data split). A frame whose first segment does not hold all the headers and the datagram header counts in `daqiri_rx_malformed`, and here as well when it holds the Ethernet/IPv4/UDP headers. |
| `daqiri_rx_zero_filled_bytes` | Bytes zero-filled for lost datagrams (`lossy=1`). |
| `daqiri_blocks_completed` | Blocks filled. |
| `daqiri_blocks_failed` | Blocks completed with an error (`-EIO`, ...); cancelled blocks are not counted. |
| `daqiri_blocks_with_gaps` | Blocks that report a loss: every block failed with `-EIO` for a gap, a resynchronisation or a host overrun, and, with `lossy=1`, every block flagged for an overrun, or holding zero-fill. |
| `daqiri_stream_id` | Stream ID from the setup of the last enable (hex); `0` before the first. |
| `daqiri_last_error` | Last error, as a negative errno (0 if none). |
| `daqiri_lossy` | The `lossy` setting. Read/write. |
| `daqiri_mem_domain` | `host`, `pinned` or `device`. |
| `daqiri_engine_kind` | DAQIRI engine in use: `socket`, `dpdk` or `ibverbs`; `none` over TCP. |
| `daqiri_data_path` | `udp` or `tcp`: the data path of the open buffer, or the one the decision table would give it. |

The context also has the static attributes `uri`, `ip,ip-addr`, `daqiri,yaml`,
`daqiri,data`, `daqiri,mem`, `daqiri,engine` and `daqiri,version`.

## Measuring

`iio_daqiri_bench` queues blocks, receives for a given time and prints the
rate, dequeue latency percentiles, CPU use and the counters above (as the
difference over the run). It only uses the public API, so it runs against `ip:`
as well:

```
iio_daqiri_bench -u "daqiri:192.168.2.1,data=udp,yaml=1g.yaml" \
    -d cf-ad9361-lpc -c voltage0,voltage1 -s 8944 -n 64 -T 10 \
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
- **UDP sample mode is receive only.** There is no UDP transmit and no
  cyclic buffer (`-ENOTSUP`); with `data=auto` output buffers use TCP.
- **One buffer per `(interface, queue)`**; opening a second one on the same
  queue fails with `-EBUSY`. A queue carries one stream.
- **The server may refuse a channel mask** it cannot stream (`-ENOTSUP` at
  setup: TCP with `data=auto`, a failed open with `data=udp`).
- **Late datagrams are dropped** (and counted), not put back in order.
  Whether a late datagram arrived before is known for the 1024 sequence
  numbers below the expected one; an older one counts as late.
- Linux only.

## Security

UDP sample mode receives raw UDP: there is no authentication, no encryption
and no session. Anyone who can send datagrams to the receive port (and, with
the socket engine, from the configured source address) can inject samples
under the stream ID, which is not a secret. Use it on an isolated, trusted
network or a direct link between the board and the host. The management
connection, and the TCP data path, are as unauthenticated as `ip:`.

## License

The `daqiri:` module is LGPL-2.1-or-later, like the rest of libiio. It links
the Apache-2.0 licensed DAQIRI library and the NVIDIA CUDA runtime, but only as
a separate shared object (`libiio-daqiri.so`) that is loaded on demand:
`libiio.so` does not link either of them. If you redistribute a build with the
module, the DAQIRI and CUDA licenses apply to those libraries as well.
