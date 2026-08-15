set(DOLPHIN_SWITCH_RELEASE_NAME "Dolphin")
set(DOLPHIN_SWITCH_RELEASE_AUTHOR "Dolphin Emulator Project")
set(DOLPHIN_SWITCH_RELEASE_VERSION "1.0.3" CACHE STRING
    "Semantic release version for the standalone Nintendo Switch frontend")

add_executable(dolphin-switch
  CoverDownload.cpp
  CoverDownload.h
  DolphinTools.cpp
  DolphinTools.h
  ExceptionStack.c
  Forwarder.cpp
  Forwarder.h
  ForwarderStructs.h
  GciSync.cpp
  GciSync.h
  Launcher.cpp
  Launcher.h
  Localization.cpp
  Localization.h
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
  Updater.cpp
  Updater.h
)

set_source_files_properties(ExceptionStack.c PROPERTIES COMPILE_OPTIONS "-fno-lto")

target_include_directories(dolphin-switch PRIVATE
  "${DEVKITPRO}/libnx/include"
  "${DEVKITPRO}/portlibs/switch/include"
)
target_compile_definitions(dolphin-switch PRIVATE
  DOLPHIN_SWITCH_RELEASE_AUTHOR="${DOLPHIN_SWITCH_RELEASE_AUTHOR}"
  DOLPHIN_SWITCH_RELEASE_VERSION="${DOLPHIN_SWITCH_RELEASE_VERSION}"
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
  Switch::ELF
  Switch::Expat
  OpenGL::EGL
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
          "-I${DEVKITPRO}/libnx/include" -D__SWITCH__
          "-DVERSION=\"${DOLPHIN_SWITCH_RELEASE_VERSION}\""
          "-L${DEVKITPRO}/libnx/lib" -lnx -Wl,--gc-sections -Wl,--build-id=none
          -Wl,--wrap,exit
  COMMAND "${NX_ELF2NSO_EXE}" "${_forwarder_elf}" "${_forwarder_output}/hbl.nso"
  COMMAND "${NX_NPDMTOOL_EXE}" "${_forwarder_source}/hbl.json"
          "${_forwarder_output}/hbl.npdm"
  COMMAND "${NX_NACPTOOL_EXE}" --create "${DOLPHIN_SWITCH_RELEASE_NAME}"
          "${DOLPHIN_SWITCH_RELEASE_AUTHOR}"
          "${DOLPHIN_SWITCH_RELEASE_VERSION}"
          "${_forwarder_output}/default.nacp"
  DEPENDS
    "${_forwarder_source}/main.c"
    "${_forwarder_source}/trampoline.s"
    "${_forwarder_source}/hbl.json"
  COMMENT "Building Dolphin HOME Menu forwarder payload"
  VERBATIM
)
add_custom_target(dolphin_forwarder_payload DEPENDS ${_forwarder_assets})

find_program(DOLPHIN_SWITCH_MSGFMT_EXECUTABLE NAMES msgfmt REQUIRED)
set(_switch_translation_output "${CMAKE_SOURCE_DIR}/Data/Sys/i18n")
set(_switch_translation_languages fr de es it pt)
set(_switch_translation_assets)
set(_switch_launcher_translation_assets)
foreach(_language IN LISTS _switch_translation_languages)
  set(_translation_source "${CMAKE_SOURCE_DIR}/Languages/po/${_language}.po")
  set(_translation_output "${_switch_translation_output}/${_language}.mo")
  set(_launcher_translation
      "${_switch_translation_output}/launcher/${_language}.json")
  add_custom_command(
    OUTPUT "${_translation_output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_switch_translation_output}"
    COMMAND "${DOLPHIN_SWITCH_MSGFMT_EXECUTABLE}" -o "${_translation_output}"
            "${_translation_source}"
    DEPENDS "${_translation_source}"
    COMMENT "Compiling ${_language} launcher translations"
    VERBATIM
  )
  list(APPEND _switch_translation_assets "${_translation_output}")
  list(APPEND _switch_launcher_translation_assets "${_launcher_translation}")
endforeach()
add_custom_target(dolphin_switch_translations DEPENDS ${_switch_translation_assets})

dkp_add_asset_target(dolphin_romfs "${CMAKE_SOURCE_DIR}/Data/Sys")
add_dependencies(dolphin_romfs dolphin_forwarder_payload dolphin_switch_translations)
set_property(TARGET dolphin_romfs APPEND PROPERTY DKP_ASSET_FILES
  ${_forwarder_assets}
  ${_switch_translation_assets}
  ${_switch_launcher_translation_assets}
)

nx_generate_nacp(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/dolphin.nacp"
  NAME "${DOLPHIN_SWITCH_RELEASE_NAME}"
  AUTHOR "${DOLPHIN_SWITCH_RELEASE_AUTHOR}"
  VERSION "${DOLPHIN_SWITCH_RELEASE_VERSION}"
)

nx_create_nro(dolphin_nro
  TARGET dolphin-switch
  OUTPUT "${CMAKE_BINARY_DIR}/Binaries/dolphin.nro"
  ICON "${CMAKE_SOURCE_DIR}/Data/Switch/dolphin_icon.jpg"
  NACP "${CMAKE_CURRENT_BINARY_DIR}/dolphin.nacp"
  ROMFS dolphin_romfs
)
