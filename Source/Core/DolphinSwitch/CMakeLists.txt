set(DOLPHIN_SWITCH_RELEASE_NAME "Dolphin")
set(DOLPHIN_SWITCH_RELEASE_AUTHOR "naga")
set(DOLPHIN_SWITCH_RELEASE_VERSION "1.2.2" CACHE STRING
    "Semantic release version for the standalone Nintendo Switch frontend")

add_executable(dolphin-switch
  CoverDownload.cpp
  CoverDownload.h
  DolphinTools.cpp
  DolphinTools.h
  EmbeddedRuntime.cpp
  EmbeddedRuntime.h
  ExceptionStack.c
  Forwarder.cpp
  Forwarder.h
  ForwarderStructs.h
  FrameGeneration.cpp
  FrameGeneration.h
  GBAOverlay.cpp
  GBAOverlay.h
  GciSync.cpp
  GciSync.h
  Launcher.cpp
  Launcher.h
  Localization.cpp
  Localization.h
  Main.cpp
  RuntimeOverlay.cpp
  RuntimeOverlay.h
  ScreenLayout.cpp
  ScreenLayout.h
  Skylanders.cpp
  Skylanders.h
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
  "${CMAKE_SOURCE_DIR}/Externals/lsfg/include"
)

if(NOT DOLPHIN_SWITCH_DEKO3D_RUNTIME)
  target_sources(dolphin-switch PRIVATE
    "${CMAKE_SOURCE_DIR}/Externals/lsfg/src/cache_format.cpp"
    "${CMAKE_SOURCE_DIR}/Externals/lsfg/src/sha256.cpp"
  )
endif()
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

if(DOLPHIN_SWITCH_DEKO3D_RUNTIME)
  set(_switch_nro_name dolphin-deko3d)
else()
  set(_switch_nro_name dolphin)
endif()
set_target_properties(dolphin-switch PROPERTIES OUTPUT_NAME "${_switch_nro_name}")

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
set(_switch_translation_languages fr de es it pt zh-CN zh-TW)
set(_switch_translation_assets)
set(_switch_launcher_translation_assets)
foreach(_language IN LISTS _switch_translation_languages)
  set(_translation_source_language "${_language}")
  if(_language STREQUAL "zh-CN")
    set(_translation_source_language zh_CN)
  elseif(_language STREQUAL "zh-TW")
    set(_translation_source_language zh_TW)
  endif()
  set(_translation_source
      "${CMAKE_SOURCE_DIR}/Languages/po/${_translation_source_language}.po")
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

if(NOT DOLPHIN_SWITCH_DEKO3D_RUNTIME AND DOLPHIN_SWITCH_BUNDLE_DEKO3D)
  include(ExternalProject)
  set(DOLPHIN_SWITCH_RUNTIME_JOBS 4 CACHE STRING "Parallel jobs for the embedded runtime")
  set(_runtime_build "${CMAKE_BINARY_DIR}/deko3d-runtime")
  set(_runtime_nro "${_runtime_build}/Binaries/dolphin-deko3d.nro")
  ExternalProject_Add(dolphin_deko3d_runtime
    SOURCE_DIR "${CMAKE_SOURCE_DIR}"
    BINARY_DIR "${_runtime_build}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS
      "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_SOURCE_DIR}/CMake/Toolchain-Switch.cmake"
      "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
      "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
      "-DDOLPHIN_SWITCH_DEKO3D_RUNTIME=ON"
      "-DDOLPHIN_SWITCH_BUNDLE_DEKO3D=OFF"
      "-DDOLPHIN_SWITCH_MESA_SDK_ROOT=${DOLPHIN_SWITCH_MESA_SDK_ROOT}"
      "-DDOLPHIN_SWITCH_NVK_OBJECT=${DOLPHIN_SWITCH_NVK_OBJECT}"
      "-DDOLPHIN_SWITCH_PGO=${DOLPHIN_SWITCH_PGO}"
      "-DDOLPHIN_SWITCH_RELEASE_VERSION=${DOLPHIN_SWITCH_RELEASE_VERSION}"
      "-DENABLE_LTO=${ENABLE_LTO}"
      "-DFETCHCONTENT_SOURCE_DIR_SWITCH_LIBSMB2=${switch_libsmb2_SOURCE_DIR}"
      "-DFETCHCONTENT_SOURCE_DIR_SWITCH_LIBUSBHSFS=${switch_libusbhsfs_SOURCE_DIR}"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build "${_runtime_build}"
                  --target dolphin_nro --parallel "${DOLPHIN_SWITCH_RUNTIME_JOBS}"
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS "${_runtime_nro}"
    INSTALL_COMMAND ""
    DEPENDS dolphin_forwarder_payload dolphin_switch_translations
  )

  set(_switch_romfs "${CMAKE_CURRENT_BINARY_DIR}/romfs")
  set(_runtime_id_script "${CMAKE_CURRENT_BINARY_DIR}/write-runtime-id.cmake")
  file(GENERATE OUTPUT "${_runtime_id_script}" CONTENT [=[
file(SHA256 "${RUNTIME_NRO}" runtime_digest)
file(WRITE "${RUNTIME_ID}" "${runtime_digest}\n")
]=])
  add_custom_target(dolphin_romfs_stage
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/Data/Sys" "${_switch_romfs}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_switch_romfs}/runtime"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_runtime_nro}"
            "${_switch_romfs}/runtime/dolphin-deko3d.nro"
    COMMAND "${CMAKE_COMMAND}" "-DRUNTIME_NRO=${_runtime_nro}"
            "-DRUNTIME_ID=${_switch_romfs}/runtime/dolphin-deko3d.sha256"
            -P "${_runtime_id_script}"
    DEPENDS dolphin_deko3d_runtime dolphin_forwarder_payload dolphin_switch_translations
    VERBATIM
  )
  dkp_add_asset_target(dolphin_romfs "${_switch_romfs}")
  add_dependencies(dolphin_romfs dolphin_romfs_stage)
  set_property(TARGET dolphin_romfs APPEND PROPERTY DKP_ASSET_FILES "${_runtime_nro}")
else()
  dkp_add_asset_target(dolphin_romfs "${CMAKE_SOURCE_DIR}/Data/Sys")
endif()
add_dependencies(dolphin_romfs dolphin_forwarder_payload dolphin_switch_translations)
set_property(TARGET dolphin_romfs APPEND PROPERTY DKP_ASSET_FILES
  "${CMAKE_SOURCE_DIR}/Data/Sys/icons/settings.svg"
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
  OUTPUT "${CMAKE_BINARY_DIR}/Binaries/${_switch_nro_name}.nro"
  ICON "${CMAKE_SOURCE_DIR}/Data/Switch/dolphin_icon.jpg"
  NACP "${CMAKE_CURRENT_BINARY_DIR}/dolphin.nacp"
  ROMFS dolphin_romfs
)
