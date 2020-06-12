#!/bin/bash
set -x
apt update
apt upgrade -qq
apt install bison flex cmake git build-essential libxml2-dev doxygen dot
apt install python3 python3-sphinx
mkdir build && cd build
cmake .. -DPYTHON_BINDINGS=ON
make
