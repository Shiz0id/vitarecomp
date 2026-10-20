# bridgetest — built with VitaSDK, not with the host toolchain.
#
#   cmake -S tests/vita/bridgetest -B build-bt -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#   cmake --build build-bt
#
# Produces bridgetest.self.out (the ELF armrecomp reads, via the .velf) and
# bridgetest.vpk (what Vita3K installs). See docs/VITA3K_BRIDGE.md.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Set VITASDK or pass -DCMAKE_TOOLCHAIN_FILE")
    endif()
endif()

project(bridgetest C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "bridgetest")
set(VITA_TITLEID  "VRBT00001")
set(VITA_VERSION  "01.00")

add_executable(bridgetest main.c)
# -fno-tree-vectorize keeps GCC from turning main's framebuffer loop into
# wide NEON (vmul/vshr/widening forms) the emitter does not translate yet. The
# point of this module is to exercise the bridge end to end; NEON coverage is
# measured on its own. newlib still brings its own NEON (vmov.i32, vadd.i32,
# vorr in the allocator), which IS translated and so is still exercised.
target_compile_options(bridgetest PRIVATE -O2 -Wall -fno-tree-vectorize)
target_link_libraries(bridgetest
    SceDisplay_stub
    SceLibKernel_stub
    SceKernelThreadMgr_stub
    SceIofilemgr_stub
    SceSysmem_stub
)

vita_create_self(bridgetest.self bridgetest)
vita_create_vpk(bridgetest.vpk ${VITA_TITLEID} bridgetest.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
