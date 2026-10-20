# DAQIRI backend tests

Tests of the `daqiri:` backend (`daqiri*.c`, `daqiri-engine.cpp`) and the
tools they use. User documentation is `doc/source/daqiri-backend.md`; build
notes for DAQIRI itself are in `CI/daqiri/README.md`.

## Configure

The unit tests need nothing but `-DWITH_TESTS=ON`. The loopback tests need the
module, the emulator and the build tree as module directory:

    cmake -S . -B build-daqiri -DWITH_MODULES=ON -DWITH_DAQIRI_BACKEND=ON \
        -DWITH_TESTS=ON -DWITH_EMU_BACKEND=ON -DWITH_EMU_BACKEND_DYNAMIC=OFF \
        -DWITH_IIOD_EMU=ON \
        -DCMAKE_PREFIX_PATH=$HOME/opt/daqiri -DCUDAToolkit_ROOT=/usr/local/cuda-13.4 \
        -DIIO_MODULES_DIR_OVERRIDE=$PWD/build-daqiri/
    cmake --build build-daqiri -j

## Unit tests (label `daqiri`)

No DAQIRI, CUDA, network or board: they run in the plain `tests.yml` job.

    ctest --test-dir build-daqiri -L '^daqiri$' --output-on-failure

`test_vrt_prologue` (header parsing against the golden vectors),
`test_vrt_gap` (packet count / timestamp loss detection) and
`test_vrt_assembler` (block assembly: boundaries, straddling packets, short
tails, zero-fill).

## Loopback tests (label `daqiri-loopback`)

Each starts its own `iiod-emu` on 127.0.0.1:30431 through `with_emu.sh`, and
the `framing=vrt` ones use the DAQIRI socket engine on 127.0.0.1:25101 with
`resources/socket-udp-loopback.yaml`. They share the ports, so run them one at
a time:

    ctest --test-dir build-daqiri -L daqiri-loopback -j1 --output-on-failure

| Test | What it covers |
|---|---|
| `daqiri_context` | management plane against `ip:` |
| `daqiri_block_equivalence` | `daqiri:...,framing=iiod` carries the same bytes and context as `ip:` |
| `daqiri_lifecycle`, `daqiri_lifecycle_mixed` | block states, destroy with a buffer open, one YAML per process, `daqiri_*` attributes |
| `daqiri_vrt_loopback` | `vrt_sender` to the engine: block contents and counters against the sender's `--json` oracle, clean and with drops (`lossy=0` and `1`), reordering, duplicates, a host overrun (slow consumer) and a burst gap |
| `daqiri_loopback_api` | the `api` tests over `daqiri:...,framing=iiod` for every emu XML (`run_loopback.sh`); `DAQIRI_LOOPBACK_URI=ip:127.0.0.1` runs the control through `ip:` |

`with_emu.sh <xml> <cmd...>` runs a command against an emulator serving
`emu:<xml>` (use an absolute path for the XML). Set `IIOD_EMU` to the
`iiod-emu` binary (`build-daqiri/iiod-emu/iiod-emu`). It refuses to run when
the port is taken.

## The software sender

`vrt_sender` (built in `build-daqiri/tests/daqiri/`) is a stand-in for a
hardware VITA 49.2 framer: IF-Data packets over UDP, from the YAML's
`remote_addr`, with a ramp or a replayed file as payload and optional drops,
reordering and duplicates. `vrt_sender --help` lists the options.

To run it by hand, with a benchmark:

    export IIOD_EMU=$PWD/build-daqiri/iiod-emu/iiod-emu
    tests/daqiri/with_emu.sh $PWD/tests/resources/xmls/fmcomms2.xml sh -c '
        (sleep 0.3; exec build-daqiri/tests/daqiri/vrt_sender \
            --to 127.0.0.1:25101 --duration 7 --pps 20000 --source ramp) &
        build-daqiri/utils/iio_daqiri_bench \
            -u "daqiri:127.0.0.1,framing=vrt,yaml=$PWD/tests/daqiri/resources/socket-udp-loopback.yaml" \
            -d cf-ad9361-lpc -c voltage0,voltage1 -s 1440 -n 64 -T 5 \
            --check ramp -j /tmp/perf.json
        wait'

The sender is started after the benchmark has its buffer: a stream drops
whatever arrived before it started. The run should end with `bad_samples 0`
and about 0.23 Gbit/s. The benchmark needs `-c`: the default is all four
channels of the device, which is not the stream layout.

## Golden vectors

`tests/resources/vrt/*.bin`, `manifest.json` and `expected.h` are produced by
`gen_golden.py` (pure Python, deterministic):

    python3 tests/daqiri/gen_golden.py tests/resources/vrt
    ./format.sh

Packets are VITA 49.2 IF-Data with Stream ID `0x01000000`, a Class ID with OUI
`0x000456` and packet class `0x0010` (the vrt49 CI16 format: an I/Q pair of
16-bit samples, big-endian), TSI UTC at 1700000000 and the sample count as
fractional timestamp. The payload pattern `ramp16` has sample n with
`I = n & 0x7FFF` and `Q = (0x8000 - n) & 0xFFFF`; after the backend's byte swap
the host sees little-endian int16 I at offset 0 and Q at offset 2. `vrt_sender`
and `iio_daqiri_bench --check ramp` use the same definition.

## Manual gate: a Kuiper board with ASCII-only `iiod`

Boards that ship libiio 0.25 (Kuiper 2023_R2_P1) run an `iiod` that speaks only
the ASCII protocol. No CI job has one; run this once per change to the
`framing=iiod` data path, with a board on the network:

- [ ] `iio_info -u daqiri:<board>,framing=iiod` shows the same devices and
      channels as `iio_info -u ip:<board>` (the `daqiri,*` context attributes
      are extra).
- [ ] `iio_rwdev -u daqiri:<board>,framing=iiod -b 4096 -s 16384 cf-ad9361-lpc voltage0 voltage1 > d.bin`
      captures data, and with the board in a fixed test pattern, matches what `ip:<board>`
      produces.
- [ ] `iio_daqiri_bench -u daqiri:<board>,framing=iiod -d cf-ad9361-lpc -s 65536 -T 5`
      completes, and its rate is comparable to the same run through `ip:`.
- [ ] Creating a block with the board's ASCII `iiod` falls back to the library's
      buffer path (no native blocks) and does not fail.
