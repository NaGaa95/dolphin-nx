#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${DOLPHIN_SWITCH_BUILD_DIR:-${ROOT}/build-switch}"
NVK_ZIP="${1:-${DOLPHIN_SWITCH_NVK_ZIP:-}}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITA64="${DEVKITA64:-${DEVKITPRO}/devkitA64}"
JOBS="${DOLPHIN_SWITCH_JOBS:-18}"

if [[ -z "${NVK_ZIP}" ]]; then
  echo "Usage: $0 <mesa-switch-vulkan-sdk.zip|builddir-switch.zip>" >&2
  echo "Alternatively set DOLPHIN_SWITCH_NVK_ZIP." >&2
  exit 1
fi

if [[ ! -f "${DEVKITPRO}/cmake/Switch.cmake" ]]; then
  echo "A devkitPro installation with libnx was not found at ${DEVKITPRO}." >&2
  exit 1
fi

if [[ ! -d "${BUILD_DIR}/tmp" ]]; then
  mkdir -p "${BUILD_DIR}/tmp"
fi
export DEVKITPRO DEVKITA64
export PATH="${DEVKITPRO}/tools/bin:${PATH}"
export TMPDIR="${BUILD_DIR}/tmp"

# devkitA64 uses Windows TEMP paths under MSYS2.
if command -v cygpath >/dev/null 2>&1; then
  SWITCH_TMP_WINDOWS="$(cygpath -w "${TMPDIR}")"
  export TEMP="${SWITCH_TMP_WINDOWS}"
  export TMP="${SWITCH_TMP_WINDOWS}"
else
  export TEMP="${TMPDIR}"
  export TMP="${TMPDIR}"
fi

"${ROOT}/Tools/Switch/prepare_nvk.sh" "${NVK_ZIP}" "${BUILD_DIR}/nvk"

cmake -S "${ROOT}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${ROOT}/CMake/Toolchain-Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_LTO=ON

cmake --build "${BUILD_DIR}" --target dolphin_nro --parallel "${JOBS}"

echo "Built ${BUILD_DIR}/Binaries/dolphin.nro"
