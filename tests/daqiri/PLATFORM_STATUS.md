# UDP platform host qualification checkpoint, 2026-11-10

Branch: `feature/daqiri-udp-platforms`, based on UDP implementation
`f7be49923a3565e6beb318193569e476f604ec31`. This branch adds endpoint regression
coverage and bring-up templates; the backend already supports independent
management and data addresses, so it needs no platform-name conditions.

## Software evidence

Built in `build-daqiri` on Ubuntu 24.04.5 LTS, Linux 7.0.0-42-generic, x86_64,
against the existing DAQIRI 2026.7.0 ABI-2 installation with socket and ibverbs
engines. The configured source pin for rebuilding DAQIRI is
`7c1f5d05d01e5de6042791d5b10cabc7dc9b13f0`; the installed version and hashes
were verified, but its exact source-tree/patch provenance was not reconstructed
by this checkpoint. Record that provenance before hardware release.

Installed library SHA256:

```text
67dea37195a8eb98d3900b4fd12f2cb9e33ac066ffac53f1f434d6b5d23a4068  libdaqiri.so.2026.7.0
34d9c7c330a5f13762bec6eed08b2b1af4c0f0ef84bf4e7cd4789ee615cfb288  libdaqiri_ibverbs.so.2026.7.0
```

The unchanged-source baseline passed 8 unit, 6 emulator and 17 loopback tests
with `DAQIRI_REQUIRE_RAW_ENGINES=1`. The raw-flow parser actually ran; it did
not skip. The receive-buffer burst test ran and passed at the existing
`net.core.rmem_max=268435456`; no sysctl or NIC changes were made.

New tests prove TCP management at 127.0.0.1 can stream 64 correctly decoded
sample datagrams from 127.0.0.2, with exact oracle/counter closure. A second
test rejects the management IP when it is incorrectly offered as the data
source. Both reuse the production backend, socket engine and emulator.
Platform template endpoint tests first failed for the missing templates and
then passed with socket and ibverbs endpoint tuples checked in host byte order.

Final software suites: **8/8 unit, 6/6 emulator and 19/19 loopback passed**, with
raw engines required and no skips (loopback 94.71 s). The template's adjusted
9100-byte buffer also passed the raw endpoint parser after this combined run.

## Reproduce

```sh
cmake -S . -B build-daqiri -G Ninja \
  -DWITH_MODULES=ON -DWITH_DAQIRI_BACKEND=ON -DWITH_TESTS=ON \
  -DWITH_EMU_BACKEND=ON -DWITH_EMU_BACKEND_DYNAMIC=OFF -DWITH_IIOD_EMU=ON \
  -DCMAKE_PREFIX_PATH="$HOME/opt/daqiri-raw" \
  -DCUDAToolkit_ROOT=/usr/local/cuda-13.4 \
  -DIIO_MODULES_DIR_OVERRIDE="$PWD/build-daqiri/"
cmake --build build-daqiri -j4
ctest --test-dir build-daqiri -L '^daqiri$' -j1 --output-on-failure
ctest --test-dir build-daqiri -L daqiri-emu -j1 --output-on-failure
DAQIRI_REQUIRE_RAW_ENGINES=1 ctest --test-dir build-daqiri \
  -L daqiri-loopback -j1 --output-on-failure
```

Ports are shared between suites; run them sequentially. These commands do not
initialise the fake PCI device in the parser-tested raw template.

## Platform gates remain open

Neither VCU118/AD9084 nor ADRV9009-ZU11EG is qualified by this result. The raw
test parses flow rules; it does not install them on a NIC or receive packets
through ibverbs. Hardware reception, link/cabling/NIC/CPU selection, exact
source/firmware provenance, sustained rates, measured stop tails and Q0-Q8
still require each board's matched image and available rig. Both example
YAMLs contain documentation addresses and must be adapted after discovery.
VCU118 additionally needs its exact AD9084 card/profile and working Linux/IIO
BSP. Do not substitute FMCDAQ3/VRT49 traffic for version-1 sample reception.
