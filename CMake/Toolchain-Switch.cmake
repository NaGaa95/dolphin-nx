if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO must point to the devkitPro installation")
endif()

set(DOLPHIN_SWITCH ON CACHE BOOL "Build the standalone Nintendo Switch frontend" FORCE)
include("$ENV{DEVKITPRO}/cmake/Switch.cmake")

add_compile_options(
  "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
  "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=."
  -fno-ident
)
