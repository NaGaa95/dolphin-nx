# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The Switch 2 build's run-time shader compiler, in place of Uam.cmake's uam: uam2rt, drako3d's UAM2 as a library
# (Mesa's GLSL front end and NAK, linked as one object whose only global symbols are uam2rt's). It comes with the
# Switch 2 Mesa SDK prefix (Tools/Switch/build-switch2.sh). The uam library below implements UamBridge.h
# on it (VideoBackends/Deko3D/UamBridgeSwitch2.cpp), so the backend and LSFG compile shaders as on the Switch.

find_package(uam2rt CONFIG REQUIRED
  PATHS "${DOLPHIN_SWITCH_MESA_SDK_ROOT}/lib/cmake/uam2rt"
  NO_DEFAULT_PATH)
message(STATUS "Switch 2 shader compiler: uam2rt ${uam2rt_VERSION}")

# uam: UamBridge.h's UamCompileGlsl on uam2rt, for the deko3d backend and Externals/lsfg alike.
add_library(uam STATIC "${CMAKE_SOURCE_DIR}/Source/Core/VideoBackends/Deko3D/UamBridgeSwitch2.cpp")
target_include_directories(uam PRIVATE "${CMAKE_SOURCE_DIR}/Source/Core")
target_link_libraries(uam PUBLIC uam2rt::uam2rt)

# Externals/lsfg versions its shader cache with this (the Switch's is uam's commit).
set(SWITCH_UAM2RT_REVISION "uam2rt-${uam2rt_VERSION}")
