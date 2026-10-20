# vita3krecomp bridge — GPLv2, like the Vita3K code it links.
#
# Not built by the toolkit. Vita3K's own build adds this directory when
# configured with -DVITA3K_RECOMP=ON (see third_party/Vita3K/vita3k/CMakeLists.txt),
# because it needs Vita3K's targets and they only exist inside that build.

add_library(vita3krecomp_bridge STATIC
    bridge.cpp
    bridge.h
    recomp_cpu.cpp
    recomp_cpu.h
)

target_include_directories(vita3krecomp_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vita3krecomp_bridge PUBLIC vitarecomp PRIVATE cpu emuenv modules util)

# Vita3K runs each guest thread on its own host thread, so the runtime's
# register file must be per thread. recomp_cpu.cpp #errors without it.
if(NOT VITARECOMP_THREADED)
    message(FATAL_ERROR "vita3krecomp_bridge needs VITARECOMP_THREADED=ON")
endif()

# --- the recompiled module --------------------------------------------------
#
# Two libraries, and their order is the point. The generated functions call
# `vita_hle_<name>` for every import. A hand-written body in the runtime
# (src/hle_libc.c) must win over the generated trapping default, so the
# runtime is searched before the defaults: a linker pulls an archive member
# only for a symbol still undefined, and by the time it reaches the defaults
# the implemented ones are already resolved. Whatever is left falls to a
# default, which offers the call to Vita3K before trapping.

if(NOT VITA3K_RECOMP_GENERATED)
    message(FATAL_ERROR "Set VITA3K_RECOMP_GENERATED to the .c file `armrecomp emit` wrote")
endif()
set(_imports_dir "${VITA3K_RECOMP_GENERATED}")
string(REGEX REPLACE "\\.c$" "_imports" _imports_dir "${_imports_dir}")
file(GLOB _import_sources CONFIGURE_DEPENDS "${_imports_dir}/*.c")
if(NOT _import_sources)
    message(FATAL_ERROR "No import defaults in ${_imports_dir}")
endif()

add_library(vita3krecomp_imports STATIC ${_import_sources})
target_link_libraries(vita3krecomp_imports PRIVATE vitarecomp)

add_library(vita3krecomp_game STATIC "${VITA3K_RECOMP_GENERATED}")
# Generated C is long and mechanical; it is checked by being run, not linted.
target_compile_options(vita3krecomp_game PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
target_link_libraries(vita3krecomp_game PUBLIC vitarecomp vita3krecomp_imports vitarecomp)
