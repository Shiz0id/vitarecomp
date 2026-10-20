# neontest — built with VitaSDK; see main.c and tests/homebrew/run.sh.
# -fno-tree-vectorize keeps GCC's own NEON out of the C around the asm, so
# what is compared is the instructions written here.
cmake_minimum_required(VERSION 3.16)
project(neontest C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_APP_NAME "neontest")
set(VITA_TITLEID  "VRNT00001")
set(VITA_VERSION  "01.00")
add_executable(neontest main.c)
target_compile_options(neontest PRIVATE -O1 -mfpu=neon -fno-tree-vectorize)
target_link_libraries(neontest SceLibKernel_stub)
vita_create_self(eboot.bin neontest)
vita_create_vpk(neontest.vpk ${VITA_TITLEID} eboot.bin VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME})
