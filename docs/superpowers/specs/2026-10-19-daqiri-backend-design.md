# DAQIRI backend for libiio — design

Date: 2026-10-19. Repo: /home/tcollins/dev/libiio (tfcollins/libiio). Work branch: `feature/daqiri-backend` from master.
Companion repos touched: ~/dev/labgrid-plugins (exporter config + place tags), ~/dev/infra (ansible host prep, HOSTS.md).

## Context

ADI embedded platforms (Zynq/ZynqMP/VCU118 carriers + converters) can emit sample streams over
Ethernet faster than iiod's TCP path can absorb. NVIDIA DAQIRI (Data Acquisition for Integrated
Real-time Instruments; Apache-2.0; C++17, CUDA mandatory at build) is a packet I/O library with
three engines: Linux sockets (any NIC), DPDK (mlx5, kernel bypass, GPUDirect via dma-buf), raw
ibverbs (mlx5dv/DevX striding RQ). It defines **no wire protocol and no discovery** — the sender
contract and control plane are ours.

Goal: a `daqiri:` backend so unmodified libiio apps (C, Python bindings, pyadi-iio) receive
high-rate streams through DAQIRI at 1G, 10G and 100G, with software tests in GitHub CI and HIL
tests on the labgrid fleet (coordinator 10.0.0.41:20408).

### Decisions (user, 2026-10-19)

| Topic | Decision |
|---|---|
| Prior work | Fresh design. Salvage ideas (not code) from `origin/design/daqiri-backend` (DQI1 private protocol, raw-ibverbs-only, /etc profiles, one ctx/process: all dropped). |
| Framings | (a) VITA 49.2 IF-Data over UDP as emitted by vrt49 FPGA framers; (b) stock iiod binary protocol so **unchanged libiio v1 boards running iiod** work with zero board changes. |
| Senders v1 | Stock iiod on Kuiper boards; vrt49 FPGA framers (Z7035 1G SFP, ZU11EG 10G SFP+, VCU118+DAQ3 100G QSFP). No iiod/tinyiiod/Zephyr changes. |
| Loss model | Free-running sender; sequence-based loss detection + counters; no credits. |
| 100G receiver | DAQIRI DPDK engine on picard's ConnectX-5. |
| HIL | VCU118+DAQ3 is already labgrid place `nuc`; add picard's 100G NIC as a new place `picard-cx5`. |
| GPU | Design the device-memory path now; host memory is the baseline. |
| Packaging (my call) | Dynamic module `libiio-daqiri.so` via WITH_MODULES, so the C core never links C++/CUDA. |

### Verified lab facts

| Tier | Sender (place) | Receiver NIC | Notes |
|---|---|---|---|
| 1G-iiod | any Kuiper place: `bq`, `nemo`, `tron`, `lablp` | the runner host's RJ45 | stock iiod |
| 1G-vrt | `lablp` ADRV9361-Z7035 1G SFP (`adrv9361z7035_vrt49_sfp`) | **unknown**: no physical receiver leg exists in vrt49 CI | cabling survey needed |
| 10G-vrt | `tron` ADRV9009-ZU11EG SFP+ (`adrv9009zu11eg_vrt49_corundum`) | nemo `enp2s0` Intel 82599, 192.0.2.2/24, **link down today** | vrt49 serializes tron with `/tmp/vrt49-corundum-hardware.lock` on nemo |
| 100G-vrt | `nuc` VCU118+DAQ3 (BootFabric; JTAG + Vivado resources on nuc) | picard `enp101s0np0` ConnectX-5 (MT27800, fw 16.32.1010), link up 100G, 192.0.2.2/24, MTU 1500, PCI 0000:65:00.0 | vrt49 PR #50 `daq3_vcu118_vrt49_100g`; picard: 36-core W-2295, 62 GB, Quadro RTX 4000, CUDA 13.4, libibverbs present, **no DPDK, no hugepages, no exporter, sudo needs password** |

DAQIRI source facts that matter: DPDK engine **hardcodes** mlx5 devargs `dv_flow_en=2` (HW steering,
needs ConnectX-6 Dx+; CX-5 only supports `dv_flow_en=1`), no YAML/env override. The ibverbs engine
has **no generation check** — only mlx5dv + DevX + striding-RQ, all of which CX-5 has. Socket engine
ignores memory `kind` (always malloc'd host memory), no HDS/flows, TCP = one packet per `recv()`.
One engine per process (`daqiri_init` twice → INTERNAL_ERROR). No C API; `find_package(daqiri)`,
target `daqiri::daqiri`; `find_package(CUDAToolkit REQUIRED)` unconditional. All facts are from
`https://github.com/NVIDIA/daqiri` **main as of 2026-10-19 (VERSION 2026.7.0)**; M0 pins that HEAD SHA in
`CI/daqiri/DAQIRI_REF` and every DAQIRI claim is re-verified against the pinned tree when bumped.
(The abandoned branch pinned `993363da`; do not reuse it.)

Known blockers: `gh` unauthenticated for tfcollins (cannot inspect/register runners); picard sudo
password (all host prep via ansible); labgrid-client not on picard.

---

## Design

### Architecture

```
app (C / Python / pyadi-iio) ── unchanged libiio public API
        │
  libiio.so  ──dlopen──▶ libiio-daqiri.so  (module, C + C++ shim)
        │                    ├─ daqiri.c            ops table, URI, create/shutdown, attr forwarding, buffer dispatch
        │                    ├─ daqiri-mgmt.c       plain TCP to iiod (iiod_client_ops), eventfd cancel
        │                    ├─ (framing=iiod: per-buffer plain TCP via iiod-client, as network.c; no new file)
        │                    ├─ daqiri-vrt.c        framing=vrt: VRT IF-Data RX engine, gap detect, block FSM, counters
        │                    ├─ daqiri-vrt-prologue.h  vendored header-only VITA 49.2 prologue parser (no libvrt49 dep)
        │                    ├─ daqiri-engine.cpp   ONLY file including <daqiri/daqiri.h>: broker, rxq, conn, mem alloc
        │                    ├─ daqiri-gather.cu    (M5) GPU gather/bswap kernel for mem=device
        │                    └─ daqiri-private.h    extern "C" boundary, pdata structs
        ▼
  management: iiod TCP (XML, attrs, triggers, reg, events, ping)   data: DAQIRI engine (socket | dpdk | ibverbs)
```

Management is always plain-socket iiod (exactly `network.c`'s pattern via `iiod_client_create_context`).
The DAQIRI engine is acquired lazily at the first `open_buffer` and released in `shutdown`, so
attribute-only contexts never touch hugepages/EAL.

**framing=iiod decision (my call after builder review):** per-buffer **plain TCP** connection running
the stock iiod protocol through the existing iiod-client, exactly as network.c:307-466 does. DAQIRI's
raw engines have no TCP stack and its socket engine is plain Linux sockets with one packet per
`recv()`, so routing iiod over DAQIRI bursts would add a transport and a probe (old risk 6) for zero
behavioural difference. The ops table therefore also wires `readbuf`/`writebuf` to
`iiod_client_readbuf/writebuf` so **v0 iiod boards (Kuiper 2023_R2_P1 ships libiio 0.25 iiod, ASCII
protocol only)** work via the core's legacy fallback, and `create_block` returns -ENOSYS for them
(block.c:47-58 then mallocs). `daqiri-iiod-transport.c` is dropped. What `daqiri:…,framing=iiod` buys
over `ip:` is one URI family, the same `daqiri_*` counters/bench/launcher, and later `mem=pinned`
blocks; DAQIRI is reserved for `framing=vrt`. Rejected: tunnelling management (couples `iio_info` to
engine init).

**VRT stream layout (settled from vrt49 docs, verify on hardware in M2):** the framer taps the ADC
pack output *before* DMA (`vrt49_sample_tap` / `util_fifo_wr_to_axis`), is enabled by its own IIO
device attrs (`v49_enable`, `v49_stream_id`, `v49_packet_mtu`, `v49_udp_*`) and does **not** require the
board's DMA buffer to be open; one Stream ID carries one channel group (I/Q pair, CI16: I high half,
Q low half of each big-endian 32-bit word), so one `(port,queue)` ↔ one libiio buffer whose enabled
mask must be exactly that pair. Payload is VITA 49 big-endian (`vrt49_axis_to_network` converts), hence
`swap=1` default. Golden vectors and the assembler tests are written to this layout.

**framing=vrt:** one RX poller thread per open buffer owns one DAQIRI `(port, queue)`. Per packet:
validate L2/L3/L4 when raw (`l2_offset` 42, VLAN 46), parse prologue, filter by type (IF_DATA only;
Context counted and dropped), lock/filter Stream ID, bounds-check payload, 4-bit `pkt_count` gap
detection with TSF sample-count assisted sizing (`gap_ts=1`), copy into the current block with
BE→XML-format conversion (`bswap16/32` when the channel format is not `be:`; `swap=0` escape hatch),
carry buffer for packets straddling blocks. Block FSM:
`FREE →enqueue→ QUEUED →pop→ FILLING →full→ DONE`; gap with `lossy=0` → ERROR(-EIO) and resync on
the next block; `lossy=1` → zero-fill, flag, continue; cancel → -ECANCELED. No enqueued block →
`dropped_no_block++` (host overrun path). Double enqueue/dequeue → -EPERM (core bypasses its guard
for native blocks, block.c:155/182); RX blocks must fill completely (no byte count on dequeue).

**Engine broker** (process-wide, keyed by realpath of YAML): second YAML in one process → -EBUSY at
`open_buffer` (documented limitation). **Engine lifetime = process lifetime (my call):** DPDK EAL is
not re-entrant and DAQIRI's shutdown→init cycle is unproven, so the broker never calls
`daqiri::shutdown()` before `atexit`, and the module pins itself with
`dlopen(self, RTLD_NOLOAD|RTLD_NODELETE)` on first engine init so `iio_context_destroy`'s `dlclose`
cannot unload live DAQIRI threads. Contexts/buffers still refcount `(port,queue)` ownership
(`open_buffer` twice on one queue → -EBUSY) and all backend threads are joined in `close_buffer`.
Engine selection is purely YAML; the module only inspects the kind for `l2_offset`, HDS and legal
`mem=` values.

**Diagnostics:** context attrs are static strings and never hit the backend (attr.c:23-37), so
counters are **buffer attrs** added at create for every input buffer and served locally when the name
starts with `daqiri_`: `daqiri_rx_packets`, `daqiri_rx_bytes`, `daqiri_rx_lost_packets`,
`daqiri_rx_gap_events`, `daqiri_rx_dropped_no_block`, `daqiri_rx_foreign_packets`,
`daqiri_rx_context_packets`, `daqiri_rx_malformed`, `daqiri_rx_zero_filled_bytes`,
`daqiri_blocks_completed`, `daqiri_blocks_failed`, `daqiri_blocks_with_gaps`, `daqiri_stream_id`,
`daqiri_last_error`, `daqiri_lossy` (RW), `daqiri_mem_domain`, `daqiri_engine_kind`. Static context
attrs: `uri`, `ip,ip-addr`, `daqiri,yaml`, `daqiri,framing`, `daqiri,mem`, `daqiri,engine`, `daqiri,version`.

**URI:** `daqiri:<host>[:port][,key=value]*` (host parse copied from network.c:663-694; option split
like serial.c). Keys: `yaml` (default `$IIO_DAQIRI_YAML`, else -EINVAL), `framing=vrt|iiod` (vrt),
`mem=host|pinned|device` (host), `gpu=<ordinal>`, `lossy=0|1`, `iface`, `rxq`, `sid`/`sid_mask`,
`swap=0|1`, `gap_ts=0|1`, `data_port` (iiod framing; default mgmt port), `poll=<µs>` (50).

**GPU path (designed now, built in M4/M5):** `mem=pinned` = cudaHostAlloc blocks, CPU copy path.
`mem=device` = cudaMalloc blocks; requires a raw engine with HDS (seg 0 host for prologues, seg 1
device/pinned); CPU parses prologues, builds a gather-op table, one gather+bswap kernel per burst,
DAQIRI burst freed after the kernel's event; `dequeue_block` waits on the block's event so data is
complete on return. **Upstream-bound (user decision):** the module is developed against an
**unmodified core**. Memory domain is exposed first through the `daqiri_mem_domain` buffer attr
(`host|pinned|device`) plus `daqiri_gpu_device`, which needs no core change. The public API
(`enum iio_mem_domain {HOST, HOST_PINNED, DEVICE}`, `int iio_block_get_mem_domain(const struct
iio_block*, enum iio_mem_domain*, int *device_id)` in include/iio/iio.h + libiio.map, backend op
`get_block_mem_domain` appended to the END of `iio_backend_ops` under `IIO_BACKEND_API_V2`, with
api_version stored in `struct iio_context` at context.c:774 / dynamic.c:150-156 and the new slot read
only for V2 backends) goes in a **separate small core PR to analogdevicesinc/libiio** for review; the
module adopts it when merged. `iio_block_start()` keeps returning the data pointer (device pointer for
`mem=device`); `iio_block_first/foreach_sample` documented CPU-only. Also upstream-facing: keep all
new core-side changes to CMake/install/config/zephyr lists; no `tfcollins`-specific paths in tree
(CI images under `ghcr.io/tfcollins` are referenced only from workflow files, which ADI can repoint).

**Error map:** bad URI/YAML → create -EINVAL; engine YAML conflict → open_buffer -EBUSY; VRT TX,
cyclic, device-without-HDS, non-16/32-bit lanes → -ENOTSUP; board lacks binary iiod → -EPROTONOSUPPORT;
nonblocking not ready → -EBUSY; dequeue timeout → -ETIMEDOUT (block stays queued); loss (lossy=0) →
-EIO; malformed/layout mismatch → -EPROTO; cancelled → -ECANCELED; peer closed → -EPIPE.
DAQIRI Status map lives once in daqiri-engine.cpp (NOT_READY→-EAGAIN, NO_FREE_*→-ENOBUFS,
INVALID_PARAMETER→-EINVAL, NOT_SUPPORTED→-ENOTSUP, RESOURCE_IN_USE→-EBUSY, else -EIO; bad_alloc→-ENOMEM).

**Out of scope v1:** VRT TX/deframer, cyclic VRT, Context→IIO events, RoCE, hardware reorder (4-bit
pkt_count cannot key it), multi-buffer SID demux on one queue, DNS-SD scan, non-Linux, async CUDA
event hand-out, DMA-BUF export, backend-driven `v49_enable` (app sets framer attrs via normal iiod).

### 100G engine strategy (risk-driven)

1. Spike A (cheapest): raw **ibverbs** engine on CX-5 — the code only requires mlx5dv/DevX/striding-RQ.
2. Spike B: **DPDK** engine with a one-line local patch making `dv_flow_en` configurable (env var
   `DAQIRI_MLX5_DV_FLOW_EN`), carried in `CI/daqiri/patches/` and proposed upstream to NVIDIA.
3. Fallback: socket engine multi-queue (loss-accounting tier only).
The user chose DPDK; A is a 1-hour experiment run first because it needs no patch. Pick by measurement
during M5 and record the decision in the tier README.

### Build / packaging

`CMakeLists.txt` (after the network block, mirror :772-793):
`WITH_DAQIRI_BACKEND` (OFF, POSIX; FATAL if not Linux+BUILD_SHARED_LIBS+WITH_MODULES), `enable_language(CXX)`,
`find_package(daqiri CONFIG REQUIRED)`, `find_package(CUDAToolkit REQUIRED)`, `WITH_DAQIRI_GPU_BLOCKS` (ON,
enables CUDA language + `daqiri-gather.cu`), `add_library(iio-daqiri SHARED ...)` PREFIX `lib`, hidden
visibility, link `iio daqiri::daqiri CUDA::cudart Threads::Threads`, set `IIOD_CLIENT`/`NEED_LIBXML2`,
`toggle_iio_feature`. Framing code (`daqiri-vrt.c`, `daqiri-vrt-prologue.h`, block assembler) is also
built as an OBJECT library `iio_daqiri_framing` with no DAQIRI/CUDA dependency so unit tests run in the
plain `tests.yml` job. `cmake/Install.cmake:200-212` install rule; `iio-config.h.cmakein`
`#cmakedefine01 WITH_DAQIRI_BACKEND`; `zephyr/CMakeLists.txt` `WITH_DAQIRI_BACKEND=0`. `context.c`
backends table untouched (module-only; `iio_create_dynamic_context` dlopens `libiio-daqiri.so`,
dlsyms `iio_daqiri_backend`). All module threads joined in `close_buffer`/`shutdown` (module is dlclosed).

---

## Test and HIL infrastructure

### Software-only (GitHub-hosted)

- `tests/daqiri/` (new, `add_subdirectory` from tests/CMakeLists.txt, label `daqiri`):
  `test_vrt_prologue.c`, `test_vrt_gap.c` (contiguous/single/burst/wrap 15→0/across-wrap/reorder/dup/>16 lost),
  `test_vrt_assembler.c` (boundaries, straddle, short tail, gap→flag/zero-fill), `test_iiod_framing.c`
  (golden bytes captured from iiod-emu). Golden datagrams in `tests/resources/vrt/*.bin` + `manifest.json`,
  generated by `tests/daqiri/gen_golden.py` (pure Python, deterministic; optional `--check-with-libvrt49`
  ctypes round-trip through `vrt49_prologue_pack/unpack` for codec parity).
- `tests/daqiri/vrt_sender.c`: ~400-line standalone C software framer (sendmmsg; ramp or IIO-block-file
  replay; `--drop-list/--drop-prob/--reorder-window/--dup-every/--burst-gap`; `--json` oracle of
  dropped/reordered sequences). Decision: do **not** reuse vrt49's `iiod-vrt49` (it models the daemon
  handshake, not a free-running framer, and drags in the vrt49 tree).
- Loopback (label `daqiri-loopback`, needs WITH_DAQIRI_BACKEND + WITH_IIOD_EMU): `run_loopback.sh`
  starts `iiod-emu` per XML in tests/resources/xmls, waits with `nc -z`, runs
  `TESTS_API_URI=daqiri:127.0.0.1,framing=iiod ctest -L api` (mirrors zephyr-tests.yml);
  `test_block_equivalence.c` (`ip:` vs `daqiri:` byte-equal blocks + identical XML);
  `test_vrt_loopback.c` (forks vrt_sender; clean/drop/reorder/jumbo/gap cases; counters == oracle);
  `test_daqiri_py.py` (bindings smoke; no binding changes needed, ctypes `Context("daqiri:...")`).
- CI image: `CI/daqiri/Dockerfile` FROM `nvidia/cuda:13.4.0-devel-ubuntu22.04`, DPDK 25.11 + DAQIRI
  patches → /opt/dpdk, pinned DAQIRI → /opt/daqiri (socket+dpdk+ibverbs engines), CUDA stubs on the
  loader path for GPU-less runners (verify `libdaqiri.so` loads without `libcuda.so.1` in M0).
  `.github/workflows/daqiri-ci-image.yml` → `ghcr.io/tfcollins/libiio-daqiri-ci:<daqiri-sha>-cuda13.4`.
- `.github/workflows/daqiri.yml`: required PR job `software` in that image: cmake with
  `-DWITH_MODULES=ON -DWITH_DAQIRI_BACKEND=ON -DWITH_EMU_BACKEND=ON -DWITH_IIOD_EMU=ON -DWITH_TESTS=ON`,
  `ctest -L daqiri`, `ctest -L daqiri-loopback -j1`, Python smoke. Framing unit tests also run in the
  existing `tests.yml` (no DAQIRI) as the fast signal.
- Bench: `utils/iio_daqiri_bench.c` (public API only, so it benchmarks `ip:` too):
  `-u URI -d DEV -c CH -s BLOCK -T SEC [--check ramp|prbs15] [--gpu] [-j perf.json]` → Gbps, pps,
  lost/reordered/dropped (from `daqiri_rx_*` attrs), dequeue p50/p99, CPU% per thread, D2H time.

### Hardware tiers (`tests/hardware/daqiri/`, pytest + labgrid 26.0 + labgrid-plugins pinned)

Files: `README.md`, `requirements.txt`, `pytest.ini`, `launcher.py`, `conftest.py`, `receiver.py` (NIC
counter gate ported from vrt49 `scripts/verify-direct-10g.py`), `patterns.py` (ramp/PRBS continuity,
logic of tests/hardware/test_ad9364.c), `perf.py` (perf.json schema + `--compare baseline`),
`test_common.py`, `test_tier_iiod_1g.py`, `test_tier_vrt_1g.py`, `test_tier_vrt_10g.py`,
`test_tier_vrt_100g.py`, `baselines/*.json`.

`launcher.py --tier {1g-iiod,1g-vrt,10g-vrt,100g-vrt,100g-gpu} --build <abs> --evidence <abs> [--place]`:
preflight (venv pins, cmake build, lib sha256, coordinator reachable, `validate_place` tags,
**exporter identity from an explicit `exporter-address=<ip>` place tag, bare hostnames rejected** —
this is what broke the abandoned harness on tron) → host lock (`/tmp/vrt49-corundum-hardware.lock`
on nemo when tron is used; `/tmp/libiio-daqiri-<place>.lock` otherwise) → for vrt tiers acquire the
receiver NIC place first via the plugins' `Reservation` API (reserve, capture token, bounded separate
`wait`, acquire; never `reserve --wait`) → board via `adi-lg request --part --carrier --mode reserve
--run "<pytest>"` (one place per request) → boot via place strategy with `exporter:` payload prefixes →
board IP via `resolve_uri` → tier tests, `--junitxml`, perf.json → cleanup legs each with own timeout
(power off, target cleanup, release both places, readback `acquired: None`, own reservation absent)
→ `cleanup.json`; launcher fails if any leg is false even when tests pass.

| Tier | Places / runner | Config | Correctness gate | Capacity gate |
|---|---|---|---|---|
| 1G-iiod | `nemo`/`bq` (then tron, lablp); hw-nemo / hw-bq | `framing=iiod`, socket TCP | TX→RX loopback ramp continuous in 200 blocks; 32 blocks memcmp `ip:` vs `daqiri:` | 30 s zero dequeue errors; Gbps ≥ 1.0× `ip:` baseline same session |
| 1G-vrt | `lablp` → receiver TBD; hw-bq | `framing=vrt`, socket UDP; framer attrs (`v49_*`) set via iiod | prologue == framer sysfs; pkt_count contiguous; ramp continuity 60 s | zero lost 60 s at 1G; NIC `rx_missed`=0; `UdpRcvbufErrors`=0 |
| 10G-vrt | `tron` → nemo `enp2s0`; hw-nemo; vrt49 flock | `framing=vrt`, socket UDP (RSS multi-queue only if framer can vary src port) | ¼ rate: zero loss 60 s + continuity | full rate 60 s: `lost == rx_missed + UdpRcvbufErrors` ±1 %, loss ≤ 1e-4 initially |
| 100G-vrt | `nuc` + `picard-cx5`; hw-picard | `framing=vrt`, ibverbs or DPDK (per spike), jumbo 8968, MTU 9000; socket fallback as 2nd case | DAQ3 ≈ 32 Gbps payload: ramp/PRBS continuity 60 s, zero loss | DPDK/ibverbs: zero loss 60 s; `rte_eth_stats` imissed reconciles with `lost` |
| 100G-gpu | same + `mem=device` | HDS + device regions | bench `--gpu` D2H + ramp check, zero loss 60 s | + nvidia-smi util, `topo -m` recorded |

Artifacts per tier: junit.xml, perf.json, nic-before/after.json, cleanup.json, failure.json (redacted),
backend log, provenance.txt (libiio sha, lib sha256, daqiri sha, plugin sha, board kernel/boot_id,
bitstream sha), 64-packet pcap via `RawNetworkInterfaceDriver.record()` for socket vrt tiers.

**picard as a labgrid place:** `~/dev/labgrid-plugins/examples/lg_picard_cx5_exporter.yaml`
(`NetworkInterface ifname: enp101s0np0` + `NetworkService 10.0.0.101`), exporter `-n picard
--hostname 10.0.0.101`, user unit `labgrid-exporter.service` (linger). Place `picard-cx5`, match
`picard/cx5/*`, tags `nic=connectx5 speed=100g peer=nuc pci=0000:65:00.0 driver=mlx5_core
link-address=192.0.2.2/24 mtu=9000 runner=hw-picard exporter-address=10.0.0.101 exporter-hostname=picard`;
add to `exporter_configs/place-tags.home-lab.yaml`; also seed `exporter-address/-hostname` on tron
(10.0.0.232), nuc (10.0.0.121), lablp, bq, nemo. HOSTS.md gets a `picard-cx5` row.

**Host prep (ansible, ~/dev/infra):** `deploy-daqiri-host.yml` (picard: DPDK 25.11+patches → /opt/dpdk,
DAQIRI → /opt/daqiri, GRUB `default_hugepagesz=1G hugepagesz=1G hugepages=8` + `/dev/hugepages`
group-rw (**reboot to schedule**; picard hosts hal-voice), sysctl rmem_max/backlog, netplan
`enp101s0np0` 192.0.2.2/24 mtu 9000, udev rw on `/dev/infiniband/*` for tcollins, limited NOPASSWD
sudoers for `tcpdump -i enp101s0np0`/`ethtool -S`, `nvidia-peermem` for GPU tier; nemo/bq: run the
tier inside the CI image with `--network host --cap-add NET_RAW`, `enp2s0` persistent address, ring
4096, venv under `/mnt/onetb/scratch`), `deploy-labgrid-exporter.yml` (picard), templates.

**CI:** `.github/workflows/daqiri-hil.yml`: `workflow_dispatch` (tier, duration, wait, ref) +
nightly 04:00 UTC (clear of daily-infra), `concurrency: daqiri-hil`; job `preflight` on
`[self-hosted, hw-coordinator]` emits the matrix (skips disabled/acquired places); job `hil` matrix
`runs-on: [self-hosted, ${{matrix.runner}}]`, `fail-fast: false`, 60-min timeout, `continue-on-error`
until 5 green nightlies; "require non-skipping JUnit evidence" step; artifact upload. Runner labels
needed for tfcollins/libiio: `hw-coordinator`+`hw-nemo` (nemo), `hw-bq` (bq), `hw-picard` (picard),
registered with `adi-lg setup-runner --scope repo:tfcollins/libiio` into `~/actions-runner-libiio`
(user unit `actions-runner-libiio.service` on nemo/bq, `github-runner-libiio.service` on picard).
Interim with zero new runners: dispatch the launcher from `tfcollins/labgrid-adi-hw-test`.

---

## Implementation order (each step ends runnable; TDD per step)

0. **Setup:** `gh auth login`; `git checkout -b feature/daqiri-backend`; commit this design as
   `docs/superpowers/specs/2026-10-19-daqiri-backend-design.md` (`git commit -s`).
1. **M0 skeleton + management** (daqiri.c, daqiri-mgmt.c, daqiri-private.h, daqiri-engine.cpp with
   `dq_yaml_validate`/`dq_engine_version` only; CMake/install/config/zephyr). Done: `iio_info -u
   "daqiri:host,yaml=x.yaml"` shows XML/attrs; `ldd libiio.so` has no C++/CUDA; module load/unload OK.
   Also: framing OBJECT lib + `tests/daqiri` unit tests + golden vectors + `gen_golden.py`; CI image
   + `daqiri-ci-image.yml`; prove `libdaqiri.so` loads GPU-less.
2. **M1 framing=iiod + broker** (per-buffer plain-TCP iiod-client path incl. readbuf/writebuf for v0
   iiod; `dq_engine_*` broker with process-lifetime pinning). Done: `iio_readdev -u
   "daqiri:host,framing=iiod"` byte-identical to `ip:` against iiod-emu (v1) and a Kuiper board (v0);
   `run_loopback.sh` + `test_block_equivalence` green; `daqiri.yml` required on PRs.
3. **M2 framing=vrt over socket UDP** (daqiri-vrt-prologue.h, daqiri-vrt.c, FSM, counters, attrs,
   lossy; `vrt_sender.c`; `test_vrt_loopback`). Done: counters == sender oracle; works against vrt49
   `vrt49-send-test-pattern` and a real framer.
4. **M3 HIL 1G-iiod** (launcher, conftest, receiver.py, tier test, place tags, runners on nemo/bq,
   `daqiri-hil.yml` with this tier only, `iio_daqiri_bench`). Done: 5 green nightlies on nemo and bq;
   baseline perf.json committed.
5. **M4 1G-vrt (lablp)** after cabling survey; **10G-vrt (tron→nemo)** with vrt49 flock, bitstream
   provenance from vrt49 artifacts, DAQIRI in container on nemo, pcap golden captured into
   `tests/resources/vrt/`.
6. **M5 raw engines + 100G** (l2 validation, VLAN, HDS-aware `dq_pkt`, `iface`/`rxq`, hugepage-backed
   host blocks; ansible host prep; picard exporter + place; picard runner; 2-place launcher path;
   engine spikes A/B). Done: zero loss 60 s at DAQ3 design rate; both places released and verified.
7. **M6 memory domains** (`mem=pinned`; `daqiri_mem_domain`/`daqiri_gpu_device` attrs; no core change)
   then **mem=device** (daqiri-gather.cu, deferred burst free, event wait in dequeue; CUDA sample;
   100G-gpu tier). In parallel, open the separate upstream core PR (`iio_block_get_mem_domain`,
   `IIO_BACKEND_API_V2`, map/header/block.c/context.c/dynamic.c; symbol_map passes).
8. **M7 hardening + docs**: ASan/TSan over cancel/timeout/double-free races; `set_timeout`
   propagation; `doc/source/daqiri-backend.md` + index; tier README with manual 100G bring-up.

## Verification

- Software: `ctest -L daqiri`, `ctest -L daqiri-loopback -j1`, `ctest -L api` under
  `TESTS_API_URI=daqiri:127.0.0.1,framing=iiod` for every emu XML, `symbol_map` ctest, Python smoke;
  `daqiri.yml` green on the PR.
- Hardware: `python tests/hardware/daqiri/launcher.py --tier <t> --build <b> --evidence <e>` per tier;
  JUnit has every required case passing with no skips; `cleanup.json` all true; perf.json within
  baseline (`perf.py --compare`); coordinator readback shows places free.
- 100G: `iio_daqiri_bench -u "daqiri:<nuc-ip>,yaml=/etc/libiio/daqiri/picard-cx5.yaml" -T 60 --check ramp`
  reports zero lost at design rate; `ethtool -S enp101s0np0` / `rte_eth_stats` reconcile.

## Risks / open items

1. DPDK on CX-5 (`dv_flow_en=2` hardcoded) → spikes A/B above; socket fallback.
2. `libdaqiri.so` on GPU-less hosts (links `libcuda.so.1`?) → CUDA stubs in image; verify in M0.
3. No libiio runners, `gh` unauthenticated → step 0; interim dispatch from labgrid-adi-hw-test.
4. picard reboot for 1G hugepages (hosts hal-voice/GPU watch) → schedule.
5. nemo `enp2s0` link down; lablp 1G SFP receiver unknown → physical surveys before M4.
6. **100G tier depends on vrt49 PR #50** (`daq3_vcu118_vrt49_100g`) actually streaming; only the
   physical link is verified. M5's libiio exit criterion is the engine spike + 2-place launcher;
   the zero-loss gate is contingent on vrt49 validating the bitstream (tracked in vrt49, not here).
7. 4-bit pkt_count ambiguity → relies on framer TSF=SAMPLE_CNT; confirm on ZU11EG.
8. Mask must equal stream layout (CI16 ↔ two 16-bit scan elements); confirm Kuiper XML formats.
9. Upstream review of the core PR (`IIO_BACKEND_API_V2`) may take time; the attr-based domain
   signal keeps the module fully functional meanwhile.
10. License note in docs: LGPL module linking Apache-2.0 DAQIRI + CUDA runtime as a separate `.so`.
