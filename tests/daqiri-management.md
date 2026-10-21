# DAQIRI public API management regression

`daqiri-management.py` uses Python's standard library and ctypes to call the real
shared libiio library. An independent loopback TCP ASCII iiod fixture supplies
XML and attribute responses. DAQIRI itself is linked, not mocked. This fixture
explicitly rejects BINARY and ZPRINT negotiation; it does not test binary iiod.

Use a dedicated build and **dedicated writable profile directory**, never the
production `/etc/libiio/daqiri`. The argument must match the directory compiled
into libiio. Random profile names and ephemeral TCP ports permit parallel runs;
profiles and connections are cleaned up even on failure. Socket and thread waits
are bounded, thread errors propagate, and checks remain enabled under `python -O`.

Example (real DAQIRI installation, including its CMake package, required):

```sh
root=/tmp/libiio-daqiri-werror-regression
cmake -S . -B "$root/build" \
  -DWITH_DAQIRI_BACKEND=ON \
  -DCMAKE_PREFIX_PATH=/tmp/libiio-daqiri-verification/prefix \
  -DCUDAToolkit_ROOT=/usr/local/cuda \
  -DDAQIRI_PROFILE_DIR="$root/profiles" \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Werror' \
  -DWITH_IIOD=OFF -DWITH_TESTS=OFF -DWITH_EXAMPLES=OFF \
  -DWITH_USB_BACKEND=OFF -DWITH_SERIAL_BACKEND=OFF
cmake --build "$root/build" -j8
python3 tests/daqiri-management.py \
  --library "$root/build/libiio.so.1" --profile-dir "$root/profiles"
```

Coverage:

* Built-in `daqiri` registration and retained DAQIRI context identity.
* Exact rejection (`-EPROTONOSUPPORT`) of absent, VRT49, and unknown capabilities.
* Accepted `dqi1-cpu-strict-credit-v1` topology: device ID and input channel.
* Device and channel attribute lookup/read, device attribute write, and exact
  independently observed iiod command/payload dispatch.
* Connection cleanup on rejection and success; no legacy `OPEN` streaming.
* Successful management operations with nonexistent packet YAML demonstrate
  packet-runtime initialization is deferred until buffer creation.

## Verified scope and limitations

The complete configured build (library, compatibility library and utilities)
passed on `picard.local` with **C++** `-Wall -Wextra -Werror`, followed by all four
fixture cases. Imported DAQIRI and CUDA headers are correctly `-isystem` in the
CMake-generated compile flags; no new warning suppression was added.

Applying these flags globally to **C as well** is currently blocked by existing
warnings outside the owned DAQIRI files: unused parameters in `compat.c`
(`iio_create_scan_context`, `iio_buffer_get_poll_fd`) and signedness comparison in
`iiod-responder.c:iiod_command_data_read`. Those files were not changed, so this
is not a claim of a repository-wide C/C++ Werror pass.

libxml reports missing DTD validation diagnostics for the deliberately minimal
XML, as in the original fixture; the public API accepts and exercises it.

This is not a native public-buffer/block integration test or raw-NIC hardware
qualification. The production adapter intentionally accepts only raw ibverbs;
a public API loopback UDP block test would require a separate test-only runtime
adapter, not weakening that production gate. `daqiri-socket.md` documents the
separate real DAQIRI socket-engine/independent UDP peer stream-core test.
