# DAQIRI backend phase 1 (software) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A loadable `libiio-daqiri.so` that streams from unchanged iiod boards (`framing=iiod`) and from VITA 49.2 UDP senders through the DAQIRI socket engine (`framing=vrt`), with loss accounting, unit tests, loopback tests and a CUDA-capable CI job.

**Architecture:** C module (`daqiri.c`, `daqiri-mgmt.c`) that reuses iiod-client for management and for the iiod framing; a DAQIRI-free framing library (`daqiri-vrt-prologue.h`, `daqiri-vrt-stream.c`, `daqiri-asm.c`) that parses prologues, detects gaps and assembles blocks; one C++ file (`daqiri-engine.cpp`) that owns every DAQIRI call behind `daqiri-private.h`; one RX poller thread per VRT buffer in `daqiri-vrt.c`.

**Tech Stack:** C99 (module, tests), C++17 (DAQIRI shim), NVIDIA DAQIRI socket engine, CMake, ctest, iiod-emu, Python 3 (golden vector generator), Docker/GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-19-daqiri-backend-design.md`

## Global Constraints

- Linux only; module requires `BUILD_SHARED_LIBS=ON` and `WITH_MODULES=ON`; `WITH_DAQIRI_BACKEND` defaults to OFF.
- `libiio.so` must never link C++ runtime, CUDA or DAQIRI (`ldd` check in Task 2).
- Only `daqiri-engine.cpp` may include `<daqiri/daqiri.h>`; everything crossing into C uses `daqiri-private.h` with `extern "C"`, fixed-width ints, negative-errno returns, no exceptions escaping.
- Framing library (`daqiri-vrt-prologue.h`, `daqiri-vrt-stream.c`, `daqiri-asm.c`) has no DAQIRI/CUDA/libiio dependency and builds whenever `WITH_TESTS=ON` on POSIX.
- Every commit is `git commit -s`; code formatted with the repo's `format.sh` (clang-format + cmake-format).
- The DAQIRI pin is `CI/daqiri/DAQIRI_REF` (main HEAD SHA at Task 1 time); all DAQIRI claims are re-verified when it is bumped.
- Kuiper 2023_R2_P1 boards run libiio 0.25 iiod (ASCII only): `framing=iiod` must work through `readbuf`/`writebuf`.
- URI grammar: `daqiri:<host>[:port][,key=value]*`; unknown keys → `-EINVAL`.
- VITA 49.2 wire: big-endian; 4-bit `pkt_count`; CI16 = I in the high 16 bits of each 32-bit word.
- No `tfcollins`-specific paths in source; only workflow files may reference `ghcr.io/tfcollins`.

## Review Focus

1. A datagram shorter than its header claims (`size_words*4 > len`) must count as malformed and never read past the buffer — pinned in Task 3 (`test_vrt_prologue`, truncated vectors).
2. Losing exactly 16·k packets with no timestamp must not be reported as zero loss silently — pinned in Task 4 (`test_vrt_gap`, "ambiguous" case asserts `gap_events` increments and `lost_packets` stays 0 only when `gap_ts` is off and the result flags `DQ_VRT_GAP_UNKNOWN`).
3. An app that stops enqueuing blocks while the sender keeps sending must see `daqiri_rx_dropped_no_block` grow and must never have a dequeued block overwritten — pinned in Task 7 (`test_vrt_loopback`, "slow consumer" case).
4. `iio_context_destroy` while a VRT buffer is open must join the poller and not crash on module unload — pinned in Task 6 (`test_daqiri_lifecycle`).
5. A board running v0.25 iiod (no `BINARY` support) under `framing=iiod` must stream through the ASCII `readbuf` path instead of failing with `-ENOSYS` — pinned by the manual gate in Task 5 step 8 (no software v0 peer exists; iiod-emu is v1-only).

---

### Task 1: DAQIRI dev build on picard and pinned reference

**Files:**
- Create: `CI/daqiri/DAQIRI_REF` (one line: 40-char SHA)
- Create: `CI/daqiri/build-daqiri.sh`
- Create: `CI/daqiri/README.md`

**Interfaces:**
- Produces: `build-daqiri.sh <prefix> [<engine-list>]` clones `https://github.com/NVIDIA/daqiri` at `DAQIRI_REF` into a scratch dir, builds with `-DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_BUILD_EXAMPLES=OFF -DDAQIRI_ENGINE="<engine-list>" -DCMAKE_INSTALL_PREFIX=<prefix>` and installs. Default engine list is empty (sockets only). Later tasks consume `<prefix>/lib/cmake/daqiri` via `CMAKE_PREFIX_PATH`.

- [ ] **Step 1: Record the pin**

Run: `git ls-remote https://github.com/NVIDIA/daqiri HEAD | cut -f1 > CI/daqiri/DAQIRI_REF && cat CI/daqiri/DAQIRI_REF`
Expected: a 40-char hex SHA.

- [ ] **Step 2: Write `CI/daqiri/build-daqiri.sh`**

POSIX sh, `set -eu`; args `prefix` (required, absolute), `engines` (optional, default ""); clone to `${TMPDIR:-/tmp}/daqiri-src-$REF` if absent, `git -C ... checkout $REF`, `git submodule update --init --recursive`, cmake configure/build/install with `-j$(nproc)`; print `pkg-config --modversion daqiri` using `PKG_CONFIG_PATH=$prefix/lib/pkgconfig` at the end.

- [ ] **Step 3: Build on picard into a user prefix**

Run: `CI/daqiri/build-daqiri.sh $HOME/opt/daqiri`
Expected: last line prints `2026.7.0` (or the version in `VERSION`); `ls $HOME/opt/daqiri/lib/cmake/daqiri/daqiriConfig.cmake` exists. Note the CUDA toolkit is found at `/usr/local/cuda-13.4`; if CMake cannot find it, pass `-DCUDAToolkit_ROOT=/usr/local/cuda-13.4` through an env var `DAQIRI_CMAKE_EXTRA` that the script appends.

- [ ] **Step 4: Probe socket-engine init/shutdown**

Create `CI/daqiri/probe-socket.cpp` (kept, ~40 lines): parse `tests/daqiri/resources/socket-udp-loopback.yaml` (written in Task 6; for this step use an inline YAML string with one `udp://127.0.0.1:25101` server interface, one RX queue, one `host` memory region), `daqiri_init`, print `get_port_id("data")`, `shutdown()`, exit 0. Build: `g++ -std=c++17 $(PKG_CONFIG_PATH=$HOME/opt/daqiri/lib/pkgconfig pkg-config --cflags --libs daqiri) CI/daqiri/probe-socket.cpp -o /tmp/probe`.
Run: `/tmp/probe`; Expected: exit 0, port id `0`. Then run it twice in one process? No: record in `CI/daqiri/README.md` whether a second `daqiri_init` after `shutdown()` succeeds (run the probe with an env `PROBE_TWICE=1` that repeats init/shutdown). This decides nothing in this plan (engine is pinned for process lifetime) but is recorded for the spec.

- [ ] **Step 5: Document and commit**

`CI/daqiri/README.md`: what the pin is, how to build, the probe result (including the init-twice answer).
```bash
git add CI/daqiri && git commit -s -m "ci: pin DAQIRI and add dev build script"
```

---

### Task 2: Module skeleton with management plane

**Files:**
- Create: `daqiri.c`, `daqiri-mgmt.c`, `daqiri-private.h`, `daqiri-engine.cpp`
- Modify: `CMakeLists.txt` (after the network block ending near line 839), `cmake/Install.cmake:200-212`, `iio-config.h.cmakein` (next to `WITH_EMU_BACKEND_DYNAMIC`), `zephyr/CMakeLists.txt` (backend-off list), `README_BUILD.md` (option table)
- Create: `tests/daqiri/CMakeLists.txt`, `tests/daqiri/with_emu.sh`, `tests/daqiri/test_daqiri_context.c`
- Modify: `tests/CMakeLists.txt` (`add_subdirectory(daqiri)`)

**Interfaces:**
- Produces (`daqiri-private.h`, C):
  ```c
  enum dq_framing { DQ_FRAMING_VRT = 0, DQ_FRAMING_IIOD = 1 };
  enum dq_mem { DQ_MEM_HOST = 0, DQ_MEM_PINNED = 1, DQ_MEM_DEVICE = 2 };
  struct dq_opts { char yaml[4096]; char iface[64]; int rxq; enum dq_framing framing;
                   enum dq_mem mem; int gpu; bool lossy; bool swap; bool gap_ts;
                   uint32_t sid, sid_mask; uint16_t data_port; unsigned poll_us; };
  int dq_opts_parse(struct dq_opts *o, const char *list, const struct iio_context_params *p); /* list = text after first ',' or NULL; applies defaults: framing=vrt, mem=host, gpu=0, lossy=0, swap=1, gap_ts=1, poll=50, yaml=$IIO_DAQIRI_YAML */
  struct dq_mgmt { int fd; int cancel_fd; const struct iio_context_params *params; };  /* this IS struct iiod_client_pdata for this module */
  int  dq_mgmt_connect(struct dq_mgmt *m, const char *host, const char *port, int timeout_ms); /* getaddrinfo + non-blocking connect; TCP_NODELAY; eventfd */
  void dq_mgmt_close(struct dq_mgmt *m);
  extern const struct iiod_client_ops dq_mgmt_ops;   /* read/write via poll(), cancel via eventfd + shutdown(fd) */
  /* C++ side */
  int  dq_yaml_validate(const char *path, char *err, size_t errlen);  /* parse only */
  const char *dq_engine_version(void);
  const char *dq_yaml_engine_kind(const char *path);  /* "socket" | "dpdk" | "ibverbs" | "roce" | "unknown" */
  ```
- Produces: exported symbol `const struct iio_backend iio_daqiri_backend` (`name = "daqiri"`, `uri_prefix = "daqiri:"`, `default_timeout_ms = 5000`).
- Produces: `tests/daqiri/with_emu.sh <xml> <cmd...>`: starts `iiod-emu -p 30431 emu:<xml>` from `$IIOD_EMU` (CMake passes `$<TARGET_FILE:iiod-emu>`), waits with `nc -z 127.0.0.1 30431` up to 10 s, runs `<cmd...>`, kills the emu, propagates the exit code.

- [ ] **Step 1: Write the failing context test**

`tests/daqiri/test_daqiri_context.c` (test_framework.h style; URI from `TESTS_DAQIRI_URI`, default `daqiri:127.0.0.1,framing=iiod`; the comparison URI from `TESTS_IP_URI`, default `ip:127.0.0.1`):
```c
test_has_backend:   TEST_ASSERT(iio_has_backend(NULL, "daqiri"), "module found");
test_bad_key:       ctx = iio_create_context(NULL, "daqiri:127.0.0.1,bogus=1"); TEST_ASSERT_EQ(iio_err(ctx), -EINVAL, ...);
test_bad_framing:   "daqiri:127.0.0.1,framing=xyz" -> -EINVAL
test_topology:      ctx_d = create(daqiri uri); ctx_i = create(ip uri);
                    TEST_ASSERT_EQ(iio_context_get_devices_count(ctx_d), iio_context_get_devices_count(ctx_i), ...);
                    TEST_ASSERT_STR_EQ(iio_context_get_xml(ctx_d) device section ..., ) -> simpler: compare iio_context_get_name(ctx_d)=="daqiri"; for each device compare iio_device_get_id and channel counts.
test_ctx_attrs:     attr = iio_context_find_attr(ctx_d, "daqiri,framing"); TEST_ASSERT_STR_EQ(iio_attr_get_static_value(attr), "iiod", ...); also "uri" == the uri string, "daqiri,version" non-empty.
test_attr_roundtrip: read "sampling_frequency" (or first device attr) via both contexts, assert equal strings.
test_ping/timeout:  iio_context_set_timeout(ctx_d, 1000) == 0.
main(): run all, print summary, return test_failed ? 1 : 0
```
Registered in `tests/daqiri/CMakeLists.txt` as `add_test(NAME daqiri_context COMMAND with_emu.sh ${XMLS}/fmcomms2.xml $<TARGET_FILE:test_daqiri_context>)`, label `daqiri-loopback`, with env `IIOD_EMU=$<TARGET_FILE:iiod-emu>`.

Module lookup: libiio has no runtime override for the module directory (`IIO_MODULES_DIR` is baked in at configure time, `CMakeLists.txt:271`). Add an advanced cache variable `IIO_MODULES_DIR_OVERRIDE`; when set, it replaces the computed `IIO_MODULES_DIR`, so test builds point it at `${CMAKE_BINARY_DIR}/` where `libiio-daqiri.so` is produced. Document it in `CI/daqiri/README.md`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake -S . -B build -DWITH_MODULES=ON -DWITH_DAQIRI_BACKEND=ON -DWITH_TESTS=ON -DWITH_EMU_BACKEND=ON -DWITH_IIOD_EMU=ON -DCMAKE_PREFIX_PATH=$HOME/opt/daqiri -DIIO_MODULES_DIR_OVERRIDE=$PWD/build/ && cmake --build build -j`
Expected: configure fails (option/targets not yet defined) — that is the failing state.

- [ ] **Step 3: Add the CMake wiring**

In `CMakeLists.txt` after the emu backend block: `iio_option(WITH_DAQIRI_BACKEND "Build the DAQIRI backend module (requires DAQIRI, C++17, CUDA toolkit)" OFF "POSIX")`; inside: FATAL_ERROR unless Linux + `BUILD_SHARED_LIBS` + `WITH_MODULES`; `enable_language(CXX)`; `find_package(daqiri CONFIG REQUIRED)`; `find_package(CUDAToolkit REQUIRED)`; `find_package(Threads REQUIRED)`; `add_library(iio-daqiri SHARED daqiri.c daqiri-mgmt.c daqiri-engine.cpp)`; `target_compile_features(iio-daqiri PRIVATE cxx_std_17)`; `target_link_libraries(iio-daqiri PRIVATE iio daqiri::daqiri CUDA::cudart Threads::Threads)`; `set_target_properties(iio-daqiri PROPERTIES PREFIX lib C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)`; `set(IIOD_CLIENT 1)`; `set(NEED_LIBXML2 1)`; `toggle_iio_feature("${WITH_DAQIRI_BACKEND}" daqiri)`; `IIO_MODULES_DIR_OVERRIDE` cache var applied where `IIO_MODULES_DIR` is computed (line ~271). Install rule in `cmake/Install.cmake`. `#cmakedefine01 WITH_DAQIRI_BACKEND` in `iio-config.h.cmakein`. `WITH_DAQIRI_BACKEND=0` in `zephyr/CMakeLists.txt`. `tests/daqiri/CMakeLists.txt` guarded by `if (WITH_DAQIRI_BACKEND AND WITH_IIOD_EMU)` for the loopback tests (unit tests come in Task 3).

- [ ] **Step 4: Implement `daqiri-mgmt.c`**

`dq_mgmt_connect`, `dq_mgmt_close`, `dq_mgmt_ops` with `read`/`write` using `poll()` on `{fd, cancel_fd}` and `timeout_ms` (-1 = infinite), returning `-ETIMEDOUT`, `-EPIPE` on EOF, `-EINTR`→retry; `cancel` writes 1 to the eventfd and `shutdown(fd, SHUT_RDWR)`; `read_line = NULL`. Salvage the `wait_socket`/`management_io` shape from `git show origin/design/daqiri-backend:daqiri.c`.

- [ ] **Step 5: Implement `daqiri.c` create/shutdown/attrs**

`dq_opts_parse` (strtok_r on `,` then `=`; `yaml` max 4095 bytes; `framing`, `mem`, numbers via `strtoul` with full-consumption check). `daqiri_create_context(params, uri)`: split host part at first `,`; host/port parse copied from `network.c:663-694` (no DNS-SD branch); `dq_mgmt_connect`; `iiod_client_new(params, (struct iiod_client_pdata *)&pdata->mgmt, &dq_mgmt_ops)`; if `opts.yaml[0]` then `dq_yaml_validate` → `-EINVAL` with `prm_err` on failure; ctx attrs `uri`, `ip,ip-addr`, `daqiri,yaml`, `daqiri,framing`, `daqiri,mem`, `daqiri,engine`, `daqiri,version`; `iiod_client_create_context(client, &iio_daqiri_backend, "DAQIRI to <host>", attrs, values, 7)`; `iio_context_set_pdata`; re-point `pdata->mgmt.params = iio_context_get_params(ctx)`. Ops: `read_attr`/`write_attr` → `iiod_client_attr_read/write`; `get_trigger`/`set_trigger`; `set_timeout` → `iiod_client_set_timeout`; `ping` → `iiod_client_nop`; `reg_read`/`reg_write`/`refresh_format` copied from `network.c:495-554`; `open_ev` → `iiod_client_open_event_stream`, `close_ev`, `read_ev`; `shutdown` cancels, destroys client, closes, frees. No buffer ops yet. `daqiri-engine.cpp`: `dq_engine_version` returns `DAQIRI_VERSION_STRING` (or the macro the pinned `version.h` defines); `dq_yaml_validate` wraps `daqiri::parse_network_config_from_yaml_file` in try/catch; `dq_yaml_engine_kind` from `NetworkConfig::common_.stream_type_`/`engine_` (verify member names in the pinned headers).

- [ ] **Step 6: Build and run the test**

Run: the cmake command from Step 2, then `ctest --test-dir build -L daqiri-loopback --output-on-failure`
Expected: `daqiri_context` PASS.

- [ ] **Step 7: Verify core isolation**

Run: `ldd build/libiio.so.1 | grep -Ei 'stdc\+\+|cuda|daqiri'` → Expected: no output. `ldd build/libiio-daqiri.so | grep -c daqiri` → Expected: ≥1.

- [ ] **Step 8: Format and commit**

```bash
./format.sh && git add -A && git commit -s -m "daqiri: add backend module skeleton with iiod management plane"
```

---

### Task 3: VITA 49.2 prologue parser and golden vectors

**Files:**
- Create: `daqiri-vrt-prologue.h`
- Create: `tests/daqiri/gen_golden.py`, `tests/resources/vrt/manifest.json`, `tests/resources/vrt/*.bin`
- Create: `tests/daqiri/test_vrt_prologue.c`
- Modify: `tests/daqiri/CMakeLists.txt` (unit tests built whenever `WITH_TESTS` on POSIX; label `daqiri`)

**Interfaces:**
- Produces (`daqiri-vrt-prologue.h`, header-only `static inline`):
  ```c
  enum dq_vrt_type { DQ_VRT_IF_DATA_NOSID = 0, DQ_VRT_IF_DATA_SID = 1, DQ_VRT_EXT_DATA_NOSID = 2,
                     DQ_VRT_EXT_DATA_SID = 3, DQ_VRT_IF_CONTEXT = 4, DQ_VRT_EXT_CONTEXT = 5,
                     DQ_VRT_COMMAND = 6, DQ_VRT_EXT_COMMAND = 7 };
  enum dq_vrt_tsf { DQ_VRT_TSF_NONE = 0, DQ_VRT_TSF_SAMPLE_COUNT = 1, DQ_VRT_TSF_REAL_TIME = 2, DQ_VRT_TSF_FREE_RUNNING = 3 };
  struct dq_vrt_prologue {
      uint8_t type; bool has_sid, has_class, has_trailer; uint8_t tsm, tsi, tsf, pkt_count;
      uint16_t size_words; uint32_t stream_id, oui; uint16_t info_class, packet_class;
      uint32_t tsi_secs; uint64_t tsf_frac;
      unsigned prologue_bytes; unsigned payload_bytes; /* excludes trailer */
  };
  static inline int dq_vrt_parse(const uint8_t *p, size_t len, struct dq_vrt_prologue *out);
  /* 0 on success; -EBADMSG if len < 4, len < size_words*4, size_words*4 < prologue+trailer, or type 2/3/6/7 */
  ```
  Bit layout of word 0: `type[31:28] C[27] T[26] rsv[25] TSM[24] TSI[23:22] TSF[21:20] pkt_count[19:16] size[15:0]`. `has_sid` = type odd or type ≥ 4. Word order: SID, class (2 words: OUI low 24 bits; `info_class<<16|packet_class`), TSI (1), TSF (2).
- Produces: `gen_golden.py` writes the manifest `{ "files": [ { "name", "type", "has_class", "tsi", "tsf", "pkt_count", "size_words", "stream_id", "oui", "info_class", "packet_class", "tsi_secs", "tsf_frac", "prologue_bytes", "payload_bytes", "payload_pattern": "ramp16"|"zero" } ] }` and files: `if_data_sid_full.bin` (SID+class+TSI utc+TSF sample-count, 360 samples CI16), `if_data_nosid.bin` (no class, no timestamps), `if_data_trailer.bin` (T=1), `if_data_wrap_seq_NN.bin` for NN=00..17 (pkt_count 0..15,0,1; tsf_frac advancing by 360 per packet), `if_data_jumbo_8968.bin`, `context_cif0.bin` (type 4), `truncated_header.bin` (size_words says 100, file is 40 bytes), `short_3bytes.bin`.

- [ ] **Step 1: Write `gen_golden.py` and generate vectors**

Pure Python `struct.pack('>I', ...)`, seed-free (deterministic). Payload `ramp16`: sample n has I = n & 0x7FFF, Q = (0x8000 - n) & 0xFFFF, packed BE.
Run: `python3 tests/daqiri/gen_golden.py tests/resources/vrt` ; Expected: manifest + 25 `.bin` files; `git add` them.

- [ ] **Step 2: Write the failing parser test**

`test_vrt_prologue.c`: load each manifest entry (tiny hand-written JSON reader is overkill — instead `gen_golden.py` also emits `tests/resources/vrt/expected.h` with a C array `static const struct dq_vrt_golden golden[] = { {"if_data_sid_full.bin", 1, true, 1, 1, 0, 364, 0x01000000, 0x000456, 0, 1, 1700000000, 0, 28, 1440}, ... };`). For each: read file, `dq_vrt_parse`, assert every field; for `truncated_header.bin` and `short_3bytes.bin` assert `-EBADMSG`; for `context_cif0.bin` assert `type == 4`, `has_sid`. Add a synthetic case: a buffer with `type = 6` → `-EBADMSG`; `size_words = 7` with `has_trailer` and 7-word prologue → `-EBADMSG` (no payload room).

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build && ctest --test-dir build -R test_vrt_prologue` → Expected: build error (header missing).

- [ ] **Step 4: Implement `daqiri-vrt-prologue.h`**

- [ ] **Step 5: Run to verify it passes**

Run: `ctest --test-dir build -R test_vrt_prologue --output-on-failure` → PASS.

- [ ] **Step 6: Optional parity check against libvrt49** (only if `~/dev/vrt49/build/libvrt49.so` exists): `gen_golden.py --check-with-libvrt49 <path>` round-trips each vector through `vrt49_prologue_unpack` via ctypes and asserts equality; record the result in `tests/daqiri/README.md`.

- [ ] **Step 7: Commit**

```bash
./format.sh && git add daqiri-vrt-prologue.h tests/daqiri tests/resources/vrt && git commit -s -m "daqiri: add VITA 49.2 prologue parser with golden vectors"
```

---

### Task 4: Sequence/gap tracker and block assembler (DAQIRI-free)

**Files:**
- Create: `daqiri-vrt-stream.h`, `daqiri-vrt-stream.c`, `daqiri-asm.h`, `daqiri-asm.c`
- Create: `tests/daqiri/test_vrt_gap.c`, `tests/daqiri/test_vrt_assembler.c`
- Modify: `tests/daqiri/CMakeLists.txt` (OBJECT library `iio_daqiri_framing` = `daqiri-vrt-stream.c daqiri-asm.c`, linked by unit tests and, in Task 6, by `iio-daqiri`)

**Interfaces:**
- Produces (`daqiri-vrt-stream.h`):
  ```c
  struct dq_vrt_counters { uint64_t packets, bytes, lost_packets, gap_events, gap_unknown,
                           foreign, context, malformed; };
  struct dq_vrt_cfg { uint32_t sid, sid_mask; bool gap_ts; size_t sample_size; };
  struct dq_vrt_rx { struct dq_vrt_cfg cfg; bool sid_locked; uint32_t sid; int expected_count;
                     bool have_ts; uint8_t tsf; uint64_t last_frac; uint32_t samples_per_pkt;
                     bool class_checked; struct dq_vrt_counters c; };
  struct dq_vrt_data { const uint8_t *payload; size_t len; uint32_t gap; bool gap_unknown; };
  #define DQ_VRT_DATA 0      /* out is valid */
  #define DQ_VRT_SKIP 1      /* counted, nothing to copy */
  void dq_vrt_rx_init(struct dq_vrt_rx *rx, const struct dq_vrt_cfg *cfg);
  int  dq_vrt_rx_packet(struct dq_vrt_rx *rx, const uint8_t *dgram, size_t len, struct dq_vrt_data *out);
  /* returns DQ_VRT_DATA, DQ_VRT_SKIP, or -EPROTO (payload not a multiple of sample_size, or
     packet_class incompatible with sample_size: class 0x0001 (CI16) needs 4, 0x0000/REAL16 needs 2) */
  ```
  Gap rule: `gap = (pkt_count - expected) & 0xF`; if `gap != 0`: when `cfg.gap_ts && have_ts && tsf ∈ {SAMPLE_COUNT, FREE_RUNNING} && samples_per_pkt > 0` then `gap = (frac - last_frac)/samples_per_pkt - 1` (64-bit math, must be ≥ 1 else `gap_unknown`); `lost_packets += gap`; `gap_events++`; `gap_unknown` set and counted when timestamps are unavailable (because a 16-multiple loss is indistinguishable). `samples_per_pkt = payload_bytes / sample_size` from the first data packet. SID: if `sid_mask` nonzero, require match else `foreign++`, SKIP; else lock on first IF_DATA packet.
- Produces (`daqiri-asm.h`):
  ```c
  enum dq_swap { DQ_SWAP_NONE = 0, DQ_SWAP_16 = 2, DQ_SWAP_32 = 4 };
  struct dq_asm { uint8_t *dst; size_t size, off; enum dq_swap swap; bool lossy;
                  uint8_t *carry; size_t carry_cap, carry_len; uint64_t zero_filled; bool had_gap; };
  int  dq_asm_init(struct dq_asm *a, enum dq_swap swap, bool lossy, size_t carry_cap); /* mallocs carry */
  void dq_asm_fini(struct dq_asm *a);
  void dq_asm_attach(struct dq_asm *a, void *dst, size_t size);  /* flushes carry into dst first */
  int  dq_asm_push(struct dq_asm *a, const uint8_t *payload, size_t len, uint32_t gap_packets, bool *done);
  /* gap_packets>0 && !lossy: returns -EIO without copying, sets had_gap (block is failed);
     gap_packets>0 && lossy: zero-fills min(gap_packets*len, size-off) bytes, zero_filled += n, had_gap=true, then copies.
     Copy applies swap (bswap16 per 2 bytes / bswap32 per 4 bytes). Bytes that do not fit go to carry.
     *done = (off == size). Returns bytes written to dst (0..len) or -EIO. -ENOBUFS if carry would overflow. */
  ```

- [ ] **Step 1: Write failing `test_vrt_gap.c`**

Build datagrams in-memory with a tiny helper `mk_pkt(buf, pkt_count, tsf_frac, nsamples)` (SID 0x01000000, class CI16, TSF sample-count). Cases and assertions:
contiguous 0..15,0 → `lost_packets==0, gap_events==0`; drop one (skip count 5) → `lost 1, events 1`; drop 3 → `lost 3`; wrap 14,15,1 (0 dropped) → `lost 1`; 20 packets dropped with timestamps → `lost 20, gap_unknown 0`; 16 dropped with `gap_ts=false` → `lost 0, gap_events 1, gap_unknown 1, out.gap_unknown==true`; duplicate (same count twice, same frac) → second returns SKIP? no — spec says duplicates are counted as loss-free reorder; decide: duplicate = `gap == 15` with `frac == last_frac` → SKIP and `foreign` untouched, add counter `duplicates` (add to counters struct). Reorder (count 7 arrives before 6) → first yields gap 1, second yields gap 15 with frac < last_frac → SKIP, `duplicates`? no: add counter `late` for frac < last_frac. SID mismatch with mask → `foreign++`, SKIP. Context packet → `context++`, SKIP. Truncated → `malformed++`, SKIP. Payload 6 bytes with sample_size 4 → `-EPROTO`.

- [ ] **Step 2: Run to verify it fails** (`ctest -R test_vrt_gap` → build error)

- [ ] **Step 3: Implement `daqiri-vrt-stream.c`** using `dq_vrt_parse`.

- [ ] **Step 4: Run to verify it passes**

- [ ] **Step 5: Write failing `test_vrt_assembler.c`**

Cases: block 1440 bytes, three 480-byte pushes → `done` on third, dst equals input; swap16 on BE input `00 01 00 02` → `01 00 02 00`; straddle: block 1000, push 600 twice → first `done=false`, second returns 400 with `done=true` and `carry_len==200`; attach new block → carry flushed, `off==200`; gap lossy=false → `-EIO`, nothing written, `had_gap`; gap lossy=true, gap 2, len 480, block 1440 at off 0 → `zero_filled==960`, then 480 copied, `done`; gap lossy at off 1200 → zero_filled 240 (clamped), payload goes to carry; carry overflow → `-ENOBUFS`.

- [ ] **Step 6: Run to verify it fails; implement `daqiri-asm.c`; run to verify it passes**

- [ ] **Step 7: Commit**

```bash
./format.sh && git add daqiri-vrt-stream.[ch] daqiri-asm.[ch] tests/daqiri && git commit -s -m "daqiri: add VRT sequence tracker and block assembler"
```

---

### Task 5: `framing=iiod` streaming path

**Files:**
- Modify: `daqiri.c` (buffer/block ops), `daqiri-private.h` (`struct iio_buffer_pdata`, `struct iio_block_pdata`)
- Create: `tests/daqiri/test_block_equivalence.c`, `tests/daqiri/run_loopback.sh`
- Modify: `tests/daqiri/CMakeLists.txt`

**Interfaces:**
- Produces (`daqiri-private.h`):
  ```c
  struct iio_buffer_pdata { struct iio_context_pdata *ctx; const struct iio_device *dev; unsigned idx;
      enum dq_framing framing;
      struct { struct dq_mgmt io; struct iiod_client *client; struct iiod_client_buffer_pdata *pdata; } iiod;
      struct dq_vrt_buffer *vrt;   /* Task 6 */ };
  struct iio_block_pdata { struct iio_buffer_pdata *buf; struct iio_block_pdata *iiod_block; struct dq_vrt_block *vrt; };
  ```
- Ops added to the table: `open_buffer`, `close_buffer`, `enable_buffer`, `cancel_buffer`, `readbuf`, `writebuf`, `create_block`, `free_block`, `enqueue_block`, `dequeue_block`, `get_dmabuf_fd` (→ `-ENOTSUP`), `disable_cpu_access` (→ `-ENOSYS`).
- `framing=iiod` behaviour: `open_buffer` = `dq_mgmt_connect(host, opts.data_port ? : mgmt port)` → `iiod_client_new` → `iiod_client_open_buffer(buf->iiod.client, ctx->client, dev, idx, mask)`; `create_block` wraps `iiod_client_create_block` (returns `-ENOSYS` pass-through for ASCII peers so the core mallocs); enqueue/dequeue/free unwrap; `readbuf`/`writebuf` → `iiod_client_readbuf/writebuf`; `enable_buffer` → `iiod_client_enable_buffer`; `cancel_buffer` → `dq_mgmt_ops.cancel`; `close_buffer` → `iiod_client_close_buffer`, `iiod_client_destroy`, `dq_mgmt_close`, free.

- [ ] **Step 1: Write failing `test_block_equivalence.c`**

Open `TESTS_IP_URI` and `TESTS_DAQIRI_URI` against the same iiod-emu (`fmcomms2.xml`, device `cf-ad9361-lpc`, enable channels `voltage0`,`voltage1`); `iio_buffer_create(dev, 0, mask)` on both; `iio_buffer_stream_create_block(size=4096)` ×4 each; enqueue all, `iio_buffer_enable`; dequeue 8 blocks from each; locate the first 64 bytes of `tests/resources/xmls/cf-ad9361-lpc_buf0.bin` in each stream to align; `TEST_ASSERT(memcmp(aligned_ip, aligned_daqiri, 8*4096 - offset) == 0)`; also assert `iio_context_get_xml` strings equal after replacing the `uri`/`ip,ip-addr`/`daqiri,*` context-attr lines (or compare device-by-device ids). Registered via `with_emu.sh`, label `daqiri-loopback`.

- [ ] **Step 2: Write `run_loopback.sh`**

For each xml in `tests/resources/xmls/*.xml`: `with_emu.sh $xml env TESTS_API_URI=daqiri:127.0.0.1,framing=iiod ctest --test-dir build -L api -j1 --output-on-failure`; exits non-zero on first failure. Registered as ctest `daqiri_loopback_api` (label `daqiri-loopback`, `RUN_SERIAL`). Note: `test_scan` under label api expects... check: if an api test hard-codes `local:` behaviour and fails under `ip:` in zephyr-tests.yml, mirror its exclusion list (`grep -n ctest .github/workflows/zephyr-tests.yml`).

- [ ] **Step 3: Run to verify both fail** (`ctest -L daqiri-loopback` → `-ENOSYS` from `iio_buffer_create`).

- [ ] **Step 4: Implement the buffer/block ops in `daqiri.c`** per the Interfaces block.

- [ ] **Step 5: Run to verify they pass**

Run: `ctest --test-dir build -L daqiri-loopback --output-on-failure` → `daqiri_context`, `daqiri_block_equivalence`, `daqiri_loopback_api` PASS.

- [ ] **Step 6: Utilities smoke**

Run: `build/utils/iio_readdev -u 'daqiri:127.0.0.1,framing=iiod' -b 4096 -s 8192 cf-ad9361-lpc | wc -c` (with emu running) → `32768`.

- [ ] **Step 7: Commit**

```bash
./format.sh && git add -A && git commit -s -m "daqiri: stream unchanged iiod targets through framing=iiod"
```

- [ ] **Step 8: ASCII-peer gate (manual, needs a Kuiper board; record result in `tests/daqiri/README.md`)**

Run against any reachable Kuiper board (e.g. the `nemo` place's ZC706 when free): `iio_readdev -u "daqiri:<board-ip>,framing=iiod" -s 4096 <adc-device> | cmp - <(iio_readdev -u ip:<board-ip> -s 4096 <adc-device>)`... streams are not aligned; instead assert both produce 4096 samples × sample size bytes and exit 0. Expected: works through the ASCII `readbuf` path (v0.25 iiod). If no board is free, leave the checkbox unticked and note it.

---

### Task 6: `framing=vrt` over the DAQIRI socket engine

**Files:**
- Modify: `daqiri-engine.cpp` (broker, rxq, mem alloc, status map), `daqiri-private.h`, `daqiri.c` (dispatch, buffer attrs), `CMakeLists.txt` (link `iio_daqiri_framing`, add `daqiri-vrt.c`)
- Create: `daqiri-vrt.c`
- Create: `tests/daqiri/resources/socket-udp-loopback.yaml`, `tests/daqiri/test_daqiri_lifecycle.c`

**Interfaces:**
- Produces (`daqiri-private.h`, implemented in C++):
  ```c
  struct dq_engine; struct dq_rxq;
  int  dq_engine_acquire(const char *yaml, struct dq_engine **out, char *err, size_t errlen); /* process-wide; -EBUSY if another realpath already initialized; pins module via dlopen(self, RTLD_NOLOAD|RTLD_NODELETE) on first init; never shuts down before exit */
  void dq_engine_release(struct dq_engine *e);  /* refcount only */
  int  dq_engine_get_port(struct dq_engine *e, const char *iface, int *port_id);
  int  dq_rxq_open(struct dq_engine *e, int port_id, int queue_id, struct dq_rxq **out); /* -EBUSY if open */
  void dq_rxq_close(struct dq_rxq *q);
  struct dq_pkt { const uint8_t *data; uint32_t len; };
  struct dq_burst { void *handle; unsigned nb_pkts; unsigned l2_offset; };
  int  dq_rxq_poll(struct dq_rxq *q, struct dq_burst *b);              /* 0 / -EAGAIN / -errno */
  unsigned dq_burst_pkts(const struct dq_burst *b, struct dq_pkt *out, unsigned max);
  void dq_burst_free(struct dq_burst *b);
  int  dq_mem_alloc(enum dq_mem k, int gpu, size_t size, void **ptr);  /* host: aligned_alloc 4096 + madvise HUGEPAGE; pinned: cudaHostAlloc; device: -ENOTSUP in this phase */
  void dq_mem_free(enum dq_mem k, void *ptr, size_t size);
  ```
- Produces (`daqiri-vrt.c`):
  ```c
  struct dq_vrt_block { struct dq_vrt_block *next; void *data; size_t size; enum { DQ_BLK_FREE, DQ_BLK_QUEUED, DQ_BLK_FILLING, DQ_BLK_DONE, DQ_BLK_ERROR, DQ_BLK_CANCELLED } state; int err; };
  struct dq_vrt_buffer;  /* poller thread, mutex, cond, eventfd wake, FIFO of QUEUED blocks, struct dq_vrt_rx rx, struct dq_asm asm, atomic cancel */
  struct dq_vrt_buffer *dq_vrt_open(struct iio_buffer_pdata *buf, struct dq_rxq *q, const struct dq_opts *o, enum dq_swap swap, size_t sample_size);
  void dq_vrt_close(struct dq_vrt_buffer *v);          /* cancel + join + free */
  int  dq_vrt_enable(struct dq_vrt_buffer *v, bool enable);  /* start/stop poller */
  void dq_vrt_cancel(struct dq_vrt_buffer *v);
  struct dq_vrt_block *dq_vrt_block_new(struct dq_vrt_buffer *v, size_t size, void **data);
  void dq_vrt_block_free(struct dq_vrt_buffer *v, struct dq_vrt_block *b);
  int  dq_vrt_enqueue(struct dq_vrt_buffer *v, struct dq_vrt_block *b, size_t bytes_used);  /* -EPERM if not FREE; -EINVAL if bytes_used != size */
  int  dq_vrt_dequeue(struct dq_vrt_buffer *v, struct dq_vrt_block *b, bool nonblock, int timeout_ms);
  ssize_t dq_vrt_read_attr(struct dq_vrt_buffer *v, const char *name, char *dst, size_t len);
  int  dq_vrt_write_attr(struct dq_vrt_buffer *v, const char *name, const char *src, size_t len); /* only daqiri_lossy */
  ```
  Poller loop: `dq_rxq_poll`; `-EAGAIN` → spin 64 then `poll(wake_fd, poll_us/1000 or 1 ms)`; for each pkt: `dq_vrt_rx_packet(data + l2_offset, len - l2_offset)`; DATA → if no current block pop FIFO (none → `dropped_no_block++`, continue); `dq_asm_push`; `-EIO` → block ERROR(-EIO), `blocks_failed++`, next block starts with this packet (push again after attach); done → DONE, `blocks_completed++`, broadcast; `-EPROTO` from rx → fail current block `-EPROTO`, continue. Counters exported through `dq_vrt_read_attr` with the names from the spec (`daqiri_rx_packets` … `daqiri_engine_kind`).
- `daqiri.c` dispatch: `framing=vrt`: `open_buffer` rejects output buffers (`-ENOTSUP`), requires `opts.yaml`; `dq_engine_acquire` once per context (stored in `ctx pdata`, released in `shutdown`); mask validation: every enabled channel is a scan element with `length ∈ {16,32}`, `repeat==1`, `shift==0`; `sample_size = iio_device_get_sample_size(dev, mask)`; swap = `DQ_SWAP_16/32` when `!fmt->is_be && opts.swap`; buffer attrs `daqiri_*` added at create for every input buffer (`iio_buffer_add_attr`), `read_attr` routes names starting with `daqiri_` to the open `dq_vrt_buffer` for that `(dev, idx)` (or returns `"0"` / `"host"` when none), everything else to `iiod_client_attr_read`.
- `tests/daqiri/resources/socket-udp-loopback.yaml`: `stream_type: socket`, one memory region `host` 64×9216, one interface `data` with `socket_config: {mode: server, local_addr: 'udp://127.0.0.1:25101', remote_addr: 'udp://127.0.0.1:25102', max_payload_size: 9216}`, one RX queue id 0 batch_size 8.

- [ ] **Step 1: Write failing `test_daqiri_lifecycle.c`**

Against iiod-emu (`fmcomms2.xml`) with URI `daqiri:127.0.0.1,framing=vrt,yaml=<abs path to socket-udp-loopback.yaml>`: (a) open buffer on `cf-ad9361-lpc` with voltage0+voltage1, create 2 blocks of 1440, enqueue, enable, `iio_block_dequeue(block, nonblock=true)` → `-EBUSY`; (b) `iio_buffer_cancel` then blocking dequeue → `-ECANCELED`; (c) `iio_context_destroy` with the buffer still open returns without crash; (d) create a second context with a different yaml path (copy of the file) and open a buffer → `-EBUSY`; (e) open the same `(port,queue)` twice in one context → second `-EBUSY`; (f) output buffer (`cf-ad9361-dds-core-lpc`) → `-ENOTSUP`; (g) `iio_buffer_find_attr(buf, "daqiri_rx_packets")` exists and reads `"0"`.
Registered via `with_emu.sh`, label `daqiri-loopback`, env `DAQIRI_YAML=${CMAKE_SOURCE_DIR}/tests/daqiri/resources/socket-udp-loopback.yaml`.

- [ ] **Step 2: Run to verify it fails** (open_buffer `-EINVAL`/`-ENOTSUP` today).

- [ ] **Step 3: Implement broker/rxq/mem in `daqiri-engine.cpp`**

`daqiri_init(cfg)` under a `std::mutex`; status map per spec; `get_rx_burst(&burst, port, queue)` → `NOT_READY`/`NULL_PTR`-with-no-burst → `-EAGAIN`; `dq_burst_pkts` from `get_num_packets`/`get_packet_ptr`/`get_packet_length` (verify exact names in the pinned `common.h`); `free_all_packets_and_burst_rx`. Pin module: `Dl_info info; dladdr((void*)&dq_engine_acquire, &info); dlopen(info.dli_fname, RTLD_NOW|RTLD_NOLOAD|RTLD_NODELETE)`.

- [ ] **Step 4: Implement `daqiri-vrt.c` and the `daqiri.c` dispatch**

- [ ] **Step 5: Run to verify it passes** (`ctest -L daqiri-loopback`)

- [ ] **Step 6: Commit**

```bash
./format.sh && git add -A && git commit -s -m "daqiri: add VITA 49.2 framing over the DAQIRI socket engine"
```

---

### Task 7: Software VRT sender and end-to-end VRT loopback test

**Files:**
- Create: `tests/daqiri/vrt_sender.c`, `tests/daqiri/test_vrt_loopback.c`
- Modify: `tests/daqiri/CMakeLists.txt`

**Interfaces:**
- Produces: `vrt_sender --to IP:PORT [--from IP:PORT] --samples N (per packet, default 360) --sid 0x01000000 --packets N | --duration S --pps N --source ramp|FILE --drop-list a,b,c --drop-every N --drop-prob P --seed S --reorder-window K --dup-every N --burst-gap-ms M --json PATH`. Emits IF_DATA_SID, class CI16 (OUI 0x000456, packet_class 0x0010 per vrt49 packet-classes.md — same values as `gen_golden.py`), TSI utc, TSF sample-count (frac = cumulative sample index), 4-bit count. `--source ramp` = `ramp16` pattern continuing across packets; `--source FILE` replays the file cyclically as payload bytes. `--json` writes `{ "sent": N, "dropped": [seq...], "reordered": [[a,b]...], "duplicated": [seq...] }` where seq is the absolute packet index. Uses `sendmmsg` in batches of 32; `--pps` paces with `clock_nanosleep`. Binds `--from` (default `127.0.0.1:25102` to match the YAML `remote_addr`).

- [ ] **Step 1: Write failing `test_vrt_loopback.c`**

Fixture: `fork`/`exec` `vrt_sender` (path from env `VRT_SENDER`) with `--to 127.0.0.1:25101`; open `daqiri:127.0.0.1,framing=vrt,yaml=$DAQIRI_YAML` on `cf-ad9361-lpc` (voltage0+voltage1 → sample_size 4, swap16 since XML is `le:`); blocks of 1440 bytes (= 360 samples = 1 packet) ×8 via `iio_stream_create`. Cases (each with its own sender invocation):
- clean: `--packets 64 --source ramp` → 32 consecutive blocks; after swap, sample n of block k equals `ramp16(k*360+n)` in host order; `daqiri_rx_lost_packets == 0`.
- drop-list `5,6,20`: dequeue returns `-EIO` exactly at the blocks that would contain seq 5/6 (one block, gap 2) and 20; `daqiri_rx_lost_packets == 3`, `daqiri_rx_gap_events == 2`, `daqiri_blocks_failed == 2`; subsequent blocks are again contiguous ramps.
- lossy=1 with the same drop list: no `-EIO`; `daqiri_rx_zero_filled_bytes == 3*1440`; the zero-filled regions are all-zero and following data is correct; `daqiri_blocks_with_gaps == 3` (one-packet blocks: packets 5, 6 and 20 each receive zero-fill; zero-fill is unclamped and preserves sample alignment).
- reorder-window 3: `daqiri_rx_lost_packets == 0` (a late arrival un-counts the loss it was counted as) and `daqiri_rx_late_packets == number of reordered pairs`; late payloads are skipped, so assert only the counters and that streaming continues.
- slow consumer: enqueue only 2 blocks, sender sends 200 packets at 10 kpps, wait 100 ms before dequeuing: both dequeued blocks are valid ramps, `daqiri_rx_dropped_no_block >= 190`.
- burst-gap 500 ms: stream resumes, `lost == 0`.
Registered via `with_emu.sh`, label `daqiri-loopback`.

- [ ] **Step 2: Run to verify it fails** (sender missing).

- [ ] **Step 3: Implement `vrt_sender.c`** (standalone; no libiio).

- [ ] **Step 4: Run to verify it passes**

- [ ] **Step 5: Commit**

```bash
./format.sh && git add -A && git commit -s -m "daqiri: add software VRT sender and loopback loss tests"
```

---

### Task 8: CI image and workflows

**Files:**
- Create: `CI/daqiri/Dockerfile`, `.github/workflows/daqiri-ci-image.yml`, `.github/workflows/daqiri.yml`
- Modify: `.github/workflows/tests.yml` (run `ctest -L daqiri` unit tests in the plain build; they need no DAQIRI), `tests/daqiri/CMakeLists.txt` (unit tests always, loopback gated)

**Interfaces:**
- Image `ghcr.io/tfcollins/libiio-daqiri-ci:<DAQIRI_REF-short>-cuda13.4` and `:latest`: `FROM nvidia/cuda:13.4.0-devel-ubuntu22.04`; apt: libiio build deps (same list as `CI/ubuntu/` scripts), `netcat-openbsd`, `python3`, `rdma-core`, `libibverbs-dev`; `COPY CI/daqiri/build-daqiri.sh CI/daqiri/DAQIRI_REF /tmp/`; `RUN /tmp/build-daqiri.sh /opt/daqiri ""`; `ENV CMAKE_PREFIX_PATH=/opt/daqiri LD_LIBRARY_PATH=/opt/daqiri/lib:/usr/local/cuda/lib64/stubs`.
- `daqiri-ci-image.yml`: `workflow_dispatch` + push on `CI/daqiri/**`; `docker/build-push-action` with `GITHUB_TOKEN` (`packages: write`).
- `daqiri.yml`: `on: [push, pull_request]`; job `software` in the image; cmake `-DWITH_MODULES=ON -DWITH_DAQIRI_BACKEND=ON -DWITH_TESTS=ON -DWITH_EMU_BACKEND=ON -DWITH_IIOD_EMU=ON -DIIO_MODULES_DIR_OVERRIDE=$GITHUB_WORKSPACE/build/`; `ctest -L daqiri`; `ctest -L daqiri-loopback -j1`; upload `build/Testing`.

- [ ] **Step 1: Write the Dockerfile and build it locally**

Run: `docker build -f CI/daqiri/Dockerfile -t libiio-daqiri-ci:dev .` → Expected: success; `docker run --rm libiio-daqiri-ci:dev pkg-config --modversion daqiri` prints the version; `docker run --rm libiio-daqiri-ci:dev sh -c 'ldd /opt/daqiri/lib/libdaqiri.so | grep "not found"'` → no output (CUDA stubs resolve `libcuda.so.1`).

- [ ] **Step 2: Run the software job locally in the image**

Run: `docker run --rm -v $PWD:/src -w /src libiio-daqiri-ci:dev sh -c 'cmake -S . -B /tmp/b <flags> && cmake --build /tmp/b -j && ctest --test-dir /tmp/b -L daqiri -L daqiri-loopback --output-on-failure'` → all PASS without a GPU.

- [ ] **Step 3: Write both workflows; add the unit-test ctest line to `tests.yml`**

- [ ] **Step 4: Validate workflow syntax**

Run: `actionlint .github/workflows/daqiri.yml .github/workflows/daqiri-ci-image.yml` (install if missing) → no errors.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -s -m "ci: add CUDA/DAQIRI image and daqiri software test workflow"
```

- [ ] **Step 6: After push (needs `gh auth`): dispatch `daqiri-ci-image.yml`, then confirm `daqiri.yml` is green on the branch.**

---

### Task 9: Benchmark utility and documentation

**Files:**
- Create: `utils/iio_daqiri_bench.c`, `doc/source/daqiri-backend.md`, `tests/daqiri/README.md`
- Modify: `utils/CMakeLists.txt`, `doc/source/index.rst`, `man/` (add `iio_daqiri_bench.1.in` following an existing util man page)

**Interfaces:**
- `iio_daqiri_bench -u URI -d DEVICE [-c ch1,ch2] -s BLOCK_BYTES [-n NBLOCKS=8] -T SECONDS [--check ramp|none] [-j perf.json]`; public API only; prints `bytes, blocks, gbps, blocks_per_s, dequeue_p50_ms, dequeue_p99_ms, cpu_pct` and, when present, the `daqiri_rx_*` buffer attrs; `--check ramp` validates the `ramp16` pattern (same definition as `gen_golden.py`) after dequeue and counts mismatching samples; JSON schema `{ "schema_version": 1, "uri", "device", "block_bytes", "duration_s", "bytes", "blocks", "gbps", "blocks_per_s", "dequeue_ms": {"p50","p99"}, "cpu_pct", "counters": {...}, "check": {"mode","bad_samples"} }`.

- [ ] **Step 1: Implement the bench** (follow `utils/iio_readdev.c` option handling and `iio_utils_helper` linkage).

- [ ] **Step 2: Verify on loopback**

Run (emu + `vrt_sender --duration 5 --pps 20000 --source ramp` running): `build/utils/iio_daqiri_bench -u "daqiri:127.0.0.1,framing=vrt,yaml=..." -d cf-ad9361-lpc -s 1440 -T 5 --check ramp -j /tmp/p.json` → `bad_samples 0`, `gbps ≈ 0.23`. And `-u ip:127.0.0.1 -d cf-ad9361-lpc -s 65536 -T 3` runs and prints a rate.

- [ ] **Step 3: Write `doc/source/daqiri-backend.md`** — status, build flags, URI keys table (from the spec), framings, counters table, limitations (one YAML per process, socket engine only in this phase, VRT RX only), security note (raw UDP unauthenticated; isolated network), license note. Add to `index.rst`. `tests/daqiri/README.md`: how to run unit, loopback and the manual ASCII-peer gate; where golden vectors come from.

- [ ] **Step 4: Build docs**

Run: the `doc.yml` workflow's sphinx command locally (see `.github/workflows/doc.yml`) → no warnings for the new page.

- [ ] **Step 5: Commit**

```bash
./format.sh && git add -A && git commit -s -m "daqiri: add benchmark utility and documentation"
```

---

### Task 10: Branch review and push

- [ ] **Step 1:** `ctest --test-dir build -L daqiri -L daqiri-loopback` all green; `./format.sh --check` (or the format workflow's command) clean; `codespell` clean on new files.
- [ ] **Step 2:** Run the `symbol_map` ctest (no public API was added in this phase; it must still pass).
- [ ] **Step 3:** `git push -u origin feature/daqiri-backend` (requires `gh auth login` / credential helper), open a draft PR `tfcollins/libiio` ← `feature/daqiri-backend` titled "daqiri: experimental DAQIRI streaming backend (phase 1: software)" with the spec and this plan linked.
