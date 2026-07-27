if(NOT DOLPHIN_SWITCH OR NOT NINTENDO_SWITCH)
  message(FATAL_ERROR "SwitchDependencies.cmake is only valid with the libnx Switch toolchain")
endif()

set(DOLPHIN_SWITCH_NVK_OBJECT
    "${CMAKE_BINARY_DIR}/nvk/libnvk_local.o"
    CACHE FILEPATH "Localized NVK driver object produced by Tools/Switch/prepare_nvk.sh")

if(NOT EXISTS "${DOLPHIN_SWITCH_NVK_OBJECT}")
  message(FATAL_ERROR
    "The localized NVK driver was not found at ${DOLPHIN_SWITCH_NVK_OBJECT}. "
    "Run Tools/Switch/prepare_nvk.sh before configuring Dolphin.")
endif()

set(_switch_portlibs "${DEVKITPRO}/portlibs/switch")

include(FetchContent)

pkg_check_modules(SwitchSDL2Pkg REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SwitchSDL2TTFPkg REQUIRED IMPORTED_TARGET SDL2_ttf)
pkg_check_modules(SwitchSDL2ImagePkg REQUIRED IMPORTED_TARGET SDL2_image)
pkg_check_modules(SwitchCurlPkg REQUIRED IMPORTED_TARGET libcurl)

add_library(SwitchSDL2 INTERFACE)
target_link_libraries(SwitchSDL2 INTERFACE PkgConfig::SwitchSDL2Pkg)
add_library(Switch::SDL2 ALIAS SwitchSDL2)

add_library(SwitchSDL2TTF INTERFACE)
target_link_libraries(SwitchSDL2TTF INTERFACE PkgConfig::SwitchSDL2TTFPkg)
add_library(Switch::SDL2TTF ALIAS SwitchSDL2TTF)

add_library(SwitchSDL2Image INTERFACE)
target_link_libraries(SwitchSDL2Image INTERFACE PkgConfig::SwitchSDL2ImagePkg)
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
FetchContent_Declare(switch_libusbhsfs
  GIT_REPOSITORY https://github.com/ITotalJustice/libusbhsfs.git
  GIT_TAG 625269b7725a6e2a3f2724e8d45b602c1b20ead5
)
FetchContent_MakeAvailable(switch_libusbhsfs)
# Avoid libusbhsfs' get_fattime symbol colliding with Dolphin's FatFs copy.
target_compile_definitions(libusbhsfs PRIVATE get_fattime=usbhsfs_get_fattime)

add_library(SwitchNVK UNKNOWN IMPORTED GLOBAL)
set_target_properties(SwitchNVK PROPERTIES
  IMPORTED_LOCATION "${DOLPHIN_SWITCH_NVK_OBJECT}"
)
add_library(Switch::NVK ALIAS SwitchNVK)

add_library(SwitchDRMNouveau STATIC IMPORTED GLOBAL)
set_target_properties(SwitchDRMNouveau PROPERTIES
  IMPORTED_LOCATION "${_switch_portlibs}/lib/libdrm_nouveau.a"
  INTERFACE_INCLUDE_DIRECTORIES "${_switch_portlibs}/include"
)
add_library(Switch::DRMNouveau ALIAS SwitchDRMNouveau)
