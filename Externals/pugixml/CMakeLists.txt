set(PUGIXML_NO_EXCEPTIONS ON)
set(PUGIXML_STATIC_CRT OFF)
# Avoid propagating -DOFF to dependents.
set(PUGIXML_BUILD_DEFINES "")
set(PUGIXML_INSTALL OFF)

add_subdirectory(pugixml)
dolphin_disable_warnings(pugixml-static)
