#!/bin/bash
set -e
export VITASDK=${VITASDK:-/usr/local/vitasdk}
export PATH=$VITASDK/bin:$PATH
cd "$(dirname "$0")"
if ! command -v arm-vita-eabi-gcc >/dev/null; then
  echo "arm-vita-eabi-gcc not found - check VITASDK ($VITASDK)"; exit 1
fi
if [ -f build/CMakeCache.txt ] && ! grep -q "arm-vita-eabi-gcc" build/CMakeCache.txt; then
  echo "Old build folder uses the wrong compiler, deleting it"; rm -rf build
fi
mkdir -p build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
make -j4
echo "BUILD OK: build/aniread.vpk"
