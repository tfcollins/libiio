# CI image for the libiio daqiri: backend (software tests, socket engine only).
# Build from the repository root:
#   docker build -f CI/daqiri/Dockerfile -t libiio-daqiri-ci:dev .
# 13.4.0 is not published for ubuntu22.04; 13.4.3 is the newest 13.4.x tag.
FROM nvidia/cuda:13.4.3-devel-ubuntu22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config git ca-certificates \
        libxml2-dev libzstd-dev libusb-1.0-0-dev libserialport-dev \
        libavahi-client-dev libaio-dev libcdk5-dev bison flex \
        python3 netcat-openbsd lcov rdma-core libibverbs-dev \
    && rm -rf /var/lib/apt/lists/*

COPY CI/daqiri/build-daqiri.sh CI/daqiri/DAQIRI_REF /tmp/
RUN /tmp/build-daqiri.sh /opt/daqiri "" \
    && rm -rf /tmp/daqiri-src-* /tmp/build-daqiri.sh /tmp/DAQIRI_REF

# The toolkit ships only libcuda.so in the stubs directory, but libdaqiri.so
# needs libcuda.so.1. The stubs directory is last on LD_LIBRARY_PATH so a real
# driver (NVIDIA container runtime) is preferred when present.
RUN ln -s libcuda.so /usr/local/cuda/lib64/stubs/libcuda.so.1

ENV PKG_CONFIG_PATH=/opt/daqiri/lib/pkgconfig \
    CMAKE_PREFIX_PATH=/opt/daqiri \
    LD_LIBRARY_PATH=/opt/daqiri/lib:/usr/local/cuda/lib64:/usr/local/cuda/lib64/stubs
