#!/usr/bin/env bash
set -euo pipefail

CI_ROOT="${DXMT_CI_ROOT:-/opt/dxmt-ci}"
TOOLCHAINS_DIR="${CI_ROOT}/toolchains"
DOWNLOADS_DIR="${CI_ROOT}/downloads"
ARTIFACTS_DIR="${CI_ROOT}/artifacts"
CCACHE_ROOT="${CI_ROOT}/ccache"
SOURCES_DIR="${CI_ROOT}/sources"
RUN_KEY="${GITHUB_RUN_ID:-local}-${GITHUB_RUN_ATTEMPT:-1}"

LLVM_VERSION="${LLVM_VERSION:-llvmorg-15.0.7}"
WINE_VERSION="${WINE_VERSION:-proton-11.0-macos}"
WINE_GIT_URL="${WINE_GIT_URL:-https://github.com/gamesir123/wine-proton-macos.git}"
WINE_GIT_BRANCH="${WINE_GIT_BRANCH:-proton-11.0-macos}"
WINE_ARM64EC_VERSION="${WINE_ARM64EC_VERSION:-wine-11.2}"
WINE_ARM64EC_URL="${WINE_ARM64EC_URL:-https://github.com/3Shain/wine/releases/download/wine-11.2/wine-11.2.tar.gz}"
LLVM_MINGW_VERSION="${LLVM_MINGW_VERSION:-20251216}"
LLVM_MINGW_DIR="${LLVM_MINGW_DIR:-llvm-mingw-20251216-ucrt-macos-universal}"
MESON_VERSION="${MESON_VERSION:-1.10.0}"

log() {
  printf '[dxmt-ci] %s\n' "$*" >&2
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
  if [[ -d "${CI_ROOT}" && -w "${CI_ROOT}" ]]; then
    mkdir -p "${TOOLCHAINS_DIR}" "${DOWNLOADS_DIR}" "${ARTIFACTS_DIR}" "${CCACHE_ROOT}" "${SOURCES_DIR}"
    return
  fi

  if [[ ! -d "${CI_ROOT}" ]]; then
    sudo_run mkdir -p "${CI_ROOT}"
  fi

  sudo_run mkdir -p "${TOOLCHAINS_DIR}" "${DOWNLOADS_DIR}" "${ARTIFACTS_DIR}" "${CCACHE_ROOT}" "${SOURCES_DIR}"
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
  for package in "$@"; do
    if ! brew list --formula "${package}" >/dev/null 2>&1; then
      log "installing Homebrew formula: ${package}"
      brew install --formula "${package}"
    fi
  done
}

ensure_command_or_formula() {
  local command_name="$1"
  local formula="$2"
  if command -v "${command_name}" >/dev/null 2>&1; then
    return
  fi
  ensure_brew_packages "${formula}"
  command -v "${command_name}" >/dev/null 2>&1 ||
    die "${command_name} is missing after installing ${formula}"
}

ensure_homebrew_build_tools() {
  ensure_command_or_formula cmake cmake
  ensure_command_or_formula ninja ninja
  ensure_command_or_formula bison bison
  ensure_command_or_formula x86_64-w64-mingw32-gcc mingw-w64
  ensure_command_or_formula i686-w64-mingw32-gcc mingw-w64
  ensure_command_or_formula ccache ccache
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
  ensure_homebrew_build_tools
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

safe_name() {
  printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '-'
}

git_with_wine_auth() {
  if [[ -n "${WINE_ACCESS_TOKEN:-}" ]]; then
    local askpass
    askpass="${RUNNER_TEMP:-${DOWNLOADS_DIR}}/dxmt-wine-git-askpass.sh"
    mkdir -p "$(dirname "${askpass}")"
    cat > "${askpass}" <<'EOF'
#!/bin/sh
case "$1" in
  *Username*) printf '%s\n' x-access-token ;;
  *Password*) printf '%s\n' "$WINE_ACCESS_TOKEN" ;;
  *) printf '\n' ;;
esac
EOF
    chmod 700 "${askpass}"
    GIT_ASKPASS="${askpass}" git "$@"
  else
    git "$@"
  fi
}

sync_wine_git_source() {
  local source_dir="$1"
  local old_commit new_commit
  export GIT_TERMINAL_PROMPT=0

  if [[ ! -d "${source_dir}/.git" ]]; then
    rm -rf "${source_dir}"
    log "cloning Wine source ${WINE_GIT_URL} (${WINE_GIT_BRANCH})"
    git_with_wine_auth clone --depth 1 --single-branch --branch "${WINE_GIT_BRANCH}" \
      "${WINE_GIT_URL}" "${source_dir}" ||
      die "failed to clone Wine source ${WINE_GIT_URL} (${WINE_GIT_BRANCH})"
    WINE_SOURCE_COMMIT="$(git -C "${source_dir}" rev-parse HEAD)" ||
      die "failed to resolve Wine source commit"
    return
  fi

  old_commit="$(git -C "${source_dir}" rev-parse HEAD 2>/dev/null || true)"
  git -C "${source_dir}" remote set-url origin "${WINE_GIT_URL}"
  log "fetching Wine source ${WINE_GIT_BRANCH}"
  git_with_wine_auth -C "${source_dir}" fetch --depth 1 origin "${WINE_GIT_BRANCH}" ||
    die "failed to fetch Wine source ${WINE_GIT_BRANCH}"
  git -C "${source_dir}" reset --hard FETCH_HEAD >&2 ||
    die "failed to reset Wine source to FETCH_HEAD"
  new_commit="$(git -C "${source_dir}" rev-parse HEAD)" ||
    die "failed to resolve Wine source commit"
  if [[ -n "${old_commit}" && "${old_commit}" != "${new_commit}" ]]; then
    rm -f "${source_dir}/.generated"
  fi
  WINE_SOURCE_COMMIT="${new_commit}"
}

wine_install_ready() {
  local install_dir="$1"
  [[ -x "${install_dir}/bin/winebuild" ]] &&
    [[ -f "${install_dir}/lib/wine/i386-windows/libwinecrt0.a" ]] &&
    [[ -f "${install_dir}/lib/wine/i386-windows/libntdll.a" ]] &&
    [[ -f "${install_dir}/lib/wine/i386-windows/dbghelp.dll" ]] &&
    [[ -f "${install_dir}/lib/wine/x86_64-windows/libwinecrt0.a" ]] &&
    [[ -f "${install_dir}/lib/wine/x86_64-windows/libntdll.a" ]] &&
    [[ -f "${install_dir}/lib/wine/x86_64-windows/dbghelp.dll" ]] &&
    [[ -f "${install_dir}/lib/wine/x86_64-unix/winemac.so" ]] &&
    [[ -f "${install_dir}/lib/wine/x86_64-unix/ntdll.so" ]]
}

build_wine_git_source() {
  local source_dir="$1"
  chmod +x "${source_dir}/scripts/build_on_m1.sh" "${source_dir}/scripts/build_on_intel.sh"
  case "$(uname -m)" in
    arm64)
      (cd "${source_dir}" && ./scripts/build_on_m1.sh)
      ;;
    x86_64)
      (cd "${source_dir}" && ./scripts/build_on_intel.sh)
      ;;
    *)
      die "unsupported host architecture for Wine build: $(uname -m)"
      ;;
  esac
}

ensure_wine_x86_64() {
  ensure_root
  local source_dir target install_dir commit stamp
  source_dir="${SOURCES_DIR}/wine-proton-macos-$(safe_name "${WINE_GIT_BRANCH}")"
  target="${TOOLCHAINS_DIR}/wine-x86_64-${WINE_VERSION}"
  WINE_SOURCE_COMMIT=""
  sync_wine_git_source "${source_dir}"
  commit="${WINE_SOURCE_COMMIT}"
  [[ -n "${commit}" ]] || die "Wine source commit is empty"
  install_dir="${source_dir}/install"
  stamp="${install_dir}/.dxmt-ci-source-commit"

  if wine_install_ready "${install_dir}" &&
     [[ -f "${stamp}" ]] &&
     [[ "$(cat "${stamp}")" == "${commit}" ]]; then
    log "Wine x86_64 already prepared from ${commit}: ${install_dir}"
  else
    log "building Wine x86_64 from ${commit}"
    rm -rf "${source_dir}/build" "${source_dir}/install"
    rm -f "${source_dir}/.generated"
    build_wine_git_source "${source_dir}"
    wine_install_ready "${install_dir}" ||
      die "Wine build finished but required install artifacts are missing: ${install_dir}"
    printf '%s\n' "${commit}" > "${stamp}"
  fi

  ln -sfn "${install_dir}" "${target}"
}

ensure_wine_arm64ec() {
  ensure_root
  local version url target archive
  version="${WINE_ARM64EC_VERSION}"
  url="${WINE_ARM64EC_URL}"
  target="${TOOLCHAINS_DIR}/wine-arm64ec-${version}"
  archive="${DOWNLOADS_DIR}/wine-arm64ec-${version}.tar.gz"

  if [[ -f "${target}/.dxmt-ci-ready" ]]; then
    log "Wine arm64ec already prepared: ${target}"
    return
  fi
  rm -rf "${target}"
  mkdir -p "${target}"
  download_file "${url}" "${archive}"
  tar -zxf "${archive}" --strip-components 2 -C "${target}"
  touch "${target}/.dxmt-ci-ready"
}

ensure_wine() {
  local flavor="$1"
  case "${flavor}" in
    x86_64)
      ensure_wine_x86_64
      ;;
    arm64ec)
      ensure_wine_arm64ec
      ;;
    *)
      die "unknown Wine flavor: ${flavor}"
      ;;
  esac
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
