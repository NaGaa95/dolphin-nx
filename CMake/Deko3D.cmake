# Copyright 2026 Dolphin Emulator Project
# Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later

if(DOLPHIN_SWITCH2)
  # Switch 2: drako3d (ouncebrew), deko3d's API on the Ampere GPU, from libounce. Source/Core/VideoBackends/Deko3D/
  # Switch2 maps the backend's <deko3d.hpp> to <drako3d.hpp>.
  set(DEKO3D_INCLUDE_DIR "${DEVKITPRO}/libounce/include")
  set(DEKO3D_LIB_DIR "${DEVKITPRO}/libounce/lib")
  set(DEKO3D_DEBUG_LIBRARY "${DEKO3D_LIB_DIR}/libdrako3dd.a")
  set(DEKO3D_RELEASE_LIBRARY "${DEKO3D_LIB_DIR}/libdrako3d.a")
  set(_deko3d_headers "${DEKO3D_INCLUDE_DIR}/drako3d.h" "${DEKO3D_INCLUDE_DIR}/drako3d.hpp")
else()
  set(DEKO3D_INCLUDE_DIR "${DEVKITPRO}/libnx/include")
  set(DEKO3D_LIB_DIR "${DEVKITPRO}/libnx/lib")
  set(DEKO3D_DEBUG_LIBRARY "${DEKO3D_LIB_DIR}/libdeko3dd.a")
  set(DEKO3D_RELEASE_LIBRARY "${DEKO3D_LIB_DIR}/libdeko3d.a")
  set(_deko3d_headers "${DEKO3D_INCLUDE_DIR}/deko3d.h" "${DEKO3D_INCLUDE_DIR}/deko3d.hpp")
endif()

set(_deko3d_missing "")
foreach(_f
        ${_deko3d_headers}
        "${DEKO3D_DEBUG_LIBRARY}"
        "${DEKO3D_RELEASE_LIBRARY}")
  if(NOT EXISTS "${_f}")
    list(APPEND _deko3d_missing "${_f}")
  endif()
endforeach()

if(_deko3d_missing)
  string(REPLACE ";" "\n  " _deko3d_missing_pretty "${_deko3d_missing}")
  message(FATAL_ERROR
    "deko3d not found:\n  ${_deko3d_missing_pretty}\n"
    "Install it with dkp-pacman -S switch-tools deko3d.")
endif()

add_library(deko3d INTERFACE)
if(DOLPHIN_SWITCH2)
  target_include_directories(deko3d BEFORE INTERFACE "${CMAKE_SOURCE_DIR}/Source/Core/VideoBackends/Deko3D/Switch2")
endif()
target_include_directories(deko3d INTERFACE "${DEKO3D_INCLUDE_DIR}")
target_link_libraries(deko3d INTERFACE
  "$<$<CONFIG:Debug>:${DEKO3D_DEBUG_LIBRARY}>"
  "$<$<NOT:$<CONFIG:Debug>>:${DEKO3D_RELEASE_LIBRARY}>"
)
target_compile_definitions(deko3d INTERFACE DK_HPP_SUPPORT_VECTOR)
