#!/bin/sh
set -eu

if [ "$#" -lt 10 ]; then
  echo "usage: build-wine.sh <source-dir> <build-dir> <windows-arch> <unix-arch> <libwinecrt0> <libntdll> <dbghelp> <winebuild> <winemac> <ntdll-so> [configure-arg...]" >&2
  exit 2
fi

src=$1
build=$2
windows_arch=$3
unix_arch=$4
out_winecrt0=$5
out_ntdll=$6
out_dbghelp=$7
out_winebuild=$8
out_winemac=$9
shift 9
out_ntdll_so=$1
shift 1

if [ ! -d "$src" ]; then
  echo "Wine source directory not found: $src" >&2
  exit 1
fi

mkdir -p "$build"

if [ ! -x "$src/configure" ]; then
  (cd "$src" && ./autogen.sh)
fi

if [ ! -f "$build/Makefile" ]; then
  (cd "$build" && "$src/configure" "$@")
fi

make_jobs=${WINE_BUILD_JOBS:-}
if [ -z "$make_jobs" ]; then
  make_jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
fi

make_targets="
dlls/winecrt0/$windows_arch/libwinecrt0.a
dlls/ntdll/$windows_arch/libntdll.a
dlls/dbghelp/$windows_arch/dbghelp.dll
tools/winebuild/winebuild
"

if [ "$unix_arch" = "x86_64-unix" ] || [ "$unix_arch" = "aarch64-unix" ]; then
  make_targets="$make_targets
dlls/winemac.drv/winemac.so
dlls/ntdll/ntdll.so
"
fi

make -C "$build" -j "$make_jobs" $make_targets

copy_artifact() {
  src_file=$1
  dst_file=$2

  mkdir -p "$(dirname "$dst_file")"
  if [ -e "$src_file" ]; then
    cp "$src_file" "$dst_file"
  else
    echo "Wine build did not produce expected artifact: $src_file" >&2
    exit 1
  fi
}

copy_artifact "$build/dlls/winecrt0/$windows_arch/libwinecrt0.a" "$out_winecrt0"
copy_artifact "$build/dlls/ntdll/$windows_arch/libntdll.a" "$out_ntdll"
copy_artifact "$build/dlls/dbghelp/$windows_arch/dbghelp.dll" "$out_dbghelp"
copy_artifact "$build/tools/winebuild/winebuild" "$out_winebuild"
chmod +x "$out_winebuild"

if [ "$unix_arch" = "x86_64-unix" ] || [ "$unix_arch" = "aarch64-unix" ]; then
  copy_artifact "$build/dlls/winemac.drv/winemac.so" "$out_winemac"
  copy_artifact "$build/dlls/ntdll/ntdll.so" "$out_ntdll_so"
else
  : >"$out_winemac"
  : >"$out_ntdll_so"
fi
