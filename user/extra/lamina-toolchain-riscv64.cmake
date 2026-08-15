# lamina-toolchain-riscv64.cmake — CMake toolchain for cross-compiling the
# Lamina1 language toolchain (lamina CLI) to riscv64 for the A20OS extra disk.
#
# Lamina1 is a C++23/CMake project.  There is no musl C++ standard library in
# the build environment, so we use the host Debian riscv64 glibc cross
# toolchain and ship the executable together with its shared libraries on the
# extra disk (the glibc runtime itself is already staged for the rust extra
# package, see tools/targets-extra.mk).
#
# NOTE on rpath: lamina's runtime loads its stdlib module "laminaCore" at
# runtime via dlopen().  glibc's dlopen() does NOT honor DT_RUNPATH (the
# default emitted by modern linkers), only DT_RPATH — so every executable and
# shared library here is linked with -Wl,--disable-new-dtags plus an $ORIGIN
# rpath.  That way dlopen("liblaminaCore.so") resolves the module sitting in
# the same directory as the lamina binary.
#
# Referenced from user/extra.mk as:
#   -DCMAKE_TOOLCHAIN_FILE=$(EXTRA_DIR)/lamina-toolchain-riscv64.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

set(CMAKE_C_COMPILER   riscv64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER riscv64-linux-gnu-g++)

# Link every executable/shared library with DT_RPATH=$ORIGIN so the dynamic
# loader AND dlopen() both find sibling modules (see NOTE above).  cmake
# escapes '$' correctly for the make generator, so write $ORIGIN directly
# (do NOT double the '$' here).
set(CMAKE_EXE_LINKER_FLAGS "-Wl,--disable-new-dtags -Wl,-rpath,$ORIGIN" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--disable-new-dtags -Wl,-rpath,$ORIGIN" CACHE STRING "" FORCE)

# The riscv64 glibc sysroot lives under /usr/riscv64-linux-gnu on Debian.
set(CMAKE_FIND_ROOT_PATH /usr/riscv64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

