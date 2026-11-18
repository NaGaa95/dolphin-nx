#!/usr/bin/env bash
# Stage the Switch 2 Mesa SDK (mesa-*-switch2-horizon-sdk.zip, with uam2rt) for Dolphin.
#
# The Switch 2 SDK's libEGL.a and libvulkan.a are linker scripts naming libmesa_egl.a, libmesa_gl.a,
# libmesa_glapi.a and libmesa_vulkan.a (NVK). Dolphin's Vulkan loader keeps its entry points in global PFN variables
# named like the Vulkan functions, so NVK's public vk* functions must not be global in the link (the Switch's
# prepare_mesa_sdk.sh does the same for its SDK). The Switch 2 archives also carry the same Mesa utility code twice
# (in the GL and the Vulkan archives), so NVK cannot be merged alone without duplicating it: everything is merged
# into one relocatable object in the linker scripts' order, which picks the same archive members as a normal link,
# and only then are the public vk* entry points (and Mesa's writev shim, which DolphinSwitch also provides)
# localized. libEGL.a and libvulkan.a are rewritten to name that object's archive.
#
# Usage: prepare_mesa_sdk_switch2.sh <mesa-switch2-sdk.zip> [output-directory]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ZIP="${1:-${DOLPHIN_SWITCH2_MESA_SDK_ZIP:-}}"
OUT="${2:-${ROOT}/build-switch2/mesa-sdk}"
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
  echo "A bsdtar-compatible archive tool is required to read the SDK zips" >&2
  exit 1
fi

if [[ -z "${ZIP}" || ! -f "${ZIP}" ]]; then
  echo "Usage: $0 <mesa-switch2-horizon-sdk.zip> [output-directory]" >&2
  echo "Switch 2 Mesa SDK not found: '${ZIP}'" >&2
  exit 1
fi

# The prefix inside an SDK zip: the directory holding lib/cmake/<package>/<file>.
zip_prefix() {
  "${BSDTAR}" -tf "$1" | awk -v marker="$2" '
    !found && index($0, marker) {
      sub(marker "$", "");
      print;
      found = 1;
    }'
}

SDK_ROOT="$(zip_prefix "${ZIP}" "/lib/cmake/OpenGL/OpenGLConfig.cmake")"
if [[ -z "${SDK_ROOT}" ]]; then
  echo "The archive is not a Mesa Switch 2 SDK (OpenGLConfig.cmake is missing)." >&2
  exit 1
fi

mkdir -p "$(dirname "${OUT}")"
WORK="$(mktemp -d "$(dirname "${OUT}")/mesa-sdk.prepare.XXXXXX")"
trap 'rm -rf -- "${WORK}"' EXIT

echo "Extracting the Switch 2 Mesa SDK from ${SDK_ROOT} ..."
"${BSDTAR}" -xf "${ZIP}" -C "${WORK}"
SDK_SOURCE="${WORK}/${SDK_ROOT}"
PREPARED="${WORK}/prepared"
mkdir -p "${PREPARED}"
cp -a "${SDK_SOURCE}/." "${PREPARED}/"
LIB="${PREPARED}/lib"

for required in \
  libmesa_egl.a libmesa_gl.a libmesa_glapi.a libmesa_vulkan.a libEGL.a libvulkan.a; do
  if [[ ! -f "${LIB}/${required}" ]]; then
    echo "Switch 2 Mesa SDK file is missing: lib/${required}" >&2
    exit 1
  fi
done
for required in include/EGL/egl.h include/vulkan/vulkan.h include/uam2rt.h lib/libuam2rt.a \
  lib/cmake/uam2rt/uam2rtConfig.cmake; do
  if [[ ! -f "${PREPARED}/${required}" ]]; then
    echo "Switch 2 Mesa SDK file is missing: ${required}" >&2
    exit 1
  fi
done
if ! grep -q "mesa_vulkan" "${LIB}/libEGL.a" 2>/dev/null; then
  echo "lib/libEGL.a is not the Switch 2 SDK's linker script; refusing to rewrite it" >&2
  exit 1
fi

# The roots: the public EGL and GL entry points (Dolphin, SDL 2 and the GL loader), the Vulkan ICD interface
# Dolphin's loader starts from, NVK's loaderless entry points and the GPU teardown libounce calls at exit.
ROOTS="${WORK}/roots.txt"
{
  for archive in libmesa_egl.a libmesa_gl.a libmesa_glapi.a; do
    "${NM}" -g --defined-only "${LIB}/${archive}" 2>/dev/null |
      awk 'NF >= 3 && $2 ~ /^[TDBRW]$/ && $3 ~ /^(egl|gl)[A-Z]/ { print $3 }'
  done
  "${NM}" -g --defined-only "${LIB}/libmesa_vulkan.a" 2>/dev/null |
    awk 'NF >= 3 && $3 ~ /^(vk_icd|nvk_loaderless_)/ { print $3 } NF >= 3 && $3 == "__nx_gpu_exit" { print $3 }'
} | LC_ALL=C sort -u > "${ROOTS}"
if ! grep -qx "eglGetProcAddress" "${ROOTS}" || ! grep -qx "vk_icdGetInstanceProcAddr" "${ROOTS}"; then
  echo "The Switch 2 SDK archives lack the expected EGL or Vulkan ICD entry points" >&2
  exit 1
fi
echo "Merging EGL, GL and NVK from $(wc -l < "${ROOTS}") entry points ..."

MERGED="${WORK}/libmesa_merged.o"
LOCAL="${PREPARED}/libmesa_local.o"
UNDEFINED_ROOTS=()
while IFS= read -r symbol; do
  UNDEFINED_ROOTS+=("-u" "${symbol}")
done < "${ROOTS}"
# (single pass, in the order of the SDK's libEGL.a script: the same members as a normal link)
"${LD}" -r "${UNDEFINED_ROOTS[@]}" \
  "${LIB}/libmesa_egl.a" "${LIB}/libmesa_gl.a" "${LIB}/libmesa_glapi.a" "${LIB}/libmesa_vulkan.a" \
  -o "${MERGED}"

PUBLIC_VK_SYMBOLS="${WORK}/vulkan_public_symbols.txt"
"${NM}" -g --defined-only "${MERGED}" | awk '$NF ~ /^vk[A-Z]/ { print $NF }' | LC_ALL=C sort -u > "${PUBLIC_VK_SYMBOLS}"
if [[ ! -s "${PUBLIC_VK_SYMBOLS}" ]]; then
  echo "The merged Mesa object has no public Vulkan entry points to localize" >&2
  exit 1
fi
printf '%s\n' writev >> "${PUBLIC_VK_SYMBOLS}"
LC_ALL=C sort -u -o "${PUBLIC_VK_SYMBOLS}" "${PUBLIC_VK_SYMBOLS}"
"${OBJCOPY}" --localize-symbols="${PUBLIC_VK_SYMBOLS}" "${MERGED}" "${LOCAL}"

if "${NM}" -g --defined-only "${LOCAL}" | awk '$NF ~ /^vk[A-Z]/ { found = 1 } END { exit !found }'; then
  echo "The localized Mesa object still exports a public Vulkan entry point" >&2
  exit 1
fi
for symbol in \
  vk_icdGetInstanceProcAddr \
  vk_icdNegotiateLoaderICDInterfaceVersion \
  nvk_loaderless_GetInstanceProcAddr \
  nvk_loaderless_GetDeviceProcAddr \
  eglGetDisplay \
  eglGetProcAddress; do
  if ! "${NM}" -g --defined-only "${LOCAL}" |
    awk -v expected="${symbol}" '$NF == expected { found = 1 } END { exit !found }'; then
    echo "The localized Mesa object does not export ${symbol}" >&2
    exit 1
  fi
done

# libEGL.a and libvulkan.a now name the merged object's archive and the C++ and math libraries. Unlike the SDK's
# scripts they leave out zlib, zstd and expat: Dolphin links its own zlib-ng and zstd (and portlibs' expat) after
# Mesa, and portlibs' zlib pulled in first would define zlib's functions twice. libvulkan_local.o/.a keep the
# Switch build's names for DOLPHIN_SWITCH_NVK_OBJECT.
"${AR}" rcs "${LIB}/libmesa_local.a" "${LOCAL}"
printf 'INPUT(-lmesa_local -lstdc++ -lm)\n' > "${LIB}/libEGL.a"
printf 'INPUT(-lmesa_local -lstdc++ -lm)\n' > "${LIB}/libvulkan.a"
cp "${LOCAL}" "${PREPARED}/libvulkan_local.o"
cp "${LIB}/libmesa_local.a" "${PREPARED}/libvulkan_local.a"
rm -f "${LOCAL}"

OLD="${OUT}.old.$$"
if [[ -e "${OUT}" ]]; then
  mv "${OUT}" "${OLD}"
fi
mv "${PREPARED}" "${OUT}"
if [[ -e "${OLD}" ]]; then
  rm -rf -- "${OLD}"
fi

echo "Prepared the Switch 2 Mesa SDK at ${OUT}"
echo "Merged Mesa object (EGL, GL, NVK; vk* localized): ${OUT}/libvulkan_local.o"
echo "uam2rt: ${OUT}/lib/cmake/uam2rt"
