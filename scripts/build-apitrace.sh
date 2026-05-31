#!/bin/sh
set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 5 ]; then
  echo "usage: build-apitrace.sh <source-dir> <build-dir> <stamp-file> [target=native|mingw] [toolchain-file]" >&2
  exit 2
fi

src=$1
build=$2
stamp=$3
target=${4:-native}
toolchain=${5:-}

if [ ! -f "$src/CMakeLists.txt" ]; then
  echo "apitrace source directory not found: $src" >&2
  exit 1
fi

case "$target" in
  native)
    cmake -S "$src" -B "$build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DAPITRACE_BUILD_TOOLS=OFF \
      -DAPITRACE_BUILD_WINDOWS_DLLS=OFF \
      -DAPITRACE_BUILD_METAL_BACKEND=ON \
      -DAPITRACE_BUILD_D3D_NATIVE_RETRACE=OFF
    cmake --build "$build" --target apitrace_core apitrace_platform_apple_metal
    ;;
  mingw)
    if [ -z "$toolchain" ]; then
      echo "build-apitrace.sh: target=mingw requires a toolchain file as 5th arg" >&2
      exit 2
    fi
    if [ ! -f "$toolchain" ]; then
      echo "build-apitrace.sh: toolchain file not found: $toolchain" >&2
      exit 1
    fi
    cmake -S "$src" -B "$build" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
      -DCMAKE_BUILD_TYPE=Release \
      -DAPITRACE_BUILD_TOOLS=OFF \
      -DAPITRACE_BUILD_WINDOWS_DLLS=OFF \
      -DAPITRACE_BUILD_METAL_BACKEND=OFF \
      -DAPITRACE_BUILD_D3D_NATIVE_RETRACE=OFF \
      -DAPITRACE_BUILD_PE_CAPTURE_STATIC=ON
    cmake --build "$build" --target \
      apitrace_core \
      apitrace_platform_windows_d3d11 \
      apitrace_platform_windows_d3d12
    ;;
  *)
    echo "build-apitrace.sh: unknown target '$target' (expected native or mingw)" >&2
    exit 2
    ;;
esac

mkdir -p "$(dirname "$stamp")"
: > "$stamp"
