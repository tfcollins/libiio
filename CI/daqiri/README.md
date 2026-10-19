# DAQIRI dev build

libiio's `daqiri:` backend uses NVIDIA DAQIRI (https://github.com/NVIDIA/daqiri)
for packet I/O. DAQIRI has a hard CUDA build dependency.

## Pin

`DAQIRI_REF` holds the pinned upstream commit (40-char SHA, upstream HEAD at
the time of pinning, version 2026.7.0).

## Build

    CI/daqiri/build-daqiri.sh $HOME/opt/daqiri [<engine-list>]

Clones DAQIRI at `DAQIRI_REF` into `${TMPDIR:-/tmp}/daqiri-src-<ref>`, builds
shared libs (no Python, no examples) and installs into the prefix. The engine
list defaults to empty (socket engine only). The last output line is
`pkg-config --modversion daqiri` (`2026.7.0`). Consumers use
`-DCMAKE_PREFIX_PATH=<prefix>` (package `daqiri`, target `daqiri::daqiri`).

CMake does not find `nvcc` on its own when it is not on PATH. On picard
(CUDA 13.4) the build needs:

    DAQIRI_CMAKE_EXTRA="-DCMAKE_CUDA_COMPILER=/usr/local/cuda-13.4/bin/nvcc -DCUDAToolkit_ROOT=/usr/local/cuda-13.4" \
        CI/daqiri/build-daqiri.sh $HOME/opt/daqiri

Without it, configure fails with "No CMAKE_CUDA_COMPILER could be found".
(`CUDAToolkit_ROOT` was passed together with the compiler; only the compiler
was demonstrably required.) At run time the CUDA libs and `<prefix>/lib` must
be resolvable, e.g. `LD_LIBRARY_PATH=$HOME/opt/daqiri/lib:/usr/local/cuda-13.4/lib64`
(the pkg-config file only sets `-rpath-link`).

## Probe

`probe-socket.cpp` initialises the socket engine from an inline YAML string
(one `data` interface, `socket_config` server `udp://127.0.0.1:25101`, remote
`udp://127.0.0.1:25102`, `max_payload_size: 9216`, one RX queue id 0, one
`host` memory region), prints `get_port_id("data")`, then calls `shutdown()`.

    g++ -std=c++17 CI/daqiri/probe-socket.cpp \
        $(PKG_CONFIG_PATH=$HOME/opt/daqiri/lib/pkgconfig pkg-config --cflags --libs daqiri) \
        -o /tmp/probe
    /tmp/probe                 # PROBE_TWICE=1 repeats init/shutdown

API used (from the installed `daqiri/daqiri.h` / `common.h`, namespace
`daqiri`): `Status daqiri_init(const std::string &yaml_string_or_path)`,
`int get_port_id(const std::string &)`, `void shutdown()`, `Status::SUCCESS`.
The YAML needs the `daqiri: { cfg: { ... } }` wrapper used by the upstream
examples; `master_core`, `affinity`, `num_bufs`, `buf_size`, queue `name`,
`cpu_core` and `batch_size` are also set.

Result: exit 0, `get_port_id("data")` = `0`.

Init twice: with `PROBE_TWICE=1`, a second `daqiri_init` after `shutdown()` in
the same process succeeds (port id `0` again, exit 0). The header documents
that a subsequent `daqiri_init()` creates a fresh engine instance. Recorded for
the spec only; the backend pins the engine for process lifetime.

## libiio module (`WITH_DAQIRI_BACKEND`)

    cmake -S . -B build-daqiri -DWITH_MODULES=ON -DWITH_DAQIRI_BACKEND=ON \
        -DWITH_TESTS=ON -DWITH_EMU_BACKEND=ON -DWITH_EMU_BACKEND_DYNAMIC=OFF \
        -DWITH_IIOD_EMU=ON \
        -DCMAKE_PREFIX_PATH=$HOME/opt/daqiri -DCUDAToolkit_ROOT=/usr/local/cuda-13.4 \
        -DIIO_MODULES_DIR_OVERRIDE=$PWD/build-daqiri/
    cmake --build build-daqiri -j
    ctest --test-dir build-daqiri -L daqiri-loopback --output-on-failure

The module needs Linux, `BUILD_SHARED_LIBS` and `WITH_MODULES` (configure stops
otherwise). It enables the C++ language only; the CUDA language is not enabled,
so no `CMAKE_CUDA_COMPILER` is needed: `find_package(CUDAToolkit)` locates the
toolkit from `CUDAToolkit_ROOT`. `libiio.so` itself gains no C++, CUDA or
DAQIRI dependency; only `libiio-daqiri.so` links them.

`IIO_MODULES_DIR_OVERRIDE` (advanced cache variable, `WITH_MODULES` builds
only) replaces the module directory that libiio bakes in at configure time
(default `<libdir>/libiio/`); a missing trailing `/` is added. All modules,
`libiio-daqiri.so`, `libiio-ip.so` and `libiio-emu.so` included, are written to
the top of the build tree, so test builds point it there. Leave it unset for
builds that get installed.

Run-time libraries: in the build tree no `LD_LIBRARY_PATH` is needed. CMake's
build-tree RUNPATH on `libiio-daqiri.so` covers `$HOME/opt/daqiri/lib` and the
CUDA `lib64`, `libdaqiri.so`'s own `$ORIGIN` RUNPATH finds `libdaqiri_socket`
and `yaml-cpp`, and on picard `libcudart`/`libcuda` also resolve through
ldconfig; `env -u LD_LIBRARY_PATH ldd build-daqiri/libiio-daqiri.so` reports
nothing missing. CMake drops the build RUNPATH on install, so an installed
module needs `LD_LIBRARY_PATH=$HOME/opt/daqiri/lib` (or an ldconfig entry)
unless DAQIRI lives in a system prefix.

`tests/daqiri/with_emu.sh <xml> <cmd...>` starts `$IIOD_EMU` (CMake passes the
built `iiod-emu`) serving `emu:<xml>` on 127.0.0.1:30431, waits up to 10 s for
it to listen, runs the command, stops the emulator and exits with the
command's status. It refuses to run when the port is already taken, so it
never tests against some other server, and prints the emulator's log only on
failure. ctest serialises the tests that use it with `RESOURCE_LOCK
iiod-emu-30431`.

`-DWITH_EMU_BACKEND_DYNAMIC=OFF` builds the emu backend into `libiio.so`. As a
module, `libiio-emu.so` calls the non-exported `iio_strndup`, and iiod-emu
crashes on the first attribute write; `tests/daqiri/CMakeLists.txt` warns at
configure time when the option is ON.

The `daqiri-loopback` label runs:

- `daqiri_context`: the management plane against `ip:`.
- `daqiri_block_equivalence`: `cf-ad9361-lpc` voltage0+voltage1 streamed
  through `ip:` and `daqiri:…,framing=iiod` must carry the same bytes, and
  both context XMLs must match once the backend's own attributes are removed.
  It also covers buffer cancel and `data_port`. iiod-emu fills a block with at
  most one wrap of its `.bin`, so the test serves a build-tree copy of
  `fmcomms2.xml` whose `cf-ad9361-lpc_buf0.bin` is the stock file three times
  over (longer than a 4096-byte block).
- `daqiri_loopback_api`: `tests/daqiri/run_loopback.sh` runs the `api` label
  once per XML in `tests/resources/xmls/` with
  `TESTS_API_URI=daqiri:127.0.0.1,framing=iiod`. The api buffer tests skip
  quietly when a buffer does not open, so each XML first runs
  `test_daqiri_open`, which must open a buffer through the same URI (the
  script lists the XMLs that have none). The script also fails when ctest
  selects no api tests or not all of them pass. Set
  `DAQIRI_LOOPBACK_URI=ip:127.0.0.1` to run the same thing through `ip:` as a
  control. Nothing is excluded: all `api` tests pass through `ip:`, as in
  `.github/workflows/zephyr-tests.yml`.
