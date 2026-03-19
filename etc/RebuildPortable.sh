#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="build-portable"
DIST_DIR="dist/portable"
BUILD_TYPE="RELEASE"
PULL_REPO="no"
THREADS=""

usage() {
  cat <<'EOF'
Usage: ./etc/RebuildPortable.sh [options]

Locally rebuild OpenROAD and export the final `openroad` executable.

Options:
  --pull                 Run git pull --ff-only before rebuilding.
  --build-dir PATH       Build directory relative to repo root.
                         Default: build-portable
  --dist-dir PATH        Output directory relative to repo root.
                         Default: dist/portable
  --build-type TYPE      CMake build type: RELEASE or DEBUG.
                         Default: RELEASE
  --threads N            Compile with N threads. Default: all visible cores.
  -h, --help             Show this help.

Output:
  - dist/portable/openroad
  - dist/portable/openroad-<git describe>
  - dist/portable/openroad-<git describe>.sha256
  - dist/portable/openroad-<git describe>.ldd.txt
  - dist/portable/openroad-<git describe>.build-info.txt

Notes:
  - This script does not use docker/podman/AppImage.
  - The output is the native Linux ELF executable `openroad`.
  - For better portability, GUI/Python/TclX/tclreadline are disabled, and the
    link step uses `-static-libstdc++ -static-libgcc`.
  - glibc is still usually dynamic, so the target Linux should have glibc >=
    the build machine.
EOF
}

log() {
  printf '[INFO] %s\n' "$*"
}

die() {
  printf '[ERROR] %s\n' "$*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing required command: $1"
}

default_threads() {
  if [[ -n "${THREADS}" ]]; then
    echo "${THREADS}"
  elif command -v getconf >/dev/null 2>&1; then
    getconf _NPROCESSORS_ONLN
  elif command -v nproc >/dev/null 2>&1; then
    nproc --all
  else
    echo "1"
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pull)
      PULL_REPO="yes"
      ;;
    --build-dir)
      [[ $# -ge 2 ]] || die "--build-dir requires a value"
      BUILD_DIR="$2"
      shift
      ;;
    --dist-dir)
      [[ $# -ge 2 ]] || die "--dist-dir requires a value"
      DIST_DIR="$2"
      shift
      ;;
    --build-type)
      [[ $# -ge 2 ]] || die "--build-type requires a value"
      BUILD_TYPE="$2"
      shift
      ;;
    --threads)
      [[ $# -ge 2 ]] || die "--threads requires a value"
      THREADS="$2"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "Unknown option: $1"
      ;;
  esac
  shift
done

case "${BUILD_TYPE}" in
  RELEASE|DEBUG)
    ;;
  *)
    die "Unsupported --build-type value: ${BUILD_TYPE}"
    ;;
esac

require_cmd git
require_cmd sha256sum
require_cmd cmake

THREADS="$(default_threads)"
BUILD_DIR_ABS="${REPO_ROOT}/${BUILD_DIR}"
DIST_DIR_ABS="${REPO_ROOT}/${DIST_DIR}"

mkdir -p "${DIST_DIR_ABS}"

if [[ "${PULL_REPO}" == "yes" ]]; then
  log "Updating repository"
  git -C "${REPO_ROOT}" pull --ff-only --recurse-submodules
fi

log "Synchronizing submodules"
git -C "${REPO_ROOT}" submodule sync --recursive
git -C "${REPO_ROOT}" submodule update --init --recursive

GIT_DESC="$(git -C "${REPO_ROOT}" describe --tags --always --dirty 2>/dev/null || git -C "${REPO_ROOT}" rev-parse --short HEAD)"
SAFE_GIT_DESC="${GIT_DESC//\//_}"

VERSIONED_BIN="${DIST_DIR_ABS}/openroad-${SAFE_GIT_DESC}"
LATEST_BIN="${DIST_DIR_ABS}/openroad"
LDD_FILE="${VERSIONED_BIN}.ldd.txt"
INFO_FILE="${VERSIONED_BIN}.build-info.txt"
SHA_FILE="${VERSIONED_BIN}.sha256"

log "Building OpenROAD locally with ${THREADS} threads"

CMAKE_OPTIONS=(
  "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
  "-DENABLE_TESTS=OFF"
  "-DBUILD_PYTHON=OFF"
  "-DBUILD_TCLX=OFF"
  "-DTCL_READLINE_LIBRARY="
  "-DTCL_READLINE_H="
  '-DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"'
)

printf -v CMAKE_ARGS '%s ' "${CMAKE_OPTIONS[@]}"

"${REPO_ROOT}/etc/Build.sh" \
  -clean \
  -keep-log \
  -dir="${BUILD_DIR}" \
  -threads="${THREADS}" \
  -no-gui \
  "-cmake=${CMAKE_ARGS% }"

BIN_SOURCE="${BUILD_DIR_ABS}/src/openroad"
[[ -x "${BIN_SOURCE}" ]] || die "Build finished but executable not found: ${BIN_SOURCE}"

cp "${BIN_SOURCE}" "${VERSIONED_BIN}"
cp "${BIN_SOURCE}" "${LATEST_BIN}"

if command -v strip >/dev/null 2>&1; then
  strip "${VERSIONED_BIN}" || true
  cp "${VERSIONED_BIN}" "${LATEST_BIN}"
fi

sha256sum "${VERSIONED_BIN}" > "${SHA_FILE}"

if command -v ldd >/dev/null 2>&1; then
  ldd "${VERSIONED_BIN}" > "${LDD_FILE}" || true
fi

GLIBC_INFO="unknown"
if command -v ldd >/dev/null 2>&1; then
  GLIBC_INFO="$(ldd --version 2>/dev/null | head -n1 || true)"
fi

cat > "${INFO_FILE}" <<INFO
git_describe=${GIT_DESC}
build_type=${BUILD_TYPE}
threads=${THREADS}
host_uname=$(uname -srmo)
glibc=${GLIBC_INFO}
build_dir=${BUILD_DIR}
binary=${VERSIONED_BIN}
INFO

if [[ -f "${BUILD_DIR_ABS}/openroad_build.log" ]]; then
  cp "${BUILD_DIR_ABS}/openroad_build.log" "${VERSIONED_BIN}.build.log"
fi

log "Finished executable: ${VERSIONED_BIN}"
log "Latest symlink copy: ${LATEST_BIN}"
if [[ -f "${LDD_FILE}" ]]; then
  log "Runtime deps: ${LDD_FILE}"
fi
