# DAQIRI backend phase 2 (M5: raw engines + 100G on picard) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `framing=vrt` streams through DAQIRI's raw engines (ibverbs and DPDK) on picard's ConnectX-5, with L2/L3/L4 validation, header-data-split-aware packets, hugepage host blocks, a self-contained 100G hardware loopback test on picard, picard registered as labgrid place `picard-cx5`, and a two-place 100G tier harness ready for the VCU118+DAQ3 framer.

**Architecture:** The C++ shim (`daqiri-engine.cpp`) learns to open raw-stream queues and expose per-packet segments; a DAQIRI-free L2 parser (`daqiri-l2.[ch]`) joins the framing library and runs per packet in the VRT poller when the queue is raw. DPDK 25.11 and DAQIRI's raw engines are built by scripts under `CI/daqiri/` (dev prefix `$HOME/opt`, image prefix `/opt`), with one carried DAQIRI patch making mlx5 `dv_flow_en` overridable by environment so the ConnectX-5 can run the DPDK engine. Test traffic on picard comes from DAQIRI's single-port hardware loopback (ibverbs engine, no cable needed) driven by a new raw VRT sender. Host prep is an ansible play; the place, exporter and runner follow the labgrid-plugins conventions.

**Tech Stack:** C99 (module, framing lib, tests), C++17 (DAQIRI shim, raw sender), DPDK 25.11 + DAQIRI patches, rdma-core 50, CMake/ctest, Python 3 (golden vectors, tier harness, pytest), ansible, labgrid 26 + labgrid-plugins, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-19-daqiri-backend-design.md` (sections "100G engine strategy", "Hardware tiers", "picard as a labgrid place", "Host prep", milestone M5). Phase 1 plan: `docs/superpowers/plans/2026-10-19-daqiri-backend-phase1.md` (its Global Constraints still apply).

## Facts established 2026-10-20 (verify before relying on them)

- picard: Ubuntu 24.04, kernel 7.0.0-38, 36 threads (W-2295, one NUMA node), `intel_iommu=on iommu=pt`, Secure Boot off, 2 MB hugetlbfs mounted at `/dev/hugepages` (root 755, `HugePages_Total: 0`), 1 GB pages supported but unconfigured, CUDA 13.4 at `/usr/local/cuda` (not on PATH), driver 595.99.02, DAQIRI 2026.7.0 at `$HOME/opt/daqiri` built with the socket engine only, rdma-core/libibverbs 50.0 runtime present, **no** `libibverbs-dev`, `rdma-core` (tools), `meson`, `ninja-build`, `libnuma-dev`; apt DPDK candidate is 23.11 (unusable: DAQIRI needs 25.11 + its own patches).
- ConnectX-5 `enp101s0np0` = `mlx5_0`, PCI `0000:65:00.0`, MAC `e8:eb:d3:fe:22:44`, fw 16.32.1010, `/dev/infiniband/uverbs0` present, MTU 1500, 192.0.2.2/24 (NetworkManager, not netplan). Link is **down whenever the VCU118 is not driving its CMAC** (it flapped with vrt49 JTAG activity today); `nuc` was acquired by `bq/tcollins` at survey time.
- DAQIRI (pin `7c1f5d05`): DPDK engine builds devargs `dv_flow_en=2` unless the interface has eCPRI flows (`src/engines/dpdk/daqiri_dpdk_engine.cpp:2349`); static UDP flows use the synchronous `rte_flow_create`, the async/template API is only configured when `dynamic_flow_capacity` > 0, so `dv_flow_en=1` is viable for our static flows. The ibverbs engine checks only mlx5dv + DevX + striding RQ. `loopback: "hw"` exists **only** in the ibverbs engine (single port, TX loops into the port's own RX steering, "the NIC must remain enumerated without a link", `eth_dst_addr` = the port's own MAC). `loopback: "sw"` needs no NIC. Raw packets start at the Ethernet header (UDP payload at byte 42); HDS splits a packet at `buf_size` of the queue's first memory region into segment 0/1, read with `get_segment_packet_ptr/length(burst, seg, idx)`. Memory kinds: `host`, `host_pinned`, `huge`, `device`. TX API: `create_tx_burst_params`, `set_header(msg, port, queue, batch, 1)`, `is_tx_burst_available`, `get_tx_packet_burst`, `get_segment_packet_ptr(msg, 0, i)`, `set_packet_lengths(msg, i, {len})`, `send_tx_burst`, `free_all_packets_and_burst_tx`, `wait_for_tx_idle`.
- vrt49 PR #50 (`feat/vcu118-daq3-vrt49`) defines the VCU118+DAQ3 100G TX project but **produces no bitstream yet**; the framer's network endpoint is set through IIO attrs `v49_udp_dest_mac/dest_ip/dest_port/source_mac/source_ip/source_port/ttl/apply/status`, plus `v49_enable`, `v49_stream_id`, `v49_packet_mtu`; vrt49 CI uses `192.0.2.1 → 192.0.2.2:4991`. So the `100g-vrt` tier cannot pass until vrt49 ships a bitstream (spec risk 6); this plan's hardware gate is the loopback tier.
- labgrid-plugins: `adi-lg request --part --carrier --mode --run --wait --coord --no-boot --validate --power-down`, `adi-lg setup-runner --scope --name --labels --dir --coord --token --non-interactive`, `reserve_and_acquire(coord, filt, *, wait)` / `release(coord, reservation)` in `adi_lg_plugins/request/reservation.py`, place tags seeded with `exporter_configs/scripts/seed-place-tags.sh --coordinator 10.0.0.41:20408 --manifest exporter_configs/place-tags.home-lab.yaml`. `nuc` tags: `boot-strategy=BootFabric carrier=vcu118 daughter-board=daq3 runner=hw-nuc`. Coordinator `10.0.0.41:20408`; `labgrid-client` lives on nemo (`~/venv_WORKING/bin`), not on picard.
- infra: playbooks in `~/dev/infra/ansible/playbooks/`, inventory `ansible/inventory/hosts.yml` (picard = 10.0.0.101), run as `ansible-playbook ansible/playbooks/<name>.yml -K` for picard (sudo password), vault at `ansible/inventory/group_vars/all/vault.yml`.

## Global Constraints

- Everything in phase 1's Global Constraints (module/core isolation, only `daqiri-engine.cpp` includes DAQIRI, framing lib DAQIRI-free, `git commit -s`, `format.sh`, no `tfcollins` paths in source).
- DPDK is pinned to **25.11** with DAQIRI's `dpdk_patches/dmabuf.patch` (`--exclude=.mailmap --exclude='doc/guides/rel_notes/release_26_03.rst'`) and `dpdk.nvidia.patch`; DAQIRI stays at `CI/daqiri/DAQIRI_REF`. Our DAQIRI patch lives in `CI/daqiri/patches/daqiri/` and is applied by `build-daqiri.sh`; it must apply with `git apply --check` against the pin.
- Build scripts never need root: dev prefix `$HOME/opt/{dpdk,daqiri}`; only the ansible play touches `/opt`, `/etc`, `/dev`, GRUB, sysctl.
- No picard reboot is required by this plan: hugepages are 2 MB pages set at runtime (`vm.nr_hugepages`), persisted in `/etc/sysctl.d/`; 1 GB pages stay optional and documented.
- Raw-queue packets are validated (Ethernet → optional one VLAN tag → IPv4 IHL 5, no fragments → UDP, lengths consistent) **before** the VRT prologue is parsed; a rejected packet is counted, never parsed.
- One DAQIRI engine per process, process lifetime, atexit guard: unchanged. `mem=device` stays `-ENOTSUP` (M6). HDS in this phase: both segments host memory.
- Hardware tests carry ctest label `daqiri-hw` and run only on picard (`ctest -L daqiri-hw` with `DAQIRI_HW_PCI`, `DAQIRI_HW_MAC` set); GitHub-hosted jobs never run them.
- Tier harness: every place acquired is released by the same run, readback-verified, and recorded in `cleanup.json`; the launcher exits non-zero if any cleanup leg is false, even when tests pass.
- `sudoers` additions on picard: none. `ethtool -S` is readable unprivileged; pcap capture is not part of the 100G tier.

## Review Focus

1. A raw packet whose IPv4 total length or UDP length claims more bytes than the segment holds must be rejected and never read past the buffer — pinned in Task 3 (`test_l2_parse`, truncated vectors `l2-trunc-ip.bin`, `l2-trunc-udp.bin`).
2. A VLAN-tagged packet must be accepted with the VRT prologue found at byte 46, and a double-tagged (QinQ) packet rejected — pinned in Task 3 (`l2-vlan.bin`, `l2-qinq.bin`).
3. With HDS, a packet whose segment 0 is shorter than L2 headers + prologue (`split_boundary` misconfigured) must count as malformed, not crash — pinned in Task 4 (`test_vrt_hds_split`, unit test on `dq_vrt_packet_segs`) and Task 6 (`hds_short_split` hardware case).
4. The DPDK engine must refuse to start on a NIC that needs `dv_flow_en=1` unless the override is set, and must start with it — pinned in Task 7 (`probe-raw` with and without `DAQIRI_MLX5_DV_FLOW_EN=1`, recorded in the tier README).
5. A tier run interrupted after acquiring `picard-cx5` (SIGTERM, pytest crash) must still release the place — pinned in Task 9 (`test_launcher_cleanup.py`: fake coordinator client, inject a failing test body, assert release called and `cleanup.json` written).

---

### Task 1: picard host prep (ansible) — packages, hugepages, NIC, device permissions

**Files:**
- Create: `~/dev/infra/ansible/playbooks/deploy-daqiri-host.yml`
- Create: `~/dev/infra/ansible/playbooks/templates/daqiri-hugepages.sysctl.j2`
- Modify: `~/dev/infra/HOSTS.md` (picard row: DAQIRI host prep summary)
- Test: `ansible-playbook --syntax-check`, `--check -K` dry run, then post-apply assertions below

**Interfaces:**
- Produces on picard: packages `libibverbs-dev rdma-core ibverbs-utils infiniband-diags meson ninja-build libnuma-dev libelf-dev libpcap-dev python3-pyelftools pkg-config`; `vm.nr_hugepages=2048` (4 GB of 2 MB pages) persisted as `/etc/sysctl.d/80-daqiri-hugepages.conf`; `/dev/hugepages` remounted with `gid=<daqiri group>,mode=1770` through a systemd drop-in `/etc/systemd/system/dev-hugepages.mount.d/daqiri.conf` (`[Mount] Options=pagesize=2M,gid=…,mode=1770`, then `daemon-reload` and `systemctl restart dev-hugepages.mount`; no fstab line, systemd owns that mount); udev rule `/etc/udev/rules.d/80-daqiri-infiniband.rules` making `/dev/infiniband/uverbs*` and `rdma_cm` group `daqiri` 0660; group `daqiri` with member `tcollins`; `enp101s0np0` MTU 9000 and `192.0.2.2/24` through the existing NetworkManager connection (`nmcli con mod ... 802-3-ethernet.mtu 9000 ipv4.addresses 192.0.2.2/24 ipv4.method manual`, by device match); `net.core.rmem_max=268435456`, `net.core.netdev_max_backlog=250000` in `/etc/sysctl.d/80-daqiri-net.conf`. GRUB untouched (document the optional 1 GB variant in the play header as a commented block).
- Consumed by: Task 2 (build deps), Task 6 (hugepages, uverbs access), Task 9 (MTU 9000).

- [ ] **Step 1: Write the play**

`hosts: picard`, `become: true`, `gather_facts: true`. Tasks in this order: group `daqiri` → add `tcollins` → apt packages → sysctl files (`ansible.posix.sysctl` with `reload: true`) → systemd drop-in for `dev-hugepages.mount` (`Options=pagesize=2M,gid={{ daqiri_gid }},mode=1770`, `daemon_reload`, restart the mount unit) → udev rule + `udevadm trigger --subsystem-match=infiniband_verbs` → nmcli MTU/address (idempotent: read current with `nmcli -g 802-3-ethernet.mtu,ipv4.addresses con show`) → `ip link set enp101s0np0 up` (ok if no carrier). Header comment: purpose, run line `ansible-playbook ansible/playbooks/deploy-daqiri-host.yml -K`, and the optional GRUB 1 GB block.

- [ ] **Step 2: Syntax and dry run**

Run: `cd ~/dev/infra && ansible-playbook ansible/playbooks/deploy-daqiri-host.yml --syntax-check && ansible-playbook ansible/playbooks/deploy-daqiri-host.yml -K --check --diff -l picard`
Expected: syntax OK; check mode lists the package, sysctl, mount, udev and nmcli changes with no failures (the sudo password is entered by the user; this step is a manual gate — the executor runs it via `!` if a human is present, otherwise records BLOCKED and continues with Task 3, since Tasks 2, 6, 7, 9 depend on it).

- [ ] **Step 3: Apply (manual gate)**

Run: `cd ~/dev/infra && ansible-playbook ansible/playbooks/deploy-daqiri-host.yml -K`
Expected: `failed=0`.

- [ ] **Step 4: Verify on picard**

Run:
```bash
grep HugePages_Total /proc/meminfo; stat -c '%U:%G %a' /dev/hugepages; ls -l /dev/infiniband/uverbs0; id -nG | tr ' ' '\n' | grep -x daqiri; ip link show enp101s0np0 | grep -o 'mtu [0-9]*'; ibv_devinfo | grep -E 'hca_id|link_layer'; pkg-config --exists libibverbs && echo ibverbs-dev-ok; which meson ninja
```
Expected: `HugePages_Total: 2048`; `root:daqiri 1770`; `crw-rw---- ... root daqiri`; `daqiri` printed (log out/in or `newgrp daqiri` for the current session); `mtu 9000`; `mlx5_0` / `Ethernet`; `ibverbs-dev-ok`; both tools found.

- [ ] **Step 5: HOSTS.md and commit (infra repo)**

Add to picard's row: "DAQIRI host prep (`deploy-daqiri-host.yml`): group `daqiri` owns `/dev/hugepages` (2 MB, 2048 pages) and `/dev/infiniband/uverbs*`; `enp101s0np0` MTU 9000 192.0.2.2/24; DPDK/DAQIRI dev prefixes `$HOME/opt/{dpdk,daqiri}`; link is down unless the VCU118 drives its CMAC."

```bash
cd ~/dev/infra && git add ansible/playbooks/deploy-daqiri-host.yml ansible/playbooks/templates/daqiri-hugepages.sysctl.j2 HOSTS.md && git commit -s -m "ansible: deploy-daqiri-host: prep picard for DAQIRI raw engines (hugepages, uverbs, NIC)"
```

---

### Task 2: DPDK 25.11 + DAQIRI raw engines build scripts, dv_flow_en patch, raw probe, CI image

**Files:**
- Create: `CI/daqiri/build-dpdk.sh`
- Create: `CI/daqiri/patches/daqiri/0001-dpdk-engine-honor-DAQIRI_MLX5_DV_FLOW_EN.patch`
- Modify: `CI/daqiri/build-daqiri.sh` (apply `patches/daqiri/*.patch` after checkout; pass `PKG_CONFIG_PATH` for the DPDK prefix; `-DDAQIRI_ENGINE` from `$2`)
- Create: `CI/daqiri/probe-raw.cpp` (built by `build-daqiri.sh` into `<prefix>/bin/daqiri-probe-raw`)
- Modify: `CI/daqiri/Dockerfile` (build stage: `meson ninja-build libnuma-dev libelf-dev python3-pyelftools`; `build-dpdk.sh /opt/dpdk`; `build-daqiri.sh /opt/daqiri "dpdk ibverbs"`; runtime stage copies `/opt/dpdk` too and sets `LD_LIBRARY_PATH`/`PKG_CONFIG_PATH`)
- Modify: `CI/daqiri/README.md`
- Test: build on picard; `daqiri-probe-raw` with a `loopback: "sw"` YAML; CI image builds in `daqiri-ci-image.yml`

**Interfaces:**
- Produces: `build-dpdk.sh <prefix>` → DPDK 25.11 patched, `meson setup -Dtests=false -Dplatform=generic -Denable_docs=false -Ddisable_drivers=<DAQIRI's list>`, installed to `<prefix>` (`<prefix>/lib/x86_64-linux-gnu/pkgconfig/libdpdk.pc`). `build-daqiri.sh <prefix> "<engines>" [<dpdk-prefix>]` → DAQIRI with `-DDAQIRI_ENGINE="dpdk ibverbs"` (patched), plus `daqiri-probe-raw`.
- `daqiri-probe-raw <yaml> [--seconds N] [--port <name>] [--queue <id>]`: `daqiri_init`, prints `engine=<kind> ports=<n> init=ok|<status>`; if `--seconds` > 0 polls `get_rx_burst` on the port/queue and prints `packets=<n> bytes=<n>` per second; exit 0 on init success. Used by Task 7's spike and Task 9's preflight.
- Patch: in `daqiri_dpdk_engine.cpp` replace `const int dv_flow_en = interface_has_ecpri(name) ? 1 : 2;` with: read `getenv("DAQIRI_MLX5_DV_FLOW_EN")`; if set to `"1"` or `"2"` use it (log at INFO "mlx5 dv_flow_en forced to N by DAQIRI_MLX5_DV_FLOW_EN"), else the existing expression; any other value → log WARN and ignore.

- [ ] **Step 1: Write `build-dpdk.sh`**

Inputs: `<prefix>` absolute. Source dir `${TMPDIR:-/tmp}/dpdk-25.11`; download `https://fast.dpdk.org/rel/dpdk-25.11.tar.xz` (skip if present), apply the two DAQIRI patches from the DAQIRI checkout (`${TMPDIR:-/tmp}/daqiri-src-$(cat DAQIRI_REF)`, cloning if missing), configure with the spec'd meson flags, `ninja -C build -j$(nproc)`, `meson install -C build` (no sudo: prefix must be writable), then `PKG_CONFIG_PATH=<prefix>/lib/x86_64-linux-gnu/pkgconfig pkg-config --modversion libdpdk` must print `25.11`.

- [ ] **Step 2: Write the DAQIRI patch and apply step**

Create the patch with `git -C <daqiri-src> diff` after editing (keep it a single hunk, 5-line context). In `build-daqiri.sh`, after `git checkout "$ref"`: `git -C "$src" checkout -- . && for p in "$here"/patches/daqiri/*.patch; do git -C "$src" apply --check "$p" && git -C "$src" apply "$p"; done`.

- [ ] **Step 3: Write `probe-raw.cpp`**

Single file, links `daqiri::daqiri`; args as in Interfaces; `daqiri::daqiri_init_from_yaml_file`, `daqiri::get_engine_type`-independent output (`dq_yaml_engine_kind` logic is in the module; here print `cfg.common_.engine_type` via `engine_type_to_string`). `build-daqiri.sh` compiles it after installing DAQIRI: `g++ -std=c++17 probe-raw.cpp $(pkg-config --cflags --libs daqiri) -o <prefix>/bin/daqiri-probe-raw`.

- [ ] **Step 4: Build on picard**

Run:
```bash
export PATH=/usr/local/cuda/bin:$PATH
CI/daqiri/build-dpdk.sh $HOME/opt/dpdk 2>&1 | tail -3
CI/daqiri/build-daqiri.sh $HOME/opt/daqiri "dpdk ibverbs" $HOME/opt/dpdk 2>&1 | tail -3
nm -DC $HOME/opt/daqiri/lib/libdaqiri*.so | grep -oE 'daqiri::(IbverbsEngine|DpdkEngine|SocketEngine)::' | sort -u
grep -o 'DAQIRI_ENGINE_[A-Z]*=1' $HOME/opt/daqiri/lib/cmake/daqiri/*.cmake | sort -u
```
Expected: the three engines; `DAQIRI_ENGINE_DPDK=1`, `DAQIRI_ENGINE_IBVERBS=1`, `DAQIRI_ENGINE_SOCKET=1`.

- [ ] **Step 5: Probe with software loopback (no NIC)**

Create `tests/daqiri/resources/raw-sw-loopback-ibverbs.yaml` from DAQIRI's `examples/daqiri_bench_raw_sw_loopback.yaml` reduced to one `huge` RX region (`num_bufs: 4096, buf_size: 9216`), one RX queue id 0 on `cpu_core: 4`, one flow `udp_dst: 4991`, `master_core: 2`, `loopback: "sw"`.
Run: `$HOME/opt/daqiri/bin/daqiri-probe-raw tests/daqiri/resources/raw-sw-loopback-ibverbs.yaml`
Expected: `engine=ibverbs ... init=ok`, exit 0 (hugepages from Task 1 are required; if Task 1 is not applied yet, expected failure is the DAQIRI "hugepage pool" error and this step is re-run after Task 1).

- [ ] **Step 6: Rebuild libiio against the new prefix and rerun phase-1 suites**

Run: `cmake -S . -B build-daqiri -DCMAKE_PREFIX_PATH="$HOME/opt/daqiri;$HOME/opt/dpdk" <phase-1 flags from CI/daqiri/README.md> && cmake --build build-daqiri -j && ctest --test-dir build-daqiri -L '^daqiri$|daqiri-loopback' --output-on-failure | tail -3`
Expected: `100% tests passed`.

- [ ] **Step 7: CI image**

Edit the Dockerfile as in Files; bump nothing else. Run locally: `docker build -f CI/daqiri/Dockerfile -t libiio-daqiri-ci:dev2 . 2>&1 | tail -2` (expect success; size under 4.5 GB: `docker images libiio-daqiri-ci:dev2 --format '{{.Size}}'`). The image publish stays `workflow_dispatch` once the workflow is on master; until then push manually as in phase 1 (`docker tag … ghcr.io/tfcollins/libiio-daqiri-ci:latest && docker push …`, outward action: ask).

- [ ] **Step 8: README and commit**

`CI/daqiri/README.md`: DPDK pin and patches, the DAQIRI patch and why (`dv_flow_en=2` needs ConnectX-6 Dx+), `DAQIRI_MLX5_DV_FLOW_EN`, dev prefixes, probe usage, `DAQIRI_HW_*` env for hardware tests.
```bash
git add CI/daqiri/build-dpdk.sh CI/daqiri/build-daqiri.sh CI/daqiri/patches CI/daqiri/probe-raw.cpp CI/daqiri/Dockerfile CI/daqiri/README.md tests/daqiri/resources/raw-sw-loopback-ibverbs.yaml && git commit -s -m "CI: daqiri: build DPDK 25.11 and the DAQIRI raw engines, carry the dv_flow_en override patch"
```

---

### Task 3: L2/L3/L4 parser in the framing library with golden vectors

**Files:**
- Create: `daqiri-l2.h`, `daqiri-l2.c` (added to the `iio_daqiri_framing` OBJECT lib in the root `CMakeLists.txt` and to the module sources)
- Modify: `tests/daqiri/gen_golden.py` (new `l2-*.bin` vectors + manifest entries + `expected.h` constants)
- Create: `tests/daqiri/test_l2_parse.c`
- Modify: `tests/daqiri/CMakeLists.txt` (label `daqiri`)

**Interfaces:**
- Produces:
```c
enum dq_l2_status {
	DQ_L2_OK = 0, DQ_L2_TRUNCATED, DQ_L2_NOT_IPV4, DQ_L2_QINQ, DQ_L2_BAD_IHL,
	DQ_L2_NOT_UDP, DQ_L2_FRAGMENT, DQ_L2_BAD_IP_LEN, DQ_L2_BAD_UDP_LEN,
};
struct dq_l2_info {
	uint16_t payload_off;  /* 42, or 46 with one VLAN tag */
	uint16_t payload_len;  /* UDP length - 8, bounded by len */
	uint16_t udp_src, udp_dst, vlan_id; /* vlan_id 0 when untagged */
	uint32_t ip_src, ip_dst;            /* host order */
};
/* Pure function, no allocation; never reads beyond len. */
enum dq_l2_status dq_l2_parse(const uint8_t *p, uint32_t len, struct dq_l2_info *out);
const char *dq_l2_status_name(enum dq_l2_status s);
```
- Consumed by: Task 4 (`daqiri-vrt.c` raw path), Task 5 (sender builds headers that this must accept).

- [ ] **Step 1: Golden vectors**

In `gen_golden.py` add a `build_l2(payload, *, vlan=None, qinq=False, proto=17, ihl=5, frag=0, ip_len=None, udp_len=None, truncate=None)` helper (Ethernet 14 + optional 802.1Q 4 + IPv4 20 + UDP 8, checksums computed) and emit: `l2-plain.bin` (28-byte VRT prologue + 64 bytes payload), `l2-vlan.bin` (vlan 100), `l2-qinq.bin`, `l2-ipv6.bin` (ethertype 0x86DD), `l2-ihl6.bin`, `l2-tcp.bin`, `l2-frag.bin` (MF set), `l2-trunc-ip.bin` (ip_len = 2000, 120 bytes on wire), `l2-trunc-udp.bin` (udp_len = 2000), `l2-short.bin` (41 bytes). Manifest records expected status, `payload_off`, `payload_len`, ports, addresses.

- [ ] **Step 2: Failing test**

`test_l2_parse.c`: loads each vector from `tests/resources/vrt/`, asserts status and fields against `expected.h` constants; plus a fuzz loop: for every prefix length 0..len of `l2-plain.bin`, `dq_l2_parse` returns `DQ_L2_TRUNCATED` for len < 42 and otherwise never reports `payload_len > len - payload_off` (ASan build catches over-reads).
Run: `cmake --build build-daqiri --target test_l2_parse` → fails to link (`dq_l2_parse` undefined).

- [ ] **Step 3: Implement `daqiri-l2.c`**

Straight-line parse, big-endian field reads via the existing `daqiri-vrt-prologue.h` helpers; reject `ihl != 5`, `frag_off & 0x3FFF`, `proto != 17`, `ip_len < 28`, `ip_len > len - l2`, `udp_len < 8`, `udp_len > ip_len - 20`; `payload_len = udp_len - 8`.

- [ ] **Step 4: Run**

Run: `ctest --test-dir build-daqiri -R test_l2_parse --output-on-failure` → PASS. Also `ctest -L '^daqiri$'` all green.

- [ ] **Step 5: Commit**

```bash
git add daqiri-l2.h daqiri-l2.c CMakeLists.txt tests/daqiri/gen_golden.py tests/daqiri/test_l2_parse.c tests/daqiri/CMakeLists.txt tests/resources/vrt && git commit -s -m "daqiri: add a DAQIRI-free Ethernet/IPv4/UDP parser for raw engine queues"
```

---

### Task 4: Raw queues in the engine shim and the VRT poller (segments, L2 validation, hugepage blocks)

**Files:**
- Modify: `daqiri-private.h` (`dq_pkt`, `dq_burst`, new counters/attrs, `dq_vrt_packet_segs`)
- Modify: `daqiri-engine.cpp` (raw stream support, segments, port lookup by PCI address, hugetlb host alloc)
- Modify: `daqiri-vrt.c` (raw path, HDS, new counters), `daqiri-vrt-stream.[ch]` (`dq_vrt_rx_header`)
- Modify: `daqiri.c` (remove the `mem=device`/raw `-ENOTSUP` only where now supported; `daqiri_engine_kind` values `dpdk|ibverbs`)
- Create: `tests/daqiri/test_vrt_hds_split.c` (label `daqiri`, framing lib only)
- Modify: `doc/source/daqiri-backend.md` (counters table)

**Interfaces:**
- Produces:
```c
struct dq_pkt {
	const uint8_t *data;    /* segment 0: whole packet (single segment) or headers (HDS) */
	uint32_t len;
	const uint8_t *payload; /* segment 1 when HDS, else NULL */
	uint32_t payload_len;
};
struct dq_burst { struct dq_rxq *q; void *handle; unsigned nb_pkts; bool raw; };
/* raw queues: dq_rxq_open accepts StreamType::RAW on the dpdk and ibverbs engines */
/* dq_engine_get_port: <iface> matches InterfaceConfig name_ or address_ (PCI BDF or socket address) */
int dq_vrt_rx_header(struct dq_vrt_rx *rx, const uint8_t *hdr, size_t hdr_len,
		     size_t total_len, struct dq_vrt_data *out); /* same returns as dq_vrt_rx_packet, which becomes a wrapper */
void dq_vrt_packet_segs(struct dq_vrt_buffer *v, const uint8_t *hdr, uint32_t hdr_len,
			const uint8_t *payload, uint32_t payload_len);
```
- New counters/attrs: `daqiri_rx_l2_rejected` (sum), `daqiri_rx_l2_last_reason` (string, `dq_l2_status_name`), `daqiri_rx_vlan_packets`, `daqiri_rx_hds_packets`. `daqiri_engine_kind` now reports `dpdk` / `ibverbs` for raw YAMLs.
- `dq_mem_alloc(DQ_MEM_HOST)`: try `mmap(MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB|MAP_POPULATE)` rounded up to 2 MB when `size >= 2 MB`, fall back to the existing page-aligned + THP path; `dq_mem_free` remembers which (store the rounded size in the block). `daqiri_mem_domain` unchanged (`host`).

- [ ] **Step 1: Failing unit test for the HDS split**

`test_vrt_hds_split.c` builds a VRT packet from `tests/resources/vrt/` (an existing CI16 vector), splits it at boundaries {0, 10, 27, 28, 29, 70, len} and feeds `dq_vrt_rx_header(rx, hdr, hdr_len, total_len, &out)`; asserts: boundary < 28 → `-EPROTO`-class malformed (counter `malformed` increments, no crash); boundary ≥ 28 → identical `out` (stream id, pkt_count, payload length) to the single-buffer parse.
Run: build → fails (`dq_vrt_rx_header` undefined).

- [ ] **Step 2: Implement `dq_vrt_rx_header` and `dq_vrt_packet_segs`**

`dq_vrt_rx_header` validates the prologue from `hdr` only (needs `hdr_len >= prologue bytes`) and `size_words*4 == total_len`; `dq_vrt_rx_packet(rx, p, len, out)` = `dq_vrt_rx_header(rx, p, len, len, out)`. `dq_vrt_packet_segs` in `daqiri-vrt.c`: prologue from `hdr`, payload = `hdr + prologue .. hdr_len` followed by `payload[0..payload_len)` pushed as two `dq_asm_push` calls (sample alignment across the split must hold: assert `(hdr_len - prologue) % sample_size == 0`, else count `malformed`). The existing `dq_vrt_packet(v, data, len)` becomes `dq_vrt_packet_segs(v, data, len, NULL, 0)`.

- [ ] **Step 3: Run the unit test**

Run: `ctest --test-dir build-daqiri -R test_vrt_hds_split --output-on-failure` → PASS.

- [ ] **Step 4: Raw queues in `daqiri-engine.cpp`**

`dq_rxq_open`: allow `StreamType::RAW` when `engine_type` is DPDK or IBVERBS (`q->raw = true`); keep `-ENOTSUP` for ROCE/TCP. `dq_rxq_poll` sets `b->raw`. `dq_burst_pkts`: for each packet, `data = get_segment_packet_ptr(burst, 0, i)`, `len = get_segment_packet_length(burst, 0, i)`; if the queue has two regions, `payload = get_segment_packet_ptr(burst, 1, i)`, `payload_len = get_segment_packet_length(burst, 1, i)`, else NULL/0 (region count read once at open from `ifc->rx_.queues_[k].common_.mrs_.size()`). `dq_engine_get_port`: compare `iface` against `name_` and `address_`. `dq_yaml_engine_kind` unchanged.

- [ ] **Step 5: Raw path in the poller**

In the burst loop: if `b->raw`, `dq_l2_parse(pkt.data, pkt.len, &l2)`; on error `c.l2_rejected++`, remember the reason, `continue`; `vlan_packets++` when `l2.vlan_id`; then `dq_vrt_packet_segs(v, pkt.data + l2.payload_off, min(l2.payload_len, pkt.len - l2.payload_off), pkt.payload, pkt.payload_len)` (`hds_packets++` when `pkt.payload`). Socket queues: unchanged path through `dq_vrt_packet_segs(v, data, len, NULL, 0)`. Add the four attrs to `dq_vrt_attr_names` and the read switch.

- [ ] **Step 6: `daqiri.c`**

Keep `mem=device → -ENOTSUP`. Accept raw YAMLs (the `-ENOTSUP` for non-socket now comes only from the shim for ROCE/TCP). Static attr `daqiri,engine` already reports the kind.

- [ ] **Step 7: Regression**

Run: `cmake --build build-daqiri -j && ctest --test-dir build-daqiri -L '^daqiri$|daqiri-loopback' --output-on-failure | tail -3` → all green (socket path unchanged). `./format.sh` clean on touched files.

- [ ] **Step 8: Commit**

```bash
git add daqiri-private.h daqiri-engine.cpp daqiri-vrt.c daqiri-vrt-stream.c daqiri-vrt-stream.h daqiri.c tests/daqiri/test_vrt_hds_split.c tests/daqiri/CMakeLists.txt doc/source/daqiri-backend.md && git commit -s -m "daqiri: open raw engine queues; validate L2/L3/L4 and handle header-data split"
```

---

### Task 5: Raw VRT sender over DAQIRI TX (hardware loopback traffic source)

**Files:**
- Create: `tests/daqiri/vrt_sender_raw.cpp`
- Create: `tests/daqiri/resources/cx5-hw-loopback-ibverbs.yaml.in`, `tests/daqiri/resources/cx5-hw-loopback-ibverbs-hds.yaml.in`
- Create: `tests/daqiri/render_hw_yaml.sh`
- Modify: `tests/daqiri/CMakeLists.txt` (target `vrt_sender_raw`, only when `WITH_DAQIRI_BACKEND` and the DAQIRI target exports `DAQIRI_ENGINE_IBVERBS=1`)
- Modify: `tests/daqiri/vrt_sender.c` (move `build()` and the oracle/index generator into `vrt_sender_common.h` so both senders share the packet layout and drop/reorder oracle)

**Interfaces:**
- `vrt_sender_raw --yaml <path> --tx-port <iface-name> --tx-queue <id> --eth-dst <mac> --ip-src <a> --ip-dst <b> --udp-src <p> --udp-dst <p> [--vlan <id>]` plus the same stream options as `vrt_sender` (`--sid --samples --packets --duration --pps --drop-list --drop-every --drop-prob --seed --reorder-window --dup-every --burst-gap-ms --json`). Emits one VRT IF-data packet per DAQIRI TX packet with Ethernet/IPv4/UDP headers (checksums computed; `tx_eth_src` offload fills the source MAC), paced by `--pps`, and writes the same JSON oracle. Exit 0 after `wait_for_tx_idle`.
- `render_hw_yaml.sh <template> <out> [--hds]`: substitutes `@PCI@` (`$DAQIRI_HW_PCI`, default `0000:65:00.0`), `@MAC@` (read from `/sys/bus/pci/devices/@PCI@/net/*/address`), `@MASTER_CORE@`=2, `@RX_CORE@`=4, `@TX_CORE@`=6. Both YAMLs: `stream_type: "raw"`, `engine: "ibverbs"`, `loopback: "hw"`, `flow_isolation: true`, TX queue 0 (`huge`, 4096 × 9216) and RX queue 0 (`huge`, 32768 × 9216) with flow `udp_dst: 4991 → queue 0`; the HDS variant uses RX regions `hdr` (`huge`, 32768 × 70) then `data` (`huge`, 32768 × 9216).
- Sender and receiver are **separate processes** on the same port (sender: its own `daqiri_init` from the same YAML but only using TX; receiver: libiio module). If the spike in Step 4 shows the NIC does not deliver cross-process loopback, fall back to the ruling recorded there.

- [ ] **Step 1: Refactor the shared packet builder (no behaviour change)**

Extract `build()`, `packet_len()`, `dropped()`, `gen_*` and the oracle writer from `vrt_sender.c` into `vrt_sender_common.h` (static inline). Run: `ctest -L daqiri-loopback` still green.

- [ ] **Step 2: Write `vrt_sender_raw.cpp`**

Per burst: `create_tx_burst_params` → `set_header(msg, port, queue, batch, 1)` → `is_tx_burst_available` (else `free_tx_metadata`, sleep 50 µs) → `get_tx_packet_burst` → for each packet: `get_segment_packet_ptr(msg, 0, i)`, write 42/46 header bytes then the VRT packet from the common builder, `set_packet_lengths(msg, i, {len})` → `send_tx_burst`. Pace by bytes/sec derived from `--pps`.

- [ ] **Step 3: Build**

Run: `cmake --build build-daqiri --target vrt_sender_raw` → links against `daqiri::daqiri`.

- [ ] **Step 4: Spike A — ibverbs hardware loopback on the ConnectX-5 (two processes)**

Run on picard (after Task 1):
```bash
tests/daqiri/render_hw_yaml.sh tests/daqiri/resources/cx5-hw-loopback-ibverbs.yaml.in /tmp/cx5.yaml
$HOME/opt/daqiri/bin/daqiri-probe-raw /tmp/cx5.yaml --seconds 8 --port loopback_port --queue 0 &
sleep 2; build-daqiri/tests/daqiri/vrt_sender_raw --yaml /tmp/cx5.yaml --tx-port loopback_port --tx-queue 0 --eth-dst $(cat /sys/bus/pci/devices/0000:65:00.0/net/*/address) --ip-src 192.0.2.1 --ip-dst 192.0.2.2 --udp-src 4991 --udp-dst 4991 --packets 100000 --pps 50000; wait
```
Expected: the probe's `packets=` totals sum to exactly 100000 while the link is down. If the probe sees zero packets, add a `--self-tx` mode to `daqiri-probe-raw` (TX and RX in one process, DAQIRI's own bench shape) and rerun, to separate "the NIC loops back" from "a second process receives it". Ruling to record in the ledger: cross-process delivery works → continue as planned; only single-process works → a test-only hook inside `libiio-daqiri` is not acceptable upstream, so Task 6 shrinks to the single-process probe gate (`--self-tx` with VRT packets, counters checked by the probe) and the `100g-vrt` tier becomes the only libiio-level data-path gate; amend Tasks 6 and 9 accordingly.

- [ ] **Step 5: Commit**

```bash
git add tests/daqiri/vrt_sender_raw.cpp tests/daqiri/vrt_sender_common.h tests/daqiri/vrt_sender.c tests/daqiri/resources/cx5-hw-loopback-ibverbs.yaml.in tests/daqiri/resources/cx5-hw-loopback-ibverbs-hds.yaml.in tests/daqiri/render_hw_yaml.sh tests/daqiri/CMakeLists.txt && git commit -s -m "tests: daqiri: raw VRT sender over DAQIRI TX for ConnectX hardware loopback"
```

---

### Task 6: Hardware loopback tests on picard (`daqiri-hw`)

**Files:**
- Modify: `tests/daqiri/test_vrt_loopback.c` (env `VRT_SENDER_ARGS` prefix, env `DAQIRI_URI_EXTRA`, case selection by `--case`)
- Create: `tests/daqiri/run_hw_loopback.sh` (renders YAMLs, starts `iiod-emu` with the phase-1 XML, runs the cases, writes `hw-results.json` and `ethtool -S` before/after)
- Modify: `tests/daqiri/CMakeLists.txt` (tests `daqiri_hw_loopback_<case>`, label `daqiri-hw`, `DISABLED` unless `DAQIRI_HW_PCI` is set at configure time)
- Modify: `tests/daqiri/README.md`

**Interfaces:**
- Cases (each a ctest): `clean` (200 blocks, zero loss, counters == oracle), `drop_list` (lossy=0: block errors == oracle gaps), `lossy` (lossy=1: zero-filled bytes == oracle), `reorder`, `duplicate`, `vlan` (sender `--vlan 100`, `daqiri_rx_vlan_packets == packets`), `jumbo` (payload 8192 bytes, MTU 9000: zero loss), `hds` (HDS YAML, `daqiri_rx_hds_packets == packets`, byte-equal blocks), `hds_short_split` (HDS YAML with `hdr` buf_size 40: every packet malformed, no crash, zero blocks completed), `rate` (60 s at the sender's maximum `--pps 0` = unpaced, report Gbps, `daqiri_rx_lost_packets == 0` and `rx_out_of_buffer` delta == 0 from `ethtool -S`).
- URI used: `daqiri:127.0.0.1:30431,framing=vrt,yaml=<rendered>,iface=loopback_port,rxq=0,sid=0x01000000` (+`lossy=1` for `lossy`).

- [ ] **Step 1: Parameterize `test_vrt_loopback.c`**

Add `--case <name>` (default all socket cases as today), read `VRT_SENDER_ARGS` (prepended to the sender argv) and `DAQIRI_URI_EXTRA` (appended to the URI). Socket suite unchanged: `ctest -L daqiri-loopback` green.

- [ ] **Step 2: Write `run_hw_loopback.sh` and register the ctests**

Each ctest = `run_hw_loopback.sh <case>`; `rate` has `TIMEOUT 180`. `ethtool -S enp101s0np0 | grep -E 'rx_out_of_buffer|rx_packets_phy|rx_discards_phy'` captured before/after into `hw-results.json`.

- [ ] **Step 3: Run on picard**

Run: `cmake -S . -B build-daqiri -DDAQIRI_HW_PCI=0000:65:00.0 … && ctest --test-dir build-daqiri -L daqiri-hw --output-on-failure | tail -14`
Expected: 10/10 passed; `rate` reports ≥ 20 Gbps payload (single RX core, host memory; record the number in the README as the picard baseline).

- [ ] **Step 4: Commit**

```bash
git add tests/daqiri/test_vrt_loopback.c tests/daqiri/run_hw_loopback.sh tests/daqiri/CMakeLists.txt tests/daqiri/README.md && git commit -s -m "tests: daqiri: ConnectX hardware loopback suite (label daqiri-hw)"
```

---

### Task 7: Spike B — DPDK engine on the ConnectX-5, engine decision, bench NIC stats

**Files:**
- Create: `tests/daqiri/resources/cx5-dpdk-rx.yaml.in` (`engine: "dpdk"`, no loopback, RX queue 0 `huge` 32768 × 9216, flow `udp_dst: 4991`, `master_core: 2`, `cpu_core: 4`)
- Modify: `utils/iio_daqiri_bench.c` (`--nic-stats <ifname>`: `ethtool -S` deltas for `rx_out_of_buffer`, `rx_discards_phy`, `rx_packets_phy` printed and in `perf.json`)
- Modify: `man/iio_daqiri_bench.1.in`
- Create: `tests/hardware/daqiri/README.md` (engine decision section; the rest of the file is filled in Task 9)

- [ ] **Step 1: DPDK init without the override**

Run: `tests/daqiri/render_hw_yaml.sh tests/daqiri/resources/cx5-dpdk-rx.yaml.in /tmp/cx5-dpdk.yaml && $HOME/opt/daqiri/bin/daqiri-probe-raw /tmp/cx5-dpdk.yaml; echo rc=$?`
Expected: non-zero with the mlx5 PMD rejecting `dv_flow_en=2` (record the exact message).

- [ ] **Step 2: DPDK init with the override**

Run: `DAQIRI_MLX5_DV_FLOW_EN=1 $HOME/opt/daqiri/bin/daqiri-probe-raw /tmp/cx5-dpdk.yaml --seconds 3 --port rx_port --queue 0; echo rc=$?`
Expected: `init=ok`, port started with link down, `packets=0`, rc=0. If the EAL needs `--iova-mode=va` or the uverbs group permission blocks it, record and fix in Task 1's play (re-run).

- [ ] **Step 3: libiio through DPDK**

Start `iiod-emu` on 127.0.0.1:30431 with the phase-1 XML the way `run_loopback.sh` does, then run: `TESTS_API_URI="daqiri:127.0.0.1:30431,framing=vrt,yaml=/tmp/cx5-dpdk.yaml,iface=rx_port" DAQIRI_MLX5_DV_FLOW_EN=1 build-daqiri/tests/daqiri/test_daqiri_open` (phase-1 tool: open + enable + one dequeue with a 2 s timeout).
Expected: buffer opens, the buffer attr `daqiri_engine_kind` reads `dpdk`, dequeue returns `-ETIMEDOUT` (no traffic), clean close, exit 0.

- [ ] **Step 4: Decision**

Write `tests/hardware/daqiri/README.md` "Engine decision (picard-cx5, 2026-10-xx)": ibverbs = default for the tier (loopback-testable today, no patch); DPDK = validated at init/flow level with `DAQIRI_MLX5_DV_FLOW_EN=1`, data-path validation deferred until the VCU118 framer streams; socket = fallback. Include the exact probe outputs.

- [ ] **Step 5: Bench NIC stats + commit**

Implement `--nic-stats`, test: `iio_daqiri_bench -u "daqiri:127.0.0.1:30431,framing=vrt,yaml=/tmp/cx5.yaml,iface=loopback_port" -d <dev> -T 10 --nic-stats enp101s0np0 -j /tmp/perf.json` with `vrt_sender_raw` running → `perf.json` has `nic.rx_out_of_buffer_delta == 0`.
```bash
git add tests/daqiri/resources/cx5-dpdk-rx.yaml.in utils/iio_daqiri_bench.c man/iio_daqiri_bench.1.in tests/hardware/daqiri/README.md && git commit -s -m "daqiri: DPDK spike on ConnectX-5, engine decision, bench NIC stats"
```

---

### Task 8: picard as labgrid place `picard-cx5` (exporter, tags, runner)

**Files (labgrid-plugins repo):**
- Create: `examples/lg_picard_cx5_exporter.yaml`
- Modify: `exporter_configs/place-tags.home-lab.yaml` (`picard-cx5` entry; add `exporter-address`/`exporter-hostname` to `nuc` and `tron`)
- Modify: `docs/` page listing home-lab places (the one `seed-place-tags.sh` header points at)
**Files (infra repo):**
- Create: `ansible/playbooks/deploy-labgrid-exporter-picard.yml` (venv `~/labgrid/venv` with labgrid 26.0 + `labgrid-plugins` pinned to the current main SHA, user unit `labgrid-exporter.service`, `loginctl enable-linger tcollins`; no sudo except linger)
- Modify: `HOSTS.md` (`picard-cx5` row in the rigs table)

**Interfaces:**
- Exporter YAML: group `cx5` with `NetworkInterface` (`cls: NetworkInterface`, `ifname: enp101s0np0`) and `NetworkService` (`address: 10.0.0.101`, `username: tcollins`); run as `labgrid-exporter -c 10.0.0.41:20408 -n picard --hostname 10.0.0.101 ~/labgrid/lg_picard_cx5_exporter.yaml`.
- Place `picard-cx5`: match `picard/cx5/*`; tags `nic=connectx5 speed=100g peer=nuc pci=0000:65:00.0 driver=mlx5_core link-address=192.0.2.2/24 mtu=9000 runner=hw-picard exporter-address=10.0.0.101 exporter-hostname=picard`.
- Runner: `adi-lg setup-runner --scope repo:tfcollins/libiio --name picard-libiio --labels hw-picard --dir ~/actions-runner-libiio --non-interactive` (user unit `actions-runner-libiio.service`).

- [ ] **Step 1: Exporter config + play; deploy**

Run: `cd ~/dev/infra && ansible-playbook ansible/playbooks/deploy-labgrid-exporter-picard.yml -K` (linger needs sudo; everything else user-level).
Expected: `systemctl --user status labgrid-exporter` active on picard.

- [ ] **Step 2: Create the place and tags on the coordinator (from nemo)**

Run: `ssh nemo.local 'export LG_COORDINATOR=10.0.0.41:20408; C=~/venv_WORKING/bin/labgrid-client; $C -p picard-cx5 create; $C -p picard-cx5 add-match "picard/cx5/*"; $C -p picard-cx5 show'`, then seed tags with `seed-place-tags.sh` after editing the manifest.
Expected: `show` lists `NetworkInterface` and `NetworkService` resources and the tags.

- [ ] **Step 3: Acquire/release cycle**

Run (nemo): `$C -p picard-cx5 acquire && $C -p picard-cx5 show | grep acquired && $C -p picard-cx5 release && $C -p picard-cx5 show | grep 'acquired: None'`
Expected: both lines.

- [ ] **Step 4: Runner (manual gate: needs a token with repo admin)**

Run on picard: the `adi-lg setup-runner` line above with `--token "$(gh auth token)"`; verify `gh api repos/tfcollins/libiio/actions/runners --jq '.runners[].labels[].name' | grep hw-picard`.

- [ ] **Step 5: Commits (two repos)**

```bash
cd ~/dev/labgrid-plugins && git add examples/lg_picard_cx5_exporter.yaml exporter_configs/place-tags.home-lab.yaml docs && git commit -s -m "exporter: add picard-cx5 (ConnectX-5 100G receiver) place"
cd ~/dev/infra && git add ansible/playbooks/deploy-labgrid-exporter-picard.yml HOSTS.md && git commit -s -m "ansible: labgrid exporter on picard for place picard-cx5"
```

---

### Task 9: 100G tier harness (two-place launcher, loopback tier, nightly workflow)

**Files:**
- Create: `tests/hardware/daqiri/{requirements.txt,pytest.ini,launcher.py,conftest.py,receiver.py,perf.py,test_common.py,test_tier_vrt_100g.py,test_launcher_cleanup.py}`
- Modify: `tests/hardware/daqiri/README.md`
- Create: `.github/workflows/daqiri-hil.yml`
- Create: `tests/hardware/daqiri/baselines/picard-cx5-loopback.json`

**Interfaces:**
- `launcher.py --tier {100g-loopback,100g-vrt} --build <abs> --evidence <abs> [--duration 60] [--coord 10.0.0.41:20408] [--wait 600]`: preflight (venv pins, `libiio-daqiri.so` sha256, coordinator reachable, `picard-cx5` tags validated incl. `exporter-address` present and `exporter-hostname` not a bare name, `daqiri-probe-raw` init OK on the rendered YAML) → acquire `picard-cx5` via `reserve_and_acquire(coord, {"nic": "connectx5"}, wait=...)` → for `100g-vrt` also `adi-lg request --part daq3 --carrier vcu118 --mode reserve --wait <s> --run "pytest …"` (the board leg; sets `v49_udp_*` on the framer, `v49_enable=1`) → tier tests with `--junitxml` → `perf.json` → cleanup legs each with its own timeout (`v49_enable=0` when a board was used, release `picard-cx5`, readback `acquired: None`) → `cleanup.json`; non-zero exit if any leg false.
- `test_tier_vrt_100g.py`: tier `100g-loopback` runs the Task 6 cases through `run_hw_loopback.sh` plus `rate` for `--duration` seconds with `receiver.py`'s NIC gate (`rx_out_of_buffer` delta 0); tier `100g-vrt` requires `v49_udp_status` streaming and fails (not skips) otherwise, then ramp continuity 60 s, zero loss.
- `daqiri-hil.yml`: `workflow_dispatch` (tier, duration, ref) + nightly 04:00 UTC, `concurrency: daqiri-hil`, job `hil` on `[self-hosted, hw-picard]`, `timeout-minutes: 60`, `continue-on-error: true`, "require non-skipping JUnit evidence" step, artifacts upload.

- [ ] **Step 1: Failing cleanup test**

`test_launcher_cleanup.py`: monkeypatch `launcher.acquire_receiver` / `release_receiver` with recorders, make the tier runner raise, assert `release_receiver` was called, `cleanup.json` exists with `receiver_released: true`, and `launcher.main` returned non-zero. Run → ImportError.

- [ ] **Step 2: Implement `launcher.py` and friends; pass the test**

- [ ] **Step 3: Loopback tier end to end on picard**

Run: `python3 tests/hardware/daqiri/launcher.py --tier 100g-loopback --build $PWD/build-daqiri --evidence /tmp/ev --duration 60`
Expected: junit all pass, `cleanup.json` all true, coordinator shows `picard-cx5` free, `perf.json` written; copy as `baselines/picard-cx5-loopback.json`.

- [ ] **Step 4: Workflow dispatch**

After the runner exists: `gh workflow run daqiri-hil.yml --ref feature/daqiri-backend -f tier=100g-loopback` (only possible once the workflow is on master; otherwise document the local launcher command as the interim).

- [ ] **Step 5: Commit**

```bash
git add tests/hardware/daqiri .github/workflows/daqiri-hil.yml && git commit -s -m "tests/hardware: daqiri 100G tier launcher, loopback tier, nightly HIL workflow"
```

---

### Task 10: Documentation

**Files:**
- Modify: `doc/source/daqiri-backend.md` (new section "Raw engines (ibverbs, DPDK)": YAML for a ConnectX port, `iface=` by name or PCI, L2 validation and counters, VLAN, HDS, hugepages, `DAQIRI_MLX5_DV_FLOW_EN`, hardware loopback testing, limitations: one port per process, link required for DPDK data, `mem=device` pending)
- Modify: `README_BUILD.md` (DPDK prefix in `CMAKE_PREFIX_PATH`)
- Modify: `tests/daqiri/README.md` (hardware suite), `tests/hardware/daqiri/README.md` (manual 100G bring-up: ansible, builds, place, runner, launcher; the vrt49 dependency)
- Modify: `docs/superpowers/specs/2026-10-19-daqiri-backend-design.md` (M5 status note + facts section pointer)

- [ ] **Step 1: Write; `codespell` and the docs build clean**

Run: `codespell doc/source/daqiri-backend.md tests/daqiri/README.md tests/hardware/daqiri/README.md README_BUILD.md` → nothing.

- [ ] **Step 2: Commit**

```bash
git add doc/source/daqiri-backend.md README_BUILD.md tests/daqiri/README.md tests/hardware/daqiri/README.md docs/superpowers/specs/2026-10-19-daqiri-backend-design.md && git commit -s -m "doc: daqiri raw engines, hardware loopback and the 100G tier"
```

---

## Verification (whole plan)

- Software (any host, CI image): `ctest -L '^daqiri$'` (now incl. `test_l2_parse`, `test_vrt_hds_split`), `ctest -L daqiri-loopback`, `api` via the daqiri URI, `symbol_map`; `daqiri.yml` green.
- picard: `ctest -L daqiri-hw` 10/10; `daqiri-probe-raw` DPDK init OK with the override; `launcher.py --tier 100g-loopback` green with `cleanup.json` all true and the place free afterwards.
- Lab state: `labgrid-client -p picard-cx5 show` lists the NIC resource and tags; `hw-picard` runner online for `tfcollins/libiio`.
- Deferred to vrt49: `100g-vrt` tier (needs the VCU118+DAQ3 bitstream from PR #50); DPDK data path at line rate; M6 `mem=device` through HDS segment 1.
