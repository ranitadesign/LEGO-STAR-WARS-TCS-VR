#!/bin/sh
# Builds saga_vr.dll (x86) with zig's clang/mingw toolchain. Usage: xr/build.sh <OpenXR-SDK dir> <out dir>
set -e
here=$(cd "$(dirname "$0")" && pwd)
sdk=${1:?OpenXR-SDK directory}
out=${2:?output directory}
mkdir -p "$out"
zig cc -target x86-windows-gnu -O2 -shared -Wall -Wno-unused-function \
    -I"$sdk/include" "$here/saga_xr.c" -o "$out/saga_vr.dll" -ld3d11 -ldxgi
