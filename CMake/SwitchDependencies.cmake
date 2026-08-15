if(NOT DOLPHIN_SWITCH OR NOT NINTENDO_SWITCH)
  message(FATAL_ERROR "SwitchDependencies.cmake is only valid with the libnx Switch toolchain")
endif()

set(DOLPHIN_SWITCH_NVK_OBJECT
    "${DOLPHIN_SWITCH_MESA_SDK_ROOT}/libvulkan_local.o"
    CACHE FILEPATH
    "Localized NVK driver object produced by Tools/Switch/prepare_mesa_sdk.sh")

if(NOT EXISTS "${DOLPHIN_SWITCH_NVK_OBJECT}")
  message(FATAL_ERROR
    "The localized NVK driver was not found at ${DOLPHIN_SWITCH_NVK_OBJECT}. "
    "Run Tools/Switch/prepare_mesa_sdk.sh before configuring Dolphin.")
endif()
foreach(_switch_mesa_required_file IN ITEMS
    "${DOLPHIN_SWITCH_MESA_SDK_ROOT}/lib/libEGL.a"
    "${DOLPHIN_SWITCH_MESA_SDK_ROOT}/lib/libvulkan.a"
    "${DOLPHIN_SWITCH_MESA_SDK_ROOT}/include/EGL/egl.h"
    "${DOLPHIN_SWITCH_MESA_SDK_ROOT}/include/vulkan/vulkan.h")
  if(NOT EXISTS "${_switch_mesa_required_file}")
    message(FATAL_ERROR "Unified Mesa SDK file is missing: ${_switch_mesa_required_file}")
  endif()
endforeach()

set(_switch_portlibs "${DEVKITPRO}/portlibs/switch")

include(FetchContent)

pkg_check_modules(SwitchSDL2Pkg REQUIRED sdl2)
pkg_check_modules(SwitchSDL2TTFPkg REQUIRED SDL2_ttf)
pkg_check_modules(SwitchSDL2ImagePkg REQUIRED SDL2_image)
pkg_check_modules(SwitchCurlPkg REQUIRED IMPORTED_TARGET libcurl)

# devkitPro's SDL2 pkg-config file is tied to its old Mesa 20 EGL/glapi and
# drm_nouveau archives. Link SDL itself and its non-Mesa dependencies
# explicitly, then satisfy its EGL calls from the staged unified SDK.
add_library(SwitchSDL2 STATIC IMPORTED GLOBAL)
set_target_properties(SwitchSDL2 PROPERTIES
  IMPORTED_LOCATION "${_switch_portlibs}/lib/libSDL2.a"
  INTERFACE_INCLUDE_DIRECTORIES
    "${_switch_portlibs}/include;${_switch_portlibs}/include/SDL2"
  INTERFACE_COMPILE_DEFINITIONS _REENTRANT
  INTERFACE_LINK_LIBRARIES "OpenGL::EGL;stdc++;nx;pthread"
)
add_library(Switch::SDL2 ALIAS SwitchSDL2)

add_library(SwitchSDL2TTF STATIC IMPORTED GLOBAL)
set_target_properties(SwitchSDL2TTF PROPERTIES
  IMPORTED_LOCATION "${_switch_portlibs}/lib/libSDL2_ttf.a"
  INTERFACE_INCLUDE_DIRECTORIES
    "${_switch_portlibs}/include;${_switch_portlibs}/include/SDL2"
  INTERFACE_LINK_LIBRARIES
    "Switch::SDL2;harfbuzz;pthread;m;freetype;bz2;png16;nx;z"
)
add_library(Switch::SDL2TTF ALIAS SwitchSDL2TTF)

add_library(SwitchSDL2Image STATIC IMPORTED GLOBAL)
set_target_properties(SwitchSDL2Image PROPERTIES
  IMPORTED_LOCATION "${_switch_portlibs}/lib/libSDL2_image.a"
  INTERFACE_INCLUDE_DIRECTORIES
    "${_switch_portlibs}/include;${_switch_portlibs}/include/SDL2"
  INTERFACE_LINK_LIBRARIES "Switch::SDL2;png16;nx;z;jpeg;webp;m"
)
add_library(Switch::SDL2Image ALIAS SwitchSDL2Image)

add_library(SwitchCurl INTERFACE)
target_link_libraries(SwitchCurl INTERFACE PkgConfig::SwitchCurlPkg)
add_library(Switch::Curl ALIAS SwitchCurl)

# SDL launcher USB and SMB backends.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBKRB5 OFF CACHE BOOL "" FORCE)
set(ENABLE_GSSAPI OFF CACHE BOOL "" FORCE)
FetchContent_Declare(switch_libsmb2
  GIT_REPOSITORY https://github.com/ITotalJustice/libsmb2.git
  GIT_TAG 867beea093f2863dfddea01945204f724afd6c45
)
FetchContent_MakeAvailable(switch_libsmb2)
target_compile_options(smb2 PRIVATE -UNEED_READV -UNEED_WRITEV)
target_compile_definitions(smb2 PRIVATE AES128_ECB_encrypt=smb2_AES128_ECB_encrypt)

set(USBHSFS_GPL OFF CACHE BOOL "" FORCE)
set(USBHSFS_NTFS OFF CACHE BOOL "" FORCE)
set(USBHSFS_EXT4 OFF CACHE BOOL "" FORCE)
set(USBHSFS_SXOS_DISABLE ON CACHE BOOL "" FORCE)
set(USBHSFS_EXAMPLES OFF CACHE BOOL "" FORCE)
set(_switch_libusbhsfs_pinned_revision 625269b7725a6e2a3f2724e8d45b602c1b20ead5)
FetchContent_Declare(switch_libusbhsfs
  GIT_REPOSITORY https://github.com/ITotalJustice/libusbhsfs.git
  GIT_TAG ${_switch_libusbhsfs_pinned_revision}
)
FetchContent_MakeAvailable(switch_libusbhsfs)

# Keep the UASP work reproducible while libusbhsfs remains a pinned FetchContent
# dependency. These small patches connect transport hooks, preserve BOT fallback
# and keep local mounts clean after a failed transport reset; the implementation
# stays tracked here under libusbhsfs' ISC license.
find_package(Git REQUIRED)
execute_process(
  COMMAND "${GIT_EXECUTABLE}"
          -c "safe.directory=${switch_libusbhsfs_SOURCE_DIR}"
          rev-parse HEAD
  WORKING_DIRECTORY "${switch_libusbhsfs_SOURCE_DIR}"
  RESULT_VARIABLE _switch_libusbhsfs_revision_result
  OUTPUT_VARIABLE _switch_libusbhsfs_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT _switch_libusbhsfs_revision_result EQUAL 0 OR
   NOT "${_switch_libusbhsfs_revision}" STREQUAL "${_switch_libusbhsfs_pinned_revision}")
  message(FATAL_ERROR
    "libusbhsfs source is not the pinned ${_switch_libusbhsfs_pinned_revision} revision")
endif()

set(_switch_libusbhsfs_patches
  "${CMAKE_SOURCE_DIR}/Tools/Switch/libusbhsfs/0001-add-uasp-transport-hooks.patch"
  "${CMAKE_SOURCE_DIR}/Tools/Switch/libusbhsfs/0002-fallback-to-bot-interface.patch"
  "${CMAKE_SOURCE_DIR}/Tools/Switch/libusbhsfs/0003-cleanup-after-uasp-reset.patch"
)
foreach(_switch_libusbhsfs_patch IN LISTS _switch_libusbhsfs_patches)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}"
            -c "safe.directory=${switch_libusbhsfs_SOURCE_DIR}"
            apply --ignore-space-change --check "${_switch_libusbhsfs_patch}"
    WORKING_DIRECTORY "${switch_libusbhsfs_SOURCE_DIR}"
    RESULT_VARIABLE _switch_libusbhsfs_patch_check
    OUTPUT_QUIET ERROR_QUIET
  )
  if(_switch_libusbhsfs_patch_check EQUAL 0)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}"
              -c "safe.directory=${switch_libusbhsfs_SOURCE_DIR}"
              apply --ignore-space-change "${_switch_libusbhsfs_patch}"
      WORKING_DIRECTORY "${switch_libusbhsfs_SOURCE_DIR}"
      RESULT_VARIABLE _switch_libusbhsfs_patch_result
    )
    if(NOT _switch_libusbhsfs_patch_result EQUAL 0)
      message(FATAL_ERROR "Failed to apply ${_switch_libusbhsfs_patch}")
    endif()
  else()
    execute_process(
      COMMAND "${GIT_EXECUTABLE}"
              -c "safe.directory=${switch_libusbhsfs_SOURCE_DIR}"
              apply --ignore-space-change --reverse --check "${_switch_libusbhsfs_patch}"
      WORKING_DIRECTORY "${switch_libusbhsfs_SOURCE_DIR}"
      RESULT_VARIABLE _switch_libusbhsfs_patch_reverse_check
      OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT _switch_libusbhsfs_patch_reverse_check EQUAL 0)
      message(FATAL_ERROR
        "The pinned libusbhsfs source does not match ${_switch_libusbhsfs_patch}")
    endif()
  endif()
endforeach()

target_sources(libusbhsfs PRIVATE
  "${CMAKE_SOURCE_DIR}/Tools/Switch/libusbhsfs/usbhsfs_uasp.c"
  "${CMAKE_SOURCE_DIR}/Tools/Switch/libusbhsfs/usbhsfs_uasp.h"
)
target_include_directories(libusbhsfs PRIVATE
  "${CMAKE_SOURCE_DIR}/Tools/Switch/libusbhsfs"
  "${switch_libusbhsfs_SOURCE_DIR}/source"
)
# Avoid libusbhsfs' get_fattime symbol colliding with Dolphin's FatFs copy.
target_compile_definitions(libusbhsfs PRIVATE get_fattime=usbhsfs_get_fattime)

add_library(SwitchNVK UNKNOWN IMPORTED GLOBAL)
set_target_properties(SwitchNVK PROPERTIES
  IMPORTED_LOCATION "${DOLPHIN_SWITCH_NVK_OBJECT}"
)
add_library(Switch::NVK ALIAS SwitchNVK)

add_library(SwitchELF STATIC IMPORTED GLOBAL)
set_target_properties(SwitchELF PROPERTIES
  IMPORTED_LOCATION "${_switch_portlibs}/lib/libelf.a"
  INTERFACE_INCLUDE_DIRECTORIES "${_switch_portlibs}/include"
)
add_library(Switch::ELF ALIAS SwitchELF)

add_library(SwitchExpat STATIC IMPORTED GLOBAL)
set_target_properties(SwitchExpat PROPERTIES
  IMPORTED_LOCATION "${_switch_portlibs}/lib/libexpat.a"
  INTERFACE_INCLUDE_DIRECTORIES "${_switch_portlibs}/include"
)
add_library(Switch::Expat ALIAS SwitchExpat)
