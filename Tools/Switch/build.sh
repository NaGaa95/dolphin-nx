#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${DOLPHIN_SWITCH_BUILD_DIR:-${ROOT}/build-switch}"
MESA_SDK_ZIP="${1:-${DOLPHIN_SWITCH_MESA_SDK_ZIP:-}}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITA64="${DEVKITA64:-${DEVKITPRO}/devkitA64}"
JOBS="${DOLPHIN_SWITCH_JOBS:-18}"
RELEASE_VERSION="${DOLPHIN_SWITCH_RELEASE_VERSION:-1.0.8-v2}"

if [[ -z "${MESA_SDK_ZIP}" ]]; then
  echo "Usage: $0 <mesa-switch-unified-sdk.zip>" >&2
  echo "Alternatively set DOLPHIN_SWITCH_MESA_SDK_ZIP." >&2
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

bash "${ROOT}/Tools/Switch/prepare_mesa_sdk.sh" "${MESA_SDK_ZIP}" \
  "${BUILD_DIR}/mesa-sdk"

cmake -S "${ROOT}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${ROOT}/CMake/Toolchain-Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DDOLPHIN_SWITCH_MESA_SDK_ROOT="${BUILD_DIR}/mesa-sdk" \
  -DDOLPHIN_SWITCH_NVK_OBJECT="${BUILD_DIR}/mesa-sdk/libvulkan_local.o" \
  -DDOLPHIN_SWITCH_RELEASE_VERSION="${RELEASE_VERSION}" \
  -DENABLE_LTO=ON

cmake --build "${BUILD_DIR}" --target dolphin_nro --parallel "${JOBS}"

echo "Built ${BUILD_DIR}/Binaries/dolphin.nro"
