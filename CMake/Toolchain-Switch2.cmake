# The Nintendo Switch 2 build of the Switch frontend (kaiHEN homebrew): devkitPro's devkitA64 with ouncebrew's
# libounce, drako3d and kaiHEN's JIT ($DEVKITPRO/libounce/cmake/Switch2.cmake, which sets NINTENDO_SWITCH2,
# __SWITCH2__ and the Cortex-A78C flags; https://github.com/ouncebrew/ounce-devkit). Tools/Switch/build-switch2.sh
# configures it. Everything that differs from the Switch build is behind DOLPHIN_SWITCH2 / __SWITCH2__.
if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO must point to the devkitPro installation")
endif()
if(NOT EXISTS "$ENV{DEVKITPRO}/libounce/cmake/Switch2.cmake")
  message(FATAL_ERROR "No Switch 2 devkit in $ENV{DEVKITPRO}: install https://github.com/ouncebrew/ounce-devkit")
endif()

set(DOLPHIN_SWITCH ON CACHE BOOL "Build the standalone Nintendo Switch frontend" FORCE)
set(DOLPHIN_SWITCH2 ON CACHE BOOL "Build the Switch frontend for the Switch 2 (libounce)" FORCE)
include("$ENV{DEVKITPRO}/libounce/cmake/Switch2.cmake")

add_compile_options(
  "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
  "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=."
  -fno-ident
)
