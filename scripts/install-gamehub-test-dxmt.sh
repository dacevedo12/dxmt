#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${DXMT_BUILD_DIR:-${REPO_ROOT}/build-builtin}"
NATIVE_LLVM_PATH="${DXMT_NATIVE_LLVM_PATH:-/usr/local/opt/llvm@15}"
GAMEHUB_BUNDLE_ID="${GAMEHUB_BUNDLE_ID:-com.gamemac.test}"
GAMEHUB_DXMT_PACKAGE="${GAMEHUB_DXMT_PACKAGE:-dxmt-v0.80}"
GAMEHUB_DXMT_ROOT="${GAMEHUB_DXMT_ROOT:-${HOME}/Library/Application Support/${GAMEHUB_BUNDLE_ID}/wine-engine/downloads/${GAMEHUB_DXMT_PACKAGE}}"
BACKUP_ROOT="${DXMT_GAMEHUB_BACKUP_ROOT:-${HOME}/Library/Caches/dxmt-runtime-smoke}"
STAGE_DIR="${DXMT_GAMEHUB_STAGE_DIR:-${BACKUP_ROOT}/gamehub-test-stage}"
BACKUP_TAG="${DXMT_GAMEHUB_BACKUP_TAG:-$(date -u +%Y%m%dT%H%M%SZ)}"
BACKUP_DIR="${BACKUP_ROOT}/gamehub-${GAMEHUB_DXMT_PACKAGE}-backup-${BACKUP_TAG}"

runtime_files=(
  "x86_64-windows/winemetal.dll"
  "x86_64-windows/dxgi.dll"
  "x86_64-windows/d3d12.dll"
  "x86_64-windows/d3d12core.dll"
  "x86_64-windows/d3d11.dll"
  "x86_64-windows/d3d10core.dll"
  "x86_64-windows/nvapi64.dll"
  "x86_64-windows/nvngx.dll"
  "x86_64-unix/winemetal.so"
)

log() {
  printf '[gamehub-dxmt] %s\n' "$*"
}

die() {
  printf '[gamehub-dxmt] error: %s\n' "$*" >&2
  exit 1
}

prepend_tool_path() {
  local dir="$1"
  if [[ -d "${dir}" ]]; then
    PATH="${dir}:${PATH}"
  fi
}

setup_tool_path() {
  local bison_prefix
  bison_prefix="$(brew --prefix bison 2>/dev/null || true)"
  if [[ -n "${bison_prefix}" ]]; then
    prepend_tool_path "${bison_prefix}/bin"
  fi
  prepend_tool_path "/usr/local/opt/bison/bin"
  prepend_tool_path "/opt/homebrew/opt/bison/bin"
  export PATH
}

setup_build_dir() {
  if [[ -f "${BUILD_DIR}/build.ninja" ]]; then
    return
  fi

  log "configuring builtin build: ${BUILD_DIR}"
  meson setup \
    --cross-file "${REPO_ROOT}/build-win64.txt" \
    -Dnative_llvm_path="${NATIVE_LLVM_PATH}" \
    --buildtype release \
    "${BUILD_DIR}"
}

install_to_stage() {
  rm -rf "${STAGE_DIR}"
  mkdir -p "${STAGE_DIR}"

  log "compiling builtin build"
  meson compile -C "${BUILD_DIR}"

  log "installing runtime to staging root: ${STAGE_DIR}"
  DESTDIR="${STAGE_DIR}" meson install -C "${BUILD_DIR}" --tags runtime,nvext
}

resolve_stage_prefix() {
  local prefix
  prefix="$(meson introspect "${BUILD_DIR}" --buildoptions | python3 -c 'import json, sys
data = json.load(sys.stdin)
for item in data:
    if item.get("name") == "prefix":
        print(item.get("value"))
        break
')"
  [[ -n "${prefix}" ]] || die "failed to read Meson install prefix"
  printf '%s%s\n' "${STAGE_DIR}" "${prefix}"
}

copy_runtime() {
  local stage_prefix="$1"
  local missing=0

  for rel in "${runtime_files[@]}"; do
    if [[ ! -f "${stage_prefix}/${rel}" ]]; then
      printf '[gamehub-dxmt] missing staged runtime file: %s\n' "${stage_prefix}/${rel}" >&2
      missing=1
    fi
  done
  [[ "${missing}" == 0 ]] || die "staged runtime is incomplete"

  mkdir -p "${BACKUP_DIR}/wine" "${GAMEHUB_DXMT_ROOT}/wine"

  log "backing up current GameHub DXMT files to: ${BACKUP_DIR}"
  for rel in "${runtime_files[@]}"; do
    if [[ -e "${GAMEHUB_DXMT_ROOT}/wine/${rel}" ]]; then
      mkdir -p "${BACKUP_DIR}/wine/$(dirname "${rel}")"
      cp -p "${GAMEHUB_DXMT_ROOT}/wine/${rel}" "${BACKUP_DIR}/wine/${rel}"
    fi
  done
  if [[ -f "${GAMEHUB_DXMT_ROOT}/manifest.json" ]]; then
    cp -p "${GAMEHUB_DXMT_ROOT}/manifest.json" "${BACKUP_DIR}/manifest.json"
  fi

  log "replacing GameHub test DXMT package: ${GAMEHUB_DXMT_ROOT}"
  for rel in "${runtime_files[@]}"; do
    mkdir -p "${GAMEHUB_DXMT_ROOT}/wine/$(dirname "${rel}")"
    cp -p "${stage_prefix}/${rel}" "${GAMEHUB_DXMT_ROOT}/wine/${rel}"
  done
}

write_manifest() {
  mkdir -p "${GAMEHUB_DXMT_ROOT}"
  python3 - "$GAMEHUB_DXMT_ROOT/manifest.json" "$GAMEHUB_DXMT_PACKAGE" <<'PY'
import json
import sys

manifest_path = sys.argv[1]
package_name = sys.argv[2]
manifest = {
    "id": package_name,
    "name": "DXMT GameHub Test",
    "version": "codex-builtin-test",
    "link_unix_libs": [
        "${COMPONENT_PATH}/wine/x86_64-unix",
    ],
    "link_windows_dlls": [
        "${COMPONENT_PATH}/wine/x86_64-windows",
    ],
    "environment_template": {
        "WINEDLLOVERRIDES": "d3d10core,d3d11,d3d12,dxgi,winemetal,nvapi64,nvngx=n,b",
        "WINEDLLPATH": "${COMPONENT_PATH}/wine",
        "DYLD_FALLBACK_LIBRARY_PATH": "${COMPONENT_PATH}/wine/x86_64-unix:${WINE_INSTALL_PATH}/lib:${WINE_INSTALL_PATH}/lib/wine/x86_64-unix",
    },
}
with open(manifest_path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=4)
    f.write("\n")
PY
}

main() {
  cd "${REPO_ROOT}"
  setup_tool_path
  setup_build_dir
  install_to_stage

  local stage_prefix
  stage_prefix="$(resolve_stage_prefix)"
  copy_runtime "${stage_prefix}"
  write_manifest

  log "done"
  log "package: ${GAMEHUB_DXMT_ROOT}"
  log "backup:  ${BACKUP_DIR}"
}

main "$@"
