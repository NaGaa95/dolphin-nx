#!/usr/bin/env bash
# Stage the unified Mesa SDK and localize its Vulkan ICD for Dolphin's PFN loader.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ZIP="${1:-${DOLPHIN_SWITCH_MESA_SDK_ZIP:-}}"
OUT="${2:-${ROOT}/build-switch/mesa-sdk}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BIN="${DEVKITPRO}/devkitA64/bin"
LD="${BIN}/aarch64-none-elf-ld"
OBJCOPY="${BIN}/aarch64-none-elf-objcopy"
NM="${BIN}/aarch64-none-elf-nm"
AR="${BIN}/aarch64-none-elf-ar"

if command -v bsdtar >/dev/null 2>&1; then
  BSDTAR="$(command -v bsdtar)"
elif [[ -x /c/Windows/System32/tar.exe ]]; then
  BSDTAR=/c/Windows/System32/tar.exe
else
  echo "A bsdtar-compatible archive tool is required to read ${ZIP}" >&2
  exit 1
fi

if [[ -z "${ZIP}" ]]; then
  echo "Usage: $0 <mesa-switch-unified-sdk.zip> [output-directory]" >&2
  echo "Alternatively set DOLPHIN_SWITCH_MESA_SDK_ZIP." >&2
  exit 1
fi
if [[ ! -f "${ZIP}" ]]; then
  echo "Unified Mesa SDK not found: ${ZIP}" >&2
  exit 1
fi

ARCHIVE_LIST="$("${BSDTAR}" -tf "${ZIP}")"
SDK_ROOT="$(printf '%s\n' "${ARCHIVE_LIST}" | awk '
  !found && /\/lib\/cmake\/OpenGL\/OpenGLConfig\.cmake$/ {
    sub(/\/lib\/cmake\/OpenGL\/OpenGLConfig\.cmake$/, "");
    print;
    found = 1;
  }
')"
if [[ -z "${SDK_ROOT}" ]]; then
  echo "The archive is not a unified Mesa Switch SDK (OpenGLConfig.cmake is missing)." >&2
  exit 1
fi

mkdir -p "$(dirname "${OUT}")"
WORK="$(mktemp -d "$(dirname "${OUT}")/mesa-sdk.prepare.XXXXXX")"
trap 'rm -rf -- "${WORK}"' EXIT

echo "Extracting unified Mesa SDK from ${SDK_ROOT} ..."
"${BSDTAR}" -xf "${ZIP}" -C "${WORK}"
SDK_SOURCE="${WORK}/${SDK_ROOT}"
for required in \
  lib/libvulkan.a \
  lib/libEGL.a \
  lib/cmake/OpenGL/OpenGLConfig.cmake \
  include/EGL/egl.h \
  include/vulkan/vulkan.h; do
  if [[ ! -f "${SDK_SOURCE}/${required}" ]]; then
    echo "Unified Mesa SDK file is missing: ${required}" >&2
    exit 1
  fi
done

PREPARED="${WORK}/prepared"
mkdir -p "${PREPARED}"
cp -a "${SDK_SOURCE}/." "${PREPARED}/"

# Dolphin's Vulkan loader stores entry points in global PFN variables named
# like Vulkan functions. Resolve Mesa into one object and localize only the
# public vk* entry points which collide with those variables. Mesa's internal
# symbols, especially the Horizon runtime shared with EGL, must stay global so
# one process has one device/runtime lifetime graph.
MERGED="${WORK}/libvulkan_merged.o"
LOCAL="${PREPARED}/libvulkan_local.o"
LOCAL_ARCHIVE="${PREPARED}/libvulkan_local.a"
PUBLIC_VK_SYMBOLS="${WORK}/vulkan_public_symbols.txt"
"${LD}" -r \
  -u vk_icdGetInstanceProcAddr \
  -u vk_icdNegotiateLoaderICDInterfaceVersion \
  -u vk_icdGetPhysicalDeviceProcAddr \
  --start-group "${PREPARED}/lib/libvulkan.a" --end-group \
  -o "${MERGED}"
"${NM}" -g --defined-only "${MERGED}" | awk '$NF ~ /^vk[A-Z]/ { print $NF }' | \
  LC_ALL=C sort -u > "${PUBLIC_VK_SYMBOLS}"
if [[ ! -s "${PUBLIC_VK_SYMBOLS}" ]]; then
  echo "Mesa Vulkan object has no public entry points to localize" >&2
  exit 1
fi
# Mesa's loaderless Rust support supplies this POSIX compatibility shim, and
# DolphinSwitch supplies the same process-wide function. Keep Mesa's copy
# private to its merged object; this does not affect shared Horizon state.
printf '%s\n' writev >> "${PUBLIC_VK_SYMBOLS}"
LC_ALL=C sort -u -o "${PUBLIC_VK_SYMBOLS}" "${PUBLIC_VK_SYMBOLS}"
"${OBJCOPY}" --localize-symbols="${PUBLIC_VK_SYMBOLS}" "${MERGED}" "${LOCAL}"
"${AR}" rcs "${LOCAL_ARCHIVE}" "${LOCAL}"

for symbol in \
  vk_icdGetInstanceProcAddr \
  vk_icdNegotiateLoaderICDInterfaceVersion \
  vk_icdGetPhysicalDeviceProcAddr; do
  if ! "${NM}" -g --defined-only "${LOCAL}" | \
    awk -v expected="${symbol}" '$NF == expected { found = 1 } END { exit !found }'; then
    echo "Localized Vulkan object is missing ${symbol}" >&2
    exit 1
  fi
done

if "${NM}" -g --defined-only "${LOCAL}" | awk '$NF ~ /^vk[A-Z]/ { found = 1 } END { exit !found }'; then
  echo "Localized Vulkan object still exports a public Vulkan entry point" >&2
  exit 1
fi
for symbol in \
  nouveau_horizon_runtime_get \
  nvk_loaderless_GetInstanceProcAddr \
  nvk_loaderless_GetDeviceProcAddr; do
  if ! "${NM}" -g --defined-only "${LOCAL}" | \
    awk -v expected="${symbol}" '$NF == expected { found = 1 } END { exit !found }'; then
    echo "Localized Vulkan object no longer exports ${symbol}" >&2
    exit 1
  fi
done

OLD="${OUT}.old.$$"
if [[ -e "${OUT}" ]]; then
  mv "${OUT}" "${OLD}"
fi
mv "${PREPARED}" "${OUT}"
if [[ -e "${OLD}" ]]; then
  rm -rf -- "${OLD}"
fi

echo "Prepared unified Mesa SDK at ${OUT}"
echo "Localized Vulkan ICD: ${OUT}/libvulkan_local.o"
echo "Localized Vulkan archive: ${OUT}/libvulkan_local.a"
