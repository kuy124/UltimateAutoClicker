#!/bin/sh
set -eu
cd "$(dirname "$0")"
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --parallel
