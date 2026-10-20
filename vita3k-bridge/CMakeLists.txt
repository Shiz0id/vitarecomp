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
