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
