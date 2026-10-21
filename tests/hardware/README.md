# libiio Hardware Tests

This directory contains integration tests that require physical IIO devices to be connected.

## Overview

Hardware tests verify that libiio works correctly with actual hardware devices.

**Current tests:**

- **AD9364 Test** (`test_ad9364.c`) - Tests transmit-to-receive data path using a ramp signal. Verifies signal continuity within buffers and checks that the hardware loopback is functioning correctly.

## Dependencies

Hardware tests require:
- libiio library
- Physical IIO device connected and accessible
- Appropriate backend support (USB, network, etc.)
- CMake 3.13+
- C99 compiler

## Running Hardware Tests

Hardware tests can be run when the required hardware is available.

Build with tests enabled:

```bash
mkdir build && cd build
cmake -DTESTS=ON ..
make
```

Run the hardware test:

```bash
./tests/hardware/test_ad9364
```

## Managed labgrid baseline

See [MANAGED.md](MANAGED.md) for the opt-in tron ADRV9009-ZU11EG managed
reservation/boot gate, offline harness tests, pinned dependencies, and exact
JUnit/cleanup requirements. This is a network baseline plus legacy DAQIRI
capability rejection test, **not accelerated streaming qualification**.

## Adding Hardware Tests

When adding new hardware-specific tests:
1. Follow the naming convention: `test_<device>.c`
2. Include appropriate device detection and graceful fallback
3. Document the required hardware setup in this README
4. Update the CMakeLists.txt to include the new test