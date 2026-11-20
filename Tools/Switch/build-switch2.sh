#!/usr/bin/env bash
# Build dolphin-nx2.nro, the Nintendo Switch 2 (kaiHEN) build of the Switch frontend, with its embedded
# dolphin-drako3d.nro runtime. It needs ouncebrew's ounce-devkit in $DEVKITPRO (libounce, drako3d, kaiHEN) and the
# Switch 2 Mesa SDK (mesa-nx, with uam2rt). The Switch build (Tools/Switch/build.sh) is unaffected: both use
# their own build directory.
#
# Usage: build-switch2.sh [mesa-switch2-horizon-sdk.zip]
# Environment: DOLPHIN_SWITCH2_BUILD_DIR, DOLPHIN_SWITCH2_JOBS, DOLPHIN_SWITCH2_LTO (ON),
#              DOLPHIN_SWITCH_RELEASE_VERSION.
set -euo pipefail

# MSYS2's cmake and ninja (devkitPro's CMake files refuse others, such as a CLANGARM64 shell's).
export PATH="/usr/bin:/bin:${PATH:-}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${DOLPHIN_SWITCH2_BUILD_DIR:-${ROOT}/build-switch2}"
MESA_SDK_ZIP="${1:-${DOLPHIN_SWITCH2_MESA_SDK_ZIP:-${ROOT}/../mesa-nx/dist/mesa-26.2.7-switch2-horizon-sdk.zip}}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITA64="${DEVKITA64:-${DEVKITPRO}/devkitA64}"
JOBS="${DOLPHIN_SWITCH2_JOBS:-${DOLPHIN_SWITCH_JOBS:-18}}"
RELEASE_VERSION="${DOLPHIN_SWITCH_RELEASE_VERSION:-1.2.3}"
LTO="${DOLPHIN_SWITCH2_LTO:-ON}"

if [[ ! -f "${DEVKITPRO}/libounce/cmake/Switch2.cmake" ]]; then
  echo "No Switch 2 devkit (libounce) in ${DEVKITPRO}: install https://github.com/ouncebrew/ounce-devkit" >&2
  exit 1
fi
if [[ ! -f "${MESA_SDK_ZIP}" ]]; then
  echo "Missing: ${MESA_SDK_ZIP}" >&2
  echo "Usage: $0 [mesa-switch2-horizon-sdk.zip]" >&2
  exit 1
fi

mkdir -p "${BUILD_DIR}/tmp"
export DEVKITPRO DEVKITA64
# (portlibs/switch2/bin first: its pkg-config and tools are the Switch 2's)
export PATH="${DEVKITPRO}/portlibs/switch2/bin:${DEVKITPRO}/tools/bin:${PATH}"
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

bash "${ROOT}/Tools/Switch/prepare_mesa_sdk_switch2.sh" "${MESA_SDK_ZIP}" "${BUILD_DIR}/mesa-sdk"

cmake -S "${ROOT}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${ROOT}/CMake/Toolchain-Switch2.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DDOLPHIN_SWITCH2=ON \
  -DDOLPHIN_SWITCH_MESA_SDK_ROOT="${BUILD_DIR}/mesa-sdk" \
  -DDOLPHIN_SWITCH_NVK_OBJECT="${BUILD_DIR}/mesa-sdk/libvulkan_local.o" \
  -DDOLPHIN_SWITCH_RELEASE_VERSION="${RELEASE_VERSION}" \
  -DENABLE_LTO="${LTO}" \
  -DDOLPHIN_SWITCH_RUNTIME_JOBS="${JOBS}"

cmake --build "${BUILD_DIR}" --target dolphin_nro --parallel "${JOBS}"

NRO="${BUILD_DIR}/Binaries/dolphin-nx2.nro"
if [[ ! -f "${NRO}" ]]; then
  echo "The build finished without ${NRO}" >&2
  exit 1
fi
echo "Built ${NRO}"
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "${NRO}"
fi
