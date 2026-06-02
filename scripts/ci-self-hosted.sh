#!/usr/bin/env bash
set -euo pipefail

CI_ROOT="${DXMT_CI_ROOT:-/opt/dxmt-ci}"
TOOLCHAINS_DIR="${CI_ROOT}/toolchains"
DOWNLOADS_DIR="${CI_ROOT}/downloads"
ARTIFACTS_DIR="${CI_ROOT}/artifacts"
CCACHE_ROOT="${CI_ROOT}/ccache"
RUN_KEY="${GITHUB_RUN_ID:-local}-${GITHUB_RUN_ATTEMPT:-1}"

LLVM_VERSION="${LLVM_VERSION:-llvmorg-15.0.7}"
WINE_VERSION="${WINE_VERSION:-v8.16-3shain}"
WINE_URL="${WINE_URL:-https://github.com/3Shain/wine/releases/download/v8.16-3shain/wine.tar.gz}"
WINE_ARM64EC_VERSION="${WINE_ARM64EC_VERSION:-wine-11.2}"
WINE_ARM64EC_URL="${WINE_ARM64EC_URL:-https://github.com/3Shain/wine/releases/download/wine-11.2/wine-11.2.tar.gz}"
LLVM_MINGW_VERSION="${LLVM_MINGW_VERSION:-20251216}"
LLVM_MINGW_DIR="${LLVM_MINGW_DIR:-llvm-mingw-20251216-ucrt-macos-universal}"
MESON_VERSION="${MESON_VERSION:-1.10.0}"

log() {
  printf '[dxmt-ci] %s\n' "$*"
}

die() {
  printf '[dxmt-ci] error: %s\n' "$*" >&2
  exit 1
}

sudo_run() {
  if [[ "${EUID}" == 0 ]]; then
    "$@"
  elif sudo -n true 2>/dev/null; then
    sudo "$@"
  elif [[ -n "${MAC_RUNNER_SUDO_PASS:-}" ]]; then
    printf '%s\n' "${MAC_RUNNER_SUDO_PASS}" | sudo -S "$@"
  else
    die "sudo is required for '$*'; set MAC_RUNNER_SUDO_PASS or pre-create ${CI_ROOT}"
  fi
}

append_path() {
  local dir="$1"
  [[ -d "${dir}" ]] || return 0
  case ":${PATH}:" in
    *":${dir}:"*) ;;
    *) export PATH="${dir}:${PATH}" ;;
  esac
  if [[ -n "${GITHUB_PATH:-}" ]]; then
    printf '%s\n' "${dir}" >> "${GITHUB_PATH}"
  fi
}

setup_paths() {
  append_path /opt/homebrew/bin
  append_path /usr/local/bin
  append_path /opt/homebrew/opt/bison/bin
  append_path /usr/local/opt/bison/bin
  for pybin in "${HOME}"/Library/Python/*/bin; do
    append_path "${pybin}"
  done
  if [[ -d "${GITHUB_WORKSPACE:-}/toolchains/${LLVM_MINGW_DIR}/bin" ]]; then
    append_path "${GITHUB_WORKSPACE}/toolchains/${LLVM_MINGW_DIR}/bin"
  fi
}

ensure_root() {
  if [[ ! -d "${CI_ROOT}" ]]; then
    sudo_run mkdir -p "${CI_ROOT}"
  fi
  sudo_run mkdir -p "${TOOLCHAINS_DIR}" "${DOWNLOADS_DIR}" "${ARTIFACTS_DIR}" "${CCACHE_ROOT}"
  sudo_run chown -R "$(id -u):$(id -g)" "${CI_ROOT}"
}

ensure_brew() {
  setup_paths
  command -v brew >/dev/null || die "Homebrew is required on the self-hosted runner"
  export HOMEBREW_NO_AUTO_UPDATE="${HOMEBREW_NO_AUTO_UPDATE:-1}"
  export HOMEBREW_NO_INSTALL_CLEANUP="${HOMEBREW_NO_INSTALL_CLEANUP:-1}"
}

ensure_brew_packages() {
  ensure_brew
  local missing=()
  for package in "$@"; do
    if ! brew list --formula "${package}" >/dev/null 2>&1; then
      missing+=("${package}")
    fi
  done
  if (( ${#missing[@]} )); then
    log "installing Homebrew packages: ${missing[*]}"
    brew install "${missing[@]}"
  fi
}

ensure_meson() {
  setup_paths
  if command -v meson >/dev/null 2>&1 &&
     [[ "$(meson --version)" == "${MESON_VERSION}" ]]; then
    return
  fi
  log "installing Meson ${MESON_VERSION}"
  python3 -m pip install --user "meson==${MESON_VERSION}" --break-system-packages \
    || python3 -m pip install --user "meson==${MESON_VERSION}"
  setup_paths
}

check_apple_tools() {
  command -v xcrun >/dev/null || die "xcrun is missing; install Apple Command Line Tools"
  xcrun --sdk macosx --show-sdk-path >/dev/null
  ensure_metal_toolchain
}

metal_tools_available() {
  xcrun --find metal >/dev/null 2>&1 &&
    xcrun --find metallib >/dev/null 2>&1
}

ensure_metal_toolchain() {
  if metal_tools_available; then
    return
  fi

  command -v xcodebuild >/dev/null ||
    die "Metal tools are missing and xcodebuild is unavailable; install Xcode or the Metal Toolchain manually"

  log "installing optional Metal Toolchain"
  xcodebuild -downloadComponent metalToolchain ||
    xcodebuild -downloadComponent MetalToolchain ||
    true
  if metal_tools_available; then
    return
  fi

  ensure_root
  local export_dir="${DOWNLOADS_DIR}/metalToolchain-export"
  rm -rf "${export_dir}"
  mkdir -p "${export_dir}"
  xcodebuild -downloadComponent metalToolchain -exportPath "${export_dir}" ||
    xcodebuild -downloadComponent MetalToolchain -exportPath "${export_dir}"

  local bundle
  bundle="$(find "${export_dir}" -maxdepth 1 -name 'MetalToolchain*.exportedBundle' -print -quit)"
  [[ -n "${bundle}" ]] || die "Metal Toolchain export bundle was not created"
  xcodebuild -importComponent metalToolchain -importPath "${bundle}" ||
    xcodebuild -importComponent MetalToolchain -importPath "${bundle}"

  metal_tools_available ||
    die "Metal Toolchain install completed but xcrun still cannot find metal/metallib"
}

setup_host() {
  ensure_root
  setup_paths
  ensure_brew_packages cmake ninja bison mingw-w64 ccache
  ensure_meson
  check_apple_tools
}

download_file() {
  local url="$1"
  local output="$2"
  if [[ ! -f "${output}" ]]; then
    log "downloading ${url}"
    curl -fL "${url}" -o "${output}"
  fi
}

ensure_wine() {
  local flavor="$1"
  ensure_root
  local version url target archive strip_args=()
  case "${flavor}" in
    x86_64)
      version="${WINE_VERSION}"
      url="${WINE_URL}"
      target="${TOOLCHAINS_DIR}/wine-x86_64-${version}"
      archive="${DOWNLOADS_DIR}/wine-x86_64-${version}.tar.gz"
      ;;
    arm64ec)
      version="${WINE_ARM64EC_VERSION}"
      url="${WINE_ARM64EC_URL}"
      target="${TOOLCHAINS_DIR}/wine-arm64ec-${version}"
      archive="${DOWNLOADS_DIR}/wine-arm64ec-${version}.tar.gz"
      strip_args=(--strip-components 2)
      ;;
    *)
      die "unknown Wine flavor: ${flavor}"
      ;;
  esac
  if [[ -f "${target}/.dxmt-ci-ready" ]]; then
    log "Wine ${flavor} already prepared: ${target}"
    return
  fi
  rm -rf "${target}"
  mkdir -p "${target}"
  download_file "${url}" "${archive}"
  tar -zxf "${archive}" "${strip_args[@]}" -C "${target}"
  touch "${target}/.dxmt-ci-ready"
}

ensure_llvm_mingw() {
  ensure_root
  local target="${TOOLCHAINS_DIR}/${LLVM_MINGW_DIR}"
  local archive="${DOWNLOADS_DIR}/${LLVM_MINGW_DIR}.tar.xz"
  if [[ -x "${target}/bin/x86_64-w64-mingw32-gcc" ]]; then
    log "LLVM-MinGW already prepared: ${target}"
    return
  fi
  rm -rf "${target}"
  download_file "https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION}/${LLVM_MINGW_DIR}.tar.xz" "${archive}"
  mkdir -p "${TOOLCHAINS_DIR}"
  tar -xf "${archive}" -C "${TOOLCHAINS_DIR}"
}

ensure_llvm_project() {
  ensure_root
  local target="${TOOLCHAINS_DIR}/llvm-project-${LLVM_VERSION}"
  if [[ -d "${target}/llvm" ]]; then
    log "LLVM source already prepared: ${target}"
    return
  fi
  rm -rf "${target}"
  git clone --depth 1 --branch "${LLVM_VERSION}" https://github.com/llvm/llvm-project.git "${target}"
}

llvm_project_dir() {
  printf '%s\n' "${TOOLCHAINS_DIR}/llvm-project-${LLVM_VERSION}"
}

ensure_llvm_darwin() {
  local arch="$1"
  ensure_root
  setup_paths
  local target="${TOOLCHAINS_DIR}/llvm-darwin-${LLVM_VERSION}-${arch}"
  local build="${TOOLCHAINS_DIR}/llvm-darwin-${LLVM_VERSION}-${arch}-build"
  if [[ -d "${target}/lib" || -d "${target}/include" ]]; then
    log "LLVM Darwin ${arch} already prepared: ${target}"
    return
  fi
  ensure_llvm_project
  rm -rf "${build}" "${target}"
  mkdir -p "${build}"
  cmake -B "${build}" -S "$(llvm_project_dir)/llvm" \
    -DCMAKE_INSTALL_PREFIX="${target}" \
    -DCMAKE_OSX_ARCHITECTURES="${arch}" \
    -DLLVM_HOST_TRIPLE="${arch}-apple-darwin" \
    -DLLVM_ENABLE_ASSERTIONS=On \
    -DLLVM_ENABLE_ZSTD=Off \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_TARGETS_TO_BUILD="" \
    -DLLVM_BUILD_TOOLS=Off \
    -DBUG_REPORT_URL="https://github.com/gamesir-labs/dxmt" \
    -DPACKAGE_VENDOR="DXMT" \
    -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off \
    -G Ninja
  cmake --build "${build}" --parallel "$(sysctl -n hw.ncpu)"
  cmake --install "${build}"
}

ensure_llvm_win() {
  ensure_root
  setup_paths
  local target="${TOOLCHAINS_DIR}/llvm-win-${LLVM_VERSION}-x86_64-w64-mingw32"
  local build="${TOOLCHAINS_DIR}/llvm-win-${LLVM_VERSION}-x86_64-w64-mingw32-build"
  if [[ -d "${target}/lib" || -d "${target}/include" ]]; then
    log "LLVM Windows already prepared: ${target}"
    return
  fi
  ensure_llvm_project
  ensure_llvm_mingw
  rm -rf "${build}" "${target}"
  mkdir -p "${build}"
  append_path "${TOOLCHAINS_DIR}/${LLVM_MINGW_DIR}/bin"
  cmake -B "${build}" -S "$(llvm_project_dir)/llvm" -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_INSTALL_PREFIX="${target}" \
    -DLLVM_HOST_TRIPLE=x86_64-w64-mingw32 \
    -DLLVM_ENABLE_ASSERTIONS=On \
    -DLLVM_ENABLE_ZSTD=Off \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_TARGETS_TO_BUILD="" \
    -DLLVM_BUILD_TOOLS=Off \
    -DCMAKE_SYSROOT="${TOOLCHAINS_DIR}/${LLVM_MINGW_DIR}" \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DBUG_REPORT_URL="https://github.com/gamesir-labs/dxmt" \
    -DPACKAGE_VENDOR="DXMT" \
    -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off \
    -G Ninja
  cmake --build "${build}" --parallel "$(sysctl -n hw.ncpu)"
  cmake --install "${build}"
}

prepare_job() {
  setup_paths
  ensure_meson
  check_apple_tools
}

link_toolchains() {
  local wine_flavor="${1:-x86_64}"
  mkdir -p toolchains
  case "${wine_flavor}" in
    x86_64)
      ln -sfn "${TOOLCHAINS_DIR}/wine-x86_64-${WINE_VERSION}" toolchains/wine
      ;;
    arm64ec)
      ln -sfn "${TOOLCHAINS_DIR}/wine-arm64ec-${WINE_ARM64EC_VERSION}" toolchains/wine
      ;;
    *)
      die "unknown Wine flavor for link-toolchains: ${wine_flavor}"
      ;;
  esac
  ln -sfn "${TOOLCHAINS_DIR}/${LLVM_MINGW_DIR}" "toolchains/${LLVM_MINGW_DIR}"
  ln -sfn "${TOOLCHAINS_DIR}/llvm-darwin-${LLVM_VERSION}-x86_64" toolchains/llvm-darwin
  ln -sfn "${TOOLCHAINS_DIR}/llvm-darwin-${LLVM_VERSION}-arm64" toolchains/llvm-darwin-arm64
  ln -sfn "${TOOLCHAINS_DIR}/llvm-win-${LLVM_VERSION}-x86_64-w64-mingw32" toolchains/llvm
  append_path "${GITHUB_WORKSPACE}/toolchains/${LLVM_MINGW_DIR}/bin"
}

prepare_ccache() {
  local namespace="$1"
  shift
  local dir="${CCACHE_ROOT}/${namespace}"
  local wrapper_dir="${GITHUB_WORKSPACE}/.ccache-bin-${namespace}"
  mkdir -p "${dir}" "${wrapper_dir}"
  export CCACHE_DIR="${dir}"
  if [[ -n "${GITHUB_ENV:-}" ]]; then
    printf 'CCACHE_DIR=%s\n' "${dir}" >> "${GITHUB_ENV}"
  fi
  for compiler in "$@"; do
    local real_compiler
    real_compiler="$(command -v "${compiler}")" || die "missing compiler: ${compiler}"
    cat > "${wrapper_dir}/${compiler}" <<WRAPPER
#!/bin/sh
exec ccache "${real_compiler}" "\$@"
WRAPPER
    chmod +x "${wrapper_dir}/${compiler}"
  done
  append_path "${wrapper_dir}"
  ccache --set-config=max_size=4G
  ccache --zero-stats
}

artifact_dir() {
  local name="$1"
  printf '%s/%s/%s\n' "${ARTIFACTS_DIR}" "${RUN_KEY}" "${name}"
}

stage_artifact() {
  local name="$1"
  local source="$2"
  local dest
  dest="$(artifact_dir "${name}")"
  rm -rf "${dest}"
  mkdir -p "${dest}"
  cp -a "${source}/." "${dest}/"
  log "staged artifact ${name}: ${dest}"
}

copy_artifact() {
  local name="$1"
  local dest="$2"
  local source
  source="$(artifact_dir "${name}")"
  [[ -d "${source}" ]] || die "artifact not found: ${source}"
  mkdir -p "${dest}"
  cp -a "${source}/." "${dest}/"
}

stage_files() {
  local name="$1"
  shift
  local dest
  dest="$(artifact_dir "${name}")"
  rm -rf "${dest}"
  mkdir -p "${dest}"
  for file in "$@"; do
    cp -a "${file}" "${dest}/"
  done
  log "staged files ${name}: ${dest}"
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    {
      printf '### DXMT local artifact\n\n'
      printf '`%s`\n' "${dest}"
    } >> "${GITHUB_STEP_SUMMARY}"
  fi
}

usage() {
  cat <<'EOF'
usage: ci-self-hosted.sh <command> [args]

commands:
  setup-host
  prepare-job
  ensure-wine <x86_64|arm64ec>
  ensure-llvm-mingw
  ensure-llvm-project
  ensure-llvm-darwin <x86_64|arm64>
  ensure-llvm-win
  link-toolchains [x86_64|arm64ec]
  prepare-ccache <namespace> <compiler>...
  stage-artifact <name> <source-dir>
  copy-artifact <name> <dest-dir>
  stage-files <name> <file>...
EOF
}

command="${1:-}"
shift || true
case "${command}" in
  setup-host) setup_host "$@" ;;
  prepare-job) prepare_job "$@" ;;
  ensure-wine) ensure_wine "$@" ;;
  ensure-llvm-mingw) ensure_llvm_mingw "$@" ;;
  ensure-llvm-project) ensure_llvm_project "$@" ;;
  ensure-llvm-darwin) ensure_llvm_darwin "$@" ;;
  ensure-llvm-win) ensure_llvm_win "$@" ;;
  link-toolchains) link_toolchains "$@" ;;
  prepare-ccache) prepare_ccache "$@" ;;
  stage-artifact) stage_artifact "$@" ;;
  copy-artifact) copy_artifact "$@" ;;
  stage-files) stage_files "$@" ;;
  ""|-h|--help) usage ;;
  *) usage; die "unknown command: ${command}" ;;
esac
