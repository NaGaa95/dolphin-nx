add_executable(dolphin-switch
  CoverDownload.cpp
  CoverDownload.h
  DolphinTools.cpp
  DolphinTools.h
  ExceptionStack.c
  Forwarder.cpp
  Forwarder.h
  ForwarderStructs.h
  Launcher.cpp
  Launcher.h
  Main.cpp
  RuntimeOverlay.cpp
  RuntimeOverlay.h
  SwitchPosixCompat.cpp
  Storage.cpp
  Storage.h
  SystemLanguage.cpp
  SystemLanguage.h
  UiAudio.cpp
  UiAudio.h
)

set_source_files_properties(ExceptionStack.c PROPERTIES COMPILE_OPTIONS "-fno-lto")

target_include_directories(dolphin-switch PRIVATE
  "${DEVKITPRO}/libnx/include"
  "${DEVKITPRO}/portlibs/switch/include"
)

target_link_libraries(dolphin-switch PRIVATE
  core
  uicommon
  imgui
  Switch::SDL2
  Switch::SDL2TTF
  Switch::SDL2Image
  Switch::Curl
  Switch::NVK
  Switch::DRMNouveau
  smb2
  libusbhsfs
  turbojpeg
  zstd::zstd
  ZLIB::ZLIB
)

target_link_options(dolphin-switch PRIVATE
  -Wl,--gc-sections
  -Wl,--build-id=none
)

set_target_properties(dolphin-switch PROPERTIES OUTPUT_NAME dolphin)

set(_forwarder_source "${CMAKE_SOURCE_DIR}/Tools/Switch/ForwarderPayload")
set(_forwarder_output "${CMAKE_SOURCE_DIR}/Data/Sys/fwd")
set(_forwarder_elf "${CMAKE_CURRENT_BINARY_DIR}/forwarder_hbl.elf")
set(_forwarder_assets
  "${_forwarder_output}/hbl.nso"
  "${_forwarder_output}/hbl.npdm"
  "${_forwarder_output}/default.nacp"
)
add_custom_command(
  OUTPUT ${_forwarder_assets}
  BYPRODUCTS "${_forwarder_elf}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_forwarder_output}"
  COMMAND "${CMAKE_C_COMPILER}"
          "${_forwarder_source}/main.c" "${_forwarder_source}/trampoline.s"
          -o "${_forwarder_elf}"
          "-specs=${DEVKITPRO}/libnx/switch.specs"
          -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft
          -fPIE -O2 -DNDEBUG -ffunction-sections -fdata-sections -fno-ident
          "-I${DEVKITPRO}/libnx/include" -D__SWITCH__ "-DVERSION=\"0.3.21\""
          "-L${DEVKITPRO}/libnx/lib" -lnx -Wl,--gc-sections -Wl,--build-id=none
          -Wl,--wrap,exit
  COMMAND "${NX_ELF2NSO_EXE}" "${_forwarder_elf}" "${_forwarder_output}/hbl.nso"
  COMMAND "${NX_NPDMTOOL_EXE}" "${_forwarder_source}/hbl.json"
          "${_forwarder_output}/hbl.npdm"
  COMMAND "${NX_NACPTOOL_EXE}" --create Dolphin "Dolphin Emulator Project" 0.3.21
          "${_forwarder_output}/default.nacp"
  DEPENDS
    "${_forwarder_source}/main.c"
    "${_forwarder_source}/trampoline.s"
    "${_forwarder_source}/hbl.json"
  COMMENT "Building Dolphin HOME Menu forwarder payload"
  VERBATIM
)
add_custom_target(dolphin_forwarder_payload DEPENDS ${_forwarder_assets})
dkp_add_asset_target(dolphin_romfs "${CMAKE_SOURCE_DIR}/Data/Sys")
add_dependencies(dolphin_romfs dolphin_forwarder_payload)
set_property(TARGET dolphin_romfs APPEND PROPERTY DKP_ASSET_FILES ${_forwarder_assets})

nx_generate_nacp(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/dolphin.nacp"
  NAME "Dolphin"
  AUTHOR "Dolphin Emulator Project"
  VERSION "0.3.21"
)

nx_create_nro(dolphin_nro
  TARGET dolphin-switch
  OUTPUT "${CMAKE_BINARY_DIR}/Binaries/dolphin.nro"
  ICON "${CMAKE_SOURCE_DIR}/Data/Switch/dolphin_icon.jpg"
  NACP "${CMAKE_CURRENT_BINARY_DIR}/dolphin.nacp"
  ROMFS dolphin_romfs
)
